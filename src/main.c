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

	if (path[0] == '/' && path[1] == '\0') {
		/* Serve the root's own attributes from the local index (its
		 * dir_st, same value readdir fills for "."). meta_get_index
		 * loads the persisted .omdfs.dir first and only falls back to
		 * the backend on a miss, so a cached root works with the
		 * backend gone — consistent with every other path. */
		struct omdfs_index idx;
		int r = meta_get_index("/", &idx);
		if (r != 0)
			return r;
		*st = idx.dir_st;
		meta_free_index(&idx);
		return 0;
	}

	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);

	struct omdfs_index idx;
	int r = meta_get_index(parent, &idx);
	if (r != 0)
		return r;

	struct omdfs_entry *e = meta_lookup(&idx, name);
	if (!e) {
		meta_free_index(&idx);
		return -ENOENT;
	}
	*st = e->st;
	meta_free_index(&idx);
	return 0;
}

static int omdfs_readlink(const char *path, char *buf, size_t size)
{
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);

	struct omdfs_index idx;
	int r = meta_get_index(parent, &idx);
	if (r != 0)
		return r;

	struct omdfs_entry *e = meta_lookup(&idx, name);
	if (!e || e->type != OMDFS_T_LINK || !e->link) {
		meta_free_index(&idx);
		return e ? -EINVAL : -ENOENT;
	}
	snprintf(buf, size, "%s", e->link);
	meta_free_index(&idx);
	return 0;
}

static int omdfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi,
			 enum fuse_readdir_flags flags)
{
	(void)offset;
	(void)fi;
	(void)flags;

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
	struct content_handle *h = (struct content_handle *)(uintptr_t)fi->fh;
	ssize_t n = content_read(h, buf, size, offset);
	return (int)n; /* already bytes-read or -errno */
}

static int omdfs_write(const char *path, const char *buf, size_t size,
		       off_t offset, struct fuse_file_info *fi)
{
	(void)path;
	struct content_handle *h = (struct content_handle *)(uintptr_t)fi->fh;
	ssize_t n = content_write(h, buf, size, offset);
	return (int)n;
}

static int omdfs_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	content_close((struct content_handle *)(uintptr_t)fi->fh);
	syncer_kick(); /* push any just-closed dirty content */
	return 0;
}

static int omdfs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	(void)path;
	(void)datasync;
	int r = content_fsync((struct content_handle *)(uintptr_t)fi->fh);
	if (r == 0)
		syncer_kick();
	return r;
}

/* ---- mutating ops: apply locally + queue a structural WAL record / dirty flag ---- */

static int omdfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	if (is_reserved(name))
		return -EPERM;

	struct content_handle *h;
	int r = content_create(path, mode, &h);
	if (r != 0)
		return r;

	struct omdfs_entry e;
	memset(&e, 0, sizeof(e));
	e.type = OMDFS_T_FILE;
	e.flags = OMDFS_F_CONTENT_CACHED | OMDFS_F_DIRTY_CONTENT |
		  OMDFS_F_DIRTY_ATTR;
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
	wal_append(WAL_CREATE, path, NULL);
	syncer_kick();
	fi->fh = (uint64_t)(uintptr_t)h;
	return 0;
}

static int omdfs_mkdir(const char *path, mode_t mode)
{
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	if (is_reserved(name))
		return -EPERM;

	char cp[PATH_MAX];
	cache_path(cp, path);
	if (mkdirs(cp) != 0)
		return -errno;

	struct omdfs_entry e;
	memset(&e, 0, sizeof(e));
	e.type = OMDFS_T_DIR;
	e.flags = OMDFS_F_DIRTY_ATTR;
	init_stat(&e.st, S_IFDIR | (mode & 07777), 0);
	/* Give the new dir an empty index so it is listable from cache alone. */
	meta_init_dir(path, &e.st);
	e.name = name;
	int r = meta_add_entry(parent, &e);
	if (r != 0)
		return r;
	wal_append(WAL_MKDIR, path, NULL);
	syncer_kick();
	return 0;
}

static int omdfs_unlink(const char *path)
{
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
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);

	/* Refuse a non-empty directory (the index is the source of truth). */
	struct omdfs_index idx;
	if (meta_get_index(path, &idx) == 0) {
		size_t n = idx.n;
		meta_free_index(&idx);
		if (n > 0)
			return -ENOTEMPTY;
	}

	int r = meta_remove_entry(parent, name);
	if (r != 0)
		return r;
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
	if (flags) /* RENAME_EXCHANGE / RENAME_NOREPLACE unsupported */
		return -EINVAL;

	char fp[PATH_MAX], fn[PATH_MAX], tp[PATH_MAX], tn[PATH_MAX];
	omdfs_split_path(from, fp, fn);
	omdfs_split_path(to, tp, tn);
	if (is_reserved(fn) || is_reserved(tn))
		return -EPERM;

	/* For a file, make sure its content is cached so it lives at the new cache
	 * path and stays readable before the backend rename syncs. */
	struct omdfs_index idx;
	int isfile = 0;
	if (meta_get_index(fp, &idx) == 0) {
		struct omdfs_entry *e = meta_lookup(&idx, fn);
		if (e)
			isfile = (e->type == OMDFS_T_FILE);
		meta_free_index(&idx);
	}
	if (isfile)
		content_prefetch(from); /* best-effort */

	char cfrom[PATH_MAX], cto[PATH_MAX], ctodir[PATH_MAX];
	cache_path(cfrom, from);
	cache_path(cto, to);
	cache_path(ctodir, tp);
	mkdirs(ctodir);
	/* Uncached file -> no cache file to move (ENOENT is fine). */
	(void)rename(cfrom, cto);

	int r = meta_move_entry(fp, fn, tp, tn);
	if (r != 0)
		return r;
	/* A renamed directory carries its dirty descendants to new paths; re-key
	 * their dirty-set membership so the syncer still finds them. (meta_move_entry
	 * already re-flags the destination parent if the moved entry itself is dirty.) */
	dirtyset_rename(from, to);
	evict_rename(from, to);
	wal_append(WAL_RENAME, from, to);
	syncer_kick();
	return 0;
}

static int omdfs_symlink(const char *target, const char *path)
{
	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);
	if (is_reserved(name))
		return -EPERM;

	char cp[PATH_MAX], cdir[PATH_MAX];
	cache_path(cp, path);
	cache_path(cdir, parent);
	mkdirs(cdir);
	if (symlink(target, cp) != 0)
		return -errno;

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
	int r = content_truncate(path, size);
	if (r == 0)
		syncer_kick();
	return r;
}

static int omdfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)fi;
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
		"\n"
		"  --config <file>     read backend/datadir/cache-size from a key=value file\n"
		"  --backend <dir>     already-mounted network FS to cache (sshfs/nfs/...)\n"
		"  --datadir <dir>     local cache/journal/state directory\n"
		"  --cache-size <sz>   cache budget; accepts K/M/G suffix (0 = unlimited)\n"
		"  --resync            push the whole cache (content + metadata) to the\n"
		"                      backend so it matches the cache, then exit; no mount.\n"
		"                      Run while unmounted. Reports a summary on stderr.\n"
		"  <mountpoint>        where omdfs is exposed\n"
		"\n"
		"Command-line options override values from --config. While mounted, the\n"
		"daemon publishes sync progress to <datadir>/state/status.\n",
		prog, prog);
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
	/* Seed the cache-size accounting from content already on disk and arm the
	 * evictor (no-op when --cache-size is 0/unset). */
	evict_init(g_cfg.cache_budget);
	/* Publish an initial status file so an operator sees the mount come up even
	 * before the first sync cycle. */
	status_init();
	/* The syncer drains any WAL/dirty state left by a previous run (recovery)
	 * and then keeps the backend in sync while we serve the mount. */
	if (syncer_start() != 0) {
		fprintf(stderr, "omdfs: cannot start syncer\n");
		free(fuse_argv);
		return 1;
	}

	int ret = fuse_main(fuse_argc, fuse_argv, &omdfs_ops, NULL);

	syncer_stop(); /* final drain so pending mutations reach the backend */
	free(fuse_argv);
	return ret;
}
