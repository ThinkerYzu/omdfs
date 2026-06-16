/*
 * omdfs — in-memory LRU index of cached content files.
 *
 * A pure data structure: it tracks, per cached content file, its last-access
 * recency, its accounted size in bytes, and an "open" pin count. It owns the
 * running total of accounted content bytes and the LRU ordering, but it knows
 * nothing about watermarks, the metadata index, or deleting files — that policy
 * lives in evict.c. All operations are internally locked, so callers may use it
 * from the foreground and from background threads concurrently.
 *
 * Keys are FUSE paths ("/a/b/c"). Sizes are the content bytes the file occupies
 * in the cache tree.
 */
#ifndef OMDFS_LRU_H
#define OMDFS_LRU_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

/* Record/refresh `path` as most-recently-used with `size` accounted bytes
 * (inserts a node if absent, else updates its size and bumps it to MRU). The
 * running total is adjusted by the signed change in size. */
void lru_touch(const char *path, off_t size);

/* Adjust `path`'s open pin count by `delta` (+1 on open, -1 on close). A node
 * with a positive pin count is never returned as an eviction candidate. A touch
 * is implied if the node is absent (size 0), so an open always pins something. */
void lru_pin(const char *path, int delta);

/* Whether `path` is currently pinned (open count > 0). */
int lru_is_pinned(const char *path);

/* Forget `path` entirely; returns its accounted size (0 if it was absent). The
 * running total is reduced by that size. */
off_t lru_forget(const char *path);

/* Re-key `old_path` -> `new_path`, preserving size, recency, and pin count. If a
 * node already exists at `new_path` it is forgotten first (overwrite). */
void lru_rename(const char *old_path, const char *new_path);

/* Total accounted content bytes across all tracked files. */
off_t lru_total(void);

/* Snapshot up to `cap` least-recently-used, currently-UNPINNED paths into `out`
 * (LRU first). Returns the number written. A point-in-time view: the caller must
 * re-check pin/dirty state before acting on each entry. */
size_t lru_snapshot_lru(char out[][PATH_MAX], size_t cap);

#endif /* OMDFS_LRU_H */
