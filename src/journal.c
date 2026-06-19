/*
 * omdfs — structural write-ahead log implementation.
 *
 * On-disk record: len(u32) | crc(u32) | payload[len], where
 *   payload = seq(u64) | op(u8) | p1len(u16) | p2len(u16) | p1[] | p2[]
 * and crc = CRC32(payload). Recovery scans from the start; the first record that
 * is short or fails its CRC is a torn tail (partial write at a crash) and ends
 * the scan — everything before it is intact.
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
static uint64_t g_next_seq = 1;	  /* next seq to assign */
static uint64_t g_synced_seq = 0; /* from the checkpoint */
static uint64_t g_last_rename_seq = 0; /* seq of the most recent appended RENAME;
					* > g_synced_seq means a rename has not yet
					* reached the backend (see
					* wal_has_pending_rename / eviction). */

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
			 uint64_t seq, size_t *out_len)
{
	uint16_t p1 = (uint16_t)(path ? strlen(path) : 0);
	uint16_t p2 = (uint16_t)(path2 ? strlen(path2) : 0);
	size_t plen = 8 + 1 + 2 + 2 + p1 + p2;
	size_t total = 4 + 4 + plen;
	char *rec = malloc(total);
	if (!rec)
		return NULL;

	char *pay = rec + 8;
	size_t o = 0;
	memcpy(pay + o, &seq, 8); o += 8;
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

/* Scan the WAL, invoking cb(rec) for each valid record in order; stops at a torn
 * tail. Tracks the max seq seen in *max_seq. */
static int wal_scan(uint64_t *max_seq,
		    void (*cb)(const struct wal_record *, void *), void *arg)
{
	char wp[PATH_MAX];
	wal_path(wp);
	int fd = open(wp, O_RDONLY);
	if (fd < 0)
		return errno == ENOENT ? 0 : -errno;

	for (;;) {
		uint32_t len32, crc;
		int r = read_full(fd, &len32, 4);
		if (r <= 0)
			break; /* clean EOF or torn length */
		if (read_full(fd, &crc, 4) != 1)
			break;
		if (len32 < 13 || len32 > 2 * PATH_MAX + 13)
			break; /* implausible -> torn */
		char *pay = malloc(len32);
		if (!pay)
			break;
		if (read_full(fd, pay, len32) != 1 ||
		    crc32_buf(pay, len32) != crc) {
			free(pay);
			break; /* torn tail */
		}

		struct wal_record rec;
		memset(&rec, 0, sizeof(rec));
		size_t o = 0;
		memcpy(&rec.seq, pay + o, 8); o += 8;
		rec.op = (uint8_t)pay[o++];
		uint16_t p1, p2;
		memcpy(&p1, pay + o, 2); o += 2;
		memcpy(&p2, pay + o, 2); o += 2;
		if ((size_t)13 + p1 + p2 != len32 || p1 >= PATH_MAX ||
		    p2 >= PATH_MAX) {
			free(pay);
			break;
		}
		memcpy(rec.path, pay + o, p1);
		rec.path[p1] = '\0';
		o += p1;
		memcpy(rec.path2, pay + o, p2);
		rec.path2[p2] = '\0';
		free(pay);

		if (rec.seq > *max_seq)
			*max_seq = rec.seq;
		if (cb)
			cb(&rec, arg);
	}
	close(fd);
	return 0;
}

/* ---- collect pending records ---- */

struct collect {
	uint64_t after;
	struct wal_record *arr;
	size_t n, cap;
};

/* Recovery helper: track the highest RENAME seq seen so wal_init can restore
 * g_last_rename_seq across a remount (a crash may leave un-synced renames). */
static void max_rename_cb(const struct wal_record *rec, void *arg)
{
	uint64_t *m = arg;
	if (rec->op == WAL_RENAME && rec->seq > *m)
		*m = rec->seq;
}

static void collect_cb(const struct wal_record *rec, void *arg)
{
	struct collect *c = arg;
	if (rec->seq <= c->after)
		return;
	if (c->n == c->cap) {
		size_t cap = c->cap ? c->cap * 2 : 32;
		struct wal_record *p = realloc(c->arr, cap * sizeof(*p));
		if (!p)
			return;
		c->arr = p;
		c->cap = cap;
	}
	c->arr[c->n++] = *rec;
}

int wal_load_pending(uint64_t after_seq, struct wal_record **out, size_t *n)
{
	struct collect c = { .after = after_seq };
	uint64_t max = 0;
	pthread_mutex_lock(&g_wal);
	int r = wal_scan(&max, collect_cb, &c);
	pthread_mutex_unlock(&g_wal);
	if (r != 0) {
		free(c.arr);
		return r;
	}
	*out = c.arr;
	*n = c.n;
	return 0;
}

/* ---- public API ---- */

int wal_init(void)
{
	char cp[PATH_MAX];
	ckpt_path(cp);
	int fd = open(cp, O_RDONLY);
	if (fd >= 0) {
		uint64_t s = 0;
		if (read_full(fd, &s, 8) == 1)
			g_synced_seq = s;
		close(fd);
	}

	uint64_t max = 0, last_rename = 0;
	int r = wal_scan(&max, max_rename_cb, &last_rename);
	if (r != 0)
		return r;
	g_last_rename_seq = last_rename;
	uint64_t base = g_synced_seq > max ? g_synced_seq : max;
	g_next_seq = base + 1;
	return 0;
}

uint64_t wal_append(uint8_t op, const char *path, const char *path2)
{
	char wp[PATH_MAX];
	wal_path(wp);

	pthread_mutex_lock(&g_wal);
	uint64_t seq = g_next_seq;
	size_t len;
	char *rec = pack_record(op, path, path2, seq, &len);
	if (!rec) {
		pthread_mutex_unlock(&g_wal);
		return 0;
	}
	int fd = open(wp, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0 || write_all(fd, rec, len) != 0) {
		if (fd >= 0)
			close(fd);
		free(rec);
		pthread_mutex_unlock(&g_wal);
		return 0;
	}
	close(fd);
	free(rec);
	g_next_seq = seq + 1;
	if (op == WAL_RENAME)
		g_last_rename_seq = seq;
	pthread_mutex_unlock(&g_wal);
	return seq;
}

int wal_has_pending_rename(void)
{
	pthread_mutex_lock(&g_wal);
	int pending = g_last_rename_seq > g_synced_seq;
	pthread_mutex_unlock(&g_wal);
	return pending;
}

uint64_t wal_synced_seq(void)
{
	pthread_mutex_lock(&g_wal);
	uint64_t s = g_synced_seq;
	pthread_mutex_unlock(&g_wal);
	return s;
}

int wal_checkpoint(uint64_t synced_seq)
{
	char cp[PATH_MAX], tmp[PATH_MAX], wp[PATH_MAX];
	ckpt_path(cp);
	snprintf(tmp, sizeof(tmp), "%s.tmp", cp);
	wal_path(wp);

	pthread_mutex_lock(&g_wal);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		pthread_mutex_unlock(&g_wal);
		return -errno;
	}
	if (write_all(fd, &synced_seq, 8) != 0 || fsync(fd) != 0) {
		close(fd);
		unlink(tmp);
		pthread_mutex_unlock(&g_wal);
		return -EIO;
	}
	close(fd);
	if (rename(tmp, cp) != 0) {
		int e = errno;
		unlink(tmp);
		pthread_mutex_unlock(&g_wal);
		return -e;
	}
	g_synced_seq = synced_seq;
	/* Fully drained -> reclaim the log. */
	if (synced_seq == g_next_seq - 1) {
		int wfd = open(wp, O_WRONLY | O_TRUNC);
		if (wfd >= 0)
			close(wfd);
	}
	pthread_mutex_unlock(&g_wal);
	return 0;
}
