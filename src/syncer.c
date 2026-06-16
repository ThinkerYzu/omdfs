/*
 * omdfs — background two-phase syncer implementation.
 */
#define _GNU_SOURCE
#include "syncer.h"

#include "dirtyset.h"
#include "evict.h"
#include "journal.h"
#include "meta.h"
#include "omdfs.h"
#include "status.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SYNC_INTERVAL_MS 700
#define COPY_BUF (128 * 1024)
#define DRAIN_MAX 8 /* final-drain cycles attempted on clean unmount */

/* ---- thread control ---- */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_t s_thread;
static int s_stop, s_kick, s_running;
static long long s_last_clean; /* epoch of the last fully-drained cycle */

/* ---- helpers ---- */

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

static void join_path(char out[PATH_MAX], const char *dir, const char *name)
{
	if (dir[0] == '/' && dir[1] == '\0')
		snprintf(out, PATH_MAX, "/%s", name);
	else
		snprintf(out, PATH_MAX, "%s/%s", dir, name);
}

/* Copy a whole file src -> dst via a temp file + atomic rename. */
static int copy_file(const char *src, const char *dst)
{
	int sfd = open(src, O_RDONLY);
	if (sfd < 0)
		return -errno;

	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.omdfs.tmp", dst);
	int dfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (dfd < 0) {
		int e = -errno;
		close(sfd);
		return e;
	}

	char *buf = malloc(COPY_BUF);
	int err = 0;
	if (!buf) {
		err = -ENOMEM;
	} else {
		ssize_t n;
		while ((n = read(sfd, buf, COPY_BUF)) > 0) {
			if (write_all(dfd, buf, (size_t)n) != 0) {
				err = -EIO;
				break;
			}
		}
		if (n < 0)
			err = -errno;
		free(buf);
	}
	close(sfd);
	if (err == 0 && fsync(dfd) != 0)
		err = -errno;
	close(dfd);
	if (err) {
		unlink(tmp);
		return err;
	}
	if (rename(tmp, dst) != 0) {
		int e = -errno;
		unlink(tmp);
		return e;
	}
	return 0;
}

/* Best-effort apply of a (file/dir) entry's logical attributes to the backend. */
static void apply_stat_attrs(const char *bp, const struct stat *s)
{
	int ignored = chmod(bp, s->st_mode & 07777);
	ignored = chown(bp, s->st_uid, s->st_gid); /* needs privilege */
	(void)ignored;
	struct timespec ts[2] = { s->st_atim, s->st_mtim };
	utimensat(AT_FDCWD, bp, ts, 0);
}

static void apply_attrs(const char *bp, const struct omdfs_entry *e)
{
	apply_stat_attrs(bp, &e->st);
}

/* ---- phase 1: structural drain ---- */

/* Human-readable name for a WAL op, for the status file. */
static const char *op_name(uint8_t op)
{
	switch (op) {
	case WAL_CREATE: return "create";
	case WAL_MKDIR: return "mkdir";
	case WAL_UNLINK: return "unlink";
	case WAL_RMDIR: return "rmdir";
	case WAL_RENAME: return "rename";
	case WAL_SYMLINK: return "symlink";
	}
	return "?";
}

/* Is a sync error permanent (won't clear by waiting for the backend to return)?
 * Permanent errors mean a stuck op needs operator attention; transient ones are
 * just "backend down/slow" and clear on their own. */
static int is_permanent(int err)
{
	switch (err) {
	case EACCES:
	case EPERM:
	case ENOSPC:
	case EROFS:
	case EDQUOT:
	case ENOTEMPTY:
	case EEXIST:
		return 1;
	default:
		return 0; /* ENETUNREACH/EIO/ETIMEDOUT/ESTALE/... = transient */
	}
}

/* Apply one structural record to the backend. Returns 0 if applied (or
 * idempotently a no-op), else the positive errno to retry on (blocks later ops). */
static int apply_structural(const struct wal_record *r)
{
	char bp[PATH_MAX], bp2[PATH_MAX];
	backend_path(bp, r->path);

	switch (r->op) {
	case WAL_CREATE: {
		/* O_EXCL so we never reopen an existing file: the create's
		 * intent is just "the file exists", and an existing target
		 * (e.g. a read-only git loose object replayed after rename)
		 * already satisfies that. Without O_EXCL, O_WRONLY on an
		 * existing 0444 file returns EACCES and wedges the queue. */
		int fd = open(bp, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0)
			return errno == EEXIST ? 0 : errno;
		close(fd);
		return 0;
	}
	case WAL_MKDIR:
		if (mkdir(bp, 0755) != 0 && errno != EEXIST)
			return errno;
		return 0;
	case WAL_UNLINK:
		if (unlink(bp) != 0 && errno != ENOENT)
			return errno;
		return 0;
	case WAL_RMDIR:
		if (rmdir(bp) != 0 && errno != ENOENT)
			return errno;
		return 0;
	case WAL_RENAME:
		backend_path(bp2, r->path2);
		if (rename(bp, bp2) != 0 && errno != ENOENT)
			return errno;
		return 0;
	case WAL_SYMLINK:
		if (symlink(r->path2, bp) != 0 && errno != EEXIST)
			return errno;
		return 0;
	}
	return 0; /* unknown op: skip rather than wedge the queue */
}

static void phase_structural(struct omdfs_status *st)
{
	uint64_t synced = wal_synced_seq();
	struct wal_record *recs = NULL;
	size_t n = 0;
	if (wal_load_pending(synced, &recs, &n) != 0)
		return;

	uint64_t applied = synced;
	size_t done = 0;
	for (size_t i = 0; i < n; i++) {
		int err = apply_structural(&recs[i]);
		if (err == 0) {
			applied = recs[i].seq;
			done++;
			continue;
		}
		/* Strict in-order: the head op is blocked; surface it and stop. */
		st->stuck_errno = err;
		st->stuck = is_permanent(err);
		snprintf(st->stuck_op, sizeof(st->stuck_op), "%s",
			 op_name(recs[i].op));
		snprintf(st->stuck_path, sizeof(st->stuck_path), "%s",
			 recs[i].path);
		break;
	}
	st->pending_structural += (long)(n - done);
	free(recs);
	if (applied != synced)
		wal_checkpoint(applied);
}

/* ---- phase 2: dirty flush ---- */

/* Flush one dirty entry. Returns 1 if it is still dirty afterwards (the flush
 * failed — backend not ready), 0 if it is now clean. */
static int flush_entry(const char *fuse_dir, const struct omdfs_entry *e)
{
	char fpath[PATH_MAX], bp[PATH_MAX];
	join_path(fpath, fuse_dir, e->name);
	backend_path(bp, fpath);

	int ok = 1;
	if (e->flags & OMDFS_F_DIRTY_CONTENT) {
		char cp[PATH_MAX];
		cache_path(cp, fpath);
		if (copy_file(cp, bp) == 0)
			apply_attrs(bp, e);
		else
			ok = 0; /* backend path not ready yet; retry */
	} else if (e->flags & OMDFS_F_DIRTY_ATTR) {
		apply_attrs(bp, e);
	}
	if (ok)
		meta_mark_flags(fuse_dir, e->name, 0,
				OMDFS_F_DIRTY_CONTENT | OMDFS_F_DIRTY_ATTR);
	return !ok;
}

/* Flush the dirty entries of an already-loaded index. Returns 1 if any entry is
 * still dirty afterwards (its flush failed — backend not ready), else 0. */
static int flush_loaded(const char *fuse_dir, struct omdfs_index *idx,
			struct omdfs_status *st)
{
	int still_dirty = 0;
	for (size_t i = 0; i < idx->n; i++) {
		struct omdfs_entry *e = &idx->entries[i];
		if (!(e->flags & (OMDFS_F_DIRTY_CONTENT | OMDFS_F_DIRTY_ATTR)))
			continue;
		if (flush_entry(fuse_dir, e)) {
			/* Still pending: count it in the backlog. */
			still_dirty = 1;
			st->dirty_files++;
			if (e->flags & OMDFS_F_DIRTY_CONTENT)
				st->dirty_bytes += (long long)e->st.st_size;
		}
	}
	return still_dirty;
}

/* Flush one directory's own dirty entries (non-recursive). Returns 1 if any
 * stayed dirty. A removed/uncached directory has nothing to flush. */
static int flush_one_dir(const char *fuse_dir, struct omdfs_status *st)
{
	struct omdfs_index idx;
	if (meta_try_load(fuse_dir, &idx) != 0)
		return 0;
	int still_dirty = flush_loaded(fuse_dir, &idx, st);
	meta_free_index(&idx);
	return still_dirty;
}

/* Recovery / fallback: walk the whole cached tree, flush every dirty entry, and
 * re-record any directory that couldn't be fully flushed so the fast path retries
 * it. Used on the first cycle (dirty flags survive a crash but the in-memory set
 * starts empty) and whenever the set asks for a full rescan. */
static void flush_tree(const char *fuse_dir, struct omdfs_status *st)
{
	struct omdfs_index idx;
	if (meta_try_load(fuse_dir, &idx) != 0)
		return; /* not a cached directory */

	if (flush_loaded(fuse_dir, &idx, st))
		dirtyset_add(fuse_dir);

	for (size_t i = 0; i < idx.n; i++) {
		if (idx.entries[i].type != OMDFS_T_DIR)
			continue;
		char child[PATH_MAX];
		join_path(child, fuse_dir, idx.entries[i].name);
		flush_tree(child, st);
	}
	meta_free_index(&idx);
}

/* Flush all pending dirty content/attrs. Normally this visits only the recorded
 * dirty directories (dirtyset); on the initial cycle or after a set overflow it
 * falls back to a full tree walk that also re-seeds the set. */
static void flush_pending(struct omdfs_status *st)
{
	size_t n;
	int full;
	char **dirs = dirtyset_take(&n, &full);
	if (full) {
		dirtyset_free(dirs, n); /* the walk rediscovers everything */
		flush_tree("/", st);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		if (flush_one_dir(dirs[i], st))
			dirtyset_add(dirs[i]); /* still dirty: retry next cycle */
	}
	dirtyset_free(dirs, n);
}

/* ---- full resync (operator-triggered reconcile) ---- */

struct resync_stats {
	long dirs, files, links, meta_only;
	long long bytes;
	int failed;
};

/* Push one entry to the backend unconditionally (existence + content + attrs),
 * ignoring dirty flags. Used by the offline --resync reconcile, not the cycle. */
static void resync_entry(const char *fuse_dir, const struct omdfs_entry *e,
			 struct resync_stats *rs)
{
	char fpath[PATH_MAX], bp[PATH_MAX];
	join_path(fpath, fuse_dir, e->name);
	backend_path(bp, fpath);

	if (e->type == OMDFS_T_DIR) {
		if (mkdir(bp, e->st.st_mode & 07777) != 0 && errno != EEXIST) {
			rs->failed++;
			return;
		}
		apply_stat_attrs(bp, &e->st);
		rs->dirs++;
		return;
	}
	if (e->type == OMDFS_T_LINK) {
		if (!e->link)
			return;
		unlink(bp); /* re-point an existing link; symlink() can't replace */
		if (symlink(e->link, bp) != 0) {
			rs->failed++;
			return;
		}
		rs->links++;
		return;
	}
	/* regular file */
	if (e->flags & OMDFS_F_CONTENT_CACHED) {
		char cp[PATH_MAX];
		cache_path(cp, fpath);
		if (copy_file(cp, bp) != 0) { /* tmp + rename: replaces a 0444 file */
			rs->failed++;
			return;
		}
		apply_stat_attrs(bp, &e->st);
		rs->files++;
		rs->bytes += (long long)e->st.st_size;
	} else {
		/* Content was never fetched into the cache, so the cache has no
		 * bytes to push — don't clobber whatever is on the backend. Just
		 * ensure the file exists and reconcile its attrs. O_RDONLY|O_CREAT
		 * never re-opens an existing (possibly read-only) file for write. */
		int fd = open(bp, O_RDONLY | O_CREAT, e->st.st_mode & 07777);
		if (fd < 0) {
			rs->failed++;
			return;
		}
		close(fd);
		apply_stat_attrs(bp, &e->st);
		rs->meta_only++;
	}
}

/* Recursively force every cached entry under `fuse_dir` onto the backend. */
static void resync_tree(const char *fuse_dir, struct resync_stats *rs)
{
	struct omdfs_index idx;
	if (meta_try_load(fuse_dir, &idx) != 0)
		return; /* not a cached directory */

	/* The directory itself: ensure it exists with its own attrs. */
	char bp[PATH_MAX];
	backend_path(bp, fuse_dir);
	if (mkdir(bp, idx.dir_st.st_mode & 07777) != 0 && errno != EEXIST)
		rs->failed++;
	apply_stat_attrs(bp, &idx.dir_st);

	for (size_t i = 0; i < idx.n; i++) {
		struct omdfs_entry *e = &idx.entries[i];
		resync_entry(fuse_dir, e, rs);
		if (e->type == OMDFS_T_DIR) {
			char child[PATH_MAX];
			join_path(child, fuse_dir, e->name);
			resync_tree(child, rs);
		}
	}
	meta_free_index(&idx);
}

int syncer_resync(void)
{
	/* Drain the structural WAL first so pending deletes/renames apply before
	 * we re-assert existence from the cache. Best-effort: a stuck op is
	 * reported but we still push content/attrs for everything present. */
	struct omdfs_status st;
	memset(&st, 0, sizeof(st));
	phase_structural(&st);

	struct resync_stats rs;
	memset(&rs, 0, sizeof(rs));
	resync_tree("/", &rs);

	fprintf(stderr,
		"omdfs: resync pushed %ld dirs, %ld files (%lld bytes), "
		"%ld symlinks; %ld files metadata-only (content not cached); "
		"%d failed\n",
		rs.dirs, rs.files, rs.bytes, rs.links, rs.meta_only, rs.failed);
	if (st.stuck_errno)
		fprintf(stderr,
			"omdfs: warning: a structural op is blocked (%s %s, "
			"errno %d) — some namespace changes may not have applied\n",
			st.stuck_op, st.stuck_path, st.stuck_errno);
	return (rs.failed == 0 && !st.stuck_errno) ? 0 : 1;
}

/* ---- cycle + thread ---- */

/* Run one full sync cycle and fill `st` with the resulting backlog/health so the
 * caller can publish it. */
static void sync_once(struct omdfs_status *st)
{
	memset(st, 0, sizeof(*st));

	phase_structural(st); /* backend dirs/files exist before content flush */
	flush_pending(st);

	/* Flushed content is now clean and may be reclaimed; this also refreshes
	 * the backpressure flag (clears it once dirty data has drained). */
	evict_run_if_needed();

	st->cache_bytes = (long long)evict_total();
	st->cache_budget = (long long)evict_budget();
	st->cache_hard_limit = (long long)evict_hard_limit();
	st->backpressure = evict_pressure();

	/* A cycle that left no backlog and hit no stuck op is a clean sync. */
	if (!st->pending_structural && !st->dirty_files && !st->stuck_errno)
		s_last_clean = (long long)time(NULL);
	st->last_sync_epoch = s_last_clean;
}

static void *syncer_loop(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&s_lock);
		if (!s_stop && !s_kick) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			long ns = ts.tv_nsec +
				  (SYNC_INTERVAL_MS % 1000) * 1000000L;
			ts.tv_sec += SYNC_INTERVAL_MS / 1000 + ns / 1000000000L;
			ts.tv_nsec = ns % 1000000000L;
			pthread_cond_timedwait(&s_cond, &s_lock, &ts);
		}
		int stop = s_stop;
		s_kick = 0;
		pthread_mutex_unlock(&s_lock);

		struct omdfs_status st;
		sync_once(&st);
		status_publish(&st);
		if (stop)
			break;
	}
	return NULL;
}

int syncer_start(void)
{
	if (pthread_create(&s_thread, NULL, syncer_loop, NULL) != 0)
		return -1;
	s_running = 1;
	return 0;
}

void syncer_kick(void)
{
	pthread_mutex_lock(&s_lock);
	s_kick = 1;
	pthread_cond_signal(&s_cond);
	pthread_mutex_unlock(&s_lock);
}

void syncer_stop(void)
{
	if (!s_running)
		return;
	pthread_mutex_lock(&s_lock);
	s_stop = 1;
	pthread_cond_signal(&s_cond);
	pthread_mutex_unlock(&s_lock);
	pthread_join(s_thread, NULL);
	s_running = 0;

	/* Clean unmount: the thread ran one final cycle on stop, but a single cycle
	 * can leave work if an op transiently failed mid-drain. Keep draining (now
	 * single-threaded) until the backlog is empty or stops shrinking, so a
	 * reachable backend is fully caught up before we exit. With the backend down
	 * this converges immediately (no progress) and leaves the WAL/dirty flags for
	 * recovery on the next mount. */
	struct omdfs_status st;
	memset(&st, 0, sizeof(st));
	for (int i = 0; i < DRAIN_MAX; i++) {
		long prev = st.pending_structural + st.dirty_files;
		sync_once(&st);
		long now = st.pending_structural + st.dirty_files;
		if (now == 0)
			break; /* fully drained */
		if (i > 0 && now >= prev)
			break; /* no progress: backend down, nothing more to do */
	}
	status_publish(&st);
}
