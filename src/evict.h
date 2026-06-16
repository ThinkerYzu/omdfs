/*
 * omdfs — bounded-cache eviction policy.
 *
 * Keeps DATADIR/cache within a configured byte budget. The LRU index (lru.c)
 * owns recency/size/pin bookkeeping; this layer owns the watermark policy and
 * the eviction action: when the cache exceeds a high watermark it evicts the
 * least-recently-used CLEAN, unpinned, fully-cached content files down to a low
 * watermark. Eviction deletes the cache file and clears CONTENT_CACHED on the
 * parent index entry — the metadata entry stays, so the directory remains
 * listable and the file is transparently re-fetched on next access.
 *
 * Never evicted: dirty (un-synced) files, currently-open files, in-flight
 * fetches, and the WAL/metadata indexes (DESIGN § Eviction).
 *
 * A budget of 0 disables eviction entirely (the default), so the cache grows
 * unbounded exactly as it did before Phase 5.
 */
#ifndef OMDFS_EVICT_H
#define OMDFS_EVICT_H

#include <sys/types.h>

/* Set the cache budget in bytes (0 = unlimited / eviction off) and seed the LRU
 * index from content files already present in DATADIR/cache (so accounting is
 * correct across restarts). Call once at startup, after the datadir exists. */
void evict_init(off_t budget);

/* Note that `path`'s content is present with `size` bytes (a fetch completed, a
 * file was created, or a write/truncate changed its size). Updates recency and
 * the accounted total. */
void evict_note_present(const char *path, off_t size);

/* Pin/unpin an open file. A pinned file is never chosen for eviction. */
void evict_pin(const char *path);
void evict_unpin(const char *path);

/* Drop `path` from accounting (on unlink). */
void evict_forget(const char *path);

/* Re-key accounting on rename, carrying size/recency/pin. */
void evict_rename(const char *old_path, const char *new_path);

/* If the cache is over the high watermark, evict LRU clean unpinned files down
 * to the low watermark. Cheap (a counter compare) when under budget; safe to
 * call from the foreground and from background threads. Also refreshes the
 * backpressure flag (see evict_pressure). */
void evict_run_if_needed(void);

/* Backpressure (DESIGN § Eviction: backend-down). Un-evictable dirty content can
 * push the cache past the budget; we tolerate that up to a hard limit (2x the
 * budget), past which writes must fail rather than grow the cache without bound.
 *
 * evict_backpressure() asks whether adding `growth` bytes of (dirty, hence
 * un-reclaimable) content would cross the hard limit AFTER first reclaiming any
 * clean space: returns 1 if the write should be refused (-ENOSPC), 0 if allowed.
 * evict_pressure() is the latched flag from the most recent check/eviction, for
 * the status file. Both are always permissive (0) when the budget is unlimited. */
int evict_backpressure(off_t growth);
int evict_pressure(void);

/* Read-only accounting accessors for the status file. */
off_t evict_total(void);       /* accounted content bytes in the cache */
off_t evict_budget(void);      /* configured soft budget (0 = unlimited) */
off_t evict_hard_limit(void);  /* backpressure ceiling (0 = unlimited) */

#endif /* OMDFS_EVICT_H */
