/*
 * omdfs — structural write-ahead log implementation.
 *
 * On-disk record: len(u32) | crc(u32) | payload[len], where
 *   payload = op(u8) | p1len(u16) | p2len(u16) | p1[] | p2[]
 * and crc = CRC32(payload). Records carry no sequence number — the WAL is
 * append-only, so a record's ordinal position in the file IS its identity and apply
 * order, and the checkpoint is a positional index (the count of applied records).
 * Recovery scans from the start; the first record that is short or fails its CRC is a
 * torn tail (partial write at a crash) and ends the scan — everything before it is
 * intact. (See DESIGN.md § 2026-06-20, positional-index checkpoint.)
 */
#define _GNU_SOURCE
#include "journal.h"

#include "omdfs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_wal = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_total = 0;	/* records currently in the WAL (since last reclaim) */
static uint64_t g_applied = 0;	/* records applied to the backend (the checkpoint) */
static int64_t g_last_rename_idx = -1; /* index of the most recent appended RENAME,
					* or -1 for none; >= g_applied means a rename
					* has not yet reached the backend (see
					* wal_has_pending_rename / eviction). Reset to
					* -1 on reclaim (full drain => none pending). */

/* ---- crc32 (IEEE, reflected) ---- */

static uint32_t crc32_buf(const void *data, size_t len)
{
	static uint32_t tbl[256];
	static int init = 0;
	if (!init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tbl[i] = c;
		}
		init = 1;
	}
	const uint8_t *p = data;
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++)
		c = tbl[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

/* ---- paths + tiny I/O ---- */

static void wal_path(char out[PATH_MAX])
{
	snprintf(out, PATH_MAX, "%s/journal/wal", g_cfg.datadir);
}
static void ckpt_path(char out[PATH_MAX])
{
	snprintf(out, PATH_MAX, "%s/journal/checkpoint", g_cfg.datadir);
}

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* Read exactly len bytes; returns 1 on full read, 0 on clean EOF/short read
 * (torn tail), -1 on hard error. */
static int read_full(int fd, void *buf, size_t len)
{
	char *p = buf;
	while (len) {
		ssize_t n = read(fd, p, len);
		if (n == 0)
			return 0;
		if (n < 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 1;
}

/* ---- record (de)serialization ---- */

static char *pack_record(uint8_t op, const char *path, const char *path2,
			 size_t *out_len)
{
	uint16_t p1 = (uint16_t)(path ? strlen(path) : 0);
	uint16_t p2 = (uint16_t)(path2 ? strlen(path2) : 0);
	size_t plen = 1 + 2 + 2 + p1 + p2;
	size_t total = 4 + 4 + plen;
	char *rec = malloc(total);
	if (!rec)
		return NULL;

	char *pay = rec + 8;
	size_t o = 0;
	pay[o++] = (char)op;
	memcpy(pay + o, &p1, 2); o += 2;
	memcpy(pay + o, &p2, 2); o += 2;
	if (p1) { memcpy(pay + o, path, p1); o += p1; }
	if (p2) { memcpy(pay + o, path2, p2); o += p2; }

	uint32_t len32 = (uint32_t)plen;
	uint32_t crc = crc32_buf(pay, plen);
	memcpy(rec, &len32, 4);
	memcpy(rec + 4, &crc, 4);
	*out_len = total;
	return rec;
}

/* ---- recovery scan ---- */

/* Scan the WAL, invoking cb(rec, index, arg) for each valid record in file order
 * (index is the 0-based record position); stops at a torn tail. Returns the count of
 * valid records in *count (= the index just past the last record). */
static int wal_scan(uint64_t *count,
		    void (*cb)(const struct wal_record *, uint64_t, void *),
		    void *arg)
{
	*count = 0;
	char wp[PATH_MAX];
	wal_path(wp);
	int fd = open(wp, O_RDONLY);
	if (fd < 0)
		return errno == ENOENT ? 0 : -errno;

	uint64_t idx = 0;
	for (;;) {
		uint32_t len32, crc;
		int r = read_full(fd, &len32, 4);
		if (r <= 0)
			break; /* clean EOF or torn length */
		if (read_full(fd, &crc, 4) != 1)
			break;
		if (len32 < 5 || len32 > 2 * PATH_MAX + 5)
			break; /* implausible -> torn */
		char *pay = malloc(len32);
		if (!pay)
			break;
		if (read_full(fd, pay, len32) != 1 ||
		    crc32_buf(pay, len32) != crc) {
			free(pay);
			break; /* torn tail */
		}

		char p1buf[PATH_MAX], p2buf[PATH_MAX];
		size_t o = 0;
		uint8_t op = (uint8_t)pay[o++];
		uint16_t p1, p2;
		memcpy(&p1, pay + o, 2); o += 2;
		memcpy(&p2, pay + o, 2); o += 2;
		if ((size_t)5 + p1 + p2 != len32 || p1 >= PATH_MAX ||
		    p2 >= PATH_MAX) {
			free(pay);
			break;
		}
		memcpy(p1buf, pay + o, p1);
		p1buf[p1] = '\0';
		o += p1;
		memcpy(p2buf, pay + o, p2);
		p2buf[p2] = '\0';
		free(pay);

		/* The record's paths point at these stack buffers — valid only for the
		 * duration of cb, which must copy anything it keeps (collect_cb strdups). */
		struct wal_record rec = {
			.op = op,
			.path = p1buf,
			.path2 = p2 ? p2buf : NULL,
		};
		if (cb)
			cb(&rec, idx, arg);
		idx++;
	}
	close(fd);
	*count = idx;
	return 0;
}

/* ---- collect pending records ---- */

struct collect {
	uint64_t after;
	struct wal_record **arr; /* individually-allocated records (caller owns each) */
	size_t n, cap;
};

/* Recovery helper: track the index of the last RENAME seen so wal_init can restore
 * g_last_rename_idx across a remount (a crash may leave un-synced renames). */
static void last_rename_cb(const struct wal_record *rec, uint64_t idx, void *arg)
{
	int64_t *m = arg;
	if (rec->op == WAL_RENAME)
		*m = (int64_t)idx;
}

static void collect_cb(const struct wal_record *rec, uint64_t idx, void *arg)
{
	struct collect *c = arg;
	if (idx < c->after)
		return;
	if (c->n == c->cap) {
		size_t cap = c->cap ? c->cap * 2 : 32;
		struct wal_record **p = realloc(c->arr, cap * sizeof(*p));
		if (!p)
			return; /* OOM: drop the rest (best-effort recovery; ops stay on disk) */
		c->arr = p;
		c->cap = cap;
	}
	struct wal_record *nr = malloc(sizeof(*nr));
	if (!nr)
		return;
	nr->op = rec->op;
	nr->path = rec->path ? strdup(rec->path) : NULL;
	nr->path2 = rec->path2 ? strdup(rec->path2) : NULL;
	if ((rec->path && !nr->path) || (rec->path2 && !nr->path2)) {
		wal_record_free(nr);
		return;
	}
	c->arr[c->n++] = nr;
}

int wal_load_pending(uint64_t after_count, struct wal_record ***out, size_t *n)
{
	struct collect c = { .after = after_count };
	uint64_t total = 0;
	pthread_mutex_lock(&g_wal);
	int r = wal_scan(&total, collect_cb, &c);
	pthread_mutex_unlock(&g_wal);
	if (r != 0) {
		for (size_t i = 0; i < c.n; i++)
			wal_record_free(c.arr[i]);
		free(c.arr);
		return r;
	}
	*out = c.arr;
	*n = c.n;
	return 0;
}

/* ---- public API ---- */

void wal_record_free(struct wal_record *rec)
{
	if (!rec)
		return;
	free(rec->path);
	free(rec->path2);
	free(rec);
}

/* Atomically (re)write the checkpoint file = `applied` (write tmp, fsync, rename).
 * g_wal held by callers that touch shared state; wal_init runs single-threaded. */
static int write_checkpoint(uint64_t applied)
{
	char cp[PATH_MAX], tmp[PATH_MAX];
	ckpt_path(cp);
	snprintf(tmp, sizeof(tmp), "%s.tmp", cp);

	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -errno;
	if (write_all(fd, &applied, 8) != 0 || fsync(fd) != 0) {
		close(fd);
		unlink(tmp);
		return -EIO;
	}
	close(fd);
	if (rename(tmp, cp) != 0) {
		int e = errno;
		unlink(tmp);
		return -e;
	}
	return 0;
}

int wal_init(void)
{
	char cp[PATH_MAX];
	ckpt_path(cp);
	uint64_t applied = 0;
	int fd = open(cp, O_RDONLY);
	if (fd >= 0) {
		uint64_t s = 0;
		if (read_full(fd, &s, 8) == 1)
			applied = s;
		close(fd);
	}

	uint64_t total = 0;
	int64_t last_rename = -1;
	int r = wal_scan(&total, last_rename_cb, &last_rename);
	if (r != 0)
		return r;

	/* Clamp a stale-high checkpoint (the only way applied > total is a crash in the
	 * reclaim window, after the WAL was truncated but before the checkpoint reset —
	 * which provably means everything had been applied). Rewrite it durably so a
	 * later append can't be miscounted against the stale value. */
	if (applied > total) {
		applied = total;
		write_checkpoint(applied); /* best-effort; re-clamped next mount on failure */
	}
	g_total = total;
	g_applied = applied;
	g_last_rename_idx = last_rename;
	return 0;
}

struct wal_record *wal_append(uint8_t op, const char *path, const char *path2)
{
	/* Build the heap record first (this is what we hand back); only on a clean
	 * disk write does it become a real, counted WAL record. */
	struct wal_record *r = malloc(sizeof(*r));
	if (!r)
		return NULL;
	r->op = op;
	r->path = path ? strdup(path) : NULL;
	r->path2 = path2 ? strdup(path2) : NULL;
	if ((path && !r->path) || (path2 && !r->path2)) {
		wal_record_free(r);
		return NULL;
	}

	char wp[PATH_MAX];
	wal_path(wp);

	pthread_mutex_lock(&g_wal);
	size_t len;
	char *rec = pack_record(op, path, path2, &len);
	if (!rec) {
		pthread_mutex_unlock(&g_wal);
		wal_record_free(r);
		return NULL;
	}
	int fd = open(wp, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0 || write_all(fd, rec, len) != 0) {
		if (fd >= 0)
			close(fd);
		free(rec);
		pthread_mutex_unlock(&g_wal);
		wal_record_free(r);
		return NULL;
	}
	close(fd);
	free(rec);
	uint64_t idx = g_total; /* 0-based position of the record just written */
	g_total = idx + 1;
	if (op == WAL_RENAME)
		g_last_rename_idx = (int64_t)idx;
	pthread_mutex_unlock(&g_wal);
	return r;
}

int wal_has_pending_rename(void)
{
	pthread_mutex_lock(&g_wal);
	int pending = g_last_rename_idx >= 0 &&
		      (uint64_t)g_last_rename_idx >= g_applied;
	pthread_mutex_unlock(&g_wal);
	return pending;
}

int wal_fully_drained(void)
{
	pthread_mutex_lock(&g_wal);
	int drained = (g_applied == g_total);
	pthread_mutex_unlock(&g_wal);
	return drained;
}

uint64_t wal_applied(void)
{
	pthread_mutex_lock(&g_wal);
	uint64_t a = g_applied;
	pthread_mutex_unlock(&g_wal);
	return a;
}

uint64_t wal_total(void)
{
	pthread_mutex_lock(&g_wal);
	uint64_t t = g_total;
	pthread_mutex_unlock(&g_wal);
	return t;
}

int wal_checkpoint(uint64_t applied)
{
	char wp[PATH_MAX];
	wal_path(wp);

	pthread_mutex_lock(&g_wal);
	if (applied >= g_total) {
		/* Full drain -> reclaim. Truncate the WAL FIRST, then reset the
		 * checkpoint to 0. A crash between leaves {empty WAL, stale checkpoint},
		 * which recovery clamps to 0 (the only state where checkpoint > records).
		 * The reverse order could leave {full WAL, checkpoint 0} -> replay the
		 * whole applied prefix. */
		int wfd = open(wp, O_WRONLY | O_TRUNC);
		if (wfd >= 0) {
			close(wfd);
			/* WAL is now empty on disk; the in-memory model must agree even
			 * if the checkpoint write below fails (recovery re-clamps to 0
			 * against the empty WAL, so the stale on-disk checkpoint is safe). */
			g_total = 0;
			g_applied = 0;
			g_last_rename_idx = -1;
			int r = write_checkpoint(0);
			pthread_mutex_unlock(&g_wal);
			return r;
		}
		/* Truncate failed: don't reset to 0 (a full WAL with checkpoint 0 would
		 * replay everything). Record applied == total instead — recovery then
		 * replays nothing, safely, leaving the WAL to be reclaimed next time. */
		applied = g_total;
	}
	int r = write_checkpoint(applied);
	if (r != 0) {
		pthread_mutex_unlock(&g_wal);
		return r;
	}
	g_applied = applied;
	pthread_mutex_unlock(&g_wal);
	return 0;
}
