/*
 * omdfs — in-memory set of directories that hold un-synced (dirty) entries.
 *
 * The dirty content/attr flags are per-entry and live in each directory's
 * .omdfs.dir index, so finding the dirty work means knowing which directories
 * to look in. This set records exactly those directories, fed from the metadata
 * layer whenever an entry gains a dirty flag, so the syncer flushes just the
 * dirty directories instead of walking the whole cached tree every cycle.
 *
 * It is a hint, not the source of truth: the on-disk flags remain authoritative.
 * On the first cycle (and after any internal allocation failure) the set asks the
 * syncer for a full tree scan, which both flushes everything and re-seeds the set
 * — so a crash-recovered mount, whose set starts empty, still drains correctly.
 *
 * Keys are FUSE directory paths ("/a/b"). All operations are internally locked.
 */
#ifndef OMDFS_DIRTYSET_H
#define OMDFS_DIRTYSET_H

#include <stddef.h>

/* Record that directory `fuse_dir` holds at least one dirty child entry.
 * Idempotent; safe to call from foreground and background threads. */
void dirtyset_add(const char *fuse_dir);

/* Re-key membership across a rename of `from` to `to`: any recorded directory
 * equal to `from` or nested under it ("from/...") is rewritten to the matching
 * path under `to`. A no-op when `from` names a file (no such keys exist). */
void dirtyset_rename(const char *from, const char *to);

/* Atomically remove and return every recorded dirty directory. The caller owns
 * the returned array and each string; release them with dirtyset_free. *n is the
 * count. *full is set non-zero when the syncer must do a full tree scan instead
 * (the initial run, or after an internal allocation failure may have dropped a
 * member); in that case the returned list is incomplete and should be ignored. */
char **dirtyset_take(size_t *n, int *full);

/* Free an array returned by dirtyset_take. */
void dirtyset_free(char **paths, size_t n);

#endif /* OMDFS_DIRTYSET_H */
