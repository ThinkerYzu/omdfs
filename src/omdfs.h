/*
 * omdfs — shared configuration and path helpers.
 */
#ifndef OMDFS_H
#define OMDFS_H

#include <limits.h>
#include <sys/types.h>

/* Reserved local-only per-directory metadata index file name. */
#define OMDFS_INDEX_NAME ".omdfs.dir"

struct omdfs_config {
	char *backend; /* BACKEND_ROOT — the network mount (no trailing slash) */
	char *datadir; /* DATADIR — cache + journal + state (no trailing slash) */
	off_t cache_budget; /* cache size limit in bytes; 0 = unlimited (no eviction) */
	int resync; /* --resync: reconcile cache → backend, then exit (no mount) */
	int mark_dirty; /* --mark-dirty: flag whole cache dirty, then exit (no mount) */
};

extern struct omdfs_config g_cfg;

/* Map a FUSE path ("/a/b") to the backend path (BACKEND_ROOT "/a/b"). */
void backend_path(char out[PATH_MAX], const char *path);

/* Map a FUSE path to its mirrored cache path (DATADIR/cache "/a/b"). Root
 * ("/") maps to DATADIR/cache with no trailing slash. */
void cache_path(char out[PATH_MAX], const char *path);

/* Create all directory components of `path` (like `mkdir -p`). */
int mkdirs(const char *path);

/* Split a FUSE path "/a/b/c" into its parent dir ("/a/b") and final component
 * ("c"). The caller must handle root ("/") itself. `parent` and `name` must each
 * be PATH_MAX bytes. */
void omdfs_split_path(const char *path, char *parent, char *name);

#endif /* OMDFS_H */
