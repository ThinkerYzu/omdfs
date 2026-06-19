/*
 * omdfs — structural write-ahead log (WAL).
 *
 * The WAL records *only* structural namespace ops (create/mkdir/unlink/rmdir/
 * rename/symlink) in foreground execution order; content/attribute changes ride
 * as dirty flags on the .omdfs.dir entry instead (DESIGN Decision 4). Each record
 * is length-prefixed and CRC32-checksummed, so a torn trailing record from a
 * crash is detected and discarded on recovery — the log is "always recoverable,"
 * never corrupt (SPEC F10a).
 */
#ifndef OMDFS_JOURNAL_H
#define OMDFS_JOURNAL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

enum wal_op {
	WAL_CREATE = 1, /* path = new file */
	WAL_MKDIR = 2,	/* path = new dir */
	WAL_UNLINK = 3, /* path = removed file */
	WAL_RMDIR = 4,	/* path = removed dir */
	WAL_RENAME = 5, /* path = old, path2 = new */
	WAL_SYMLINK = 6 /* path = link, path2 = target */
};

struct wal_record {
	uint64_t seq;
	uint8_t op;
	char path[PATH_MAX];
	char path2[PATH_MAX];
};

/* Open/create the WAL under DATADIR/journal, load the checkpoint, and recover the
 * next sequence number by scanning the log. Returns 0 or -errno. */
int wal_init(void);

/* Append a structural op (path2 may be NULL). Returns the assigned seq, or 0 on
 * error. */
uint64_t wal_append(uint8_t op, const char *path, const char *path2);

/* Load every valid record with seq > after_seq into a freshly-malloc'd array
 * (caller frees). Stops at the first torn/invalid record. Returns 0 or -errno;
 * *out may be NULL with *n == 0 if there is nothing pending. */
int wal_load_pending(uint64_t after_seq, struct wal_record **out, size_t *n);

/* The highest seq known to be applied to the backend (from the checkpoint). */
uint64_t wal_synced_seq(void);

/* True if a structural RENAME has been appended but not yet applied to the backend
 * (the last appended rename's seq is beyond the synced checkpoint). A rename is the
 * only structural op that relocates an existing cached file, so while one is pending
 * the backend namespace may not match the cache and a clean file's content may still
 * sit at its OLD backend path. Eviction consults this so it never drops a clean
 * file's cache copy whose backend copy isn't yet at the file's current path (which
 * would make a refetch fail ENOENT). Cheap: an in-memory seq compare. */
int wal_has_pending_rename(void);

/* Persist a new synced watermark (atomic checkpoint write + fsync). If the log is
 * fully drained (synced == last appended), the WAL file is truncated. */
int wal_checkpoint(uint64_t synced_seq);

#endif /* OMDFS_JOURNAL_H */
