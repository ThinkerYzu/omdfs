/*
 * omdfs-syncer.pml -- comprehensive SPIN model of the omdfs write-back engine.
 *
 * This is the broad model (companion to the focused omdfs-flush.pml clobber proof).
 * It models the concurrency-critical core of omdfs -- the two-phase background
 * syncer running against a nondeterministic stream of foreground mutations -- and
 * checks two end-to-end correctness properties over EVERY interleaving of EVERY
 * op sequence up to a bound:
 *
 *   converge  -- when the system goes quiescent (foreground done, WAL drained,
 *                nothing dirty/flushing/pending), the backend exactly equals the
 *                live local namespace: every live entry's bytes are on the backend
 *                at its current path, removed entries leave nothing, no orphan
 *                copies, and the backend directory matches. This single property
 *                subsumes "no clobber", "no lost update", and "deletes propagate".
 *   ordering  -- inline assertions in the structural drain: a backend create/rename
 *                never targets a path whose parent dir is missing, rename's source
 *                exists, rmdir only removes an empty dir. (mkdir-before-create,
 *                create-before-rename, the in-order WAL guarantee.)
 *   + SPIN's invalid-end-state check => the syncer never deadlocks.
 *
 * Modeled faithfully (vs. the C):
 *   - structural WAL = an in-memory FIFO of {op, entry, from, to} nodes drained in
 *     append order (DESIGN 2026-06-20); content/attr changes are a per-entry dirty
 *     flag, not a WAL record (Decision 4).
 *   - per-entry exclusion: pending_count (appended-but-undrained structural ops) +
 *     flushing flag. The flush gate = entry & ancestor dir at count 0; phase 1
 *     defers a rename/unlink of a flushing entry. Mutators publish dirty AFTER the
 *     append + count bump (Change 1).
 *   - meta_mtx critical sections are atomic{}; the backend copy and the backend
 *     structural op run with no lock held.
 *
 * Bounded abstraction (to stay finite): a namespace of one optional subdirectory
 * `d` and NF=2 file identities, each living under root or under `d`; content is an
 * abstract version tag; the foreground issues at most MAXOPS ops. The dirty content
 * flush is one process (concurrent flushes of *different* entries are independent;
 * the same entry is never flushed twice at once because the claim clears dirty) --
 * the critical phase-1-vs-phase-2 concurrency is fully modeled (separate processes).
 *
 * Switches: -DFIX (model the fixed syncer; omit for the buggy pre-exclusion one),
 *           -DMAXOPS=n (foreground program length; default 3).
 */

#ifndef MAXOPS
#define MAXOPS 3
#endif

#define NF   2        /* file identities 0,1                       */
#define NENT 3        /* pcount[] indices: files 0,1 + dir at 2    */
#define DIRX 2        /* the directory's index in pcount[]         */
#define ROOT 0
#define D    1

#define SLOT(dd,ii) ((dd)*NF + (ii))    /* backend file slot = dir*NF + name */

mtype = { CREATE, MKDIR, UNLINKF, RMDIR, RENAMEF };

/* ---- local namespace (the "truth", as the in-memory meta index) ---------- */
bool live[NF];        /* entry exists in the namespace          */
byte fdir[NF];        /* ROOT or D : the entry's current parent */
byte ver[NF];         /* content version (1,2); 0 = empty/none  */
bool dirty[NF];       /* un-synced content/attr change pending  */
bool dlive;           /* subdirectory d exists                  */

/* ---- per-entry exclusion ------------------------------------------------- */
byte pcount[NENT];    /* appended-but-undrained structural ops  */
bool flushing[NF];    /* a flush of this entry is in progress   */

/* ---- backend (the durable mirror) ---------------------------------------- */
bool bpres[2*NF];     /* file present at slot (dir,name)         */
byte bver[2*NF];      /* its content version                    */
bool bdir;            /* subdirectory d exists on the backend   */

/* ---- the structural WAL: an in-memory FIFO drained in append order ------- */
chan wal = [MAXOPS] of { mtype, byte, byte, byte };   /* op, entry, from, to */
byte wlen;            /* # records in the WAL (len()/empty() aren't allowed in LTL) */

bool fg_done;

/* FIX vs BUGGY, isolated to two macros */
#ifdef FIX
#define EXCL_OK(o,i)   ( ((o) != RENAMEF && (o) != UNLINKF) || !flushing[i] )
#define GATE(i)        ( pcount[i] == 0 && (fdir[i] != D || pcount[DIRX] == 0) )
#else
#define EXCL_OK(o,i)   ( true )
#define GATE(i)        ( true )
#endif

/* apply one drained structural record to the backend (no lock held) */
inline apply_head(o, i, a, b) {
	if
	:: o == MKDIR  -> bdir = true                            /* idempotent */
	:: o == RMDIR  -> assert(bdir);
	                  assert(!bpres[SLOT(D,0)] && !bpres[SLOT(D,1)]);  /* empty */
	                  bdir = false
	:: o == CREATE -> if :: a == D -> assert(bdir) :: else -> skip fi;  /* parent exists */
	                  bpres[SLOT(a,i)] = true; bver[SLOT(a,i)] = 0      /* created empty */
	:: o == UNLINKF-> bpres[SLOT(a,i)] = false; bver[SLOT(a,i)] = 0    /* idempotent */
	:: o == RENAMEF-> if :: b == D -> assert(bdir) :: else -> skip fi;  /* target parent */
	                  assert(bpres[SLOT(a,i)]);                          /* source exists */
	                  bpres[SLOT(b,i)] = bpres[SLOT(a,i)];
	                  bver[SLOT(b,i)] = bver[SLOT(a,i)];
	                  bpres[SLOT(a,i)] = false; bver[SLOT(a,i)] = 0
	fi
}

inline decr_head(o, i) {
	if
	:: (o == MKDIR || o == RMDIR) -> pcount[DIRX]--
	:: else -> pcount[i]--
	fi
}

/* ---- foreground mutators (each = local apply, then WAL append + count bump,
 *      then publish dirty -- Change 1) -------------------------------------- */
inline do_create(i) {
	if
	:: atomic { live[i]=true; fdir[i]=ROOT; ver[i]=1; wal!CREATE,i,ROOT,0; pcount[i]++; wlen++ }
	:: dlive ->
	   atomic { live[i]=true; fdir[i]=D;    ver[i]=1; wal!CREATE,i,D,0;    pcount[i]++; wlen++ }
	fi;
	dirty[i] = true                       /* publish dirty AFTER the append */
}
inline do_write(i)  { ver[i] = 3 - ver[i]; dirty[i] = true }   /* content write: dirty, no WAL */
inline do_rename(i) {
	if
	:: fdir[i] == D ->
	   atomic { pcount[i]++; fdir[i]=ROOT; wal!RENAMEF,i,D,ROOT; wlen++ }   /* pre-bump under move lock */
	:: fdir[i] == ROOT && dlive ->
	   atomic { pcount[i]++; fdir[i]=D;    wal!RENAMEF,i,ROOT,D; wlen++ }
	fi
}
inline do_unlink(i) {
	atomic { live[i]=false; wal!UNLINKF,i,fdir[i],0; pcount[i]++; dirty[i]=false; wlen++ }
}
inline do_mkdir() { atomic { dlive=true;  wal!MKDIR,DIRX,0,0; pcount[DIRX]++; wlen++ } }
inline do_rmdir() { atomic { dlive=false; wal!RMDIR,DIRX,0,0; pcount[DIRX]++; wlen++ } }

#define live_in_D ( (live[0] && fdir[0]==D) || (live[1] && fdir[1]==D) )

active proctype foreground() {
	byte n = 0;
	do
	:: n < MAXOPS ->
	   n++;
	   if
	   :: !live[0]                            -> do_create(0)
	   :: !live[1]                            -> do_create(1)
	   :: live[0]                             -> do_write(0)
	   :: live[1]                             -> do_write(1)
	   :: live[0] && (fdir[0]==D || dlive)    -> do_rename(0)
	   :: live[1] && (fdir[1]==D || dlive)    -> do_rename(1)
	   :: live[0]                             -> do_unlink(0)
	   :: live[1]                             -> do_unlink(1)
	   :: !dlive                              -> do_mkdir()
	   :: dlive && !live_in_D                 -> do_rmdir()
	   fi
	:: break                                  /* stop at any point */
	od;
	fg_done = true
}

/* ---- syncer phase 1: structural drain (in-order, with exclusion) --------- */
active proctype phase1() {
	mtype o; byte i, a, b;
	do
	:: atomic {
		nempty(wal) ->
		wal?<o,i,a,b>;                 /* peek the head */
		if
		:: EXCL_OK(o,i) ->             /* applicable: pop, apply off the meta lock, decrement */
		   wal?o,i,a,b; wlen--;
		   apply_head(o,i,a,b);
		   decr_head(o,i)
		:: else -> skip               /* defer a rename/unlink of a flushing entry */
		fi
	   }
	:: atomic { wlen==0 && fg_done -> break }
	od
}

/* ---- syncer phase 2: dirty flush (gated; claim-then-push; no lock across I/O).
 * NB: sp/sv are declared in the proctype (not the inline) so the FIRST statement
 * of flush_one is the guarded claim atomic -- a leading declaration would always
 * be executable and would (wrongly) become the do-branch guard. */
inline flush_one(i) {
	atomic {                           /* meta_flush_claim: snapshot, clear dirty, mark flushing */
		dirty[i] && GATE(i) ->
		sp = fdir[i]; sv = ver[i]; dirty[i] = false;
#ifdef FIX
		flushing[i] = true;
#endif
	}
	if :: sp == D -> assert(bdir) :: else -> skip fi;     /* parent exists at push time */
	bpres[SLOT(sp,i)] = true; bver[SLOT(sp,i)] = sv;      /* the push (off-lock) */
#ifdef FIX
	flushing[i] = false;
#endif
}

active proctype phase2() {
	byte sp, sv;
	do
	:: flush_one(0)
	:: flush_one(1)
	:: atomic { fg_done && wlen==0 && !dirty[0] && !dirty[1] -> break }
	od
}

/* ---- correctness properties ---------------------------------------------- */
#define quiesced ( fg_done && wlen==0 && \
	pcount[0]==0 && pcount[1]==0 && pcount[DIRX]==0 && \
	!dirty[0] && !dirty[1] && !flushing[0] && !flushing[1] )

/* a live entry: its bytes are on the backend at its current path, and nowhere else */
#define econv(i) ( !live[i] || ( bpres[SLOT(fdir[i],i)] && bver[SLOT(fdir[i],i)]==ver[i] \
	                          && !bpres[SLOT((1-fdir[i]),i)] ) )
/* a removed entry: gone from the backend entirely */
#define edead(i) ( live[i] || ( !bpres[SLOT(ROOT,i)] && !bpres[SLOT(D,i)] ) )

#define converged ( econv(0) && econv(1) && edead(0) && edead(1) && (bdir == dlive) )

#ifndef NOLTL
ltl converge { [] (quiesced -> converged) }
#endif
