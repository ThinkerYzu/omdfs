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

/* Signal the thread to stop, run a final drain, and join it. */
void syncer_stop(void);

/* One-shot offline reconcile (the --resync tool): drain the structural WAL, then
 * walk the whole cached tree and force every entry's existence + content + attrs
 * onto the backend regardless of dirty flags, making the backend match the cache.
 * Does NOT start the background thread; intended to run on an unmounted datadir.
 * Returns 0 if everything was pushed, 1 if any entry failed or a structural op is
 * blocked (a summary is printed to stderr). */
int syncer_resync(void);

#endif /* OMDFS_SYNCER_H */
