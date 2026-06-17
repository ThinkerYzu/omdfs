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
#include "omdfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	}
	idx->entries[idx->n++] = *e;
	return 0;
}

void meta_free_index(struct omdfs_index *idx)
{
	for (size_t i = 0; i < idx->n; i++) {
		free(idx->entries[i].name);
		free(idx->entries[i].link);
	}
	free(idx->entries);
	idx->entries = NULL;
	idx->n = idx->cap = 0;
}

struct omdfs_entry *meta_lookup(struct omdfs_index *idx, const char *name)
{
	for (size_t i = 0; i < idx->n; i++)
		if (!strcmp(idx->entries[i].name, name))
			return &idx->entries[i];
	return NULL;
}

/* ---- persistence ---- */

static int save_index(const char *fuse_dir, const struct omdfs_index *idx)
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

	struct index_header h = {
		.magic = OMDFS_INDEX_MAGIC,
		.version = OMDFS_INDEX_VERSION,
		.count = (uint32_t)idx->n,
		.dir_st = idx->dir_st,
	};
	if (write_all(fd, &h, sizeof(h)) != 0)
		goto fail;

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
		if (write_all(fd, &eh, sizeof(eh)) != 0 ||
		    write_all(fd, e->name, nlen) != 0 ||
		    (llen && write_all(fd, e->link, llen) != 0))
			goto fail;
	}

	if (fsync(fd) != 0)
		goto fail;
	close(fd);

	char ipath[PATH_MAX];
	index_path(ipath, fuse_dir);
	if (rename(tmpl, ipath) != 0) {
		int e = errno;
		unlink(tmpl);
		return -e;
	}
	return 0;

fail:
	close(fd);
	unlink(tmpl);
	return -EIO;
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
};

static struct cnode *cache_buckets[META_CACHE_BUCKETS];
static struct cnode *lru_head, *lru_tail;
static size_t cache_count;

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

/* Drop the canonical for `dir` if present (next access reparses from disk). */
static void cache_drop(const char *dir)
{
	struct cnode *n = cache_find(dir);
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
	size_t b = cache_hash(dir);
	n->hnext = cache_buckets[b];
	cache_buckets[b] = n;
	lru_push_front(n);
	cache_count++;
	while (cache_count > META_CACHE_MAX && lru_tail && lru_tail != n)
		cache_remove_node(lru_tail);
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
	for (size_t b = 0; b < META_CACHE_BUCKETS; b++) {
		struct cnode *n = cache_buckets[b];
		while (n) {
			struct cnode *next = n->hnext;
			if (!strcmp(n->dir, prefix) ||
			    (!strncmp(n->dir, prefix, plen) &&
			     n->dir[plen] == '/'))
				cache_remove_node(n);
			n = next;
		}
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
		r = save_index(fuse_dir, idx);
		if (r != 0)
			cache_drop(fuse_dir); /* revert to on-disk state */
	}
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && (set_bits & OMDFS_F_DIRTY))
		dirtyset_add(fuse_dir);
	return r;
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
		r = save_index(fuse_dir, idx);
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
	struct omdfs_index idx;
	memset(&idx, 0, sizeof(idx));
	idx.dir_st = *dir_st;
	pthread_mutex_lock(&meta_mtx);
	int r = save_index(fuse_dir, &idx);
	if (r == 0) {
		/* Publish the fresh empty index as the canonical so the new dir
		 * is immediately served from cache (replacing any stale entry
		 * left from a prior same-path dir). */
		cache_drop(fuse_dir);
		struct omdfs_index fresh;
		memset(&fresh, 0, sizeof(fresh));
		fresh.dir_st = *dir_st;
		cache_insert(fuse_dir, &fresh); /* NULL on OOM: fine, loads later */
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
	r = save_index(fuse_dir, idx);
	if (r != 0)
		cache_drop(fuse_dir);
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && (e->flags & OMDFS_F_DIRTY))
		dirtyset_add(fuse_dir);
	return r;
}

/* meta_mtx held. Drop entry at index i (swap with last; order is irrelevant). */
static void drop_at(struct omdfs_index *idx, size_t i)
{
	free(idx->entries[i].name);
	free(idx->entries[i].link);
	idx->entries[i] = idx->entries[--idx->n];
}

int meta_remove_entry(const char *fuse_dir, const char *name)
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
	drop_at(idx, (size_t)(e - idx->entries));
	r = save_index(fuse_dir, idx);
	if (r != 0)
		cache_drop(fuse_dir);
	pthread_mutex_unlock(&meta_mtx);
	return r;
}

int meta_move_entry(const char *old_dir, const char *old_name,
		    const char *new_dir, const char *new_name)
{
	pthread_mutex_lock(&meta_mtx);
	int r;
	int dst_dirty = 0; /* moved entry carries dirty flags -> new_dir is dirty */

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
			drop_at(idx, (size_t)(t - idx->entries));
		/* meta_lookup pointer may have moved after drop_at swap. */
		e = meta_lookup(idx, old_name);
		char *nn = strdup(new_name);
		if (!nn) {
			r = -ENOMEM;
			goto out;
		}
		free(e->name);
		e->name = nn;
		dst_dirty = (e->flags & OMDFS_F_DIRTY) != 0;
		r = save_index(old_dir, idx);
		if (r != 0)
			cache_drop(old_dir);
		goto out;
	}

	/* Cross-directory: extract from old, insert into new, carrying flags. */
	struct omdfs_index *oidx = cache_canonical(old_dir, &r);
	if (!oidx)
		goto out;
	struct omdfs_entry *e = meta_lookup(oidx, old_name);
	if (!e) {
		r = -ENOENT;
		goto out;
	}
	struct omdfs_entry moved = *e;
	dst_dirty = (moved.flags & OMDFS_F_DIRTY) != 0;
	moved.name = strdup(new_name);
	moved.link = e->link ? strdup(e->link) : NULL;
	if (!moved.name || (e->link && !moved.link)) {
		free(moved.name);
		free(moved.link);
		r = -ENOMEM;
		goto out;
	}
	drop_at(oidx, (size_t)(e - oidx->entries));
	r = save_index(old_dir, oidx);
	if (r != 0) {
		cache_drop(old_dir);
		free(moved.name);
		free(moved.link);
		goto out;
	}

	/* NB: cache_canonical(new_dir) may evict the old_dir node (already saved
	 * and no longer used here) if RAM is tight — harmless. */
	struct omdfs_index *nidx = cache_canonical(new_dir, &r);
	if (!nidx) {
		free(moved.name);
		free(moved.link);
		goto out;
	}
	struct omdfs_entry *t = meta_lookup(nidx, new_name);
	if (t)
		drop_at(nidx, (size_t)(t - nidx->entries));
	if (idx_push(nidx, &moved) != 0) {
		free(moved.name);
		free(moved.link);
		r = -ENOMEM;
		goto out;
	}
	r = save_index(new_dir, nidx);
	if (r != 0)
		cache_drop(new_dir);

out:
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && dst_dirty)
		dirtyset_add(new_dir);
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
	r = save_index(fuse_dir, idx);
	if (r != 0)
		cache_drop(fuse_dir);
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && (dirty & OMDFS_F_DIRTY))
		dirtyset_add(fuse_dir);
	return r;
}
