/*
 * omdfs-rename-replace.pml -- SPIN model of the CROSS-IDENTITY replacing rename.
 *
 * The existing exclusion (omdfs-flush.pml, omdfs-syncer.pml, omdfs-impl.pml) only ever
 * renames an entry to a *new* name -- a reparent. This model covers the case those can't
 * express: rename A onto an EXISTING entry B's path P, where B is being flushed.
 *
 * The setup
 * ---------
 *   B lives at path P, dirty (cached bytes BV not yet on the backend).
 *   A lives at path Q, already synced (backend[Q] = AV, clean).
 *   The user renames A -> P, which REPLACES B: meta_move_entry drops B's index entry
 *   (ind_release: B's indirection is detached, and -- if a flush holds it -- freed only
 *   lazily once that flush ends). The WAL rename is logged on A's indirection; B's
 *   removal is implicit, so B's pending_count is never bumped.
 *   After the replace the truth is: A's bytes (AV) at P, B gone.
 *
 * The race
 * --------
 *   phase 2 can claim B's flush (snapshot path P + bytes BV, set flushing) BEFORE the
 *   replace drops B. The replace then removes B, but the claimed copy is in flight off
 *   the lock. phase 1 drains the rename (backend P := backend Q = AV). If B's flush copy
 *   then lands, it writes backend[P] = BV -- B's discarded bytes clobber A at P. A is
 *   clean, so no later flush repairs it: silent cross-identity data loss.
 *
 * Why the current fix does NOT cover it
 * -------------------------------------
 *   FLUSHING_DEFER defers a rename/unlink of a flushing entry -- but it checks the
 *   *renamed* entity (A), which is not the one flushing. B (the destination being
 *   replaced) is flushing, and nothing defers the rename ONTO a flushing entry's path.
 *   So with the current exclusion (FIX) the clobber is still reachable.
 *
 * The fix (PUBLISH_GUARD) -- as shipped, "validate at publish"
 * ------------------------------------------------------------
 *   The flush's publish is made atomic, under meta_mtx, with a check that the flushing
 *   entry is still live (its indirection not detached by the replace's ind_release). A
 *   superseded flush -- one whose entry was replaced/removed mid-flush -- is discarded
 *   instead of publishing, so it can never clobber the path's new owner. This single
 *   abstract guard maps to BOTH C publish primitives: meta_flush_publish() for the content
 *   rename(tmp,dst), and meta_flush_attr_begin/end() for the attr apply (chmod/utimens) --
 *   content bytes vs attrs is immaterial to the race, so one model covers both. Likewise
 *   b_gone abstracts the entry being dropped by EITHER a replacing rename OR an unlink
 *   (both call ind_release). Because the structural rename can only land AFTER the replace
 *   detaches B, by the time B's publish could win the race B is already detached -- so the
 *   locked check discards it.
 *
 * Switches:
 *   FIX            model the current per-entry exclusion (gate + flushing-defer on the
 *                  renamed entity). Without it, the buggy pre-exclusion syncer.
 *   PUBLISH_GUARD  the shipped fix: publish a flush only if its entry is still live
 *                  (meta_flush_publish), atomic with the replace. Implies FIX.
 *
 * Property (safety): at quiescence the backend at A's current path holds A's bytes --
 * B's discarded flush never clobbers the survivor. See `no_clobber`.
 *
 * Simplifications (faithful to the exclusion, smaller than the code): two entities, one
 * replacing rename; A clean so the clobber is unambiguous (no A re-flush to mask it);
 * content is a version tag; the WAL is the single pending rename; meta_mtx sections are
 * atomic{}, the backend copy / rename run with no lock held, as in the code.
 */

#define NONE 0
#define AV   1        /* A's bytes (the survivor)            */
#define BV   2        /* B's dirty bytes (must be discarded) */
#define B0   3        /* B's older synced bytes              */

#define P 0           /* B's path -- the replace target      */
#define Q 1           /* A's source path                     */

byte backend[2];      /* backend content at P and Q                              */

/* entity A (the source, clean, at Q -> P) */
byte a_cache;         /* A's cached bytes = AV (the truth)                       */
byte a_path;          /* A's current path (Q, then P)                           */
bool a_dirty;
byte a_pcount;        /* A's appended-but-undrained structural ops              */
bool a_flushing;      /* a flush of A is in progress (stays false: A is clean)  */

/* entity B (the destination, dirty, at P, replaced) */
byte b_cache;         /* B's dirty bytes = BV                                    */
bool b_dirty;
bool b_flushing;      /* a flush of B is in progress                            */
bool b_gone;          /* B's index entry has been dropped by the replace        */

/* the WAL, abstracted to the single pending rename A: Q -> P */
bool wal_pending;
byte ren_from, ren_to;

bool fg_done, p1_done, p2_done;

/* FIX: the current per-entry exclusion. The rename defers only while the RENAMED
 * entity (A) is flushing -- vacuous here, since B is the one flushing. */
#ifdef FIX
#define DRAIN_DEFER (a_flushing)
#define DEC_A       a_pcount--
#else
#define DRAIN_DEFER (false)
#define DEC_A       skip
#endif

/* PUBLISH_GUARD: the shipped fix -- a flush publishes only if its entry is still live
 * (not dropped by the replace). The check + publish are atomic (meta_flush_publish under
 * meta_mtx). PUBLISH_OK is evaluated inside that atomic in phase 2. */
#ifdef PUBLISH_GUARD
#define PUBLISH_OK (!b_gone)
#else
#define PUBLISH_OK (true)
#endif

/* ---- foreground: rename A -> P, replacing B --------------------------------- */
proctype foreground()
{
	/* meta_move_entry under meta_mtx: rekey A to P (FIX pre-bumps A's count) and
	 * drop B. B's ind is detached; a flush already holding it (b_flushing) lingers
	 * and its copy is still in flight -- the danger. */
	atomic {
		a_path = P;
#ifdef FIX
		a_pcount++;
#endif
		b_gone = true;
		b_dirty = false;   /* B's dirty content is discarded by the replace */
	}
	/* wal_append the rename on A's indirection */
	atomic { wal_pending = true; ren_from = Q; ren_to = P; }
	fg_done = true;
}

/* ---- phase 1: structural drain --------------------------------------------- */
proctype phase1()
{
	do
	:: atomic {
		wal_pending && !DRAIN_DEFER ->
		backend[ren_to] = backend[ren_from];   /* rename(from,to): to gets from's bytes */
		backend[ren_from] = NONE;
		wal_pending = false;
		DEC_A;
	   }
	:: atomic { fg_done && !wal_pending -> break }
	od;
	p1_done = true;
}

/* ---- phase 2: dirty flush (of B) ------------------------------------------- */
proctype phase2()
{
	byte snap_path, snap_content;
	if
	:: atomic {
		/* meta_flush_claim(B): only while B is still in the index (!b_gone) and
		 * dirty. Snapshot path+bytes, clear dirty, set flushing. */
		b_dirty && !b_gone ->
		snap_path = P; snap_content = b_cache; b_dirty = false; b_flushing = true;
	   }
	   /* copy_file writes the temp off the lock (the window), then PUBLISHES. The
	    * publish (meta_flush_publish) is atomic with the live-entry check: a flush whose
	    * entry was replaced/removed (b_gone) is discarded, never clobbering P. */
	   atomic {
		if
		:: PUBLISH_OK -> backend[snap_path] = snap_content   /* rename(tmp,dst) */
		:: else       -> skip                                /* superseded: discard temp */
		fi
	   }
	   atomic { b_flushing = false; }       /* meta_flush_release */
	:: atomic { (b_gone || !b_dirty) -> skip }   /* nothing to flush: B removed first */
	fi;
	p2_done = true;
}

init {
	atomic {
		backend[Q] = AV;     /* A already synced at Q (clean)        */
		backend[P] = B0;     /* B's older synced bytes               */
		a_cache = AV; a_path = Q; a_dirty = false; a_pcount = 0; a_flushing = false;
		b_cache = BV; b_dirty = true; b_flushing = false; b_gone = false;
		wal_pending = false;
		run foreground();
		run phase1();
		run phase2();
	}
}

/* quiescent = all three done; correct = A's bytes survive at A's current path (P),
 * i.e. B's discarded flush did not clobber the survivor. */
#define quiescent (fg_done && p1_done && p2_done)
#define correct   (backend[a_path] == a_cache)

ltl no_clobber { [] (quiescent -> correct) }
