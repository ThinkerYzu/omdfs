/*
 * omdfs — content cache + progressive read implementation.
 *
 * A cache miss spawns a detached background thread that copies the whole file
 * BACKEND/<path> -> DATADIR/cache/<path>, advancing a `fetched_len` watermark as
 * bytes land. Readers wait on a condition variable only until the watermark
 * passes the byte range they asked for, so the head of a large file is readable
 * before its tail has been fetched (DESIGN Decision 3). On success the parent
 * index entry is marked CONTENT_CACHED; a partial file from a failed fetch is
 * removed so it can never masquerade as cached.
 *
 * Concurrency: a global list of in-flight fetches, keyed by path, lets several
 * opens of the same file share one fetch. The list and each fetch's watermark
 * are guarded by g_lock; the actual network copy runs outside the lock.
 */
#define _GNU_SOURCE
#include "content.h"

#include "evict.h"
#include "journal.h"
#include "meta.h"
#include "omdfs.h"
#include "syncer.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FETCH_BUF (128 * 1024)

/* One in-flight whole-file fetch, shared by every open of the same path. */
struct fetch {
	char path[PATH_MAX];
	off_t fetched_len; /* bytes durably written to the cache file so far */
	off_t total_len;   /* expected size (from the index); actual size on done */
	int wfd;	   /* write fd on the cache file (owned by the thread) */
	int done;	   /* fetch finished (success or failure) */
	int err;	   /* -errno on failure, else 0 */
	int refs;	   /* the fetch thread + each open handle */
	pthread_cond_t cond; /* signaled as fetched_len advances / on done */
	struct fetch *next;
};

struct content_handle {
	int fd;		 /* cache-file fd (O_RDONLY, or O_RDWR if writable) */
	struct fetch *f; /* shared in-flight fetch, or NULL for a warm hit */
	int writable;	 /* opened for write -> flush metadata at fsync/close */
	char path[PATH_MAX]; /* FUSE path (for the metadata flush) */
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fetch *g_fetches; /* in-flight fetches, keyed by path */

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

/* g_lock held. */
static struct fetch *fetch_find(const char *path)
{
	for (struct fetch *f = g_fetches; f; f = f->next)
		if (!strcmp(f->path, path))
			return f;
	return NULL;
}

/* g_lock held. */
static void fetch_unlink(struct fetch *f)
{
	struct fetch **pp = &g_fetches;
	while (*pp && *pp != f)
		pp = &(*pp)->next;
	if (*pp)
		*pp = f->next;
}

static void fetch_unref(struct fetch *f)
{
	pthread_mutex_lock(&g_lock);
	int dead = (--f->refs == 0);
	pthread_mutex_unlock(&g_lock);
	if (dead) {
		pthread_cond_destroy(&f->cond);
		free(f);
	}
}

/* Open the backend source for a fetch, tolerating a not-yet-applied rename. If
 * the file is missing because a structural rename hasn't reached the backend yet
 * (the cache moved the file to a new path but the backend still holds it at the
 * old one — e.g. the file was evicted, then a directory rename moved it), kick the
 * syncer to drain the rename and retry. Without this, re-fetching an evicted file
 * in a freshly-renamed subtree would fail ENOENT until the rename happened to
 * drain. Bounded (> the syncer's max backoff) so a genuine miss or a down backend
 * still fails in reasonable time. */
static int open_backend_for_fetch(const char *bp)
{
	int sfd = open(bp, O_RDONLY);
	for (int tries = 0; sfd < 0 && errno == ENOENT &&
			    wal_has_pending_rename() && tries < 150; tries++) {
		syncer_kick();
		nanosleep(&(struct timespec){ 0, 100 * 1000 * 1000 }, NULL); /* 100ms */
		sfd = open(bp, O_RDONLY);
	}
	return sfd;
}

static void *fetch_thread(void *arg)
{
	struct fetch *f = arg;
	char bp[PATH_MAX];
	backend_path(bp, f->path);

	int err = 0;
	off_t total = 0;
	int sfd = open_backend_for_fetch(bp);
	if (sfd < 0) {
		err = -errno;
	} else {
		char *buf = malloc(FETCH_BUF);
		if (!buf) {
			err = -ENOMEM;
		} else {
			for (;;) {
				ssize_t n = read(sfd, buf, FETCH_BUF);
				if (n < 0) {
					err = -errno;
					break;
				}
				if (n == 0)
					break; /* EOF: fetch complete */
				if (write_all(f->wfd, buf, (size_t)n) != 0) {
					err = -EIO;
					break;
				}
				total += n;
				pthread_mutex_lock(&g_lock);
				f->fetched_len = total;
				pthread_cond_broadcast(&f->cond);
				pthread_mutex_unlock(&g_lock);
			}
			free(buf);
		}
		close(sfd);
	}

	fsync(f->wfd);
	close(f->wfd);

	if (!err) {
		/* Mark the entry cached so future opens skip the backend entirely.
		 * Done before publishing `done`, so a concurrent open either sees
		 * the flag (warm hit) or joins this still-listed fetch — never
		 * re-fetches. */
		char parent[PATH_MAX], name[PATH_MAX];
		omdfs_split_path(f->path, parent, name);
		meta_mark_flags(parent, name, OMDFS_F_CONTENT_CACHED,
				OMDFS_F_FETCHING);
		/* Now fully cached: account it and reclaim space if over budget. */
		evict_note_present(f->path, total);
		evict_run_if_needed();
	} else {
		/* Never leave a partial file looking like a complete cache entry. */
		char cp[PATH_MAX];
		cache_path(cp, f->path);
		unlink(cp);
	}

	pthread_mutex_lock(&g_lock);
	f->err = err;
	if (!err)
		f->total_len = total;
	f->done = 1;
	fetch_unlink(f); /* no new joiners past this point */
	pthread_cond_broadcast(&f->cond);
	pthread_mutex_unlock(&g_lock);

	fetch_unref(f); /* drop the thread's reference */
	return NULL;
}

/* Start a new fetch for `fuse_path`. g_lock held on entry and exit. On success
 * returns the listed, running fetch with its ref already taken for the caller's
 * handle (refs == 2: handle + thread). On failure returns NULL and sets *err. */
static struct fetch *fetch_start(const char *fuse_path, const char *parent,
				 off_t size, int *err)
{
	char cdir[PATH_MAX], cp[PATH_MAX];
	cache_path(cdir, parent);
	cache_path(cp, fuse_path);

	if (mkdirs(cdir) != 0) {
		*err = -errno;
		return NULL;
	}
	int wfd = open(cp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (wfd < 0 && errno == ENOENT && mkdirs(cdir) == 0)
		/* A cold-metadata eviction may have rmdir'd the parent cache dir
		 * between our mkdirs and here; recreate it once and retry. */
		wfd = open(cp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (wfd < 0) {
		*err = -errno;
		return NULL;
	}

	struct fetch *f = calloc(1, sizeof(*f));
	if (!f) {
		close(wfd);
		unlink(cp);
		*err = -ENOMEM;
		return NULL;
	}
	snprintf(f->path, sizeof(f->path), "%s", fuse_path);
	f->total_len = size;
	f->wfd = wfd;
	f->refs = 2; /* the caller's handle + the fetch thread */
	pthread_cond_init(&f->cond, NULL);
	f->next = g_fetches;
	g_fetches = f;

	pthread_t tid;
	if (pthread_create(&tid, NULL, fetch_thread, f) != 0) {
		fetch_unlink(f);
		close(wfd);
		unlink(cp);
		pthread_cond_destroy(&f->cond);
		free(f);
		*err = -EIO;
		return NULL;
	}
	pthread_detach(tid);
	return f;
}

int content_open(const char *fuse_path, struct content_handle **out)
{
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(fuse_path, parent, name);

	/* Consult the index for cache state and the expected size. */
	struct stat est;
	uint8_t etype, eflags;
	int r = meta_stat(parent, name, &est, &etype, &eflags);
	if (r != 0)
		return r;
	if (etype == OMDFS_T_DIR)
		return -EISDIR;
	int cached = (eflags & OMDFS_F_CONTENT_CACHED) != 0;
	off_t size = est.st_size;

	char cp[PATH_MAX];
	cache_path(cp, fuse_path);

	struct content_handle *h = calloc(1, sizeof(*h));
	if (!h)
		return -ENOMEM;
	h->fd = -1;

	/* Warm hit: serve straight from the cache file. */
	if (cached) {
		int fd = open(cp, O_RDONLY);
		if (fd >= 0) {
			h->fd = fd;
			h->f = NULL;
			snprintf(h->path, sizeof(h->path), "%s", fuse_path);
			evict_note_present(fuse_path, size); /* recency */
			evict_pin(fuse_path); /* open -> never evicted */
			*out = h;
			return 0;
		}
		/* Flag claimed cached but the file is gone — fall through and
		 * re-fetch. */
	}

	/* Miss: pin before starting the fetch so a racing evictor (which always
	 * targets the cold LRU tail) can never delete the file we're fetching. */
	evict_pin(fuse_path);

	/* Miss: join an in-flight fetch or start a new one. The cache file is
	 * created under the lock (a fast local op), so any joiner that finds the
	 * fetch can immediately open it for reading. */
	struct fetch *f;
	pthread_mutex_lock(&g_lock);
	f = fetch_find(fuse_path);
	if (f) {
		f->refs++;
	} else {
		int err = 0;
		f = fetch_start(fuse_path, parent, size, &err);
		if (!f) {
			pthread_mutex_unlock(&g_lock);
			evict_unpin(fuse_path);
			free(h);
			return err;
		}
	}
	pthread_mutex_unlock(&g_lock);

	int rfd = open(cp, O_RDONLY);
	if (rfd < 0) {
		int err = -errno;
		fetch_unref(f);
		evict_unpin(fuse_path);
		free(h);
		return err;
	}
	h->fd = rfd;
	h->f = f;
	snprintf(h->path, sizeof(h->path), "%s", fuse_path);
	*out = h;
	return 0;
}

/* Block until `fuse_path`'s whole content is in the cache (warm hit, or wait for
 * the background fetch to finish). Returns 0 or -errno. */
static int content_ensure_cached(const char *fuse_path)
{
	struct content_handle *h;
	int r = content_open(fuse_path, &h);
	if (r != 0)
		return r;
	if (h->f) {
		pthread_mutex_lock(&g_lock);
		while (!h->f->done)
			pthread_cond_wait(&h->f->cond, &g_lock);
		r = h->f->err;
		pthread_mutex_unlock(&g_lock);
	}
	content_close(h);
	return r;
}

int content_create(const char *fuse_path, mode_t mode, struct content_handle **out)
{
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(fuse_path, parent, name);

	char cdir[PATH_MAX], cp[PATH_MAX];
	cache_path(cdir, parent);
	cache_path(cp, fuse_path);
	if (mkdirs(cdir) != 0)
		return -errno;

	int fd = open(cp, O_RDWR | O_CREAT | O_TRUNC, mode);
	if (fd < 0 && errno == ENOENT && mkdirs(cdir) == 0)
		/* A cold-metadata eviction may have rmdir'd the parent cache dir
		 * between our mkdirs and here; recreate it once and retry. */
		fd = open(cp, O_RDWR | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -errno;

	struct content_handle *h = calloc(1, sizeof(*h));
	if (!h) {
		close(fd);
		return -ENOMEM;
	}
	h->fd = fd;
	h->f = NULL;
	h->writable = 1;
	snprintf(h->path, sizeof(h->path), "%s", fuse_path);
	evict_note_present(fuse_path, 0); /* new empty file */
	evict_pin(fuse_path);
	*out = h;
	return 0;
}

int content_open_rw(const char *fuse_path, struct content_handle **out)
{
	/* Pin across the whole fetch-then-reopen window. content_ensure_cached opens
	 * an inner handle and then CLOSES it, which unpins the file; without this
	 * outer pin a clean+cached file could be evicted in the gap before we reopen
	 * it O_RDWR and re-pin below. That race is silently destructive: open(O_RDWR)
	 * would still succeed on the just-unlinked inode, our write would land in it
	 * and be lost when the fd closes, CONTENT_CACHED is now clear, and a later
	 * read would re-fetch the STALE backend copy. Hold the pin continuously so the
	 * evictor can never target this file mid-open. */
	evict_pin(fuse_path);

	int r = content_ensure_cached(fuse_path);
	if (r != 0) {
		evict_unpin(fuse_path);
		return r;
	}

	char cp[PATH_MAX];
	cache_path(cp, fuse_path);
	int fd = open(cp, O_RDWR);
	if (fd < 0) {
		evict_unpin(fuse_path);
		return -errno;
	}

	struct content_handle *h = calloc(1, sizeof(*h));
	if (!h) {
		close(fd);
		evict_unpin(fuse_path);
		return -ENOMEM;
	}
	h->fd = fd;
	h->f = NULL;
	h->writable = 1;
	snprintf(h->path, sizeof(h->path), "%s", fuse_path);
	struct stat st;
	if (fstat(fd, &st) == 0)
		evict_note_present(fuse_path, st.st_size);
	evict_pin(fuse_path);     /* the handle's own pin (released by content_close) */
	evict_unpin(fuse_path);   /* drop the outer pin; the handle pin now holds */
	*out = h;
	return 0;
}

/* Record the cache file's current size/mtime in the index and flag it dirty. */
static int flush_meta(struct content_handle *h);

ssize_t content_write(struct content_handle *h, const void *buf, size_t len,
		      off_t off)
{
	if (!h->writable)
		return -EBADF;

	/* Backpressure: when the backend is down, dirty content cannot sync or be
	 * evicted. Allow it to overshoot the budget up to a hard limit, then refuse
	 * the growth (ENOSPC) rather than fill the disk. Only the *new* bytes count;
	 * rewriting already-cached bytes is always allowed (it doesn't grow the
	 * cache). The file being written is itself dirty+pinned, so it is never a
	 * victim — backpressure is the only thing bounding it. */
	off_t end = off + (off_t)len;
	struct stat cur;
	if (fstat(h->fd, &cur) == 0 && end > cur.st_size &&
	    evict_backpressure(end - cur.st_size))
		return -ENOSPC;

	ssize_t n = pwrite(h->fd, buf, len, off);
	if (n < 0)
		return -errno;
	/* Keep the index size/mtime current: FUSE release() is asynchronous, so a
	 * size update deferred to close races with the next command's getattr.
	 * Updating here (writes are synchronous) keeps getattr authoritative. */
	flush_meta(h);
	/* A long write to one file can grow the cache past budget before close;
	 * reclaim clean space now (the file being written is dirty + pinned, so
	 * it is itself never a victim). */
	evict_run_if_needed();
	return n;
}

/* Record the cache file's current size/mtime in the index and flag it dirty. */
static int flush_meta(struct content_handle *h)
{
	struct stat st;
	if (fstat(h->fd, &st) != 0)
		return -errno;
	evict_note_present(h->path, st.st_size); /* keep accounting in step */
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(h->path, parent, name);
	return meta_set(parent, name, &st, META_SIZE | META_TIMES,
			OMDFS_F_DIRTY_CONTENT);
}

int content_fsync(struct content_handle *h)
{
	if (!h->writable)
		return 0;
	if (fsync(h->fd) != 0)
		return -errno;
	return flush_meta(h);
}

int content_ftruncate(struct content_handle *h, off_t size)
{
	if (!h->writable)
		return -EBADF;
	/* Growing via truncate is subject to the same backpressure as a write. */
	struct stat cur;
	if (fstat(h->fd, &cur) == 0 && size > cur.st_size &&
	    evict_backpressure(size - cur.st_size))
		return -ENOSPC;
	return ftruncate(h->fd, size) == 0 ? 0 : -errno;
}

int content_truncate(const char *fuse_path, off_t size)
{
	struct content_handle *h;
	int r = content_open_rw(fuse_path, &h);
	if (r != 0)
		return r;
	r = content_ftruncate(h, size);
	content_close(h); /* records the new size + DIRTY_CONTENT */
	return r;
}

int content_prefetch(const char *fuse_path)
{
	return content_ensure_cached(fuse_path);
}

ssize_t content_read(struct content_handle *h, char *buf, size_t len, off_t off)
{
	if (h->f) {
		struct fetch *f = h->f;
		off_t want = off + (off_t)len;
		pthread_mutex_lock(&g_lock);
		while (!f->done && f->fetched_len < want)
			pthread_cond_wait(&f->cond, &g_lock);
		int err = f->err;
		pthread_mutex_unlock(&g_lock);
		if (err)
			return err;
	}
	ssize_t n = pread(h->fd, buf, len, off);
	return n < 0 ? -errno : n;
}

void content_close(struct content_handle *h)
{
	if (!h)
		return;
	if (h->writable && h->fd >= 0)
		flush_meta(h); /* size/mtime + DIRTY_CONTENT for the syncer */
	if (h->fd >= 0)
		close(h->fd);
	if (h->f)
		fetch_unref(h->f);
	if (h->path[0]) {
		evict_unpin(h->path); /* no longer open -> may be evicted */
		evict_run_if_needed();
	}
	free(h);
}

int content_is_fetching(const char *fuse_path)
{
	pthread_mutex_lock(&g_lock);
	int yes = fetch_find(fuse_path) != NULL;
	pthread_mutex_unlock(&g_lock);
	return yes;
}
