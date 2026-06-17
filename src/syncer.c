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

#include <dirent.h>
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

/* Guards the shared status/resync stat counters updated from pool workers. */
static pthread_mutex_t s_stat_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---- bounded worker pool (parallel copy_file across backend connections) ----
 *
 * The expensive part of a sync is copy_file's per-file fsync — a full network
 * round-trip (~100 ms over sshfs) that the single syncer thread would otherwise
 * issue one at a time, lighting up just one connection. A backend mounted with
 * several connections (e.g. sshfs max_conns) only speeds up if the fsyncs run
 * concurrently. So both the dirty flush and the --resync push hand each file to a
 * small pool of worker threads and overlap their round-trips; measured ~4x at 16
 * workers on a 16-connection sshfs mount, plateauing at the connection count.
 *
 * The pool runs self-contained void(*)(void*) jobs whose args are heap-allocated
 * by the producer and freed by the worker. The job queue is bounded, so a producer
 * submitting a whole-tree walk blocks when workers fall behind (natural memory cap
 * and backpressure). pool_drain() is the barrier the producer waits on before
 * reading the aggregated stats. */
#define SYNC_WORKERS_DEFAULT 16
#define SYNC_WORKERS_MAX 64
#define POOL_QUEUE_MAX 256

struct pool_job {
	void (*fn)(void *);
	void *arg;
};

static pthread_mutex_t pool_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t pool_not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t pool_idle = PTHREAD_COND_INITIALIZER;
static struct pool_job pool_q[POOL_QUEUE_MAX];
static int pool_head, pool_tail, pool_len;
static int pool_inflight; /* jobs claimed by a worker but not yet finished */
static int pool_started, pool_quit, pool_nthreads;
static pthread_t pool_threads[SYNC_WORKERS_MAX];

static int desired_workers(void)
{
	const char *s = getenv("OMDFS_SYNC_WORKERS");
	if (!s || !*s)
		return SYNC_WORKERS_DEFAULT;
	int v = atoi(s);
	if (v < 1)
		v = 1;
	if (v > SYNC_WORKERS_MAX)
		v = SYNC_WORKERS_MAX;
	return v;
}

static void *pool_worker(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&pool_mtx);
		while (pool_len == 0 && !pool_quit)
			pthread_cond_wait(&pool_not_empty, &pool_mtx);
		if (pool_len == 0 && pool_quit) {
			pthread_mutex_unlock(&pool_mtx);
			return NULL;
		}
		struct pool_job job = pool_q[pool_head];
		pool_head = (pool_head + 1) % POOL_QUEUE_MAX;
		pool_len--;
		pool_inflight++;
		pthread_cond_signal(&pool_not_full);
		pthread_mutex_unlock(&pool_mtx);

		job.fn(job.arg);

		pthread_mutex_lock(&pool_mtx);
		pool_inflight--;
		if (pool_len == 0 && pool_inflight == 0)
			pthread_cond_broadcast(&pool_idle);
		pthread_mutex_unlock(&pool_mtx);
	}
}

/* Start the pool if not already running. Idempotent. */
static void pool_start(void)
{
	pthread_mutex_lock(&pool_mtx);
	if (pool_started) {
		pthread_mutex_unlock(&pool_mtx);
		return;
	}
	pool_quit = 0;
	pool_head = pool_tail = pool_len = pool_inflight = 0;
	int want = desired_workers(), n = 0;
	for (; n < want; n++)
		if (pthread_create(&pool_threads[n], NULL, pool_worker, NULL) != 0)
			break;
	pool_nthreads = n;
	pool_started = (n > 0); /* n==0: pool_submit falls back to inline */
	pthread_mutex_unlock(&pool_mtx);
}

/* Enqueue a job (blocks while the queue is full). With no pool started, runs the
 * job inline so callers work unconditionally. */
static void pool_submit(void (*fn)(void *), void *arg)
{
	pthread_mutex_lock(&pool_mtx);
	if (!pool_started) {
		pthread_mutex_unlock(&pool_mtx);
		fn(arg);
		return;
	}
	while (pool_len == POOL_QUEUE_MAX && !pool_quit)
		pthread_cond_wait(&pool_not_full, &pool_mtx);
	pool_q[pool_tail].fn = fn;
	pool_q[pool_tail].arg = arg;
	pool_tail = (pool_tail + 1) % POOL_QUEUE_MAX;
	pool_len++;
	pthread_cond_signal(&pool_not_empty);
	pthread_mutex_unlock(&pool_mtx);
}

/* Block until every submitted job has completed. */
static void pool_drain(void)
{
	pthread_mutex_lock(&pool_mtx);
	while (pool_started && (pool_len > 0 || pool_inflight > 0))
		pthread_cond_wait(&pool_idle, &pool_mtx);
	pthread_mutex_unlock(&pool_mtx);
}

/* Drain, then signal the workers to exit and join them. */
static void pool_stop(void)
{
	pool_drain();
	pthread_mutex_lock(&pool_mtx);
	if (!pool_started) {
		pthread_mutex_unlock(&pool_mtx);
		return;
	}
	pool_quit = 1;
	pthread_cond_broadcast(&pool_not_empty);
	int n = pool_nthreads;
	pthread_mutex_unlock(&pool_mtx);
	for (int i = 0; i < n; i++)
		pthread_join(pool_threads[i], NULL);
	pthread_mutex_lock(&pool_mtx);
	pool_started = 0;
	pthread_mutex_unlock(&pool_mtx);
}

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

/* Flush one dirty entry to the backend, then either clear its flags (success) or
 * leave it dirty and account the still-pending backlog + re-arm its directory
 * (failure). Runs on a pool worker, so all shared-state touches are thread-safe:
 * meta_mark_flags / dirtyset_add are internally locked, the status counters are
 * guarded by s_stat_mtx.
 *
 * When `dry` is set (the flush is paused via state/flush-off) we never touch the
 * backend and never clear the flags: we just count the entry into the backlog and
 * re-arm its directory, exactly as the failure path does — so the status stays
 * honest about what is waiting and the dirty-set is preserved for when flushing
 * resumes. A dry entry always runs inline (flush_submit never hands it to the
 * pool), so `dry` is read only on the syncer thread. */
static void flush_entry(const char *fuse_dir, const struct omdfs_entry *e,
			struct omdfs_status *st, int dry)
{
	char fpath[PATH_MAX], bp[PATH_MAX];
	join_path(fpath, fuse_dir, e->name);
	backend_path(bp, fpath);

	int ok = dry ? 0 : 1; /* dry: treat as "not pushed" -> count + re-arm */
	if (dry) {
		/* no backend I/O, no flag clearing */
	} else if (e->flags & OMDFS_F_DIRTY_CONTENT) {
		char cp[PATH_MAX];
		cache_path(cp, fpath);
		if (copy_file(cp, bp) == 0)
			apply_attrs(bp, e);
		else
			ok = 0; /* backend path not ready yet; retry */
	} else if (e->flags & OMDFS_F_DIRTY_ATTR) {
		apply_attrs(bp, e);
	}
	if (ok) {
		meta_mark_flags(fuse_dir, e->name, 0,
				OMDFS_F_DIRTY_CONTENT | OMDFS_F_DIRTY_ATTR);
		return;
	}
	/* Still pending: count it in the backlog and re-record the dir so the fast
	 * path retries it next cycle. */
	pthread_mutex_lock(&s_stat_mtx);
	st->dirty_files++;
	if (e->flags & OMDFS_F_DIRTY_CONTENT)
		st->dirty_bytes += (long long)e->st.st_size;
	pthread_mutex_unlock(&s_stat_mtx);
	dirtyset_add(fuse_dir);
}

/* A single dirty entry handed to the pool. Owns its strings; freed by the worker.
 * The status pointer is shared and updated under s_stat_mtx. */
struct flush_arg {
	char *dir;
	struct omdfs_entry e; /* deep-copied: e.name / e.link owned here */
	struct omdfs_status *st;
};

static void flush_worker(void *p)
{
	struct flush_arg *a = p;
	flush_entry(a->dir, &a->e, a->st, 0);
	free(a->dir);
	free(a->e.name);
	free(a->e.link);
	free(a);
}

/* Hand one dirty entry to the pool (deep-copying what the worker needs, since the
 * loaded index is freed as soon as the producer returns). Falls back to flushing
 * inline if the copy can't be allocated. A dry (paused) flush does no backend I/O
 * and runs inline — no need to deep-copy or wake a worker. */
static void flush_submit(const char *fuse_dir, const struct omdfs_entry *e,
			 struct omdfs_status *st, int dry)
{
	if (dry) {
		flush_entry(fuse_dir, e, st, 1);
		return;
	}
	struct flush_arg *a = malloc(sizeof(*a));
	if (a) {
		a->dir = strdup(fuse_dir);
		a->e = *e;
		a->e.name = strdup(e->name);
		a->e.link = e->link ? strdup(e->link) : NULL;
		a->st = st;
		if (a->dir && a->e.name && (!e->link || a->e.link)) {
			pool_submit(flush_worker, a);
			return;
		}
		free(a->dir);
		free(a->e.name);
		free(a->e.link);
		free(a);
	}
	flush_entry(fuse_dir, e, st, 0); /* OOM: just do it here */
}

/* Submit every dirty entry of an already-loaded index to the pool. */
static void flush_loaded(const char *fuse_dir, struct omdfs_index *idx,
			 struct omdfs_status *st, int dry)
{
	for (size_t i = 0; i < idx->n; i++) {
		struct omdfs_entry *e = &idx->entries[i];
		if (!(e->flags & (OMDFS_F_DIRTY_CONTENT | OMDFS_F_DIRTY_ATTR)))
			continue;
		flush_submit(fuse_dir, e, st, dry);
	}
}

/* Submit one directory's own dirty entries (non-recursive). A removed/uncached
 * directory has nothing to flush. */
static void flush_one_dir(const char *fuse_dir, struct omdfs_status *st, int dry)
{
	struct omdfs_index idx;
	if (meta_try_load(fuse_dir, &idx) != 0)
		return;
	flush_loaded(fuse_dir, &idx, st, dry);
	meta_free_index(&idx);
}

/* Recovery / fallback: walk the whole cached tree and submit every dirty entry.
 * Used on the first cycle (dirty flags survive a crash but the in-memory set
 * starts empty) and whenever the set asks for a full rescan. A directory that
 * can't be fully flushed is re-recorded by the worker (flush_entry on failure). */
static void flush_tree(const char *fuse_dir, struct omdfs_status *st, int dry)
{
	struct omdfs_index idx;
	if (meta_try_load(fuse_dir, &idx) != 0)
		return; /* not a cached directory */

	flush_loaded(fuse_dir, &idx, st, dry);

	for (size_t i = 0; i < idx.n; i++) {
		if (idx.entries[i].type != OMDFS_T_DIR)
			continue;
		char child[PATH_MAX];
		join_path(child, fuse_dir, idx.entries[i].name);
		flush_tree(child, st, dry);
	}
	meta_free_index(&idx);
}

/* Flush all pending dirty content/attrs through the worker pool. Normally this
 * visits only the recorded dirty directories (dirtyset); on the initial cycle or
 * after a set overflow it falls back to a full tree walk that also re-seeds the
 * set. Blocks until every submitted flush has finished (so the backlog counters
 * and any failure re-arming are complete before the cycle ends).
 *
 * When `dry` is set the flush is paused (state/flush-off): the same discovery runs
 * but nothing is pushed and no flag is cleared — each visited dirty entry is only
 * counted into the backlog and re-armed in the dirty-set (so the set survives the
 * dirtyset_take here and the work resumes intact once flushing is re-enabled). */
static void flush_pending(struct omdfs_status *st, int dry)
{
	size_t n;
	int full;
	char **dirs = dirtyset_take(&n, &full);
	if (full) {
		dirtyset_free(dirs, n); /* the walk rediscovers everything */
		flush_tree("/", st, dry);
	} else {
		for (size_t i = 0; i < n; i++)
			flush_one_dir(dirs[i], st, dry);
		dirtyset_free(dirs, n);
	}
	pool_drain(); /* dry: nothing was submitted, so this is a no-op */
}

/* ---- full resync (operator-triggered reconcile) ---- */

struct resync_stats {
	long dirs, files, links, meta_only;
	long long bytes;
	int failed;
};

/* Bump the shared `failed` counter, which both the (single) walk thread and the
 * pool workers can touch. The other resync counters are partitioned by thread
 * (dirs/links/meta_only: walk-only; files/bytes: workers-only), so only `failed`
 * needs the lock. */
static void rs_fail(struct resync_stats *rs)
{
	pthread_mutex_lock(&s_stat_mtx);
	rs->failed++;
	pthread_mutex_unlock(&s_stat_mtx);
}

/* A regular file's content copy handed to the pool. Owns its path strings. */
struct resync_arg {
	char *cpath; /* cache source */
	char *bpath; /* backend destination */
	struct stat st;
	long long size;
	struct resync_stats *rs;
};

static void resync_copy_worker(void *p)
{
	struct resync_arg *a = p;
	if (copy_file(a->cpath, a->bpath) != 0) { /* tmp + rename: replaces 0444 */
		rs_fail(a->rs);
	} else {
		apply_stat_attrs(a->bpath, &a->st);
		pthread_mutex_lock(&s_stat_mtx);
		a->rs->files++;
		a->rs->bytes += a->size;
		pthread_mutex_unlock(&s_stat_mtx);
	}
	free(a->cpath);
	free(a->bpath);
	free(a);
}

/* Push one entry to the backend unconditionally (existence + content + attrs),
 * ignoring dirty flags. Used by the offline --resync reconcile, not the cycle.
 * Directory/symlink/metadata-only entries are applied inline (they must be ordered
 * before any children and are cheap); a regular file's content copy — the
 * fsync-bound bulk of the work — is handed to the pool to run concurrently. */
static void resync_entry(const char *fuse_dir, const struct omdfs_entry *e,
			 struct resync_stats *rs)
{
	char fpath[PATH_MAX], bp[PATH_MAX];
	join_path(fpath, fuse_dir, e->name);
	backend_path(bp, fpath);

	if (e->type == OMDFS_T_DIR) {
		if (mkdir(bp, e->st.st_mode & 07777) != 0 && errno != EEXIST) {
			rs_fail(rs);
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
			rs_fail(rs);
			return;
		}
		rs->links++;
		return;
	}
	/* regular file */
	if (e->flags & OMDFS_F_CONTENT_CACHED) {
		char cp[PATH_MAX];
		cache_path(cp, fpath);
		struct resync_arg *a = malloc(sizeof(*a));
		if (a) {
			a->cpath = strdup(cp);
			a->bpath = strdup(bp);
			a->st = e->st;
			a->size = (long long)e->st.st_size;
			a->rs = rs;
			if (a->cpath && a->bpath) {
				pool_submit(resync_copy_worker, a);
				return;
			}
			free(a->cpath);
			free(a->bpath);
			free(a);
		}
		/* OOM: copy inline. */
		if (copy_file(cp, bp) != 0) {
			rs_fail(rs);
			return;
		}
		apply_stat_attrs(bp, &e->st);
		pthread_mutex_lock(&s_stat_mtx);
		rs->files++;
		rs->bytes += (long long)e->st.st_size;
		pthread_mutex_unlock(&s_stat_mtx);
	} else {
		/* Content was never fetched into the cache, so the cache has no
		 * bytes to push — don't clobber whatever is on the backend. Just
		 * ensure the file exists and reconcile its attrs. O_RDONLY|O_CREAT
		 * never re-opens an existing (possibly read-only) file for write. */
		int fd = open(bp, O_RDONLY | O_CREAT, e->st.st_mode & 07777);
		if (fd < 0) {
			rs_fail(rs);
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
		rs_fail(rs);
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

/* Drain the structural WAL, then force the whole cached tree onto the backend.
 * Shared by the offline --resync tool and the live (operator-triggered)
 * reconcile. Fills *rs with per-entry stats and *blk with any blocked structural
 * op (only stuck_* fields are meaningful). Must run with no other thread writing
 * the backend — offline, or on the syncer thread itself. */
static void resync_reconcile(struct resync_stats *rs, struct omdfs_status *blk)
{
	/* Drain the structural WAL first so pending deletes/renames apply before
	 * we re-assert existence from the cache. Best-effort: a stuck op is
	 * reported but we still push content/attrs for everything present. */
	memset(blk, 0, sizeof(*blk));
	phase_structural(blk);

	memset(rs, 0, sizeof(*rs));
	resync_tree("/", rs); /* file copies run on the pool... */
	pool_drain();         /* ...wait for them before reporting stats */
}

int syncer_resync(void)
{
	struct omdfs_status st;
	struct resync_stats rs;
	pool_start(); /* offline: no syncer thread has started one yet */
	resync_reconcile(&rs, &st);
	pool_stop();

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

/* ---- mark-everything-dirty (non-blocking re-push) ---- */

struct mark_stats {
	long dirs, files, links;
	int failed; /* directories that could not be marked (save error) */
};

/* Flag every cached entry under `fuse_dir` dirty so the background syncer
 * re-pushes it, without pushing anything here. Local-only metadata writes; the
 * actual (slow) backend push is left to later sync cycles. */
static void mark_dirty_tree(const char *fuse_dir, struct mark_stats *ms)
{
	struct omdfs_index idx;
	if (meta_try_load(fuse_dir, &idx) != 0)
		return; /* not a cached directory */

	if (meta_mark_dir_dirty(fuse_dir, &ms->files, &ms->dirs, &ms->links) != 0)
		ms->failed++;

	for (size_t i = 0; i < idx.n; i++) {
		if (idx.entries[i].type != OMDFS_T_DIR)
			continue;
		char child[PATH_MAX];
		join_path(child, fuse_dir, idx.entries[i].name);
		mark_dirty_tree(child, ms);
	}
	meta_free_index(&idx);
}

int syncer_mark_dirty(void)
{
	struct mark_stats ms;
	memset(&ms, 0, sizeof(ms));
	mark_dirty_tree("/", &ms);
	fprintf(stderr,
		"omdfs: marked dirty %ld files, %ld dirs, %ld symlinks"
		"%s; the background syncer will push them on the next mount\n",
		ms.files, ms.dirs, ms.links,
		ms.failed ? " (some directories could not be marked)" : "");
	return ms.failed == 0 ? 0 : 1;
}

/* Result of the most recent live mark-dirty, carried into the status snapshot.
 * Touched only on the syncer thread (in sync_once), so no locking is needed. */
static long long s_last_mark_epoch;
static char s_last_mark[256];

/* An operator requests a live non-blocking re-push by creating
 * DATADIR/state/mark-dirty (e.g. `touch <datadir>/state/mark-dirty`). The syncer
 * thread consumes it at the top of a cycle (the unlink both tests for and claims
 * it, so it fires once), marks the whole tree dirty on this thread, and the same
 * cycle's flush_pending then begins draining the freshly-seeded backlog. Marking
 * is local-only, so the operator's `touch` returns immediately and need not wait
 * for the (slow) backend push to complete. */
static void maybe_live_mark_dirty(void)
{
	char tp[PATH_MAX];
	snprintf(tp, sizeof(tp), "%s/state/mark-dirty", g_cfg.datadir);
	if (unlink(tp) != 0)
		return; /* ENOENT (no request) or unremovable: nothing to do */

	struct mark_stats ms;
	memset(&ms, 0, sizeof(ms));
	mark_dirty_tree("/", &ms);

	s_last_mark_epoch = (long long)time(NULL);
	snprintf(s_last_mark, sizeof(s_last_mark),
		 "%ld files, %ld dirs, %ld symlinks, %d failed",
		 ms.files, ms.dirs, ms.links, ms.failed);
}

/* ---- live (operator-triggered) resync ---- */

/* Result of the most recent live resync, carried into every status snapshot so
 * an operator can confirm a triggered repair ran. Touched only on the syncer
 * thread (in sync_once), so no locking is needed. */
static long long s_last_resync_epoch;
static char s_last_resync[256];

/* An operator requests a live reconcile by creating DATADIR/state/resync (e.g.
 * `touch <datadir>/state/resync`). The syncer thread consumes it at the top of a
 * cycle: the unlink both tests for and claims the trigger in one step, so it
 * fires exactly once. The reconcile then runs on this thread — never concurrently
 * with the normal drain or another reconcile — which is why no extra signal
 * handler or backend locking is needed. Picked up within one sync interval. */
static void maybe_live_resync(void)
{
	char tp[PATH_MAX];
	snprintf(tp, sizeof(tp), "%s/state/resync", g_cfg.datadir);
	if (unlink(tp) != 0)
		return; /* ENOENT (no request) or unremovable: nothing to do */

	struct omdfs_status blk;
	struct resync_stats rs;
	resync_reconcile(&rs, &blk);

	s_last_resync_epoch = (long long)time(NULL);
	int n = snprintf(s_last_resync, sizeof(s_last_resync),
			 "%ld dirs, %ld files (%lld bytes), %ld symlinks, "
			 "%ld meta-only, %d failed",
			 rs.dirs, rs.files, rs.bytes, rs.links, rs.meta_only,
			 rs.failed);
	/* A blocked structural op is also reported via the cycle's stuck-* lines
	 * (with the full path); here we just note it happened. */
	if (blk.stuck_errno && n > 0 && n < (int)sizeof(s_last_resync))
		snprintf(s_last_resync + n, sizeof(s_last_resync) - (size_t)n,
			 "; structural op blocked (errno %d)", blk.stuck_errno);
}

/* ---- cold-metadata-index eviction ---- */

/* How often the automatic sweep runs (the first one fires on the first eligible
 * clean cycle, then at most this often). */
#define COLD_META_INTERVAL_S 60

static long long s_last_cold_sweep;    /* epoch of the last sweep (0 = never) */
static long s_cold_evicted_total;      /* cumulative indexes reclaimed, for status */

/* Whether cold-metadata-index eviction runs automatically. Tied to a configured
 * cache budget (a bounded footprint implies bounding metadata too), unless the
 * OMDFS_COLD_META env var forces it on (non-zero) or off (0). An operator can
 * always force a one-shot sweep via the state/cold-evict trigger regardless. */
static int cold_meta_enabled(void)
{
	const char *s = getenv("OMDFS_COLD_META");
	if (s && *s)
		return atoi(s) != 0;
	return evict_budget() > 0;
}

/* Post-order walk of the on-disk cache tree: reclaim each cold directory's index
 * (meta_evict_cold) only after its children, so an emptied subtree collapses
 * upward in a single pass. `cache_dir` mirrors FUSE path `fuse_dir`. Child names
 * are collected before recursing so we never mutate (rmdir) a directory while
 * iterating its own stream. */
static void cold_sweep(const char *cache_dir, const char *fuse_dir, long *evicted)
{
	char (*kids)[256] = NULL;
	size_t nk = 0, cap = 0;
	DIR *dp = opendir(cache_dir);
	if (dp) {
		struct dirent *de;
		while ((de = readdir(dp)) != NULL) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
				continue;
			char ccp[PATH_MAX];
			snprintf(ccp, sizeof(ccp), "%s/%s", cache_dir, de->d_name);
			struct stat stt;
			if (lstat(ccp, &stt) != 0 || !S_ISDIR(stt.st_mode))
				continue; /* only subdirectories carry child indexes */
			if (nk == cap) {
				size_t ncap = cap ? cap * 2 : 16;
				char (*p)[256] = realloc(kids, ncap * sizeof(*kids));
				if (!p)
					break; /* OOM: skip the rest of this dir's kids */
				kids = p;
				cap = ncap;
			}
			snprintf(kids[nk++], 256, "%s", de->d_name);
		}
		closedir(dp);
	}
	for (size_t i = 0; i < nk; i++) {
		char ccp[PATH_MAX], cfp[PATH_MAX];
		snprintf(ccp, sizeof(ccp), "%s/%s", cache_dir, kids[i]);
		join_path(cfp, fuse_dir, kids[i]);
		cold_sweep(ccp, cfp, evicted);
	}
	free(kids);

	if (meta_evict_cold(fuse_dir)) /* a no-op for "/" (root keeps its index) */
		(*evicted)++;
}

/* Reclaim cold directory indexes when it is safe to do so. Called only on a
 * fully-clean cycle (no pending structural ops, so the backend listing matches
 * the cache and an evicted index rebuilds faithfully). Runs on the operator
 * trigger DATADIR/state/cold-evict (one-shot, forces a sweep even when automatic
 * eviction is disabled) or, when enabled, on the periodic interval. */
static void maybe_cold_meta_sweep(struct omdfs_status *st)
{
	long long now = (long long)time(NULL);

	char tp[PATH_MAX];
	snprintf(tp, sizeof(tp), "%s/state/cold-evict", g_cfg.datadir);
	int forced = (unlink(tp) == 0); /* test-and-claim the trigger, one-shot */

	if (!forced) {
		if (!cold_meta_enabled())
			return;
		if (s_last_cold_sweep &&
		    now - s_last_cold_sweep < COLD_META_INTERVAL_S)
			return;
	}

	char croot[PATH_MAX];
	cache_path(croot, "/");
	long evicted = 0;
	cold_sweep(croot, "/", &evicted);
	s_cold_evicted_total += evicted;
	s_last_cold_sweep = now;
	st->cold_evicted = s_cold_evicted_total;
}

/* ---- pause the dirty flush (state/flush-off) ---- */

/* The operator pauses the dirty content/attr flush by creating DATADIR/state/flush-off
 * (`touch`); removing it resumes. Unlike the one-shot triggers this is a persistent
 * toggle — its mere presence, re-checked at the top of every cycle, gates phase 2.
 * The cheap structural WAL drain keeps running, so the backend namespace stays
 * coherent; only the network-heavy per-file content/attr push is held back. The
 * cache stays authoritative, so nothing is lost — the backend just lags until the
 * file is removed. `--no-flush` pre-creates this file so a mount can start paused. */
static int flush_disabled(void)
{
	char tp[PATH_MAX];
	snprintf(tp, sizeof(tp), "%s/state/flush-off", g_cfg.datadir);
	return access(tp, F_OK) == 0;
}

/* ---- cycle + thread ---- */

/* Run one full sync cycle and fill `st` with the resulting backlog/health so the
 * caller can publish it. */
static void sync_once(struct omdfs_status *st)
{
	memset(st, 0, sizeof(*st));

	maybe_live_resync(); /* operator-triggered full reconcile, on this thread */
	maybe_live_mark_dirty(); /* operator-triggered non-blocking re-push */

	int dry = flush_disabled(); /* state/flush-off: pause phase 2, still count it */
	st->flush_disabled = dry;

	phase_structural(st); /* backend dirs/files exist before content flush */
	flush_pending(st, dry);

	/* Flushed content is now clean and may be reclaimed; this also refreshes
	 * the backpressure flag (clears it once dirty data has drained). */
	evict_run_if_needed();

	st->cache_bytes = (long long)evict_total();
	st->cache_budget = (long long)evict_budget();
	st->cache_hard_limit = (long long)evict_hard_limit();
	st->backpressure = evict_pressure();

	/* A cycle that left no backlog and hit no stuck op is a clean sync. Only
	 * then is it safe to reclaim cold directory indexes: the backend listing
	 * matches the cache, so a rebuilt index cannot resurrect a pending delete. */
	if (!st->pending_structural && !st->dirty_files && !st->stuck_errno) {
		s_last_clean = (long long)time(NULL);
		maybe_cold_meta_sweep(st);
	}
	st->last_sync_epoch = s_last_clean;
	st->cold_evicted = s_cold_evicted_total;

	/* Carry the most recent live-resync result so it stays visible in status. */
	st->last_resync_epoch = s_last_resync_epoch;
	snprintf(st->last_resync, sizeof(st->last_resync), "%s", s_last_resync);

	/* Likewise the most recent live mark-dirty. */
	st->last_mark_epoch = s_last_mark_epoch;
	snprintf(st->last_mark, sizeof(st->last_mark), "%s", s_last_mark);
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
	pool_start(); /* parallel copy workers, used from the first cycle on */
	if (pthread_create(&s_thread, NULL, syncer_loop, NULL) != 0) {
		pool_stop();
		return -1;
	}
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
	 * can leave work if an op transiently failed mid-drain. Keep draining (the
	 * worker pool is still up, each cycle's flush_pending waits on it) until the
	 * backlog is empty or stops shrinking, so a reachable backend is fully caught
	 * up before we exit. With the backend down this converges immediately (no
	 * progress) and leaves the WAL/dirty flags for recovery on the next mount. */
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
	pool_stop(); /* no more flushes after this point */
}
