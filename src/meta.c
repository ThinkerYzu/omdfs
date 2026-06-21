/*
 * omdfs — per-directory metadata index (.omdfs.dir) implementation.
 *
 * On-disk format (local-only, regenerable; raw struct stat is fine because the
 * index never leaves this machine):
 *
 *   header:  magic u32 | version u16 | _pad u16 | count u32 | dir_st struct stat
 *   entry*:  type u8 | flags u8 | name_len u16 | link_len u16 | st struct stat
 *            | name[name_len] | link[link_len]
 */
#define _GNU_SOURCE
#include "meta.h"

#include "dirtyset.h"
#include "journal.h"
#include "omdfs.h"
#include "syncer.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OMDFS_INDEX_MAGIC 0x4f444952u /* "ODIR" */
#define OMDFS_INDEX_VERSION 1

/* Flags whose presence means a child needs flushing to the backend. */
#define OMDFS_F_DIRTY (OMDFS_F_DIRTY_CONTENT | OMDFS_F_DIRTY_ATTR)

struct index_header {
	uint32_t magic;
	uint16_t version;
	uint16_t _pad;
	uint32_t count;
	struct stat dir_st;
};

struct entry_header {
	uint8_t type;
	uint8_t flags;
	uint16_t name_len;
	uint16_t link_len;
	uint16_t _pad;
	struct stat st;
};

/* ---- small I/O helpers ---- */

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

static int read_all(int fd, void *buf, size_t len)
{
	char *p = buf;
	while (len) {
		ssize_t n = read(fd, p, len);
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* Build the index file path for `fuse_dir` -> CACHE_DIR/.omdfs.dir */
static void index_path(char out[PATH_MAX], const char *fuse_dir)
{
	char cdir[PATH_MAX];
	cache_path(cdir, fuse_dir);
	snprintf(out, PATH_MAX, "%s/%s", cdir, OMDFS_INDEX_NAME);
}

/* Is `name` the index file or one of its mkstemp temporaries (".omdfs.dir.*")?
 * Those are the only cache-directory entries that don't represent real content. */
static int is_index_name(const char *name)
{
	size_t n = strlen(OMDFS_INDEX_NAME);
	return !strcmp(name, OMDFS_INDEX_NAME) ||
	       (!strncmp(name, OMDFS_INDEX_NAME, n) && name[n] == '.');
}

/* ---- in-memory index management ---- */

/* Below this child count a linear scan beats building/probing a hash. */
#define META_HASH_MIN 32

static size_t name_hash(const char *s)
{
	size_t h = 5381;
	for (; *s; s++)
		h = ((h << 5) + h) ^ (unsigned char)*s;
	return h;
}

/* Drop any built name hash (call on every add/remove/rename of an entry; the
 * next meta_lookup rebuilds it if the directory is still large enough). */
static void idx_hash_drop(struct omdfs_index *idx)
{
	free(idx->hslots);
	idx->hslots = NULL;
	idx->hcap = 0;
}

/* Build the name->slot hash for `idx`. Returns 0, or -1 on OOM (lookups then
 * fall back to a linear scan). */
static int idx_hash_build(struct omdfs_index *idx)
{
	size_t cap = 64;
	while (cap < idx->n * 2)
		cap <<= 1;
	int32_t *t = malloc(cap * sizeof(*t));
	if (!t)
		return -1;
	for (size_t i = 0; i < cap; i++)
		t[i] = -1;
	for (size_t i = 0; i < idx->n; i++) {
		size_t h = name_hash(idx->entries[i].name) & (cap - 1);
		while (t[h] != -1)
			h = (h + 1) & (cap - 1);
		t[h] = (int32_t)i;
	}
	free(idx->hslots);
	idx->hslots = t;
	idx->hcap = cap;
	return 0;
}

static int idx_push(struct omdfs_index *idx, const struct omdfs_entry *e)
{
	if (idx->n == idx->cap) {
		size_t cap = idx->cap ? idx->cap * 2 : 16;
		struct omdfs_entry *p =
			realloc(idx->entries, cap * sizeof(*p));
		if (!p)
			return -1;
		idx->entries = p;
		idx->cap = cap;
		/* The array moved: repoint every live indirection's handle to its
		 * entry's new address (cnode is unchanged — same directory). */
		for (size_t i = 0; i < idx->n; i++)
			if (idx->entries[i].ind)
				idx->entries[i].ind->entry = &idx->entries[i];
	}
	struct omdfs_entry *ne = &idx->entries[idx->n++];
	*ne = *e;
	/* A transferred indirection (a cross-dir move carries one in) still points at
	 * its old slot; repoint it to its new home. Its cnode is refreshed by the
	 * caller's ind_ensure. A fresh entry (meta_add_entry) has ind == NULL. */
	if (ne->ind)
		ne->ind->entry = ne;
	idx_hash_drop(idx); /* names changed: invalidate the lookup hash */
	return 0;
}

/* Free an indirection iff nothing references it anymore: it is detached from its
 * entry AND has no undrained structural op (pending_count == 0) AND no in-flight
 * flush. Called wherever one of those three references drops. meta_mtx held. */
static void ind_maybe_free(struct ind *in)
{
	if (in && in->detached && in->pending_count == 0 && !in->flushing)
		free(in);
}

/* Handle audit (DESIGN.md § 2026-06-20): an attached indirection points at its live
 * entry (entry->ind == self); a detached one has no entry. Trips immediately if any
 * entry-relocation site forgot to repoint the handle (use-after-free is the hardest
 * class to catch otherwise). NDEBUG-gated; a NULL `in` is a seeded recovery node.
 * Called on every peek/pop. meta_mtx held. */
static inline void ind_audit(const struct ind *in)
{
#ifndef NDEBUG
	if (!in)
		return;
	if (in->detached)
		assert(in->entry == NULL && in->cnode == NULL);
	else
		assert(in->entry && in->entry->ind == in);
#else
	(void)in;
#endif
}

/* Release a canonical entry's indirection as the entry goes away (removed, or its
 * whole index freed). The entry reference is gone (detach — null the handle); the
 * indirection lives on while the pending-op queue or an in-flight flush still
 * references it, and is freed once neither does. NULL-safe and a no-op on reader
 * copies (ind == NULL). meta_mtx held for any entry that actually carries an ind. */
static void ind_release(struct omdfs_entry *e)
{
	struct ind *in = e->ind;
	if (!in)
		return;
	e->ind = NULL;
	in->entry = NULL;
	in->cnode = NULL;
	in->detached = 1;
	ind_maybe_free(in);
}

/* Ensure `e` (living in directory node `cn`) has an indirection, allocating it
 * lazily, and (re)establish its live-entry handle. Returns it, or NULL on OOM.
 * meta_mtx held. */
static struct ind *ind_ensure(struct cnode *cn, struct omdfs_entry *e)
{
	if (!e->ind)
		e->ind = calloc(1, sizeof(*e->ind));
	if (e->ind) {
		e->ind->entry = e;
		e->ind->cnode = cn;
	}
	return e->ind;
}

void meta_free_index(struct omdfs_index *idx)
{
	for (size_t i = 0; i < idx->n; i++) {
		ind_release(&idx->entries[i]);
		free(idx->entries[i].name);
		free(idx->entries[i].link);
	}
	free(idx->entries);
	free(idx->hslots);
	idx->entries = NULL;
	idx->hslots = NULL;
	idx->n = idx->cap = idx->hcap = 0;
}

struct omdfs_entry *meta_lookup(struct omdfs_index *idx, const char *name)
{
	if (idx->n >= META_HASH_MIN) {
		if (idx->hslots || idx_hash_build(idx) == 0) {
			size_t mask = idx->hcap - 1;
			size_t h = name_hash(name) & mask;
			for (;;) {
				int32_t slot = idx->hslots[h];
				if (slot < 0)
					return NULL;
				if (!strcmp(idx->entries[slot].name, name))
					return &idx->entries[slot];
				h = (h + 1) & mask;
			}
		}
		/* OOM building the hash: fall through to a linear scan. */
	}
	for (size_t i = 0; i < idx->n; i++)
		if (!strcmp(idx->entries[i].name, name))
			return &idx->entries[i];
	return NULL;
}

/* ---- persistence ---- */

/* Serialize `idx` into a freshly malloc'd flat buffer in the on-disk format
 * (header + entries). On success *out owns the buffer (caller frees) and *len is
 * its length; returns 0, or -ENOMEM. This is the part that runs under meta_mtx in
 * the async write-back path — a bounded memcpy, no fsync. */
static int serialize_index(const struct omdfs_index *idx, void **out, size_t *len)
{
	size_t total = sizeof(struct index_header);
	for (size_t i = 0; i < idx->n; i++) {
		const struct omdfs_entry *e = &idx->entries[i];
		total += sizeof(struct entry_header) + strlen(e->name) +
			 (e->link ? strlen(e->link) : 0);
	}

	char *buf = malloc(total);
	if (!buf)
		return -ENOMEM;

	struct index_header h = {
		.magic = OMDFS_INDEX_MAGIC,
		.version = OMDFS_INDEX_VERSION,
		.count = (uint32_t)idx->n,
		.dir_st = idx->dir_st,
	};
	size_t o = 0;
	memcpy(buf + o, &h, sizeof(h));
	o += sizeof(h);

	for (size_t i = 0; i < idx->n; i++) {
		const struct omdfs_entry *e = &idx->entries[i];
		size_t nlen = strlen(e->name);
		size_t llen = e->link ? strlen(e->link) : 0;
		struct entry_header eh = {
			.type = e->type,
			.flags = e->flags,
			.name_len = (uint16_t)nlen,
			.link_len = (uint16_t)llen,
			.st = e->st,
		};
		memcpy(buf + o, &eh, sizeof(eh));
		o += sizeof(eh);
		memcpy(buf + o, e->name, nlen);
		o += nlen;
		if (llen) {
			memcpy(buf + o, e->link, llen);
			o += llen;
		}
	}

	*out = buf;
	*len = total;
	return 0;
}

/* Atomically replace `fuse_dir`'s on-disk index with `buf` (write a temp,
 * fsync, rename). The slow part (fsync + rename) — runs OFF meta_mtx in the async
 * path. Returns 0 or -errno. */
static int write_index_buf(const char *fuse_dir, const void *buf, size_t len)
{
	char cdir[PATH_MAX];
	cache_path(cdir, fuse_dir);
	if (mkdirs(cdir) != 0)
		return -errno;

	char tmpl[PATH_MAX];
	snprintf(tmpl, sizeof(tmpl), "%s/%s.XXXXXX", cdir, OMDFS_INDEX_NAME);
	int fd = mkstemp(tmpl);
	if (fd < 0)
		return -errno;

	if (write_all(fd, buf, len) != 0 || fsync(fd) != 0) {
		close(fd);
		unlink(tmpl);
		return -EIO;
	}
	close(fd);

	char ipath[PATH_MAX];
	index_path(ipath, fuse_dir);
	if (rename(tmpl, ipath) != 0) {
		int e = errno;
		unlink(tmpl);
		return -e;
	}
	return 0;
}

/* Synchronous persist: serialize + write. Used by build_index (the load-miss
 * path) and as the fallback for the offline tools, which run before the async
 * index-writer thread is started. The mounted hot path goes through
 * meta_schedule_save instead. */
static int save_index(const char *fuse_dir, const struct omdfs_index *idx)
{
	void *buf;
	size_t len;
	int r = serialize_index(idx, &buf, &len);
	if (r != 0)
		return r;
	r = write_index_buf(fuse_dir, buf, len);
	free(buf);
	return r;
}

static int load_index(const char *fuse_dir, struct omdfs_index *idx)
{
	char ipath[PATH_MAX];
	index_path(ipath, fuse_dir);
	int fd = open(ipath, O_RDONLY);
	if (fd < 0)
		return -errno; /* ENOENT -> caller rebuilds */

	struct index_header h;
	if (read_all(fd, &h, sizeof(h)) != 0 ||
	    h.magic != OMDFS_INDEX_MAGIC ||
	    h.version != OMDFS_INDEX_VERSION) {
		close(fd);
		return -EINVAL; /* corrupt/old -> caller rebuilds */
	}

	memset(idx, 0, sizeof(*idx));
	idx->dir_st = h.dir_st;

	for (uint32_t i = 0; i < h.count; i++) {
		struct entry_header eh;
		if (read_all(fd, &eh, sizeof(eh)) != 0)
			goto corrupt;
		if (eh.name_len == 0 || eh.name_len >= PATH_MAX ||
		    eh.link_len >= PATH_MAX)
			goto corrupt;

		struct omdfs_entry e;
		memset(&e, 0, sizeof(e));
		e.type = eh.type;
		e.flags = eh.flags;
		e.st = eh.st;

		e.name = malloc(eh.name_len + 1);
		if (!e.name)
			goto corrupt;
		if (read_all(fd, e.name, eh.name_len) != 0) {
			free(e.name);
			goto corrupt;
		}
		e.name[eh.name_len] = '\0';

		if (eh.link_len) {
			e.link = malloc(eh.link_len + 1);
			if (!e.link) {
				free(e.name);
				goto corrupt;
			}
			if (read_all(fd, e.link, eh.link_len) != 0) {
				free(e.name);
				free(e.link);
				goto corrupt;
			}
			e.link[eh.link_len] = '\0';
		}

		if (idx_push(idx, &e) != 0) {
			free(e.name);
			free(e.link);
			goto corrupt;
		}
	}
	close(fd);
	return 0;

corrupt:
	close(fd);
	meta_free_index(idx);
	return -EINVAL;
}

static int build_index(const char *fuse_dir, struct omdfs_index *idx)
{
	memset(idx, 0, sizeof(*idx));

	char bp[PATH_MAX];
	backend_path(bp, fuse_dir);

	if (lstat(bp, &idx->dir_st) != 0)
		return -errno;

	DIR *dp = opendir(bp);
	if (!dp)
		return -errno;

	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (!strcmp(de->d_name, OMDFS_INDEX_NAME))
			continue; /* never expose the index */

		char cp[PATH_MAX];
		snprintf(cp, sizeof(cp), "%s/%s", bp, de->d_name);

		struct stat st;
		if (lstat(cp, &st) != 0)
			continue; /* entry vanished; skip */

		struct omdfs_entry e;
		memset(&e, 0, sizeof(e));
		e.st = st;
		if (S_ISDIR(st.st_mode)) {
			e.type = OMDFS_T_DIR;
		} else if (S_ISLNK(st.st_mode)) {
			e.type = OMDFS_T_LINK;
			char tgt[PATH_MAX];
			ssize_t n = readlink(cp, tgt, sizeof(tgt) - 1);
			if (n >= 0) {
				tgt[n] = '\0';
				e.link = strdup(tgt);
			}
		} else {
			e.type = OMDFS_T_FILE;
		}
		e.name = strdup(de->d_name);
		if (!e.name || idx_push(idx, &e) != 0) {
			free(e.name);
			free(e.link);
			closedir(dp);
			meta_free_index(idx);
			return -ENOMEM;
		}
	}
	closedir(dp);

	/* Persist; a write failure is non-fatal — we still serve from memory. */
	save_index(fuse_dir, idx);
	return 0;
}

/* All index access goes through this one mutex. Writers do their read-modify-
 * write under it so concurrent mutations (flags, add/remove/move, attrs) can't
 * clobber each other; readers take it only to copy out of the in-memory index
 * cache below (the slow load/build on a miss happens outside the lock). */
static pthread_mutex_t meta_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---- in-memory parsed-index cache ----
 *
 * Each accessed directory's parsed index is kept in RAM as the *canonical*
 * copy, so getattr/readdir/lookup (and the syncer/evictor via meta_try_load) no
 * longer reparse .omdfs.dir from disk on every call. While mounted the canonical
 * is the source of truth; the on-disk index is its durable backing — every
 * mutation saves it, so disk always equals the canonical. Readers receive a deep
 * copy (the public API still hands out caller-owned indexes); writers mutate the
 * canonical in place under meta_mtx. A bounded LRU caps RAM: an evicted (or, on a
 * failed save, invalidated) entry is simply freed and reparsed from disk on next
 * access, which is safe precisely because disk == canonical. All cache_* helpers
 * below require meta_mtx held. */
#define META_CACHE_BUCKETS 1024
#define META_CACHE_MAX 8192 /* max cached directory indexes before LRU eviction */

struct cnode {
	char *dir;              /* fuse path, the cache key */
	struct omdfs_index idx; /* canonical parsed index (owns its names/links) */
	struct cnode *hnext;    /* hash-bucket chain */
	struct cnode *lru_prev, *lru_next; /* LRU list: head = MRU, tail = LRU */
	/* Asynchronous index write-back state (all under meta_mtx). save_dirty: the
	 * canonical changed since the last on-disk snapshot. save_inflight: queued on
	 * / being written by the index-writer thread. Their disjunction (cnode_pinned)
	 * pins the node against eviction and makes meta_forget wait — the on-disk index
	 * is stale until the write lands, so the node must not be dropped or
	 * reloaded-from-disk meanwhile. wq_next chains the writer queue (a node is
	 * enqueued at most once while save_inflight holds). */
	int save_dirty;
	int save_inflight;
	struct cnode *wq_next;
};

/* Recover the owning cnode from a canonical index pointer: cache_canonical hands
 * out &cnode.idx, and the mutators pass that straight to meta_schedule_save. */
#define cnode_of(idxp) \
	((struct cnode *)((char *)(idxp) - offsetof(struct cnode, idx)))

static struct cnode *cache_buckets[META_CACHE_BUCKETS];
static struct cnode *lru_head, *lru_tail;
static size_t cache_count;

/* ---- asynchronous index write-back ----
 *
 * save_index does mkstemp + write + fsync + rename; doing it under meta_mtx made
 * every mutating op serialize the whole critical section behind a local-disk
 * fsync, stalling the foreground readers that share the lock. Instead a mutator
 * marks its node dirty and hands it to a single dedicated writer thread: the
 * snapshot (serialize to a flat buffer) happens under the lock, but the
 * fsync/rename runs off it. Repeat mutations on a dir that is mid-write coalesce
 * (save_dirty is re-set; the writer loops). See DESIGN.md "Asynchronous coalesced
 * index write-back". */
static pthread_cond_t wq_cond = PTHREAD_COND_INITIALIZER;  /* new work / stop */
static pthread_cond_t pin_cond = PTHREAD_COND_INITIALIZER; /* a pin cleared */
static struct cnode *wq_head, *wq_tail;
static pthread_t writer_thread;
static int writer_running; /* async on (mounted); else mutators save inline */
static int writer_stop;    /* shutdown: drain the queue, then exit */
static int wb_last_errno;  /* last write-back failure for status, 0 if none */
static char wb_last_path[PATH_MAX];

/* Pinned = a write is pending: never evict, and meta_forget must wait. */
static int cnode_pinned(const struct cnode *n)
{
	return n->save_dirty || n->save_inflight;
}

/* True if any entry in `n` has an appended-but-undrained structural op. Such a
 * node must NOT be LRU-evicted: dropping it and reparsing from disk would reset
 * those entries' pending_count to 0, so the flush gate would no longer hold them
 * back and the not-yet-drained rename/create could clobber a freshly-flushed path
 * (DESIGN.md § 2026-06-19). Cheap: scanned only when LRU eviction is considering
 * this node as a victim (rare; needs > META_CACHE_MAX cached dirs). */
static int cnode_has_pending(const struct cnode *n)
{
	for (size_t i = 0; i < n->idx.n; i++) {
		const struct ind *in = n->idx.entries[i].ind;
		if (in && in->pending_count > 0)
			return 1;
	}
	return 0;
}

/* meta_mtx held. Append a node to the writer queue (enqueued at most once). */
static void wq_push(struct cnode *n)
{
	n->wq_next = NULL;
	if (wq_tail)
		wq_tail->wq_next = n;
	else
		wq_head = n;
	wq_tail = n;
}

/* meta_mtx held. Pop the queue head, or NULL if empty. */
static struct cnode *wq_pop(void)
{
	struct cnode *n = wq_head;
	if (n) {
		wq_head = n->wq_next;
		if (!wq_head)
			wq_tail = NULL;
		n->wq_next = NULL;
	}
	return n;
}

/* Persist cnode `n`'s canonical index. With the writer running (mounted) this
 * only schedules: mark dirty, enqueue if not already in-flight, return 0 — the
 * fsync happens off meta_mtx on the writer thread. Without the writer (the
 * offline --resync/--mark-dirty tools) it writes synchronously and returns the
 * result, preserving the callers' revert-on-error path. meta_mtx held. */
static int meta_schedule_save(struct cnode *n)
{
	if (!writer_running)
		return save_index(n->dir, &n->idx);
	n->save_dirty = 1;
	if (!n->save_inflight) {
		n->save_inflight = 1;
		wq_push(n);
		pthread_cond_signal(&wq_cond);
	}
	return 0;
}

static void *index_writer_main(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&meta_mtx);
	for (;;) {
		while (!wq_head && !writer_stop)
			pthread_cond_wait(&wq_cond, &meta_mtx);
		struct cnode *n = wq_pop();
		if (!n) {
			if (writer_stop)
				break; /* queue drained and stop requested */
			continue;
		}

		/* Snapshot the current canonical under the lock (memcpy, no fsync). */
		n->save_dirty = 0;
		void *buf = NULL;
		size_t len = 0;
		int r = serialize_index(&n->idx, &buf, &len);
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s", n->dir);
		pthread_mutex_unlock(&meta_mtx);

		/* The fsync + rename — off the lock, so readers run meanwhile. */
		if (r == 0) {
			r = write_index_buf(dir, buf, len);
			free(buf);
		}

		pthread_mutex_lock(&meta_mtx);
		if (r == 0) {
			wb_last_errno = 0;
		} else {
			/* The mutation already returned success to the app, so we
			 * cannot revert — keep the node dirty and retry. On a clean
			 * unmount give up after this attempt so the drain terminates
			 * even on a broken disk (the on-disk index just stays stale). */
			wb_last_errno = -r;
			snprintf(wb_last_path, sizeof(wb_last_path), "%s", dir);
			if (!writer_stop)
				n->save_dirty = 1;
		}

		if (n->save_dirty) {
			wq_push(n); /* coalesced newer state, or a retry */
			if (r != 0) {
				/* Throttle a failing retry so it doesn't hot-spin. */
				struct timespec ts;
				clock_gettime(CLOCK_REALTIME, &ts);
				ts.tv_nsec += 200L * 1000 * 1000;
				if (ts.tv_nsec >= 1000000000L) {
					ts.tv_sec++;
					ts.tv_nsec -= 1000000000L;
				}
				pthread_cond_timedwait(&wq_cond, &meta_mtx, &ts);
			}
		} else {
			n->save_inflight = 0;
			pthread_cond_broadcast(&pin_cond); /* wake meta_forget */
		}
	}
	pthread_mutex_unlock(&meta_mtx);
	return NULL;
}

static size_t cache_hash(const char *s)
{
	size_t h = 5381;
	for (; *s; s++)
		h = ((h << 5) + h) ^ (unsigned char)*s;
	return h & (META_CACHE_BUCKETS - 1);
}

static void lru_unlink(struct cnode *n)
{
	if (n->lru_prev)
		n->lru_prev->lru_next = n->lru_next;
	else
		lru_head = n->lru_next;
	if (n->lru_next)
		n->lru_next->lru_prev = n->lru_prev;
	else
		lru_tail = n->lru_prev;
	n->lru_prev = n->lru_next = NULL;
}

static void lru_push_front(struct cnode *n)
{
	n->lru_prev = NULL;
	n->lru_next = lru_head;
	if (lru_head)
		lru_head->lru_prev = n;
	lru_head = n;
	if (!lru_tail)
		lru_tail = n;
}

/* Find the canonical node for `dir`, or NULL. On a hit it is promoted to MRU. */
static struct cnode *cache_find(const char *dir)
{
	for (struct cnode *n = cache_buckets[cache_hash(dir)]; n; n = n->hnext)
		if (!strcmp(n->dir, dir)) {
			if (lru_head != n) {
				lru_unlink(n);
				lru_push_front(n);
			}
			return n;
		}
	return NULL;
}

/* Unlink `n` from its bucket + the LRU list and free it (index included). */
static void cache_remove_node(struct cnode *n)
{
	struct cnode **pp = &cache_buckets[cache_hash(n->dir)];
	while (*pp && *pp != n)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = n->hnext;
	lru_unlink(n);
	cache_count--;
	meta_free_index(&n->idx);
	free(n->dir);
	free(n);
}

/* Drop the canonical for `dir` if present (next access reparses from disk). If a
 * write-back is in flight for it, wait for that to finish first: the on-disk index
 * is stale until then, and the writer thread still holds the node — freeing it
 * would resurrect a stale index (its rename landing after ours) and use-after-free
 * the writer. (With the writer not running — the offline tools — no node is ever
 * pinned, so this never blocks.) */
static void cache_drop(const char *dir)
{
	struct cnode *n = cache_find(dir);
	while (n && cnode_pinned(n)) {
		pthread_cond_wait(&pin_cond, &meta_mtx);
		n = cache_find(dir);
	}
	if (n)
		cache_remove_node(n);
}

/* Insert `idx` (ownership moved in) as the canonical for `dir`, evicting the
 * LRU tail if over budget. Returns the node, or NULL on OOM (idx untouched).
 * Caller must have ensured no node for `dir` exists yet. */
static struct cnode *cache_insert(const char *dir, struct omdfs_index *idx)
{
	struct cnode *n = malloc(sizeof(*n));
	if (!n)
		return NULL;
	n->dir = strdup(dir);
	if (!n->dir) {
		free(n);
		return NULL;
	}
	n->idx = *idx; /* move: caller relinquishes ownership of idx->entries */
	n->save_dirty = 0;
	n->save_inflight = 0;
	n->wq_next = NULL;
	size_t b = cache_hash(dir);
	n->hnext = cache_buckets[b];
	cache_buckets[b] = n;
	lru_push_front(n);
	cache_count++;
	/* Evict unpinned victims from the LRU tail; skip nodes with a pending index
	 * write (dropping one would lose the not-yet-persisted change), so the cache
	 * may transiently exceed budget by the in-flight count — bounded and fine. */
	for (struct cnode *v = lru_tail; v && cache_count > META_CACHE_MAX;) {
		struct cnode *prev = v->lru_prev;
		if (v != n && !cnode_pinned(v) && !cnode_has_pending(v))
			cache_remove_node(v);
		v = prev;
	}
	return n;
}

/* Deep-copy a canonical index into a fresh caller-owned one. On OOM `dst` is
 * left freed/empty and -ENOMEM is returned. */
static int idx_copy(struct omdfs_index *dst, const struct omdfs_index *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->dir_st = src->dir_st;
	if (src->n == 0)
		return 0;
	dst->entries = malloc(src->n * sizeof(*dst->entries));
	if (!dst->entries)
		return -ENOMEM;
	dst->cap = src->n;
	for (size_t i = 0; i < src->n; i++) {
		struct omdfs_entry e = src->entries[i];
		e.ind = NULL; /* the indirection is canonical-only; never copied out */
		e.name = strdup(src->entries[i].name);
		e.link = src->entries[i].link ? strdup(src->entries[i].link)
					      : NULL;
		if (!e.name || (src->entries[i].link && !e.link)) {
			free(e.name);
			free(e.link);
			meta_free_index(dst);
			return -ENOMEM;
		}
		dst->entries[dst->n++] = e;
	}
	return 0;
}

/* Return the canonical index for `dir`, loading or building it on a miss (the
 * miss path does backend I/O under meta_mtx, matching the pre-cache writers).
 * The returned pointer is owned by the cache — do not free. *err is set on
 * failure (returns NULL). meta_mtx held. */
static struct omdfs_index *cache_canonical(const char *dir, int *err)
{
	struct cnode *n = cache_find(dir);
	if (n) {
		*err = 0;
		return &n->idx;
	}
	struct omdfs_index built;
	int r = load_index(dir, &built);
	if (r != 0)
		r = build_index(dir, &built);
	if (r != 0) {
		*err = r;
		return NULL;
	}
	n = cache_insert(dir, &built);
	if (!n) {
		meta_free_index(&built);
		*err = -ENOMEM;
		return NULL;
	}
	*err = 0;
	return &n->idx;
}

int meta_get_index(const char *fuse_dir, struct omdfs_index *out)
{
	pthread_mutex_lock(&meta_mtx);
	struct cnode *n = cache_find(fuse_dir);
	if (n) {
		int r = idx_copy(out, &n->idx);
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	pthread_mutex_unlock(&meta_mtx);

	/* Miss: load (or, failing that, build from the backend) WITHOUT the lock
	 * so a slow backend doesn't stall every other meta op, then insert with a
	 * recheck — a concurrent writer's canonical, if any, wins. */
	struct omdfs_index built;
	int r = load_index(fuse_dir, &built);
	if (r != 0)
		r = build_index(fuse_dir, &built);
	if (r != 0)
		return r;

	pthread_mutex_lock(&meta_mtx);
	n = cache_find(fuse_dir);
	if (n) {
		meta_free_index(&built);
		r = idx_copy(out, &n->idx);
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	n = cache_insert(fuse_dir, &built);
	if (!n) {
		/* Out of memory for the cache: hand the built index to the
		 * caller directly so the op still succeeds (just uncached). */
		pthread_mutex_unlock(&meta_mtx);
		*out = built;
		return 0;
	}
	r = idx_copy(out, &n->idx);
	pthread_mutex_unlock(&meta_mtx);
	return r;
}

int meta_try_load(const char *fuse_dir, struct omdfs_index *out)
{
	pthread_mutex_lock(&meta_mtx);
	struct cnode *n = cache_find(fuse_dir);
	if (n) {
		int r = idx_copy(out, &n->idx);
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	pthread_mutex_unlock(&meta_mtx);

	/* Load-only (never build from the backend): used by the syncer/evictor,
	 * which must touch only already-cached directories. */
	struct omdfs_index loaded;
	int r = load_index(fuse_dir, &loaded);
	if (r != 0)
		return r; /* -ENOENT/-EINVAL: no usable index */

	pthread_mutex_lock(&meta_mtx);
	n = cache_find(fuse_dir);
	if (n) {
		meta_free_index(&loaded);
		r = idx_copy(out, &n->idx);
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	n = cache_insert(fuse_dir, &loaded);
	if (!n) {
		pthread_mutex_unlock(&meta_mtx);
		*out = loaded;
		return 0;
	}
	r = idx_copy(out, &n->idx);
	pthread_mutex_unlock(&meta_mtx);
	return r;
}

int meta_stat(const char *fuse_dir, const char *name, struct stat *st,
	      uint8_t *type, uint8_t *flags)
{
	pthread_mutex_lock(&meta_mtx);
	struct cnode *n = cache_find(fuse_dir);
	if (n) {
		struct omdfs_entry *e = meta_lookup(&n->idx, name);
		if (!e) {
			pthread_mutex_unlock(&meta_mtx);
			return -ENOENT;
		}
		if (st)
			*st = e->st;
		if (type)
			*type = e->type;
		if (flags)
			*flags = e->flags;
		pthread_mutex_unlock(&meta_mtx);
		return 0;
	}
	pthread_mutex_unlock(&meta_mtx);

	/* Cold miss: build via the full (tested) path, then read one entry. */
	struct omdfs_index idx;
	int r = meta_get_index(fuse_dir, &idx);
	if (r != 0)
		return r;
	struct omdfs_entry *e = meta_lookup(&idx, name);
	if (e) {
		if (st)
			*st = e->st;
		if (type)
			*type = e->type;
		if (flags)
			*flags = e->flags;
	}
	r = e ? 0 : -ENOENT;
	meta_free_index(&idx);
	return r;
}

int meta_readlink(const char *fuse_dir, const char *name, char *buf,
		  size_t size)
{
	pthread_mutex_lock(&meta_mtx);
	struct cnode *n = cache_find(fuse_dir);
	if (n) {
		struct omdfs_entry *e = meta_lookup(&n->idx, name);
		int r = -ENOENT;
		if (e) {
			if (e->type == OMDFS_T_LINK && e->link) {
				snprintf(buf, size, "%s", e->link);
				r = 0;
			} else {
				r = -EINVAL;
			}
		}
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	pthread_mutex_unlock(&meta_mtx);

	struct omdfs_index idx;
	int r = meta_get_index(fuse_dir, &idx);
	if (r != 0)
		return r;
	struct omdfs_entry *e = meta_lookup(&idx, name);
	if (!e)
		r = -ENOENT;
	else if (e->type != OMDFS_T_LINK || !e->link)
		r = -EINVAL;
	else
		snprintf(buf, size, "%s", e->link);
	meta_free_index(&idx);
	return r;
}

int meta_dir_stat(const char *fuse_dir, struct stat *st, size_t *count)
{
	pthread_mutex_lock(&meta_mtx);
	struct cnode *n = cache_find(fuse_dir);
	if (n) {
		if (st)
			*st = n->idx.dir_st;
		if (count)
			*count = n->idx.n;
		pthread_mutex_unlock(&meta_mtx);
		return 0;
	}
	pthread_mutex_unlock(&meta_mtx);

	struct omdfs_index idx;
	int r = meta_get_index(fuse_dir, &idx);
	if (r != 0)
		return r;
	if (st)
		*st = idx.dir_st;
	if (count)
		*count = idx.n;
	meta_free_index(&idx);
	return 0;
}

void meta_forget(const char *fuse_dir)
{
	pthread_mutex_lock(&meta_mtx);
	cache_drop(fuse_dir);
	pthread_mutex_unlock(&meta_mtx);
}

void meta_forget_tree(const char *prefix)
{
	size_t plen = strlen(prefix);
	pthread_mutex_lock(&meta_mtx);
	/* Remove every unpinned match in a pass; if any match still has a write-back
	 * in flight, wait for a pin to clear and rescan (its write must land before we
	 * drop it — see cache_drop). The writer makes progress, so this terminates. */
	for (;;) {
		int pinned_match = 0;
		for (size_t b = 0; b < META_CACHE_BUCKETS; b++) {
			struct cnode *n = cache_buckets[b];
			while (n) {
				struct cnode *next = n->hnext;
				if (!strcmp(n->dir, prefix) ||
				    (!strncmp(n->dir, prefix, plen) &&
				     n->dir[plen] == '/')) {
					if (cnode_pinned(n))
						pinned_match = 1;
					else
						cache_remove_node(n);
				}
				n = next;
			}
		}
		if (!pinned_match)
			break;
		pthread_cond_wait(&pin_cond, &meta_mtx);
	}
	pthread_mutex_unlock(&meta_mtx);
}

int meta_evict_cold(const char *fuse_dir)
{
	if (fuse_dir[0] == '/' && fuse_dir[1] == '\0')
		return 0; /* never evict the root's index */

	pthread_mutex_lock(&meta_mtx);

	/* Check for un-synced (dirty) children. Use the in-memory canonical if it
	 * is already present; otherwise load straight from disk (load-only, never
	 * building from the backend) so a cold directory we are about to drop is not
	 * pulled into the cache. */
	struct omdfs_index local;
	int loaded = 0;
	struct cnode *cn = cache_find(fuse_dir);
	if (cn && cnode_pinned(cn)) {
		/* A write-back is pending: it may be clearing a just-flushed dirty
		 * flag. Skip this round; a later (clean) sweep reclaims it. */
		pthread_mutex_unlock(&meta_mtx);
		return 0;
	}
	const struct omdfs_index *idx;
	if (cn) {
		idx = &cn->idx;
	} else if (load_index(fuse_dir, &local) == 0) {
		idx = &local;
		loaded = 1;
	} else {
		pthread_mutex_unlock(&meta_mtx);
		return 0; /* no usable on-disk index: nothing to evict */
	}
	int dirty = 0;
	for (size_t i = 0; i < idx->n; i++)
		if (idx->entries[i].flags & OMDFS_F_DIRTY) {
			dirty = 1;
			break;
		}
	if (loaded)
		meta_free_index(&local);
	if (dirty) {
		pthread_mutex_unlock(&meta_mtx);
		return 0; /* a child still needs flushing: keep the index */
	}

	/* The premise of reclaiming the index is that the listing rebuilds faithfully
	 * from the backend on next access — which holds only if the backend namespace
	 * already matches the cache, i.e. no structural op is still undrained. The
	 * sync_once gate checks st->pending_structural, but that is measured at the
	 * start of phase_structural; a structural op appended *after* it (foreground
	 * mkdir/unlink/rename) is pending-but-uncounted, so the cold sweep can still
	 * race one. The worst case is a fresh dir whose own mkdir hasn't drained: it
	 * looks cold (empty cache dir, no dirty children) and would be reclaimed before
	 * its mkdir reaches the backend, so a create into it would rebuild the index
	 * from a backend dir that doesn't exist yet → ENOENT. (A pending child
	 * unlink/rename is the same hazard in reverse: rebuilding from the not-yet-
	 * updated backend would resurrect the child.) Re-check live, per candidate,
	 * under meta_mtx — which blocks the mutators from re-keying this subtree — that
	 * the WAL is fully drained. Cheap: an in-memory seq compare, no backend I/O. */
	if (!wal_fully_drained()) {
		pthread_mutex_unlock(&meta_mtx);
		return 0; /* a structural op is undrained: not safe to reclaim yet */
	}

	/* The cache directory must hold nothing but the index (+ any stale temp):
	 * no cached content files and no cached child subdirectory. Then dropping
	 * the index loses no local state — the listing rebuilds from the backend on
	 * next access. Collect the index/temp names in the same pass (we must not
	 * unlink while iterating the directory stream). */
	char cdir[PATH_MAX];
	cache_path(cdir, fuse_dir);
	DIR *dp = opendir(cdir);
	if (!dp) {
		pthread_mutex_unlock(&meta_mtx);
		return 0;
	}
	char victims[64][256];
	int nv = 0, only_index = 1;
	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (is_index_name(de->d_name)) {
			if (nv < 64)
				snprintf(victims[nv++], sizeof(victims[0]), "%s",
					 de->d_name);
			continue;
		}
		only_index = 0;
		break;
	}
	closedir(dp);
	if (!only_index) {
		pthread_mutex_unlock(&meta_mtx);
		return 0; /* cached content or a child subdir is still present */
	}

	/* Reclaim: remove the index (+ temps), drop the in-memory copy, and rmdir
	 * the now-empty cache directory so an ancestor can collapse in turn. */
	for (int i = 0; i < nv; i++) {
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", cdir, victims[i]);
		unlink(p);
	}
	cache_drop(fuse_dir);
	rmdir(cdir); /* best-effort; leaves nothing behind on success */
	pthread_mutex_unlock(&meta_mtx);
	return 1;
}

int meta_mark_flags(const char *fuse_dir, const char *name, uint8_t set_bits,
		    uint8_t clear_bits)
{
	pthread_mutex_lock(&meta_mtx);
	int r;
	struct omdfs_index *idx = cache_canonical(fuse_dir, &r);
	if (!idx) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	struct omdfs_entry *e = meta_lookup(idx, name);
	if (!e) {
		pthread_mutex_unlock(&meta_mtx);
		return -ENOENT;
	}
	uint8_t nf = (uint8_t)((e->flags | set_bits) & ~clear_bits);
	r = 0;
	if (nf != e->flags) {
		e->flags = nf;
		r = meta_schedule_save(cnode_of(idx));
		if (r != 0)
			cache_drop(fuse_dir); /* revert to on-disk state */
	}
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && (set_bits & OMDFS_F_DIRTY))
		dirtyset_add(fuse_dir);
	return r;
}

/* Defined below (with the flush gate), but used here by meta_flush_claim. */
static int flushable_locked(const char *dir, const char *name);

int meta_flush_claim(const char *fuse_dir, const char *name,
		     struct omdfs_entry *out, uint8_t *cleared,
		     struct ind **out_ind)
{
	*out_ind = NULL;
	pthread_mutex_lock(&meta_mtx);
	int r;
	struct omdfs_index *idx = cache_canonical(fuse_dir, &r);
	if (!idx) {
		pthread_mutex_unlock(&meta_mtx);
		return -ENOENT;
	}
	struct omdfs_entry *e = meta_lookup(idx, name);
	if (!e) {
		pthread_mutex_unlock(&meta_mtx);
		return -ENOENT;
	}
	uint8_t d = e->flags & OMDFS_F_DIRTY;
	if (!d) {
		pthread_mutex_unlock(&meta_mtx);
		return 1; /* no longer dirty: another path handled it */
	}
	/* Re-check the flush gate atomically with the claim: a structural op may have
	 * arrived for this entry (or an ancestor) since flush_submit's cheaper
	 * pre-check, e.g. another file was just renamed onto this path. If so, don't
	 * push — leave it dirty and tell the caller to re-arm (return 2). This is what
	 * keeps a barrier-free overlap from flushing an entry whose rename is undrained. */
	if (!flushable_locked(fuse_dir, name)) {
		pthread_mutex_unlock(&meta_mtx);
		return 2; /* gated: still dirty, caller re-arms */
	}
	/* Deep-copy the entry's *current* state for the caller to push, then clear
	 * the dirty bits atomically. A mutation that re-dirties this entry after we
	 * unlock re-sets the bits (so it is re-flushed next cycle) instead of being
	 * lost when the push completes and the flag is cleared. */
	*out = *e;
	out->ind = NULL; /* canonical-only; the flush worker must not touch it */
	out->name = strdup(e->name);
	out->link = e->link ? strdup(e->link) : NULL;
	if (!out->name || (e->link && !out->link)) {
		free(out->name);
		free(out->link);
		pthread_mutex_unlock(&meta_mtx);
		return -ENOMEM;
	}
	e->flags = (uint8_t)(e->flags & ~OMDFS_F_DIRTY);
	r = meta_schedule_save(cnode_of(idx));
	if (r != 0) {
		/* Couldn't persist the cleared flags; revert so the entry stays dirty
		 * and retry next cycle. */
		e->flags |= d;
		free(out->name);
		free(out->link);
		cache_drop(fuse_dir);
		pthread_mutex_unlock(&meta_mtx);
		dirtyset_add(fuse_dir);
		return -EIO;
	}
	*cleared = d;
	/* Mark the entry's indirection in-flight so the next cycle's phase 1 defers a
	 * concurrent rename/unlink of it (Change 3), and hand the indirection back so
	 * the caller can release it after the push — reached by pointer, so it survives
	 * the entry being renamed or unlinked mid-flush. flushing also anchors the
	 * indirection's lifetime. Atomic with the claim under meta_mtx. ind_ensure can
	 * only fail on OOM, in which case we skip the exclusion (degrades safely). */
	struct ind *in = ind_ensure(cnode_of(idx), e);
	if (in)
		in->flushing = 1;
	*out_ind = in;
	pthread_mutex_unlock(&meta_mtx);
	return 0;
}

int meta_mark_dir_dirty(const char *fuse_dir, long *files, long *dirs,
			long *links)
{
	pthread_mutex_lock(&meta_mtx);
	int r;
	struct omdfs_index *idx = cache_canonical(fuse_dir, &r);
	if (!idx) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	int changed = 0, has_dirty = 0;
	for (size_t i = 0; i < idx->n; i++) {
		struct omdfs_entry *e = &idx->entries[i];
		uint8_t set;
		switch (e->type) {
		case OMDFS_T_LINK:
			/* A symlink is re-created structurally (WAL), not flushed;
			 * it carries no content/attr to push. */
			set = 0;
			if (links)
				(*links)++;
			break;
		case OMDFS_T_DIR:
			set = OMDFS_F_DIRTY_ATTR;
			if (dirs)
				(*dirs)++;
			break;
		default: /* regular file */
			set = OMDFS_F_DIRTY_ATTR;
			if (e->flags & OMDFS_F_CONTENT_CACHED)
				set |= OMDFS_F_DIRTY_CONTENT;
			if (files)
				(*files)++;
			break;
		}
		uint8_t nf = (uint8_t)(e->flags | set);
		if (nf != e->flags) {
			e->flags = nf;
			changed = 1;
		}
		if (nf & OMDFS_F_DIRTY)
			has_dirty = 1;
	}
	r = 0;
	if (changed) {
		r = meta_schedule_save(cnode_of(idx));
		if (r != 0)
			cache_drop(fuse_dir); /* revert to on-disk state */
	}
	pthread_mutex_unlock(&meta_mtx);
	/* Seed the dirty set whenever the directory now holds dirty work, even if
	 * this call set no new flag (e.g. a re-run), so the syncer revisits it. */
	if (r == 0 && has_dirty)
		dirtyset_add(fuse_dir);
	return r;
}

int meta_init_dir(const char *fuse_dir, const struct stat *dir_st)
{
	pthread_mutex_lock(&meta_mtx);
	/* Publish the fresh empty index as the canonical so the new dir is
	 * immediately served from cache (replacing any stale entry left from a prior
	 * same-path dir), then schedule its persist. */
	cache_drop(fuse_dir);
	struct omdfs_index fresh;
	memset(&fresh, 0, sizeof(fresh));
	fresh.dir_st = *dir_st;
	struct cnode *n = cache_insert(fuse_dir, &fresh);
	int r;
	if (n) {
		r = meta_schedule_save(n);
	} else {
		/* OOM: no cnode to write back from — persist synchronously so the
		 * new dir is durable (served from disk on next access). */
		r = save_index(fuse_dir, &fresh);
		meta_free_index(&fresh);
	}
	pthread_mutex_unlock(&meta_mtx);
	return r;
}

int meta_add_entry(const char *fuse_dir, const struct omdfs_entry *e)
{
	pthread_mutex_lock(&meta_mtx);
	int r;
	struct omdfs_index *idx = cache_canonical(fuse_dir, &r);
	if (!idx) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	if (meta_lookup(idx, e->name)) {
		pthread_mutex_unlock(&meta_mtx);
		return -EEXIST;
	}
	struct omdfs_entry ne = *e;
	ne.name = strdup(e->name);
	ne.link = e->link ? strdup(e->link) : NULL;
	if (!ne.name || (e->link && !ne.link) || idx_push(idx, &ne) != 0) {
		free(ne.name);
		free(ne.link);
		pthread_mutex_unlock(&meta_mtx);
		return -ENOMEM;
	}
	r = meta_schedule_save(cnode_of(idx));
	if (r != 0)
		cache_drop(fuse_dir);
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && (e->flags & OMDFS_F_DIRTY))
		dirtyset_add(fuse_dir);
	return r;
}

/* meta_mtx held. Drop entry at index i (swap with last; order is irrelevant).
 * When `release_ind` is set the entry is genuinely going away, so release its
 * indirection; a caller that has *transferred* the indirection to another entry
 * (a cross-directory move) passes 0 so the live ind isn't detached/freed. */
static void drop_at(struct omdfs_index *idx, size_t i, int release_ind)
{
	if (release_ind)
		ind_release(&idx->entries[i]);
	free(idx->entries[i].name);
	free(idx->entries[i].link);
	idx->entries[i] = idx->entries[--idx->n];
	/* The former tail entry moved into slot i: repoint its handle (cnode
	 * unchanged). Skipped when i was itself the tail (self-copy, nothing moved). */
	if (i < idx->n && idx->entries[i].ind)
		idx->entries[i].ind->entry = &idx->entries[i];
	idx_hash_drop(idx); /* slot moved: invalidate the lookup hash */
}

int meta_remove_entry(const char *fuse_dir, const char *name,
		      struct ind **out_ind)
{
	if (out_ind)
		*out_ind = NULL;
	pthread_mutex_lock(&meta_mtx);
	int r;
	struct omdfs_index *idx = cache_canonical(fuse_dir, &r);
	if (!idx) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	struct omdfs_entry *e = meta_lookup(idx, name);
	if (!e) {
		pthread_mutex_unlock(&meta_mtx);
		return -ENOENT;
	}
	/* Keep the entry's indirection (transfer it to the caller, not release it), so a
	 * concurrent flush's flushing flag stays reachable until both the flush and the
	 * upcoming unlink/rmdir op finish. Detach the entry from the visible index now. */
	struct ind *in = ind_ensure(cnode_of(idx), e); /* NULL only on OOM */
	drop_at(idx, (size_t)(e - idx->entries), 0 /* ind transferred, don't release */);
	r = meta_schedule_save(cnode_of(idx));
	if (r != 0) {
		cache_drop(fuse_dir);
		/* The removal didn't persist; the orphaned indirection has no entry — mark
		 * it detached (null the handle) and free it unless a flush still holds it. */
		if (in) {
			in->entry = NULL;
			in->cnode = NULL;
			in->detached = 1;
			ind_maybe_free(in);
		}
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	/* Pre-bump for the unlink/rmdir node the caller is about to append (committed,
	 * or rolled back, by syncer_log_attach), and mark detached (the entry is gone,
	 * so null the handle). */
	if (in) {
		in->pending_count++;
		in->entry = NULL;
		in->cnode = NULL;
		in->detached = 1;
	}
	pthread_mutex_unlock(&meta_mtx);
	if (out_ind)
		*out_ind = in;
	return 0;
}

int meta_move_entry(const char *old_dir, const char *old_name,
		    const char *new_dir, const char *new_name, int *dst_dirty_out,
		    struct ind **moved_ind)
{
	pthread_mutex_lock(&meta_mtx);
	int r;
	int dst_dirty = 0; /* moved entry carries dirty flags -> new_dir is dirty */
	struct ind *bumped = NULL; /* moved entry's ind, pre-bumped for the rename */

	if (!strcmp(old_dir, new_dir)) {
		struct omdfs_index *idx = cache_canonical(old_dir, &r);
		if (!idx)
			goto out;
		struct omdfs_entry *e = meta_lookup(idx, old_name);
		if (!e) {
			r = -ENOENT;
			goto out;
		}
		struct omdfs_entry *t = meta_lookup(idx, new_name);
		if (t && t != e)
			drop_at(idx, (size_t)(t - idx->entries), 1 /* target gone */);
		/* meta_lookup pointer may have moved after drop_at swap. */
		e = meta_lookup(idx, old_name);
		char *nn = strdup(new_name);
		if (!nn) {
			r = -ENOMEM;
			goto out;
		}
		free(e->name);
		e->name = nn; /* in-place rename: e keeps its ind */
		idx_hash_drop(idx); /* name changed in place: invalidate the hash */
		dst_dirty = (e->flags & OMDFS_F_DIRTY) != 0;
		r = meta_schedule_save(cnode_of(idx));
		if (r != 0) {
			cache_drop(old_dir);
		} else {
			/* Bump the rename's pending_count while still under the lock, so
			 * any flush that observes the (dirty) moved entry also observes
			 * count>0 and defers — closing the publish window. Done only after
			 * the last fallible step so a failure can't strand the count. */
			bumped = ind_ensure(cnode_of(idx), e);
			if (bumped)
				bumped->pending_count++;
		}
		goto out;
	}

	/* Cross-directory: extract from old, insert into new, carrying flags AND the
	 * indirection (so a pending structural-op count survives the move with the
	 * entry). */
	struct omdfs_index *oidx = cache_canonical(old_dir, &r);
	if (!oidx)
		goto out;
	struct omdfs_entry *e = meta_lookup(oidx, old_name);
	if (!e) {
		r = -ENOENT;
		goto out;
	}
	struct omdfs_entry moved = *e; /* carries moved.ind = e->ind (transferred) */
	dst_dirty = (moved.flags & OMDFS_F_DIRTY) != 0;
	moved.name = strdup(new_name);
	moved.link = e->link ? strdup(e->link) : NULL;
	if (!moved.name || (e->link && !moved.link)) {
		free(moved.name);
		free(moved.link);
		r = -ENOMEM;
		goto out;
	}
	/* release_ind = 0: the ind now lives on `moved`, so don't detach/free it. */
	drop_at(oidx, (size_t)(e - oidx->entries), 0);
	r = meta_schedule_save(cnode_of(oidx));
	if (r != 0) {
		cache_drop(old_dir);
		free(moved.name);
		free(moved.link);
		goto out;
	}

	/* NB: scheduling old_dir's save pins its node, so the cache_canonical(new_dir)
	 * below won't evict it out from under the pending write-back. */
	struct omdfs_index *nidx = cache_canonical(new_dir, &r);
	if (!nidx) {
		free(moved.name);
		free(moved.link);
		goto out;
	}
	struct omdfs_entry *t = meta_lookup(nidx, new_name);
	if (t)
		drop_at(nidx, (size_t)(t - nidx->entries), 1 /* target gone */);
	if (idx_push(nidx, &moved) != 0) {
		free(moved.name);
		free(moved.link);
		r = -ENOMEM;
		goto out;
	}
	r = meta_schedule_save(cnode_of(nidx));
	if (r != 0) {
		cache_drop(new_dir);
	} else {
		/* Bump on the entry now living in nidx (see the same-dir branch). */
		bumped = ind_ensure(cnode_of(nidx), &nidx->entries[nidx->n - 1]);
		if (bumped)
			bumped->pending_count++;
	}

out:
	pthread_mutex_unlock(&meta_mtx);
	/* Publication of the moved entry to the dirty-set is deferred to the caller
	 * (after syncer_log_attach appends + enqueues), so the flush never sees it dirty
	 * and settled while the rename is undrained — see DESIGN.md Change 1. The
	 * pre-bumped count is handed back for the caller to commit (attach) or roll
	 * back; on a failed move there is nothing pending. */
	if (dst_dirty_out)
		*dst_dirty_out = (r == 0) ? dst_dirty : 0;
	if (moved_ind)
		*moved_ind = (r == 0) ? bumped : NULL;
	return r;
}

int meta_set(const char *fuse_dir, const char *name, const struct stat *src,
	     unsigned fields, uint8_t dirty)
{
	pthread_mutex_lock(&meta_mtx);
	int r;
	struct omdfs_index *idx = cache_canonical(fuse_dir, &r);
	if (!idx) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	struct omdfs_entry *e = meta_lookup(idx, name);
	if (!e) {
		pthread_mutex_unlock(&meta_mtx);
		return -ENOENT;
	}
	if (fields & META_MODE)
		e->st.st_mode = (e->st.st_mode & S_IFMT) |
				(src->st_mode & 07777);
	if (fields & META_UID)
		e->st.st_uid = src->st_uid;
	if (fields & META_GID)
		e->st.st_gid = src->st_gid;
	if (fields & META_SIZE) {
		e->st.st_size = src->st_size;
		e->st.st_blocks = (src->st_size + 511) / 512;
	}
	if (fields & META_TIMES) {
		e->st.st_atim = src->st_atim;
		e->st.st_mtim = src->st_mtim;
	}
	e->flags |= dirty;
	int is_dir = (e->type == OMDFS_T_DIR);
	struct stat newst = e->st;
	r = meta_schedule_save(cnode_of(idx));
	if (r != 0) {
		cache_drop(fuse_dir);
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	/* A directory's mode/owner/times live in two places: this entry in the
	 * parent index (read by getattr and the dirty flush) and the directory's
	 * OWN index dir_st (read by readdir's "." and by --resync). Keep them in
	 * sync so the change isn't reverted by a stale dir_st on a later resync. */
	if (is_dir) {
		char child[PATH_MAX];
		if (fuse_dir[0] == '/' && fuse_dir[1] == '\0')
			snprintf(child, sizeof(child), "/%s", name);
		else
			snprintf(child, sizeof(child), "%s/%s", fuse_dir, name);
		int cr;
		struct omdfs_index *cidx = cache_canonical(child, &cr);
		if (cidx) {
			if (fields & META_MODE)
				cidx->dir_st.st_mode =
					(cidx->dir_st.st_mode & S_IFMT) |
					(newst.st_mode & 07777);
			if (fields & META_UID)
				cidx->dir_st.st_uid = newst.st_uid;
			if (fields & META_GID)
				cidx->dir_st.st_gid = newst.st_gid;
			if (fields & META_TIMES) {
				cidx->dir_st.st_atim = newst.st_atim;
				cidx->dir_st.st_mtim = newst.st_mtim;
			}
			meta_schedule_save(cnode_of(cidx));
		}
	}
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && (dirty & OMDFS_F_DIRTY))
		dirtyset_add(fuse_dir);
	return r;
}

/* ---- indirection lifecycle for the syncer's structural drain queue ----
 *
 * The structural drain FIFO lives in syncer.c now (DESIGN.md § 2026-06-21); meta.c
 * owns only the per-entry `ind` and the operations the syncer performs on it while
 * enqueuing, draining, or rolling back a node. The flush gate (pending_count on E +
 * ancestors) and the handle audit are unchanged; these are just the entry points the
 * syncer reaches them through. All state under meta_mtx. */
struct ind *meta_ind_acquire(const char *dir, const char *name)
{
	pthread_mutex_lock(&meta_mtx);
	struct cnode *n = cache_find(dir);
	struct omdfs_entry *e = n ? meta_lookup(&n->idx, name) : NULL;
	struct ind *in = e ? ind_ensure(n, e) : NULL; /* NULL: entry vanished or OOM */
	if (in)
		in->pending_count++;
	pthread_mutex_unlock(&meta_mtx);
	return in;
}

void meta_ind_unbump(struct ind *in)
{
	pthread_mutex_lock(&meta_mtx);
	if (in && in->pending_count > 0)
		in->pending_count--;
	ind_maybe_free(in); /* NULL-safe */
	pthread_mutex_unlock(&meta_mtx);
}

int meta_ind_flushing(struct ind *in)
{
	pthread_mutex_lock(&meta_mtx);
	ind_audit(in); /* NULL => seeded recovery node */
	int f = (in && in->flushing) ? 1 : 0;
	pthread_mutex_unlock(&meta_mtx);
	return f;
}

void meta_ind_settle(struct ind *in)
{
	pthread_mutex_lock(&meta_mtx);
	ind_audit(in);
	if (in) {
		if (in->pending_count > 0)
			in->pending_count--;
		ind_maybe_free(in);
	}
	pthread_mutex_unlock(&meta_mtx);
}

/* The flush gate, meta_mtx held. 1 if child `name` in `dir` and every ancestor
 * directory have pending_count == 0. */
static int flushable_locked(const char *dir, const char *name)
{
	struct cnode *n = cache_find(dir);
	struct omdfs_entry *e = n ? meta_lookup(&n->idx, name) : NULL;
	if (e && e->ind && e->ind->pending_count > 0)
		return 0; /* the entry's own create/rename is undrained */

	/* Walk the ancestor directories: a pending mkdir/rename on an ancestor moves
	 * E's backend path without touching E's own count, so each ancestor dir's
	 * entry (which lives in *its* parent) must also be settled. A dir with a
	 * pending op keeps its parent cnode pinned (cnode_has_pending), so a cache
	 * miss here means no pending op — treat it as settled. */
	char cur[PATH_MAX];
	snprintf(cur, sizeof(cur), "%s", dir);
	while (!(cur[0] == '/' && cur[1] == '\0')) {
		char pdir[PATH_MAX], pname[PATH_MAX];
		omdfs_split_path(cur, pdir, pname);
		struct cnode *pn = cache_find(pdir);
		struct omdfs_entry *pe = pn ? meta_lookup(&pn->idx, pname) : NULL;
		if (pe && pe->ind && pe->ind->pending_count > 0)
			return 0;
		snprintf(cur, sizeof(cur), "%s", pdir);
	}
	return 1;
}

int meta_flushable(const char *dir, const char *name)
{
	pthread_mutex_lock(&meta_mtx);
	int ok = flushable_locked(dir, name);
	pthread_mutex_unlock(&meta_mtx);
	return ok;
}

void meta_flush_release(struct ind *in)
{
	if (!in)
		return;
	pthread_mutex_lock(&meta_mtx);
	in->flushing = 0;
	ind_maybe_free(in); /* the flush was the last reference? then free it */
	pthread_mutex_unlock(&meta_mtx);
}

/* Start the async index-writer thread. Call once at mount, before any FUSE op or
 * the syncer (which clears dirty flags via meta_mark_flags). Until this runs, the
 * mutators persist synchronously (the offline tools' path). Returns 0 or -errno. */
int meta_init(void)
{
	writer_stop = 0;
	if (pthread_create(&writer_thread, NULL, index_writer_main, NULL) != 0)
		return -errno;
	pthread_mutex_lock(&meta_mtx);
	writer_running = 1;
	pthread_mutex_unlock(&meta_mtx);
	return 0;
}

/* Drain every pending index write and stop the writer thread. Call at unmount
 * AFTER syncer_stop, so dirty-flag clears from the final backend drain also
 * persist. Idempotent; a no-op if meta_init never ran. */
void meta_shutdown(void)
{
	pthread_mutex_lock(&meta_mtx);
	if (!writer_running) {
		pthread_mutex_unlock(&meta_mtx);
		return;
	}
	writer_stop = 1;
	pthread_cond_broadcast(&wq_cond);
	pthread_mutex_unlock(&meta_mtx);
	pthread_join(writer_thread, NULL);
	pthread_mutex_lock(&meta_mtx);
	writer_running = 0;
	pthread_mutex_unlock(&meta_mtx);
}

/* Report index write-back state for the status file: *pending = directories
 * queued for write (excludes the one currently being written), *last_errno = the
 * most recent write failure (0 if the last write succeeded), *path = that
 * failure's directory. Any out param may be NULL. */
void meta_writeback_status(int *pending, int *last_errno, char *path,
			   size_t pathsz)
{
	pthread_mutex_lock(&meta_mtx);
	if (pending) {
		int c = 0;
		for (struct cnode *n = wq_head; n; n = n->wq_next)
			c++;
		*pending = c;
	}
	if (last_errno)
		*last_errno = wb_last_errno;
	if (path && pathsz)
		snprintf(path, pathsz, "%s", wb_last_path);
	pthread_mutex_unlock(&meta_mtx);
}
