/*
 * omdfs-replace.pml -- write-back engine over the FULL rename op-space, INCLUDING
 * rename ONTO AN EXISTING entry (replace).
 *
 * The companion engine model omdfs-syncer.pml fuses a file's name with its identity
 * (SLOT(dir, ent)), so two entities can never collide on a path -- the entire replace
 * family is outside the op sequences it can generate, which is exactly why the
 * comprehensive models never caught the cross-identity flush clobber. This model
 * DECOUPLES name from identity: a path is (dir, name), and NF identities compete for
 * NN names per directory. So the foreground can rename one entity onto another's path,
 * dropping the occupant -- and a flush of the dropped entity, landing on the survivor's
 * path, is the clobber the fused namespace structurally cannot express.
 *
 * Verified over EVERY interleaving of EVERY op program up to MAXOPS, with the three
 * actors -- foreground mutator || phase-1 structural drain || phase-2 dirty flush --
 * all concurrent and BARRIER-FREE (the foreground keeps issuing ops with WAL records
 * undrained and flushes in flight):
 *   converge -- at quiescence every backend (dir,name) slot holds the content of the
 *               live entity now at that path, or is empty, and the backend dir matches.
 *               Subsumes no-clobber (incl. CROSS-IDENTITY), no-lost-update, deletes.
 *   ordering -- structural-drain asserts (parent exists, rename source exists, rmdir
 *               only on an empty dir).
 *   + SPIN invalid-end-state => no deadlock.
 *
 * The fix has TWO parts, each its own switch (the shipped fix is FIX + PUBLISH_GUARD):
 *   FIX          per-entry exclusion: the flush gate (entry + ancestor dir pcount==0)
 *                and FLUSHING_DEFER (phase 1 defers a rename/unlink of a *flushing*
 *                entity). Handles SAME-entity races. Does NOT cover replace -- a
 *                replacing rename is logged on the SOURCE, the dropped destination is
 *                never deferred, so its in-flight flush still clobbers.
 *   PUBLISH_GUARD validate-at-publish: a flush publishes only if its entity has not been
 *                dropped (replaced/unlinked) since the claim -- the epoch is unchanged.
 *                This is meta_flush_publish() / meta_flush_attr_begin() in the C. It
 *                closes the cross-identity clobber. (See omdfs-rename-replace.pml for
 *                the focused proof that FIX alone leaves the gap.)
 *
 * Namespace bound (minimal to make a rename collide): NF=2 identities, NN=2 names,
 * dirs {root, d}; (dir,name) = 4 backend slots. Two identities competing for names is
 * the least that lets a rename land on an occupant; content is a version tag; one
 * flusher (different entities flush independently). Directory rename and deep nesting
 * are a further stage -- they need transitive-path remap of a renamed subtree; here d
 * is a fixed optional subdir, as in omdfs-syncer.pml.
 *
 * Switches: -DFIX, -DPUBLISH_GUARD, -DMAXOPS=n (default 4), -DNOLTL (deadlock/assert).
 */

#ifndef MAXOPS
#define MAXOPS 4
#endif

#define NF   2        /* file identities 0,1                         */
#define NN   2        /* name slots 0,1                              */
#define ROOT 0
#define D    1
#define DIRX 2        /* the dir's index in pcount[]                 */
#define NENT 3        /* pcount[] : files 0,1 + dir at 2             */
#define SLOT(dd,nn) ((dd)*NN + (nn))   /* backend slot = dir*NN + name, 0..3 */

mtype = { CREATE, MKDIR, UNLINKF, RMDIR, RENAMEF };

/* ---- local namespace (the in-memory meta index, the "truth") ------------- */
bool live[NF];        /* entry exists                                */
byte fdir[NF];        /* ROOT or D : current parent dir              */
byte fname[NF];       /* 0 or 1   : current name slot                */
byte ver[NF];         /* content version (1,2)                       */
bool dirty[NF];       /* un-synced content change pending            */
byte epoch[NF];       /* bumped each time the identity is DROPPED
                         (unlinked or replaced); a flush snapshots it
                         and publishes only if unchanged (the guard)  */
bool dlive;           /* subdirectory d exists                       */

/* ---- per-entry exclusion ------------------------------------------------- */
byte pcount[NENT];    /* appended-but-undrained structural ops       */
bool flushing[NF];    /* a flush of this entry is in progress         */

/* ---- backend (durable mirror), keyed by (dir,name) slot ------------------ */
bool bpres[2*NN];     /* present at slot (dir,name)                  */
byte bver[2*NN];      /* its content version                         */
bool bdir;            /* subdirectory d exists on the backend        */

/* ---- structural WAL : in-memory FIFO drained in append order ------------- */
chan wal = [MAXOPS] of { mtype, byte, byte, byte };  /* op, ent, from_slot, to_slot */
byte wlen;
bool fg_done;

/* the OTHER identity (NF==2), and "is it the live occupant of (dd,nn)?" */
#define OTHER(i)              (1 - (i))
#define occ_by_other(i,dd,nn) ( live[OTHER(i)] && fdir[OTHER(i)]==(dd) && fname[OTHER(i)]==(nn) )
#define notcur(i,dd,nn)       ( !(fdir[i]==(dd) && fname[i]==(nn)) )

/* FIX vs BUGGY, isolated to two macros */
#ifdef FIX
#define EXCL_OK(o,i)   ( ((o) != RENAMEF && (o) != UNLINKF) || !flushing[i] )
#define GATE(i)        ( pcount[i] == 0 && (fdir[i] != D || pcount[DIRX] == 0) )
#else
#define EXCL_OK(o,i)   ( true )
#define GATE(i)        ( true )
#endif

/* apply one drained structural record to the backend (no lock held). a/b are SLOTs;
 * a slot's dir is slot/NN. A rename overwrites its target slot (POSIX replace). */
inline apply_head(o, i, a, b) {
	if
	:: o == MKDIR  -> bdir = true
	:: o == RMDIR  -> assert(bdir);
	                  assert(!bpres[SLOT(D,0)] && !bpres[SLOT(D,1)]);   /* dir empty */
	                  bdir = false
	:: o == CREATE -> if :: (a/NN) == D -> assert(bdir) :: else -> skip fi;  /* parent */
	                  bpres[a] = true; bver[a] = 0                            /* empty */
	:: o == UNLINKF-> bpres[a] = false; bver[a] = 0                          /* idempotent */
	:: o == RENAMEF-> if :: (b/NN) == D -> assert(bdir) :: else -> skip fi;  /* target parent */
	                  assert(bpres[a]);                                       /* source exists */
	                  bpres[b] = bpres[a]; bver[b] = bver[a];                 /* overwrite target */
	                  bpres[a] = false; bver[a] = 0
	fi
}

inline decr_head(o, i) {
	if :: (o == MKDIR || o == RMDIR) -> pcount[DIRX]-- :: else -> pcount[i]-- fi
}

/* ---- foreground mutators (local apply, then WAL append + count bump, then publish
 *      dirty for content ops -- Change 1) ------------------------------------ */

/* drop an identity: it ceases to exist; bump its epoch so any in-flight flush of it is
 * superseded at publish time, and cancel its un-started content flush. */
inline drop(j) { live[j] = false; epoch[j]++; dirty[j] = false }

inline mk(i, dd, nn) {
	live[i]=true; fdir[i]=dd; fname[i]=nn; ver[i]=1;
	wal!CREATE,i,SLOT(dd,nn),0; pcount[i]++; wlen++
}
inline do_create(i) {
	if
	:: !occ_by_other(i,ROOT,0)          -> atomic { mk(i,ROOT,0) }
	:: !occ_by_other(i,ROOT,1)          -> atomic { mk(i,ROOT,1) }
	:: dlive && !occ_by_other(i,D,0)    -> atomic { mk(i,D,0) }
	:: dlive && !occ_by_other(i,D,1)    -> atomic { mk(i,D,1) }
	fi;
	dirty[i] = true                       /* publish dirty AFTER the append + bump */
}

inline do_write(i) { ver[i] = 3 - ver[i]; dirty[i] = true }   /* content write: dirty, no WAL */

/* rename i onto (dd,nn): if occupied by the other identity, REPLACE it (drop). The
 * WAL rename is logged on i; the occupant's removal is implicit (its path is overwritten
 * on the backend), exactly as meta_move_entry drops the destination. */
inline mv(i, dd, nn) {
	pcount[i]++;
	if :: occ_by_other(i,dd,nn) -> drop(OTHER(i)) :: else -> skip fi;
	wal!RENAMEF,i,SLOT(fdir[i],fname[i]),SLOT(dd,nn); wlen++;
	fdir[i]=dd; fname[i]=nn
}
inline do_rename(i) {
	if
	:: notcur(i,ROOT,0)        -> atomic { mv(i,ROOT,0) }
	:: notcur(i,ROOT,1)        -> atomic { mv(i,ROOT,1) }
	:: dlive && notcur(i,D,0)  -> atomic { mv(i,D,0) }
	:: dlive && notcur(i,D,1)  -> atomic { mv(i,D,1) }
	fi
}

inline do_unlink(i) {
	atomic { wal!UNLINKF,i,SLOT(fdir[i],fname[i]),0; pcount[i]++; drop(i); wlen++ }
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
	   :: !live[0]                    -> do_create(0)
	   :: !live[1]                    -> do_create(1)
	   :: live[0]                     -> do_write(0)
	   :: live[1]                     -> do_write(1)
	   :: live[0]                     -> do_rename(0)
	   :: live[1]                     -> do_rename(1)
	   :: live[0]                     -> do_unlink(0)
	   :: live[1]                     -> do_unlink(1)
	   :: !dlive                      -> do_mkdir()
	   :: dlive && !live_in_D         -> do_rmdir()
	   fi
	:: break
	od;
	fg_done = true
}

/* ---- syncer phase 1: structural drain (in order, with exclusion) --------- */
active proctype phase1() {
	mtype o; byte i, a, b;
	do
	:: atomic {
		nempty(wal) ->
		wal?<o,i,a,b>;                            /* peek */
		if
		:: EXCL_OK(o,i) -> wal?o,i,a,b; wlen--; apply_head(o,i,a,b); decr_head(o,i)
		:: else -> skip                           /* defer rename/unlink of a flushing entity */
		fi
	   }
	:: atomic { wlen==0 && fg_done -> break }
	od
}

/* ---- syncer phase 2: dirty flush (gated claim, then publish off-lock) ----- */
inline flush_one(i) {
	atomic {                                       /* meta_flush_claim: snapshot, clear dirty */
		dirty[i] && GATE(i) ->
		sslot = SLOT(fdir[i],fname[i]); sv = ver[i]; sep = epoch[i]; dirty[i] = false;
#ifdef FIX
		flushing[i] = true;
#endif
	}
	atomic {                                       /* the publish (rename(tmp,dst)), gated */
#ifdef PUBLISH_GUARD
		if
		:: epoch[i] == sep ->                  /* entity still live & unreplaced */
		   if :: (sslot/NN)==D -> assert(bdir) :: else -> skip fi;
		   bpres[sslot] = true; bver[sslot] = sv
		:: else -> skip                        /* superseded: dropped mid-flush, discard */
		fi
#else
		if :: (sslot/NN)==D -> assert(bdir) :: else -> skip fi;
		bpres[sslot] = true; bver[sslot] = sv
#endif
	}
#ifdef FIX
	flushing[i] = false;
#endif
}

active proctype phase2() {
	byte sslot, sv, sep;
	do
	:: flush_one(0)
	:: flush_one(1)
	:: atomic { fg_done && wlen==0 && !dirty[0] && !dirty[1] -> break }
	od
}

/* ---- correctness --------------------------------------------------------- */
#define quiesced ( fg_done && wlen==0 && \
	pcount[0]==0 && pcount[1]==0 && pcount[DIRX]==0 && \
	!dirty[0] && !dirty[1] && !flushing[0] && !flushing[1] )

/* identity i is the live occupant of slot s (at most one identity per slot) */
#define at(i,s) ( live[i] && SLOT(fdir[i],fname[i])==(s) )
/* slot s converged: holds its live occupant's bytes, or is empty if no live occupant */
#define slotconv(s) ( \
	( !at(0,s) || (bpres[s] && bver[s]==ver[0]) ) && \
	( !at(1,s) || (bpres[s] && bver[s]==ver[1]) ) && \
	( at(0,s) || at(1,s) || !bpres[s] ) )

#define converged ( slotconv(0) && slotconv(1) && slotconv(2) && slotconv(3) && (bdir==dlive) )

#ifndef NOLTL
ltl converge { [] (quiesced -> converged) }
#endif
