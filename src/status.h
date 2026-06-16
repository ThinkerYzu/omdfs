/*
 * omdfs — sync-status surfacing (DESIGN § Sync failure handling).
 *
 * The daemon publishes a small, human-readable status file at
 * DATADIR/state/status so an operator can see, without attaching a debugger,
 * how far the background sync has reached and whether it is stuck. The syncer
 * fills a `struct omdfs_status` snapshot every cycle (and a final one on clean
 * unmount) and calls status_publish(), which writes the file atomically.
 *
 * The file is purely diagnostic: nothing in omdfs reads it back. It exists so a
 * stuck/permanent backend failure (DESIGN: "log loudly, surface status, never
 * drop a mutation") is visible, and so an operator can tell a still-draining
 * mount from an idle one before unmounting.
 */
#ifndef OMDFS_STATUS_H
#define OMDFS_STATUS_H

#include <limits.h>

/* A point-in-time snapshot of sync progress. The syncer owns one of these and
 * refreshes it each cycle; all counts are "still pending" backlog, not totals. */
struct omdfs_status {
	long pending_structural;   /* WAL records not yet applied to the backend */
	long dirty_files;          /* entries still carrying a dirty content/attr flag */
	long long dirty_bytes;     /* sum of dirty content sizes (bytes) */
	long long cache_bytes;     /* accounted content bytes in the cache */
	long long cache_budget;    /* --cache-size (0 = unlimited) */
	long long cache_hard_limit; /* backpressure ceiling (0 = unlimited) */
	int backpressure;          /* 1 = cache full of un-evictable dirty data; writes get ENOSPC */
	int stuck;                 /* 1 = head structural op is failing permanently */
	int stuck_errno;           /* errno of the stuck/blocked op (0 if none) */
	char stuck_op[16];         /* op name of the stuck/blocked op ("" if none) */
	char stuck_path[PATH_MAX]; /* path of the stuck/blocked op ("" if none) */
	long long last_sync_epoch; /* CLOCK_REALTIME secs of the last clean cycle (0 = never) */
};

/* Record the daemon start and write an initial "starting" status file. Call once
 * at startup, after the datadir (state/) exists. */
void status_init(void);

/* Atomically (tmp + rename) write `s` to DATADIR/state/status. Safe to call from
 * the syncer thread and from the shutdown path. */
void status_publish(const struct omdfs_status *s);

#endif /* OMDFS_STATUS_H */
