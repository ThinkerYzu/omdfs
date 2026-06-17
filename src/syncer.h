/*
 * omdfs — background two-phase syncer.
 *
 * A single background thread periodically (and on demand via syncer_kick) drains
 * pending work to the backend in two phases per cycle:
 *   1. structural drain — apply WAL records in seq order (mkdir/create/unlink/
 *      rmdir/rename/symlink); a failing op blocks later ops and is retried next
 *      cycle (strict in-order, DESIGN Decision 5).
 *   2. dirty flush — walk the cached index tree and copy whole dirty files /
 *      re-apply dirty attrs to each entry's current backend path (order-
 *      independent, idempotent).
 * Each cycle publishes a status snapshot to DATADIR/state/status (progress +
 * any stuck/permanent failure). On a clean stop it drains until the backlog is
 * empty or stops shrinking, so a reachable backend is fully caught up before
 * exit (with the backend down it leaves the WAL/dirty flags for recovery).
 */
#ifndef OMDFS_SYNCER_H
#define OMDFS_SYNCER_H

/* Spawn the syncer thread. Returns 0 or -1. */
int syncer_start(void);

/* Wake the thread to sync now (called after each foreground mutation). */
void syncer_kick(void);

/* Stamp the foreground-activity beacon. Called at the top of every FUSE op so the
 * background syncer can back off (defer its cycle) while the foreground is actively
 * using the mount, instead of contending for meta_mtx and slowing every op. Cheap
 * and lock-free (a relaxed atomic store of the monotonic clock); safe to call from
 * any thread on the per-block read/write path. Tuned by OMDFS_SYNC_BACKOFF_MS
 * (default 2000 ms; 0 disables backoff). */
void syncer_note_activity(void);

/* Signal the thread to stop, run a final drain, and join it. */
void syncer_stop(void);

/* One-shot offline reconcile (the --resync tool): drain the structural WAL, then
 * walk the whole cached tree and force every entry's existence + content + attrs
 * onto the backend regardless of dirty flags, making the backend match the cache.
 * Does NOT start the background thread; intended to run on an unmounted datadir.
 * Returns 0 if everything was pushed, 1 if any entry failed or a structural op is
 * blocked (a summary is printed to stderr).
 *
 * A *running* mount runs the same reconcile when an operator creates the trigger
 * file DATADIR/state/resync (e.g. `touch <datadir>/state/resync`): the syncer
 * thread consumes it within one sync interval and runs the reconcile on its own
 * thread (never concurrently with the live drain), reporting the result in the
 * status file's last-resync / last-resync-result lines. */
int syncer_resync(void);

/* One-shot offline "mark everything dirty" (the --mark-dirty tool): walk the
 * whole cached tree and flag every entry dirty (content for cached files, attrs
 * for all) so a later mount's background syncer re-pushes it. Unlike --resync
 * this does NOT push to the backend — it only writes local metadata and returns,
 * so it is fast and the slow backend push happens incrementally in the
 * background on the next mount. Does NOT start the syncer thread. Returns 0, or
 * 1 if any directory could not be marked (a summary is printed to stderr).
 *
 * A *running* mount marks the same way when an operator creates the trigger file
 * DATADIR/state/mark-dirty (e.g. `touch <datadir>/state/mark-dirty`): the syncer
 * thread consumes it at the top of a cycle, marks the tree on its own thread,
 * and the same cycle begins draining the dirty backlog — the operator's `touch`
 * returns immediately and need not wait for the push to finish. The result is
 * reported in the status file's last-mark-dirty / last-mark-dirty-result lines. */
int syncer_mark_dirty(void);

#endif /* OMDFS_SYNCER_H */
