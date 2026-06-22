/*
 * omdfs-flush.pml -- SPIN model of the omdfs flush / structural-drain exclusion.
 *
 * What this verifies
 * ------------------
 * The two-phase background syncer runs concurrently with the foreground:
 *   - phase 1 (structural drain): applies WAL records (here, a rename) to the
 *     backend in order, via a real backend rename(from, to);
 *   - phase 2 (dirty flush): copies a dirty entry's cached bytes to the entry's
 *     *current* backend path and clears the dirty flag.
 *
 * The "clobber" (DESIGN.md 2026-06-19, reproduced as tests/clobber.sh in session 25):
 *   E lives at P, already flushed (backend P = v1). The user writes v2 (E dirty)
 *   and renames P -> Q. If phase 2 flushes v2 to backend Q and clears the dirty
 *   flag *before* phase 1 drains WAL_RENAME(P,Q), then phase 1's rename(P,Q)
 *   overwrites Q with the stale backend P (= v1). End state: cache Q = v2 but
 *   backend Q = v1, dirty flag clear -- silent data loss. The push *succeeded*,
 *   so retry can never catch it.
 *
 * The fix (per-entry exclusion object, "the indirection"):
 *   - pending_count: number of this entry's structural ops appended but not yet
 *     drained. A flush is gated: it may start only when pending_count == 0
 *     (E and -- in the real code -- every ancestor dir are settled).
 *   - flushing: set while a flush is in progress. Phase 1 defers a rename/unlink
 *     of an entry whose flushing flag is set.
 *   So the flush's rename(tmp,Q) and phase 1's rename(P,Q) are never live at once.
 *
 * Build-time switches (pass with `spin -D...` / `-D...` to the C compiler):
 *   FIX           -- if defined, model the fixed syncer (gate + flushing flag).
 *                    if undefined, model the buggy pre-session-25 syncer.
 *   SCENARIO=1    -- documented clobber: write then rename (exercises the gate).
 *   SCENARIO=2    -- overlapping-phase clobber: an already-dirty, settled entry
 *                    whose flush can start before a fresh rename arrives
 *                    (exercises the flushing flag, Change 3).
 *
 * Correctness property (safety): at quiescence the backend at the entry's current
 * path equals its cached bytes -- no flush was silently undone.  See `no_clobber`.
 *
 * Modeling simplifications (faithful to the exclusion, smaller than the code):
 *   - one entry, one rename, no ancestor chain (E's own pending_count is the gate;
 *     the ancestor walk is the same mechanism applied up the parent chain);
 *   - the WAL is the single pending rename rather than a CRC-framed file;
 *   - content is an abstract version tag (v1/v2), not bytes;
 *   - the meta_mtx critical sections are modeled as atomic{} blocks; backend I/O
 *     (the copy / the rename) happens with no lock held, as in the code.
 */

#ifndef SCENARIO
#define SCENARIO 1
#endif

/* content versions and the "absent" marker */
#define NONE 0
#define V1   1
#define V2   2

/* backend path ids (also used to index backend[]) */
#define P 0
#define Q 1

byte backend[2];      /* backend content at path P and path Q                  */
byte cache;           /* the entry's cached bytes -- the local "truth"         */
byte cur_path;        /* meta: which backend path the entry currently occupies */
bool dirty;           /* un-synced content change pending                      */
byte pcount;          /* pending_count: appended-but-undrained structural ops  */
bool flushing;        /* a flush of this entry is in progress                  */

/* the WAL, abstracted to the single pending rename of this scenario */
bool wal_pending;
byte ren_from, ren_to;

/* per-process completion flags, for the quiescence predicate */
bool fg_done, p1_done, p2_done;

/* FIX vs BUGGY differences, isolated to two macros */
#ifdef FIX
#define CAN_DRAIN (wal_pending && !flushing)   /* phase 1 defers while flushing  */
#define DEC_COUNT pcount--                     /* drained op releases its count  */
#define FLUSH_GATE (dirty && pcount == 0)      /* flush only on the settled edge */
#else
#define CAN_DRAIN (wal_pending)                /* buggy: no exclusion            */
#define DEC_COUNT skip
#define FLUSH_GATE (dirty)                     /* buggy: flush on the dirty flag */
#endif

/* ---- foreground (the FUSE mutator) ------------------------------------- */
proctype foreground()
{
#if SCENARIO == 1
	/* content write: arms the dirty flag, no WAL record (Decision 4) */
	atomic { cache = V2; dirty = true; }
#endif
	/* rename P -> Q: meta_move_entry rekeys (FIX pre-bumps the count under the
	 * move's own lock), then wal_append records the structural op. */
	atomic {
		cur_path = Q;
#ifdef FIX
		pcount++;
#endif
	}
	atomic { wal_pending = true; ren_from = P; ren_to = Q; }
	fg_done = true;
}

/* ---- phase 1: structural drain ----------------------------------------- */
proctype phase1()
{
	do
	:: atomic {
		CAN_DRAIN ->
		/* apply the backend rename(from,to): to gets from's bytes, from cleared */
		backend[ren_to] = backend[ren_from];
		backend[ren_from] = NONE;
		wal_pending = false;
		DEC_COUNT;
	   }
	:: atomic { fg_done && !wal_pending -> break }
	od;
	p1_done = true;
}

/* ---- phase 2: dirty flush ---------------------------------------------- */
proctype phase2()
{
	byte snap_path, snap_content;

	/* meta_flush_claim: snapshot the path+bytes, clear dirty, (FIX) set flushing,
	 * all atomically under meta_mtx.  The gate decides when a claim may happen. */
	atomic {
		FLUSH_GATE ->
		snap_path = cur_path;
		snap_content = cache;
		dirty = false;
#ifdef FIX
		flushing = true;
#endif
	}
	/* the push -- backend copy + publish, with NO lock held across the I/O */
	backend[snap_path] = snap_content;
#ifdef FIX
	flushing = false;        /* meta_flush_release */
#endif
	p2_done = true;
}

init {
	atomic {
		backend[P] = V1;     /* E was flushed earlier: backend P holds v1 */
		backend[Q] = NONE;
		cur_path = P;
		pcount = 0;
		flushing = false;
		wal_pending = false;
#if SCENARIO == 1
		cache = V1;          /* foreground will write v2 */
		dirty = false;
#else
		cache = V2;          /* already-dirty, settled entry (count 0) */
		dirty = true;
#endif
		run foreground();
		run phase1();
		run phase2();
	}
}

/* quiescent = every process finished; correct = backend matches the truth */
#define quiescent (fg_done && p1_done && p2_done)
#define correct   (backend[cur_path] == cache)

ltl no_clobber { [] (quiescent -> correct) }
