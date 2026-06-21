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

/* A structural op, paths heap-allocated (slim — not embedded PATH_MAX buffers, so the
 * un-applied tail costs path-length per op). Produced by wal_append (ownership to the
 * caller) and wal_load_pending; the syncer's drain queue points straight at it, then
 * frees it with wal_record_free once the op has drained. `path2` is NULL when absent. */
struct wal_record {
	uint8_t op;
	char *path;
	char *path2;
};

/* Free a record from wal_append / wal_load_pending (frees its paths). NULL-safe. */
void wal_record_free(struct wal_record *rec);

/* Open/create the WAL under DATADIR/journal, load the checkpoint (the count of
 * already-applied records), and recover the record count by scanning the log. The
 * checkpoint is clamped to the records actually on disk — an interrupted reclaim
 * leaves it stale-high — and rewritten durably when clamped. Returns 0 or -errno. */
int wal_init(void);

/* Append a structural op (path2 may be NULL) to the on-disk WAL and return a heap
 * record carrying the op + its paths (ownership to the caller — the syncer enqueues it
 * for draining and frees it with wal_record_free). The record carries no sequence
 * number; its position in the append-only file IS its identity and apply order.
 * journal.c retains nothing in memory beyond the counters. NULL on error (the op
 * simply won't sync this mount; it stays on disk and re-seeds next mount). */
struct wal_record *wal_append(uint8_t op, const char *path, const char *path2);

/* Load every valid record from index `after_count` onward (the un-applied suffix) as
 * individually-allocated heap records into a freshly-malloc'd pointer array — the
 * caller takes ownership of each record (free with wal_record_free) and frees the
 * array. Stops at the first torn/invalid record. Returns 0 or -errno; *out may be NULL
 * with *n == 0 if nothing is pending. */
int wal_load_pending(uint64_t after_count, struct wal_record ***out, size_t *n);

/* The count of WAL records already applied to the backend (the checkpoint) — i.e.
 * the 0-based index of the next un-applied record. */
uint64_t wal_applied(void);

/* The count of records currently in the WAL (appended since the last reclaim). */
uint64_t wal_total(void);

/* True if a structural RENAME has been appended but not yet applied to the backend
 * (the last appended rename's index is at or beyond the applied count). A rename is
 * the only structural op that relocates an existing cached file, so while one is
 * pending the backend namespace may not match the cache and a clean file's content
 * may still sit at its OLD backend path. Eviction consults this so it never drops a
 * clean file's cache copy whose backend copy isn't yet at the file's current path
 * (which would make a refetch fail ENOENT). Cheap: an in-memory index compare. */
int wal_has_pending_rename(void);

/* True if every appended structural op has reached the backend (applied == total) —
 * i.e. the backend namespace currently matches the cache, with no pending mkdir/
 * create/unlink/rmdir/rename/symlink. Cold-metadata eviction consults this live (per
 * candidate) so it never reclaims a directory while any structural op is undrained: a
 * fresh dir whose own mkdir is still pending isn't yet on the backend (rebuilding its
 * index would ENOENT), and a dir with a pending child unlink/rename would resurrect
 * that child on rebuild from the stale backend. Cheap: an in-memory count compare. */
int wal_fully_drained(void);

/* Persist a new applied-count checkpoint (atomic write + fsync). At full drain
 * (applied >= total) the WAL is reclaimed: the file is truncated to empty FIRST, then
 * the checkpoint is reset to 0 — so a crash in the window leaves {empty WAL, stale
 * checkpoint}, which recovery clamps to 0 (the only state where checkpoint > records,
 * provably "all applied"); the reverse order could leave {full WAL, checkpoint 0} and
 * replay the whole applied prefix. Returns 0 or -errno. */
int wal_checkpoint(uint64_t applied);

#endif /* OMDFS_JOURNAL_H */
