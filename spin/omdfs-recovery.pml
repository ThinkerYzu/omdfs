/*
 * omdfs-recovery.pml -- SPIN model of omdfs crash recovery (the positional-index
 * checkpoint + the s_initial_drained dry-flush guard; DESIGN 2026-06-20 session 28
 * and 2026-06-19 recovery addendum).
 *
 * A crash keeps the *persistent* state (backend mirror + the on-disk WAL + the
 * checkpoint + the persisted dirty flags) and loses the *volatile* state
 * (pending_count, the in-memory drain queue, flushing). Recovery must bring the
 * backend back into agreement with the cache WITHOUT re-applying anything already
 * applied -- re-running an applied rename over a since-recreated path silently
 * corrupts the backend. Three scenarios, each checked for the post-recovery
 * invariant `recovered` (backend == the cache truth):
 *
 *   SCENARIO 1  Replay only the un-applied SUFFIX.  WAL = create A / rename A->B /
 *               create A, all applied (checkpoint = 3). FIX replays records[3:]
 *               (nothing); the buggy variant replays the whole WAL and re-runs the
 *               rename over the recreated A -> B ends up holding the wrong file.
 *
 *   SCENARIO 2  Interrupted-reclaim stale-HIGH checkpoint.  Reclaim truncates the
 *               WAL to empty then resets the checkpoint; a crash in between leaves
 *               {WAL empty, checkpoint = 3}. FIX clamps applied = min(ckpt, nrec) = 0
 *               and durably rewrites it, so a NEW op appended after recovery (at
 *               position 0) is >= applied and drains. The buggy variant trusts the
 *               stale-high checkpoint, counts the new op as already-applied, and
 *               never drains it -> a silently lost mutation.
 *
 *   SCENARIO 3  The dry-flush guard.  A recovered, un-applied rename A->B sits in
 *               the WAL while B is dirty (new bytes in the cache). Post-crash all
 *               pending_counts are 0, so the normal gate can't tell the rename is
 *               undrained. FIX holds real flushing until the recovered WAL has
 *               drained once (s_initial_drained); the buggy variant lets phase 2
 *               flush B before phase 1 drains the rename, and the rename then
 *               clobbers the freshly-flushed bytes -- the recovery-time clobber.
 *
 * Switch: -DFIX selects the correct recovery; without it, the historical hazard.
 *         -DSCENARIO=1|2|3 (default 1).
 */

#ifndef SCENARIO
#define SCENARIO 1
#endif

#define A 0
#define B 1
mtype = { WCREATE, WRENAME };

bool bpres[2];        /* backend: file present at path A / B          */
byte btag[2];         /* backend: which (file,version) tag is there   */
bool tpres[2];        /* cache truth: should-be-present at A / B       */
byte ttag[2];         /* cache truth: the tag that should be there    */

/* the on-disk WAL as an indexable record array (recovery reads a suffix) */
#define MAXR 3
mtype rop[MAXR];
byte  rp1[MAXR], rp2[MAXR], rtag[MAXR];
byte  nrec;           /* valid records on disk                        */
byte  ckpt;           /* persisted checkpoint = count of applied recs */

/* apply one structural record to the backend (idempotent against its own state) */
inline rapply(o, p1, p2, tg) {
	if
	:: o == WCREATE ->
	   if :: !bpres[p1] -> bpres[p1] = true; btag[p1] = tg    /* O_EXCL; EEXIST -> leave */
	      :: else -> skip
	   fi
	:: o == WRENAME ->
	   if :: bpres[p1] -> btag[p2] = btag[p1]; bpres[p2] = true; bpres[p1] = false; btag[p1] = 0
	      :: else -> skip                                     /* ENOENT ignored */
	   fi
	fi
}

#define recovered ( bpres[A]==tpres[A] && bpres[B]==tpres[B] && \
	(!tpres[A] || btag[A]==ttag[A]) && (!tpres[B] || btag[B]==ttag[B]) )

#if SCENARIO == 1 || SCENARIO == 2
/* ---- sequential recovery: clamp, replay the suffix, then flush dirty ------ */
init {
	byte start, k;
#if SCENARIO == 1
	/* create A=f1 ; rename A->B ; create A=f2 -- all applied pre-crash */
	rop[0]=WCREATE; rp1[0]=A; rtag[0]=1;
	rop[1]=WRENAME; rp1[1]=A; rp2[1]=B;
	rop[2]=WCREATE; rp1[2]=A; rtag[2]=2;
	nrec = 3; ckpt = 3;
	bpres[A]=true; btag[A]=2; bpres[B]=true; btag[B]=1;     /* backend after all 3 */
	tpres[A]=true; ttag[A]=2; tpres[B]=true; ttag[B]=1;     /* cache truth          */
#else
	/* interrupted reclaim: WAL truncated to empty, checkpoint not yet reset */
	nrec = 0; ckpt = 3;
	bpres[A]=false; bpres[B]=false;                        /* everything applied + reclaimed */
	tpres[A]=true; ttag[A]=7; tpres[B]=false;              /* a NEW op will (re)create A=f7  */
#endif

	/* wal_init: clamp a stale-high checkpoint down to the records on disk */
#ifdef FIX
	if :: ckpt > nrec -> ckpt = nrec :: else -> skip fi;   /* clamp + (durable) rewrite */
	start = ckpt;
#else
	start = ckpt;                                          /* SCN2 buggy: trust stale-high */
  #if SCENARIO == 1
	start = 0;                                             /* SCN1 buggy: replay whole WAL */
  #endif
#endif

	/* replay records[start:] */
	k = start;
	do
	:: k < nrec -> rapply(rop[k], rp1[k], rp2[k], rtag[k]); k++
	:: else -> break
	od;

#if SCENARIO == 2
	/* a new mutation arrives after recovery: append at position nrec, then drain
	 * records[ckpt:]. With the stale-high (unclamped) checkpoint this op is
	 * mis-counted as already applied and never drains. */
	rop[nrec]=WCREATE; rp1[nrec]=A; rtag[nrec]=7; nrec++;
	k = ckpt;
	do
	:: k < nrec -> rapply(rop[k], rp1[k], rp2[k], rtag[k]); k++
	:: else -> break
	od;
#endif

	assert(recovered)
}
#endif

#if SCENARIO == 3
/* ---- concurrent recovery: phase 1 drains the recovered rename while phase 2
 * wants to flush dirty B. Recovered (seeded) WAL nodes carry no pending_count,
 * so the normal gate is blind to the undrained rename; the only protection is the
 * dry-flush guard -- phase 2 must hold real flushing until the recovered WAL has
 * drained once (s_initial_drained). ---------------------------------------- */
bool rec_drained;     /* s_initial_drained: the recovered WAL has drained once */
bool fl_dirty;        /* B carries an un-synced write (the new bytes)          */
byte fl_tag;          /* the dirty bytes' tag                                  */
bool p1_done, p2_done;

proctype rec_phase1() {
	byte k = ckpt;
	do
	:: k < nrec -> rapply(rop[k], rp1[k], rp2[k], rtag[k]); k++
	:: else -> break
	od;
	rec_drained = true;            /* the recovered suffix has fully drained */
	p1_done = true
}

proctype rec_phase2() {
	byte sv;
	if
#ifdef FIX
	:: atomic { fl_dirty && rec_drained ->   /* dry until recovery drained; then claim */
	            sv = fl_tag; fl_dirty = false }
#else
	:: atomic { fl_dirty -> sv = fl_tag; fl_dirty = false }   /* flush immediately */
#endif
	   bpres[B] = true; btag[B] = sv          /* the push (off-lock) */
	:: !fl_dirty -> skip
	fi;
	p2_done = true
}

init {
	/* pre-crash: A=f1 on backend; the user renamed A->B and wrote new bytes to B
	 * (B dirty, tag 9), but the rename never drained. counts are lost (=0). */
	bpres[A]=true; btag[A]=1; bpres[B]=false;
	rop[0]=WRENAME; rp1[0]=A; rp2[0]=B; nrec=1; ckpt=0;
	fl_dirty = true; fl_tag = 9;
	tpres[A]=false; tpres[B]=true; ttag[B]=9;     /* truth: B holds the new bytes, A gone */
	rec_drained = false;
	run rec_phase1();
	run rec_phase2();
	(p1_done && p2_done);
	assert(recovered)
}
#endif
