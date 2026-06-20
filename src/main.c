/*
 * omdfs — Only Me Distributed File System
 *
 * Phase 4: read path from the local cache (metadata index + content), write-back
 * for mutations. Structural ops (create/mkdir/unlink/rmdir/rename/symlink) update
 * the local indexes and append an ordered structural WAL record; content/attr ops
 * (write/truncate/chmod/chown/utimens) update the cache and set per-entry dirty
 * flags. A background two-phase syncer pushes both to the backend; the WAL is
 * crash-recoverable and replayed on startup.
 *
 * Usage:
 *   omdfs --backend <dir> --datadir <dir> [fuse-opts] <mountpoint>
 */
#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include <fuse.h>

#include "content.h"
#include "dirtyset.h"
#include "evict.h"
#include "journal.h"
#include "meta.h"
#include "omdfs.h"
#include "status.h"
#include "syncer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct omdfs_config g_cfg;

/* ---- path helpers (declared in omdfs.h) ---- */

void backend_path(char out[PATH_MAX], const char *path)
{
	/* path always starts with '/'; g_cfg.backend has no trailing slash. */
	if (snprintf(out, PATH_MAX, "%s%s", g_cfg.backend, path) >= PATH_MAX)
		out[PATH_MAX - 1] = '\0';
}

void cache_path(char out[PATH_MAX], const char *path)
{
	if (path[0] == '/' && path[1] == '\0')
		snprintf(out, PATH_MAX, "%s/cache", g_cfg.datadir);
	else
		snprintf(out, PATH_MAX, "%s/cache%s", g_cfg.datadir, path);
}

int mkdirs(const char *path)
{
	char tmp[PATH_MAX];
	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return -1;
	for (char *p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* Split "/a/b/c" into parent ("/a/b") and name ("c"). Root must be handled by
 * the caller. parent and name must each be PATH_MAX bytes. */
void omdfs_split_path(const char *path, char *parent, char *name)
{
	const char *slash = strrchr(path, '/');
	size_t plen = (size_t)(slash - path);
	if (plen == 0) {
		parent[0] = '/';
		parent[1] = '\0';
	} else {
		memcpy(parent, path, plen);
		parent[plen] = '\0';
	}
	snprintf(name, PATH_MAX, "%s", slash + 1);
}

/* The metadata index name is reserved: users may not create/use it. */
static int is_reserved(const char *name)
{
	return !strcmp(name, OMDFS_INDEX_NAME);
}

/* Fill a fresh stat for a newly-created entry of the given mode, owned by the
 * caller (from the FUSE context), timestamped now. */
static void init_stat(struct stat *st, mode_t mode, off_t size)
{
	struct fuse_context *ctx = fuse_get_context();
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	memset(st, 0, sizeof(*st));
	st->st_mode = mode;
	st->st_nlink = S_ISDIR(mode) ? 2 : 1;
	st->st_uid = ctx ? ctx->uid : 0;
	st->st_gid = ctx ? ctx->gid : 0;
	st->st_size = size;
	st->st_blksize = 4096;
	st->st_blocks = (size + 511) / 512;
	st->st_atim = st->st_mtim = st->st_ctim = now;
}

/* ---- FUSE operations ---- */

static int omdfs_getattr(const char *path, struct stat *st,
			 struct fuse_file_info *fi)
{
	(void)fi;
	syncer_note_activity();

	if (path[0] == '/' && path[1] == '\0') {
		/* Serve the root's own attributes from the local index (its
		 * dir_st, same value readdir fills for "."). meta_dir_stat
		 * reads the cached index first and only falls back to the
		 * backend on a miss, so a cached root works with the backend
		 * gone — consistent with every other path. */
		return meta_dir_stat("/", st, NULL);
	}

	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);

	/* Single-entry read: copy just this child's stat, no whole-dir clone. */
	return meta_stat(parent, name, st, NULL, NULL);
}

static int omdfs_readlink(const char *path, char *buf, size_t size)
{
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);

	return meta_readlink(parent, name, buf, size);
}

static int omdfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi,
			 enum fuse_readdir_flags flags)
{
	(void)offset;
	(void)fi;
	(void)flags;
	syncer_note_activity();

	struct omdfs_index idx;
	int r = meta_get_index(path, &idx);
	if (r != 0)
		return r;

	if (filler(buf, ".", &idx.dir_st, 0, 0) != 0 ||
	    filler(buf, "..", NULL, 0, 0) != 0) {
		meta_free_index(&idx);
		return -ENOMEM;
	}
	for (size_t i = 0; i < idx.n; i++) {
		struct omdfs_entry *e = &idx.entries[i];
		if (filler(buf, e->name, &e->st, 0, 0) != 0) {
			meta_free_index(&idx);
			return -ENOMEM;
		}
	}
	meta_free_index(&idx);
	return 0;
}

static int omdfs_open(const char *path, struct fuse_file_info *fi)
{
	syncer_note_activity();
	struct content_handle *h;
	int r;
	if ((fi->flags & O_ACCMODE) == O_RDONLY) {
		r = content_open(path, &h);
	} else {
		r = content_open_rw(path, &h);
		if (r == 0 && (fi->flags & O_TRUNC))
			content_ftruncate(h, 0);
	}
	if (r != 0)
		return r;
	fi->fh = (uint64_t)(uintptr_t)h;
	return 0;
}

static int omdfs_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	(void)path;
	syncer_note_activity();
	struct content_handle *h = (struct content_handle *)(uintptr_t)fi->fh;
	ssize_t n = content_read(h, buf, size, offset);
	return (int)n; /* already bytes-read or -errno */
}

static int omdfs_write(const char *path, const char *buf, size_t size,
		       off_t offset, struct fuse_file_info *fi)
{
	(void)path;
	syncer_note_activity();
	struct content_handle *h = (struct content_handle *)(uintptr_t)fi->fh;
	ssize_t n = content_write(h, buf, size, offset);
	return (int)n;
}

static int omdfs_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	syncer_note_activity();
	content_close((struct content_handle *)(uintptr_t)fi->fh);
	syncer_kick(); /* push any just-closed dirty content */
	return 0;
}

static int omdfs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	(void)path;
	(void)datasync;
	syncer_note_activity();
	int r = content_fsync((struct content_handle *)(uintptr_t)fi->fh);
	if (r == 0)
		syncer_kick();
	return r;
}

/* ---- mutating ops: apply locally + queue a structural WAL record / dirty flag ---- */

static int omdfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	if (is_reserved(name))
		return -EPERM;

	struct content_handle *h;
	int r = content_create(path, mode, &h);
	if (r != 0)
		return r;

	/* Add the entry CLEAN (no dirty flags), so it is not flush-discoverable until
	 * we publish it below — after the WAL append and the pending_count bump. That
	 * ordering means the flush never sees the entry dirty-and-settled while its
	 * create is still undrained (DESIGN.md § 2026-06-19, Change 1). */
	struct omdfs_entry e;
	memset(&e, 0, sizeof(e));
	e.type = OMDFS_T_FILE;
	e.flags = OMDFS_F_CONTENT_CACHED;
	init_stat(&e.st, S_IFREG | (mode & 07777), 0);
	e.name = name;
	r = meta_add_entry(parent, &e);
	if (r != 0) {
		content_close(h);
		char cp[PATH_MAX];
		cache_path(cp, path);
		unlink(cp);
		return r;
	}
	uint64_t seq = wal_append(WAL_CREATE, path, NULL);
	meta_pending_add(parent, name, seq);
	/* Publish: arm the dirty flags + record the dir in the dirty-set. */
	meta_mark_flags(parent, name, OMDFS_F_DIRTY_CONTENT | OMDFS_F_DIRTY_ATTR, 0);
	syncer_kick();
	fi->fh = (uint64_t)(uintptr_t)h;
	return 0;
}

static int omdfs_mkdir(const char *path, mode_t mode)
{
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	if (is_reserved(name))
		return -EPERM;

	char cp[PATH_MAX];
	cache_path(cp, path);
	if (mkdirs(cp) != 0)
		return -errno;

	/* Add CLEAN, publish after the append + count bump (see omdfs_create). */
	struct omdfs_entry e;
	memset(&e, 0, sizeof(e));
	e.type = OMDFS_T_DIR;
	e.flags = 0;
	init_stat(&e.st, S_IFDIR | (mode & 07777), 0);
	/* Give the new dir an empty index so it is listable from cache alone. */
	meta_init_dir(path, &e.st);
	e.name = name;
	int r = meta_add_entry(parent, &e);
	if (r != 0)
		return r;
	uint64_t seq = wal_append(WAL_MKDIR, path, NULL);
	meta_pending_add(parent, name, seq);
	meta_mark_flags(parent, name, OMDFS_F_DIRTY_ATTR, 0); /* publish */
	syncer_kick();
	return 0;
}

static int omdfs_unlink(const char *path)
{
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);

	int r = meta_remove_entry(parent, name);
	if (r != 0)
		return r;
	char cp[PATH_MAX];
	cache_path(cp, path);
	unlink(cp); /* content may not be cached; ignore ENOENT */
	evict_forget(path);
	wal_append(WAL_UNLINK, path, NULL);
	syncer_kick();
	return 0;
}

static int omdfs_rmdir(const char *path)
{
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);

	/* Refuse a non-empty directory (the index is the source of truth). */
	size_t n;
	if (meta_dir_stat(path, NULL, &n) == 0 && n > 0)
		return -ENOTEMPTY;

	int r = meta_remove_entry(parent, name);
	if (r != 0)
		return r;
	/* Drain + drop the removed dir's own cached index FIRST: a pending async index
	 * write-back for it would otherwise resurrect the cache dir and index we are
	 * about to remove (write_index_buf mkdirs its target off the lock). meta_forget
	 * waits out any in-flight write before dropping the node, so the removal below
	 * can't race it. */
	meta_forget(path);
	char cp[PATH_MAX], ip[PATH_MAX];
	cache_path(cp, path);
	snprintf(ip, sizeof(ip), "%s/%s", cp, OMDFS_INDEX_NAME);
	unlink(ip);
	rmdir(cp); /* best-effort */
	wal_append(WAL_RMDIR, path, NULL);
	syncer_kick();
	return 0;
}

static int omdfs_rename(const char *from, const char *to, unsigned int flags)
{
	syncer_note_activity();
	if (flags) /* RENAME_EXCHANGE / RENAME_NOREPLACE unsupported */
		return -EINVAL;

	char fp[PATH_MAX], fn[PATH_MAX], tp[PATH_MAX], tn[PATH_MAX];
	omdfs_split_path(from, fp, fn);
	omdfs_split_path(to, tp, tn);
	if (is_reserved(fn) || is_reserved(tn))
		return -EPERM;

	/* For a file, make sure its content is cached so it lives at the new cache
	 * path and stays readable before the backend rename syncs. */
	uint8_t ftype = 0;
	int isfile = 0;
	if (meta_stat(fp, fn, NULL, &ftype, NULL) == 0)
		isfile = (ftype == OMDFS_T_FILE);
	if (isfile)
		content_prefetch(from); /* best-effort */

	/* Drain + drop the from-subtree's cached indexes BEFORE physically moving its
	 * cache directory below: a pending async index write-back keyed to an old path
	 * would otherwise recreate (resurrect) the source cache dir after the move
	 * (write_index_buf mkdirs its target off the lock). meta_forget_tree waits out
	 * any in-flight write, then drops the stale-keyed nodes so they reparse from the
	 * moved files on next access. (A no-op for a file rename.) */
	meta_forget_tree(from);

	char cfrom[PATH_MAX], cto[PATH_MAX], ctodir[PATH_MAX];
	cache_path(cfrom, from);
	cache_path(cto, to);
	cache_path(ctodir, tp);
	mkdirs(ctodir);
	/* Uncached file -> no cache file to move (ENOENT is fine). A cold-metadata
	 * eviction may also have rmdir'd the destination cache dir between mkdirs and
	 * here; on ENOENT recreate it once and retry so a cached file lands at its new
	 * path. (meta_move_entry's save_index recreates the dir for metadata anyway.) */
	if (rename(cfrom, cto) != 0 && errno == ENOENT) {
		mkdirs(ctodir);
		(void)rename(cfrom, cto);
	}

	int dst_dirty = 0;
	struct ind *moved_ind = NULL;
	int r = meta_move_entry(fp, fn, tp, tn, &dst_dirty, &moved_ind);
	if (r != 0)
		return r;
	evict_rename(from, to);
	uint64_t seq = wal_append(WAL_RENAME, from, to);
	/* Commit the rename's pending_count (pre-bumped under the move's lock) against
	 * the assigned seq, then publish — so the flush never sees the moved entry
	 * dirty-and-settled while its rename is still undrained (DESIGN.md Change 1). */
	meta_pending_commit(moved_ind, seq);
	if (dst_dirty)
		dirtyset_add(tp);
	/* A renamed directory carries its dirty descendants to new paths; re-key
	 * their dirty-set membership so the syncer still finds them. */
	dirtyset_rename(from, to);
	/* Path-based dirty-set re-keying races with the syncer's own re-arm of stale
	 * paths (a cycle that took the set, then saturated/failed and re-added the
	 * pre-rename path): the moved subtree's dirty dirs can be lost, leaving dirty
	 * content that the targeted flush never revisits (only a full --resync would).
	 * For a directory rename (which moves whole subtrees), force the next flush to
	 * do a full tree walk so every dirty entry is found at its current path
	 * regardless of those races. A file rename can't strand a subtree. */
	if (!isfile)
		dirtyset_force_full();
	syncer_kick();
	return 0;
}

static int omdfs_symlink(const char *target, const char *path)
{
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	if (is_reserved(name))
		return -EPERM;

	char cp[PATH_MAX], cdir[PATH_MAX];
	cache_path(cp, path);
	cache_path(cdir, parent);
	mkdirs(cdir);
	if (symlink(target, cp) != 0) {
		/* A cold-metadata eviction may have rmdir'd the parent cache dir
		 * between mkdirs and here; recreate it once and retry. */
		if (errno != ENOENT || mkdirs(cdir) != 0 ||
		    symlink(target, cp) != 0)
			return -errno;
	}

	struct omdfs_entry e;
	memset(&e, 0, sizeof(e));
	e.type = OMDFS_T_LINK;
	init_stat(&e.st, S_IFLNK | 0777, (off_t)strlen(target));
	e.name = name;
	e.link = (char *)target;
	int r = meta_add_entry(parent, &e);
	if (r != 0) {
		unlink(cp);
		return r;
	}
	wal_append(WAL_SYMLINK, path, target);
	syncer_kick();
	return 0;
}

static int omdfs_truncate(const char *path, off_t size,
			  struct fuse_file_info *fi)
{
	(void)fi;
	syncer_note_activity();
	int r = content_truncate(path, size);
	if (r == 0)
		syncer_kick();
	return r;
}

static int omdfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)fi;
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	struct stat st;
	st.st_mode = mode;
	int r = meta_set(parent, name, &st, META_MODE, OMDFS_F_DIRTY_ATTR);
	if (r == 0)
		syncer_kick();
	return r;
}

static int omdfs_chown(const char *path, uid_t uid, gid_t gid,
		       struct fuse_file_info *fi)
{
	(void)fi;
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	struct stat st;
	st.st_uid = uid;
	st.st_gid = gid;
	int r = meta_set(parent, name, &st, META_UID | META_GID,
			 OMDFS_F_DIRTY_ATTR);
	if (r == 0)
		syncer_kick();
	return r;
}

static int omdfs_utimens(const char *path, const struct timespec tv[2],
			 struct fuse_file_info *fi)
{
	(void)fi;
	syncer_note_activity();
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	struct stat st;
	st.st_atim = tv[0];
	st.st_mtim = tv[1];
	int r = meta_set(parent, name, &st, META_TIMES, OMDFS_F_DIRTY_ATTR);
	if (r == 0)
		syncer_kick();
	return r;
}

static const struct fuse_operations omdfs_ops = {
	.getattr = omdfs_getattr,
	.readlink = omdfs_readlink,
	.readdir = omdfs_readdir,
	.open = omdfs_open,
	.read = omdfs_read,
	.write = omdfs_write,
	.release = omdfs_release,
	.fsync = omdfs_fsync,
	.create = omdfs_create,
	.mkdir = omdfs_mkdir,
	.unlink = omdfs_unlink,
	.rmdir = omdfs_rmdir,
	.rename = omdfs_rename,
	.symlink = omdfs_symlink,
	.truncate = omdfs_truncate,
	.chmod = omdfs_chmod,
	.chown = omdfs_chown,
	.utimens = omdfs_utimens,
};

/* ---- startup ---- */

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--config <file>] --backend <dir> --datadir <dir> [--cache-size <bytes>] [fuse-opts] <mountpoint>\n"
		"       %s --resync [--config <file>] --backend <dir> --datadir <dir>\n"
		"       %s --mark-dirty [--config <file>] --backend <dir> --datadir <dir>\n"
		"\n"
		"  --config <file>     read backend/datadir/cache-size from a key=value file\n"
		"  --backend <dir>     already-mounted network FS to cache (sshfs/nfs/...)\n"
		"  --datadir <dir>     local cache/journal/state directory\n"
		"  --cache-size <sz>   cache budget; accepts K/M/G suffix (0 = unlimited)\n"
		"  --no-flush          start with the dirty content/attr flush paused\n"
		"                      (creates <datadir>/state/flush-off). Structural ops\n"
		"                      still sync. Resume with: rm <datadir>/state/flush-off;\n"
		"                      pause a running mount with: touch <datadir>/state/flush-off\n"
		"  --resync            push the whole cache (content + metadata) to the\n"
		"                      backend so it matches the cache, then exit; no mount.\n"
		"                      Run while unmounted. Reports a summary on stderr.\n"
		"  --mark-dirty        flag the whole cache dirty (local-only) so the next\n"
		"                      mount's background syncer re-pushes it incrementally,\n"
		"                      then exit; no mount and no backend push here. Use\n"
		"                      instead of --resync to avoid waiting for the push.\n"
		"  <mountpoint>        where omdfs is exposed\n"
		"\n"
		"Command-line options override values from --config. While mounted, the\n"
		"daemon publishes sync progress to <datadir>/state/status.\n",
		prog, prog, prog);
}

/* Parse a size like "100", "64K", "512M", "2G" (1024-based). Returns the byte
 * count, or -1 on a malformed value. */
static off_t parse_size(const char *s)
{
	char *end;
	errno = 0;
	long long v = strtoll(s, &end, 10);
	if (errno != 0 || end == s || v < 0)
		return -1;
	off_t mult = 1;
	switch (*end) {
	case 'g': case 'G': mult = 1024LL * 1024 * 1024; end++; break;
	case 'm': case 'M': mult = 1024LL * 1024; end++; break;
	case 'k': case 'K': mult = 1024LL; end++; break;
	case '\0': break;
	default: return -1;
	}
	if (*end != '\0')
		return -1;
	return (off_t)v * mult;
}

/* Parse a boolean config value. Returns 1/0, or -1 if unrecognized. */
static int parse_bool(const char *s)
{
	if (!strcmp(s, "1") || !strcmp(s, "true") || !strcmp(s, "yes") ||
	    !strcmp(s, "on"))
		return 1;
	if (!strcmp(s, "0") || !strcmp(s, "false") || !strcmp(s, "no") ||
	    !strcmp(s, "off"))
		return 0;
	return -1;
}

/* Trim leading/trailing ASCII whitespace in place; returns the first non-blank. */
static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
			   end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';
	return s;
}

/* Read a key=value config file. Recognized keys: backend, datadir, cache-size
 * (cache_size accepted too). Blank lines and lines starting with '#' are ignored.
 * Values are applied to g_cfg only where not already set, so a later CLI parse
 * overrides the file. Returns 0 or -1 (open/parse error). */
static int load_config_file(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "omdfs: cannot open config %s: %s\n", path,
			strerror(errno));
		return -1;
	}
	char line[PATH_MAX + 64];
	int lineno = 0, rc = 0;
	while (fgets(line, sizeof(line), fp)) {
		lineno++;
		char *s = trim(line);
		if (*s == '\0' || *s == '#')
			continue;
		char *eq = strchr(s, '=');
		if (!eq) {
			fprintf(stderr, "omdfs: %s:%d: expected key=value\n",
				path, lineno);
			rc = -1;
			break;
		}
		*eq = '\0';
		char *key = trim(s);
		char *val = trim(eq + 1);

		if (!strcmp(key, "backend")) {
			g_cfg.backend = strdup(val);
		} else if (!strcmp(key, "datadir")) {
			g_cfg.datadir = strdup(val);
		} else if (!strcmp(key, "cache-size") ||
			   !strcmp(key, "cache_size")) {
			off_t sz = parse_size(val);
			if (sz < 0) {
				fprintf(stderr, "omdfs: %s:%d: bad cache-size: %s\n",
					path, lineno, val);
				rc = -1;
				break;
			}
			g_cfg.cache_budget = sz;
		} else if (!strcmp(key, "no-flush") || !strcmp(key, "no_flush")) {
			int b = parse_bool(val);
			if (b < 0) {
				fprintf(stderr, "omdfs: %s:%d: bad no-flush: %s\n",
					path, lineno, val);
				rc = -1;
				break;
			}
			g_cfg.no_flush = b;
		} else {
			fprintf(stderr, "omdfs: %s:%d: unknown key: %s\n", path,
				lineno, key);
			rc = -1;
			break;
		}
	}
	fclose(fp);
	return rc;
}

static void rstrip_slash(char *s)
{
	size_t n = strlen(s);
	while (n > 1 && s[n - 1] == '/')
		s[--n] = '\0';
}

static int init_datadir(const char *dir)
{
	char sub[PATH_MAX];
	const char *names[] = { "", "cache", "journal", "state" };
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (names[i][0] == '\0')
			snprintf(sub, sizeof(sub), "%s", dir);
		else
			snprintf(sub, sizeof(sub), "%s/%s", dir, names[i]);
		if (mkdir(sub, 0700) == -1 && errno != EEXIST) {
			fprintf(stderr, "omdfs: cannot create %s: %s\n", sub,
				strerror(errno));
			return -1;
		}
	}
	return 0;
}

static int parse_args(int argc, char **argv, int *out_argc, char **out_argv)
{
	int oc = 0;
	out_argv[oc++] = argv[0];
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--config") && i + 1 < argc)
			i++; /* already loaded in main() before this parse */
		else if (!strcmp(argv[i], "--backend") && i + 1 < argc)
			g_cfg.backend = argv[++i];
		else if (!strcmp(argv[i], "--datadir") && i + 1 < argc)
			g_cfg.datadir = argv[++i];
		else if (!strcmp(argv[i], "--cache-size") && i + 1 < argc) {
			off_t sz = parse_size(argv[++i]);
			if (sz < 0) {
				fprintf(stderr, "omdfs: bad --cache-size: %s\n",
					argv[i]);
				return -1;
			}
			g_cfg.cache_budget = sz;
		} else if (!strcmp(argv[i], "--resync"))
			g_cfg.resync = 1;
		else if (!strcmp(argv[i], "--mark-dirty"))
			g_cfg.mark_dirty = 1;
		else if (!strcmp(argv[i], "--no-flush"))
			g_cfg.no_flush = 1;
		else
			out_argv[oc++] = argv[i];
	}
	out_argv[oc] = NULL;
	*out_argc = oc;
	return (g_cfg.backend && g_cfg.datadir) ? 0 : -1;
}

int main(int argc, char **argv)
{
	char **fuse_argv = calloc(argc + 1, sizeof(*fuse_argv));
	int fuse_argc = 0;

	if (!fuse_argv) {
		fprintf(stderr, "omdfs: out of memory\n");
		return 1;
	}
	/* Load --config first so command-line options below override it. */
	for (int i = 1; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "--config")) {
			if (load_config_file(argv[i + 1]) != 0) {
				free(fuse_argv);
				return 2;
			}
			break;
		}
	}
	if (parse_args(argc, argv, &fuse_argc, fuse_argv) != 0) {
		usage(argv[0]);
		free(fuse_argv);
		return 2;
	}

	rstrip_slash(g_cfg.backend);
	rstrip_slash(g_cfg.datadir);

	struct stat st;
	if (stat(g_cfg.backend, &st) == -1 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "omdfs: backend is not a directory: %s\n",
			g_cfg.backend);
		free(fuse_argv);
		return 1;
	}
	if (init_datadir(g_cfg.datadir) != 0) {
		free(fuse_argv);
		return 1;
	}
	if (wal_init() != 0) {
		fprintf(stderr, "omdfs: cannot initialize journal\n");
		free(fuse_argv);
		return 1;
	}
	/* --resync: one-shot reconcile of the cache onto the backend, then exit.
	 * Run while unmounted so the live syncer isn't writing the backend too. */
	if (g_cfg.resync) {
		int rc = syncer_resync();
		free(fuse_argv);
		return rc;
	}
	/* --mark-dirty: flag the whole cache dirty (local-only) so the next mount's
	 * background syncer re-pushes it incrementally; exit without mounting. */
	if (g_cfg.mark_dirty) {
		int rc = syncer_mark_dirty();
		free(fuse_argv);
		return rc;
	}
	/* --no-flush: start with the dirty content/attr flush paused by pre-creating
	 * the state/flush-off control file. The syncer checks this file every cycle;
	 * the operator resumes flushing at any time with `rm <datadir>/state/flush-off`.
	 * (The file persists across mounts — an explicit operator intent to hold back
	 * backend writes survives a remount until it is removed.) */
	if (g_cfg.no_flush) {
		char fp[PATH_MAX];
		snprintf(fp, sizeof(fp), "%s/state/flush-off", g_cfg.datadir);
		int fd = open(fp, O_WRONLY | O_CREAT, 0644);
		if (fd < 0) {
			fprintf(stderr, "omdfs: cannot create %s: %s\n", fp,
				strerror(errno));
			free(fuse_argv);
			return 1;
		}
		close(fd);
	}
	/* Seed the cache-size accounting from content already on disk and arm the
	 * evictor (no-op when --cache-size is 0/unset). */
	evict_init(g_cfg.cache_budget);
	/* Publish an initial status file so an operator sees the mount come up even
	 * before the first sync cycle. */
	status_init();
	/* Start the async index-writer thread before the syncer (which clears dirty
	 * flags) or any FUSE op, so index persistence happens off the metadata lock. */
	if (meta_init() != 0) {
		fprintf(stderr, "omdfs: cannot start index writer\n");
		free(fuse_argv);
		return 1;
	}
	/* The syncer drains any WAL/dirty state left by a previous run (recovery)
	 * and then keeps the backend in sync while we serve the mount. */
	if (syncer_start() != 0) {
		fprintf(stderr, "omdfs: cannot start syncer\n");
		meta_shutdown();
		free(fuse_argv);
		return 1;
	}

	int ret = fuse_main(fuse_argc, fuse_argv, &omdfs_ops, NULL);

	syncer_stop();   /* final drain so pending mutations reach the backend */
	meta_shutdown(); /* then flush all pending index writes (after the above) */
	free(fuse_argv);
	return ret;
}
