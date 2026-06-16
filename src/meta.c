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

int meta_get_index(const char *fuse_dir, struct omdfs_index *out)
{
	if (load_index(fuse_dir, out) == 0)
		return 0;
	return build_index(fuse_dir, out);
}

int meta_try_load(const char *fuse_dir, struct omdfs_index *out)
{
	return load_index(fuse_dir, out);
}

/* All index read-modify-write goes through this one mutex so concurrent
 * mutations (flags, add/remove/move, attrs) can't clobber each other. Readers
 * (meta_get_index/meta_try_load) stay lock-free: save_index is atomic (tmp +
 * rename), so a reader always sees a whole old or whole new index. */
static pthread_mutex_t meta_mtx = PTHREAD_MUTEX_INITIALIZER;

int meta_mark_flags(const char *fuse_dir, const char *name, uint8_t set_bits,
		    uint8_t clear_bits)
{
	pthread_mutex_lock(&meta_mtx);
	struct omdfs_index idx;
	int r = meta_get_index(fuse_dir, &idx);
	if (r != 0) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	struct omdfs_entry *e = meta_lookup(&idx, name);
	if (!e) {
		meta_free_index(&idx);
		pthread_mutex_unlock(&meta_mtx);
		return -ENOENT;
	}
	uint8_t nf = (uint8_t)((e->flags | set_bits) & ~clear_bits);
	r = 0;
	if (nf != e->flags) {
		e->flags = nf;
		r = save_index(fuse_dir, &idx);
	}
	meta_free_index(&idx);
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && (set_bits & OMDFS_F_DIRTY))
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
	pthread_mutex_unlock(&meta_mtx);
	return r;
}

int meta_add_entry(const char *fuse_dir, const struct omdfs_entry *e)
{
	pthread_mutex_lock(&meta_mtx);
	struct omdfs_index idx;
	int r = meta_get_index(fuse_dir, &idx);
	if (r != 0) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	if (meta_lookup(&idx, e->name)) {
		meta_free_index(&idx);
		pthread_mutex_unlock(&meta_mtx);
		return -EEXIST;
	}
	struct omdfs_entry ne = *e;
	ne.name = strdup(e->name);
	ne.link = e->link ? strdup(e->link) : NULL;
	if (!ne.name || (e->link && !ne.link) || idx_push(&idx, &ne) != 0) {
		free(ne.name);
		free(ne.link);
		meta_free_index(&idx);
		pthread_mutex_unlock(&meta_mtx);
		return -ENOMEM;
	}
	r = save_index(fuse_dir, &idx);
	meta_free_index(&idx);
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
	struct omdfs_index idx;
	int r = meta_get_index(fuse_dir, &idx);
	if (r != 0) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	struct omdfs_entry *e = meta_lookup(&idx, name);
	if (!e) {
		meta_free_index(&idx);
		pthread_mutex_unlock(&meta_mtx);
		return -ENOENT;
	}
	drop_at(&idx, (size_t)(e - idx.entries));
	r = save_index(fuse_dir, &idx);
	meta_free_index(&idx);
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
		struct omdfs_index idx;
		r = meta_get_index(old_dir, &idx);
		if (r != 0)
			goto out;
		struct omdfs_entry *e = meta_lookup(&idx, old_name);
		if (!e) {
			meta_free_index(&idx);
			r = -ENOENT;
			goto out;
		}
		struct omdfs_entry *t = meta_lookup(&idx, new_name);
		if (t && t != e)
			drop_at(&idx, (size_t)(t - idx.entries));
		/* meta_lookup pointer may have moved after drop_at swap. */
		e = meta_lookup(&idx, old_name);
		char *nn = strdup(new_name);
		if (!nn) {
			meta_free_index(&idx);
			r = -ENOMEM;
			goto out;
		}
		free(e->name);
		e->name = nn;
		dst_dirty = (e->flags & OMDFS_F_DIRTY) != 0;
		r = save_index(old_dir, &idx);
		meta_free_index(&idx);
		goto out;
	}

	/* Cross-directory: extract from old, insert into new, carrying flags. */
	struct omdfs_index oidx;
	r = meta_get_index(old_dir, &oidx);
	if (r != 0)
		goto out;
	struct omdfs_entry *e = meta_lookup(&oidx, old_name);
	if (!e) {
		meta_free_index(&oidx);
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
		meta_free_index(&oidx);
		r = -ENOMEM;
		goto out;
	}
	drop_at(&oidx, (size_t)(e - oidx.entries));
	r = save_index(old_dir, &oidx);
	meta_free_index(&oidx);
	if (r != 0) {
		free(moved.name);
		free(moved.link);
		goto out;
	}

	struct omdfs_index nidx;
	r = meta_get_index(new_dir, &nidx);
	if (r != 0) {
		free(moved.name);
		free(moved.link);
		goto out;
	}
	struct omdfs_entry *t = meta_lookup(&nidx, new_name);
	if (t)
		drop_at(&nidx, (size_t)(t - nidx.entries));
	if (idx_push(&nidx, &moved) != 0) {
		free(moved.name);
		free(moved.link);
		meta_free_index(&nidx);
		r = -ENOMEM;
		goto out;
	}
	r = save_index(new_dir, &nidx);
	meta_free_index(&nidx);

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
	struct omdfs_index idx;
	int r = meta_get_index(fuse_dir, &idx);
	if (r != 0) {
		pthread_mutex_unlock(&meta_mtx);
		return r;
	}
	struct omdfs_entry *e = meta_lookup(&idx, name);
	if (!e) {
		meta_free_index(&idx);
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
	r = save_index(fuse_dir, &idx);
	meta_free_index(&idx);
	pthread_mutex_unlock(&meta_mtx);
	if (r == 0 && (dirty & OMDFS_F_DIRTY))
		dirtyset_add(fuse_dir);
	return r;
}
