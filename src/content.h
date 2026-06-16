/*
 * omdfs — content cache + progressive read.
 *
 * Whole-file caching: on a read miss the entire file is fetched from the backend
 * into its mirrored cache path by a background thread, while reads of the
 * already-fetched prefix return immediately (a "fetched-length" watermark gates
 * each read). Once a file is fully fetched its parent index entry is marked
 * CONTENT_CACHED, so subsequent opens are served from local disk with no backend
 * access at all. No eviction yet (Phase 5).
 */
#ifndef OMDFS_CONTENT_H
#define OMDFS_CONTENT_H

#include <sys/types.h>

struct content_handle;

/* Open `fuse_path` for reading. On a warm hit the handle reads straight from the
 * cache file; on a miss this starts (or joins) a background whole-file fetch.
 * Returns 0 and sets *out on success, or -errno. */
int content_open(const char *fuse_path, struct content_handle **out);

/* Read [off, off+len) from the (possibly still-fetching) content. Blocks only
 * until the fetched-length watermark reaches off+len (or the fetch finishes).
 * Returns the number of bytes read, or -errno (including a failed fetch). */
ssize_t content_read(struct content_handle *h, char *buf, size_t len, off_t off);

/* Create a brand-new, empty cache file and return a writable handle. The caller
 * adds the parent index entry + WAL record. Returns 0 or -errno. */
int content_create(const char *fuse_path, mode_t mode, struct content_handle **out);

/* Open an existing file for writing. Blocks until the whole file is in cache
 * (we modify a complete local copy), then returns a writable handle. */
int content_open_rw(const char *fuse_path, struct content_handle **out);

/* Write [off, off+len) into the cache file. The on-disk size/mtime and the
 * DIRTY_CONTENT flag are updated at fsync/close, not per write. Returns bytes
 * written or -errno. */
ssize_t content_write(struct content_handle *h, const void *buf, size_t len,
		      off_t off);

/* Flush a writable handle: fsync the cache file and update its index entry
 * (size/mtime + DIRTY_CONTENT). No-op for a read handle. Returns 0 or -errno. */
int content_fsync(struct content_handle *h);

/* Truncate the cache file behind a writable handle (for open-with-O_TRUNC). */
int content_ftruncate(struct content_handle *h, off_t size);

/* Truncate a file by path: fetch it if needed, set its size, mark it dirty. */
int content_truncate(const char *fuse_path, off_t size);

/* Ensure a file's whole content is in the cache (best-effort prefetch, e.g. so a
 * rename keeps it readable at the new path before the backend rename syncs).
 * Returns 0 or -errno. */
int content_prefetch(const char *fuse_path);

/* Release a handle. For a writable handle this also updates the index entry
 * (size/mtime + DIRTY_CONTENT) so the syncer will flush it. */
void content_close(struct content_handle *h);

/* Whether a background fetch into `fuse_path` is currently in flight. Used by
 * the evictor so it never deletes a cache file that is mid-fetch. */
int content_is_fetching(const char *fuse_path);

#endif /* OMDFS_CONTENT_H */
