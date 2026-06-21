/*
 * omdfs — bounded-cache eviction policy (implementation).
 *
 * Thin policy over lru.c. The high/low watermarks are fractions of the budget:
 * eviction triggers once the accounted total crosses the high mark and runs
 * until it drops below the low mark, so we hover comfortably under the budget
 * rather than thrashing right at the limit.
 *
 * The eviction action for one victim is, in order: confirm it is still
 * unpinned, not mid-fetch, and a clean fully-cached file (consulting the parent
 * index); then clear CONTENT_CACHED, unlink the cache file, and forget it from
 * the LRU. Clearing the flag before unlinking keeps the index consistent with
 * the on-disk state; a warm open that races in either re-fetches on the cleared
 * flag or hits the existing "claimed cached but file is gone" fallback in
 * content_open. Only one evictor runs at a time (evict_mtx).
 */
#define _GNU_SOURCE
#include "evict.h"

#include "content.h"
#include "journal.h"
#include "lru.h"
#include "meta.h"
#include "omdfs.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* High/low watermarks as a fraction of the budget. */
#define HIGH_NUM 95
#define LOW_NUM 85
#define WATER_DEN 100

/* Backpressure ceiling as a multiple of the budget: un-evictable dirty content
 * may overshoot the soft budget up to here before writes start failing. */
#define HARD_MULT 2

#define EVICT_BATCH 64 /* victims sampled per snapshot pass */

static off_t g_budget; /* 0 = eviction disabled */
static off_t g_high, g_low, g_hard;
/* Latched: over the hard limit with nothing clean to evict. Set/cleared from the
 * foreground write path (evict_backpressure) and the syncer's sweep
 * (evict_run_if_needed), read by the syncer for status/backoff — so it's atomic.
 * Just a status flag with no ordering dependency on other state: relaxed is enough. */
static atomic_int g_pressure;
static pthread_mutex_t evict_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Join a FUSE dir and a child name into a FUSE path. */
static void join_path(char out[PATH_MAX], const char *dir, const char *name)
{
	if (dir[0] == '/' && dir[1] == '\0')
		snprintf(out, PATH_MAX, "/%s", name);
	else
		snprintf(out, PATH_MAX, "%s/%s", dir, name);
}

/* Is `name` an index file or its mkstemp temp ("/.omdfs.dir" / ".omdfs.dir.*")?
 * Those are never content and must not be accounted or evicted. */
static int is_index_name(const char *name)
{
	size_t n = strlen(OMDFS_INDEX_NAME);
	return !strcmp(name, OMDFS_INDEX_NAME) ||
	       (!strncmp(name, OMDFS_INDEX_NAME, n) && name[n] == '.');
}

/* Recursively seed the LRU from content files under `cache_dir` (mirrored at
 * FUSE path `fuse_dir`). Recency is insertion order; pins start at zero. */
static void seed_dir(const char *cache_dir, const char *fuse_dir)
{
	DIR *dp = opendir(cache_dir);
	if (!dp)
		return;
	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (is_index_name(de->d_name))
			continue;

		char ccp[PATH_MAX], cfp[PATH_MAX];
		snprintf(ccp, sizeof(ccp), "%s/%s", cache_dir, de->d_name);
		join_path(cfp, fuse_dir, de->d_name);

		struct stat st;
		if (lstat(ccp, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			seed_dir(ccp, cfp);
		else if (S_ISREG(st.st_mode))
			lru_touch(cfp, st.st_size);
		/* symlinks et al. are not evictable content; ignore. */
	}
	closedir(dp);
}

void evict_init(off_t budget)
{
	g_budget = budget;
	if (budget <= 0) {
		g_budget = 0;
		return;
	}
	g_high = budget * HIGH_NUM / WATER_DEN;
	g_low = budget * LOW_NUM / WATER_DEN;
	g_hard = budget * HARD_MULT;

	char croot[PATH_MAX];
	cache_path(croot, "/");
	seed_dir(croot, "/");
}

void evict_note_present(const char *path, off_t size)
{
	if (g_budget)
		lru_touch(path, size);
}

void evict_pin(const char *path)
{
	if (g_budget)
		lru_pin(path, +1);
}

void evict_unpin(const char *path)
{
	if (g_budget)
		lru_pin(path, -1);
}

void evict_forget(const char *path)
{
	if (g_budget)
		lru_forget(path);
}

void evict_rename(const char *old_path, const char *new_path)
{
	if (g_budget)
		lru_rename(old_path, new_path);
}

/* Try to evict one victim. Returns 1 if it made progress (evicted or pruned a
 * stale entry), 0 if the victim must be kept (pinned / dirty / fetching). */
static int try_evict(const char *path)
{
	if (lru_is_pinned(path) || content_is_fetching(path))
		return 0;

	char parent[PATH_MAX], name[PATH_MAX];
	omdfs_split_path(path, parent, name);

	struct omdfs_index idx;
	if (meta_try_load(parent, &idx) != 0) {
		/* Parent index gone: can't confirm state — drop the stale entry. */
		lru_forget(path);
		return 1;
	}
	struct omdfs_entry *e = meta_lookup(&idx, name);
	if (!e) {
		meta_free_index(&idx);
		lru_forget(path);
		return 1; /* entry vanished (unlinked/renamed) */
	}
	uint8_t flags = e->flags;
	meta_free_index(&idx);

	if (flags & (OMDFS_F_DIRTY_CONTENT | OMDFS_F_DIRTY_ATTR | OMDFS_F_FETCHING))
		return 0; /* un-synced or in-flight: never evict */
	if (!(flags & OMDFS_F_CONTENT_CACHED)) {
		lru_forget(path); /* not actually cached: stop accounting it */
		return 1;
	}

	/* Evict: clear the cached flag first, then remove the cache file. */
	meta_mark_flags(parent, name, 0, OMDFS_F_CONTENT_CACHED);
	char cp[PATH_MAX];
	cache_path(cp, path);
	unlink(cp);
	lru_forget(path);
	return 1;
}

void evict_run_if_needed(void)
{
	if (!g_budget)
		return;

	/* Don't evict while a structural rename has not yet reached the backend: the
	 * backend namespace can lag the cache, so a clean file's content may still
	 * sit at its OLD backend path. Dropping its cache copy now would make a
	 * refetch at the file's current path fail ENOENT (the rename hasn't moved the
	 * backend copy yet). Skip this sweep; the cache may transiently exceed the
	 * budget (bounded by the 2x hard limit) and eviction catches up once the
	 * rename drains. The backpressure latch is still refreshed below. */
	if (!wal_has_pending_rename() && lru_total() > g_high) {
		pthread_mutex_lock(&evict_mtx);
		while (lru_total() > g_low) {
			char cand[EVICT_BATCH][PATH_MAX];
			size_t m = lru_snapshot_lru(cand, EVICT_BATCH);
			if (m == 0)
				break; /* nothing unpinned left to consider */
			int progress = 0;
			for (size_t i = 0; i < m && lru_total() > g_low; i++)
				if (try_evict(cand[i]))
					progress = 1;
			if (!progress)
				break; /* every candidate is dirty/pinned/fetching */
		}
		pthread_mutex_unlock(&evict_mtx);
	}

	/* Backpressure is a latch set when a write is actually refused (see
	 * evict_backpressure). Clear it only once the cache has drained well back
	 * under the limit — i.e. the un-evictable dirty content has synced and been
	 * reclaimed — so the status flag doesn't flap right at the boundary. */
	if (lru_total() < g_high)
		atomic_store_explicit(&g_pressure, 0, memory_order_relaxed);
}

int evict_backpressure(off_t growth)
{
	if (!g_budget)
		return 0; /* unlimited cache: never refuse a write */
	if (lru_total() + growth <= g_hard)
		return 0; /* fits under the hard limit */
	/* Would cross the hard limit: try to make room by evicting clean files. */
	evict_run_if_needed();
	if (lru_total() + growth <= g_hard)
		return 0; /* reclaimed enough clean space */
	/* un-evictable: refuse the growth and latch the flag */
	atomic_store_explicit(&g_pressure, 1, memory_order_relaxed);
	return 1;
}

int evict_pressure(void)
{
	return atomic_load_explicit(&g_pressure, memory_order_relaxed);
}

off_t evict_total(void)
{
	return g_budget ? lru_total() : 0;
}

off_t evict_budget(void)
{
	return g_budget;
}

off_t evict_hard_limit(void)
{
	return g_hard;
}
