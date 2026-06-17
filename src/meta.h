/*
 * omdfs — per-directory metadata index (.omdfs.dir).
 *
 * The index is the source of truth for a directory's listing and the
 * attributes of its children. It is built on first access from the backend
 * (one readdir + a stat per child), then served from local disk so cached
 * directories cost zero per-file backend round-trips. It is local-only and is
 * never written to the backend.
 */
#ifndef OMDFS_META_H
#define OMDFS_META_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

enum omdfs_type {
	OMDFS_T_FILE = 1,
	OMDFS_T_DIR = 2,
	OMDFS_T_LINK = 3,
};

/* Per-entry cache-state flags (content/sync state arrives in later phases). */
enum omdfs_flags {
	OMDFS_F_CONTENT_CACHED = 1 << 0,
	OMDFS_F_DIRTY_CONTENT = 1 << 1,
	OMDFS_F_DIRTY_ATTR = 1 << 2,
	OMDFS_F_FETCHING = 1 << 3,
};

struct omdfs_entry {
	uint8_t type;  /* enum omdfs_type */
	uint8_t flags; /* enum omdfs_flags */
	struct stat st;
	char *name;
	char *link; /* symlink target, or NULL */
};

struct omdfs_index {
	struct stat dir_st; /* the directory's own attributes */
	size_t n;
	size_t cap;
	struct omdfs_entry *entries;
};

/* Load the index for `fuse_dir`, building (and persisting) it from the backend
 * if it is absent or unreadable. Returns 0 on success, -errno on failure. */
int meta_get_index(const char *fuse_dir, struct omdfs_index *out);

/* Load an existing index *without* building from the backend. Returns 0, or
 * -ENOENT/-EINVAL if there is no usable index on disk. Used by the syncer's
 * dirty-flush walk, which must only touch already-cached directories. */
int meta_try_load(const char *fuse_dir, struct omdfs_index *out);

/* Find a child by name, or NULL. */
struct omdfs_entry *meta_lookup(struct omdfs_index *idx, const char *name);

/* Atomically set `set_bits` and clear `clear_bits` in the flags of child `name`
 * in directory `fuse_dir`, persisting the index. Calls are serialized so
 * concurrent flag updates don't clobber each other. Returns 0 on success,
 * -ENOENT if the child is absent, or -errno on an I/O error. */
int meta_mark_flags(const char *fuse_dir, const char *name, uint8_t set_bits,
		    uint8_t clear_bits);

/* Mark every child of `fuse_dir` dirty so the background syncer re-pushes it:
 * OMDFS_F_DIRTY_ATTR on every entry, plus OMDFS_F_DIRTY_CONTENT on regular files
 * whose content is cached (symlinks carry no flushable state, only existence).
 * Persists the index once and records the directory in the dirty set. Local-only
 * — does no backend I/O. Each of files/dirs/links (any may be NULL) is
 * incremented by the count of that entry type seen. Returns 0, or -errno (the
 * index is left untouched on a save failure). */
int meta_mark_dir_dirty(const char *fuse_dir, long *files, long *dirs,
			long *links);

/* Create an empty index for a freshly-made directory, so it is immediately
 * listable from the local cache without ever consulting the backend (cache as
 * truth). Returns 0 or -errno. */
int meta_init_dir(const char *fuse_dir, const struct stat *dir_st);

/* Insert a new child into `fuse_dir`'s index (deep-copies name/link). Returns 0,
 * -EEXIST if a child of that name already exists, or -errno. */
int meta_add_entry(const char *fuse_dir, const struct omdfs_entry *e);

/* Remove child `name` from `fuse_dir`'s index. -ENOENT if absent. */
int meta_remove_entry(const char *fuse_dir, const char *name);

/* Move child `old_name` from `old_dir` to `new_dir` as `new_name`, carrying its
 * flags (so dirty state survives a rename). Overwrites an existing target.
 * -ENOENT if the source is absent. */
int meta_move_entry(const char *old_dir, const char *old_name,
		    const char *new_dir, const char *new_name);

/* Fields selectable by meta_set(). */
enum meta_field {
	META_MODE = 1 << 0,  /* permission bits only (type preserved) */
	META_UID = 1 << 1,
	META_GID = 1 << 2,
	META_SIZE = 1 << 3,  /* st_size (+ st_blocks) */
	META_TIMES = 1 << 4, /* st_atim + st_mtim */
};

/* Copy the masked stat fields from *src into child `name`, OR `dirty` into its
 * flags, and persist. -ENOENT if absent. */
int meta_set(const char *fuse_dir, const char *name, const struct stat *src,
	     unsigned fields, uint8_t dirty);

/* Drop the cached parsed index for `fuse_dir`, if any (next access reparses it
 * from disk). Call when a directory's on-disk index is removed (rmdir). */
void meta_forget(const char *fuse_dir);

/* Drop the cached parsed indexes for `prefix` and everything beneath it. Call
 * after renaming a directory: its cached subtree is now keyed by stale paths
 * (the on-disk cache subtree has moved). */
void meta_forget_tree(const char *prefix);

/* Cold-metadata-index eviction. Reclaim the on-disk `.omdfs.dir` of a cold
 * directory whose listing can be faithfully rebuilt from the backend: if
 * `fuse_dir` (never the root) has no un-synced (dirty) children and its cache
 * directory holds nothing but the index file, remove the index, drop the
 * in-memory copy, and rmdir the now-empty cache directory (so an ancestor can
 * collapse in turn). Returns 1 if the index was evicted, 0 if the directory was
 * kept (root, no on-disk index, a dirty child, or any cached content/subdir
 * present). The caller MUST ensure the backend is caught up (no pending
 * structural ops) so the rebuilt listing cannot reintroduce a stale entry. */
int meta_evict_cold(const char *fuse_dir);

/* Release an index loaded by meta_get_index(). */
void meta_free_index(struct omdfs_index *idx);

#endif /* OMDFS_META_H */
