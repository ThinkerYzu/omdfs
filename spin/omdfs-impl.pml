/*
 * omdfs-impl.pml -- function-by-function SPIN model of the omdfs write-back core.
 *
 * Where omdfs-syncer.pml models the *algorithm* abstractly, this model mirrors the
 * actual C, function by function, across the concurrency-critical modules:
 *
 *   journal.c  (the structural write-ahead log: on-disk records + checkpoint)
 *   meta.c     (the per-entry metadata index + indirections that gate the flush)
 *   syncer.c   (the two-phase background syncer + crash-recovery seeding)
 *   content.c  (the read/fetch path: a cold read pulls bytes from the backend)
 *
 * Every significant C function appears below as an inline of the SAME name, and each
 * caller CALLS it -- nothing is merged into its caller.
 *
 * NAME DECOUPLED FROM IDENTITY. A path is (dir, name): NF=2 file identities compete for
 * NN=2 names per directory, so the foreground can rename one file ONTO another's path,
 * dropping the occupant (meta_move_entry's drop_at). This makes the FULL file rename
 * op-space -- including rename-onto-existing (replace) -- reachable over every op
 * sequence, which the older fused SLOT(dir,ent) encoding structurally could not express.
 *
 * Verified, over EVERY interleaving of EVERY foreground op program up to MAXOPS, with
 * foreground || phase-1 drain || phase-2 flush all concurrent and barrier-free:
 *   converge -- at quiescence every backend (dir,name) slot holds the bytes of the live
 *               file now at that path, or is empty, the backend dir matches, and the WAL
 *               is fully drained. Subsumes no-clobber (incl. CROSS-IDENTITY replace),
 *               no-lost-update, deletes-propagate, WAL ordering.
 *   ordering -- asserts in apply_structural (parent exists before a create/rename into
 *               it, rename source present, rmdir only on an empty dir).
 *   readok   -- a read of a live file always yields its CURRENT content, never the
 *               replaced file's stale bytes (content_read's assert). do_rename models
 *               content_prefetch (it re-caches the file from the SOURCE path before the
 *               rename), so reads stay warm and correct across a rename-onto-existing.
 *               (open_backend_for_fetch's cold-fetch retry is therefore not exercised by
 *               a FILE rename -- its real trigger is a DIRECTORY rename moving an evicted
 *               child with no prefetch, which is out of scope here; -DNOREADFIX is a
 *               no-op until directory rename is modeled.)
 *   + SPIN invalid-end-state => no deadlock.
 *
 * The fix has TWO parts (the shipped fix is FIX + PUBLISH_GUARD):
 *   FIX          per-entry exclusion: the flush gate (entry + ancestor dir pcount==0),
 *                FLUSHING_DEFER (phase 1 defers a rename/unlink of a *flushing* entity),
 *                and the s_initial_drained recovery guard. Handles SAME-entity races.
 *                Does NOT cover replace: a replacing rename is logged on the SOURCE, the
 *                dropped destination is never deferred, so its in-flight flush clobbers.
 *   PUBLISH_GUARD validate-at-publish: copy_file publishes only if the entity has not been
 *                dropped (replaced/unlinked) since the claim -- meta_flush_publish() /
 *                meta_flush_attr_begin() in the C. Modeled by an epoch the flush snapshots
 *                at claim and re-checks at publish (the static ind table can't carry a
 *                per-incarnation detached flag, so the epoch stands in for it).
 *
 * Crash recovery (-DCRASH): once the foreground is done, a single crash may fire at any
 * flush-quiescent point -- volatile state (queue, pending_count, flushing, s_q_drained)
 * is lost; the on-disk WAL, checkpoint, cache and backend persist; the remount re-runs
 * wal_init + sync_seed and drains. converge must still hold.
 *
 * Bounded abstraction (to stay finite): one optional subdir d, NF=2 identities, NN=2
 * names per dir (4 backend slots); content is a version tag; one flusher. Directory
 * RENAME and deep nesting (a renamed parent transitively remapping a subtree's paths)
 * are a further stage -- here d is a fixed optional subdir.
 *
 * Switches: -DFIX, -DPUBLISH_GUARD, -DCRASH, -DMAXOPS=n (default 3), -DNOLTL,
 *           -DNOREADFIX (drop open_backend_for_fetch's retry -> breaks readok).
 */

#ifndef MAXOPS
#define MAXOPS 3
#endif

#define NF   2        /* file identities 0,1                        */
#define NN   2        /* name slots 0,1 (per directory)             */
#define DIRX 2        /* the subdirectory d's ind index             */
#define NIL  3        /* "no indirection" (a seeded recovery node)  */
#define ROOT 0
#define D    1
#define SLOT(dd,nn) ((dd)*NN + (nn))   /* backend slot = dir*NN + name, 0..3 */

mtype = { CREATE, MKDIR, UNLINKF, RMDIR, RENAMEF };

/* ===== the four C structs, as Promela typedefs (field-for-field) ============ */

/* struct wal_record { uint8_t op; char *path; char *path2; }
 * A path is modeled as a (dir,name) SLOT: from = source slot, to = target slot.
 * The record carries no sequence number -- its slot in the WAL is its identity. */
typedef wal_record {
	mtype op;
	byte ent;     /* the entity (identity) the op is logged on -- for the ind     */
	byte from;    /* source path SLOT (the only slot for create/unlink)           */
	byte to;      /* target path SLOT (rename only)                               */
};

/* struct ind { int pending_count; int flushing; int detached;
 *              struct omdfs_entry *entry; struct cnode *cnode; }
 * pending_count + flushing drive the gate; detached is the C's lazy-free flag that
 * meta_flush_publish reads -- the static ind table here can't carry a per-incarnation
 * detached, so the publish guard uses the epoch[] below instead. entry/cnode stay 0. */
typedef ind {
	byte pending_count;  /* appended-but-undrained structural ops for the entry */
	bool flushing;       /* a flush of this entry is in progress (Change 3)      */
	bool detached;       /* entry detached from the index (audit only; see epoch)*/
	byte entry;          /* live-entry handle (unused: static table)             */
	byte cnode;          /* owning dir node    (unused: static table)            */
};

/* struct omdfs_entry { uint8_t type; uint8_t flags; struct stat st;
 *                      char *name; char *link; struct ind *ind; }
 * Abstracted to: exists + parent dir + NAME + content/mtime version tag + dirty +
 * cached (OMDFS_F_CONTENT_CACHED). `ind` is the index of this entry's ind. */
typedef omdfs_entry {
	bool live;    /* entry exists in the namespace                             */
	byte dir;     /* ROOT or D : the entry's current parent                    */
	byte name;    /* 0 or 1    : the entry's current name slot                 */
	byte ver;     /* content version (1,2); 0 = empty                          */
	bool dirty;   /* un-synced content/attr change pending                     */
	bool cached;  /* OMDFS_F_CONTENT_CACHED: whole content is in the cache     */
	byte ind_slot;/* struct ind *ind, as a slot index (== the entity's own slot) */
};

/* struct sync_op { struct wal_record *rec; struct ind *ind; struct sync_op *next; } */
typedef sync_op {
	mtype op; byte ent; byte from; byte to;   /* *rec, owned + carried inline   */
	byte ind_slot;                             /* struct ind *ind, or NIL        */
};

/* ===== meta.c : the local index ("the truth") + per-entry indirections ======= */
omdfs_entry idx[NF];  /* struct omdfs_entry index, files 0,1       */
bool dlive;           /* subdirectory d exists (its own dir entry) */
ind inds[4];          /* struct ind table; [DIRX]=d, [NIL]=unused  */
byte epoch[NF];       /* bumped each time an identity is DROPPED (replaced/unlinked);
                         a flush snapshots it at claim and publishes only if unchanged
                         -- the incarnation-safe stand-in for the C ind's detached flag */

/* ===== backend : the durable mirror, keyed by (dir,name) slot =============== */
bool bpres[2*NN];     /* present at slot (dir,name)                */
byte bver[2*NN];      /* its content version                       */
bool bdir;            /* subdirectory d exists on the backend      */

/* ===== journal.c : the on-disk WAL + checkpoint (persist a crash) + counters = */
wal_record wal[MAXOPS+1]; /* the WAL file: one struct wal_record per slot   */
byte wal_n;           /* records in the WAL file (== g_total)       */
byte ckpt_disk;       /* journal/checkpoint file                   */
byte g_applied;       /* volatile checkpoint mirror                */
short g_last_rename = -1; /* index of the last appended RENAME, -1 = none */

/* ===== syncer.c : the in-memory structural drain queue + recovery bookkeeping = */
chan q = [MAXOPS] of { mtype, byte, byte, byte, byte }; /* a flattened sync_op */
byte qlen;            /* queue length (empty()/len() barred in LTL) */
byte s_q_drained;     /* monotonic nodes drained since (re)mount    */
byte s_recovery_target; /* nodes seeded from the on-disk WAL at mount */

bool mounted;         /* set once wal_init + sync_seed have run     */
bool fg_done;
#ifdef CRASH
bool crash_used;
#endif

/* the OTHER identity (NF==2), occupancy + "different from current slot" tests */
#define OTHER(i)             (1 - (i))
#define occ_other(i,dd,nn)   ( idx[OTHER(i)].live && idx[OTHER(i)].dir==(dd) && idx[OTHER(i)].name==(nn) )
#define notcur(i,dd,nn)      ( !(idx[i].dir==(dd) && idx[i].name==(nn)) )
#define curslot(i)           SLOT(idx[i].dir,idx[i].name)

/* flushable_locked: the real flush gate -- the entry has no pending structural op,
 * and (if it lives under d) d has none either. */
#define flushable_locked(i) ( inds[i].pending_count==0 && (idx[i].dir!=D || inds[DIRX].pending_count==0) )

#ifdef FIX
#define FLUSHING_DEFER(op,id) ( ((op)==RENAMEF || (op)==UNLINKF) && inds[id].flushing )
#define CLAIM_GATE(i)         flushable_locked(i)
#define INITIAL_DRAINED       ( s_q_drained >= s_recovery_target )
#else
#define FLUSHING_DEFER(op,id) ( false )
#define CLAIM_GATE(i)         ( true )      /* pre-fix: meta_flush_claim ignored the gate */
#define INITIAL_DRAINED       ( true )
#endif

/* open_backend_for_fetch's retry over a not-yet-drained rename (content.c). */
#ifdef NOREADFIX
#define READ_RETRY ( false )
#else
#define READ_RETRY ( true )
#endif

/* evictable: evict.c reclaims only a clean file whose CURRENT content is already durable
 * on the backend at its current path (no structural op in flight). */
#define evictable(i) ( idx[i].live && idx[i].cached && !idx[i].dirty && \
	inds[i].pending_count==0 && bpres[curslot(i)] && bver[curslot(i)]==idx[i].ver )

/* ============================== journal.c =================================== */

/* wal_append: pack + write the record durably, advance g_total, note a rename. */
inline wal_append(wop, went, wfrm, wto) {
	wal[wal_n].op = wop; wal[wal_n].ent = went; wal[wal_n].from = wfrm; wal[wal_n].to = wto;
	if :: wop == RENAMEF -> g_last_rename = wal_n :: else -> skip fi;
	wal_n++
}

/* wal_checkpoint: advance the checkpoint by one applied record; reclaim on full drain. */
inline wal_checkpoint() {
	g_applied++;
	if
	:: g_applied >= wal_n -> wal_n = 0; g_applied = 0; g_last_rename = -1; ckpt_disk = 0
	:: else -> ckpt_disk = g_applied
	fi
}

/* wal_init: recover g_applied, rescan for the last rename, clamp a stale-high ckpt. */
inline wal_init() {
	g_applied = ckpt_disk;
	g_last_rename = -1;
	k = 0;
	do
	:: k < wal_n -> if :: wal[k].op == RENAMEF -> g_last_rename = k :: else -> skip fi; k++
	:: else -> break
	od;
	if :: g_applied > wal_n -> g_applied = wal_n; ckpt_disk = g_applied :: else -> skip fi
}

#define wal_fully_drained      ( g_applied == wal_n )
#define wal_has_pending_rename ( g_last_rename >= 0 && g_last_rename >= g_applied )

/* ============================== meta.c (indirections) ====================== */

inline meta_ind_acquire(id) { inds[id].pending_count++ }

inline meta_ind_settle(id) {
	if
	:: id != NIL -> if :: inds[id].pending_count > 0 -> inds[id].pending_count-- :: else -> skip fi
	:: else -> skip
	fi
}

/* meta_flush_claim: under meta_mtx, snapshot the entry (path slot + bytes + epoch) and
 * clear dirty, gated on dirty + the recovery drain + the flush gate. */
inline meta_flush_claim(i) {
	atomic {
		idx[i].dirty && INITIAL_DRAINED && CLAIM_GATE(i) ->
		sp = idx[i].dir; sn = idx[i].name; sv = idx[i].ver; sep = epoch[i]; idx[i].dirty = false;
#ifdef FIX
		inds[i].flushing = true;
#endif
	}
}

inline meta_flush_release(i) {
#ifdef FIX
	inds[i].flushing = false;
#else
	skip
#endif
}

/* ============================== syncer.c (queue) ============================ */

inline sync_enqueue(qop, qent, qfrm, qto, qind) { q!qop,qent,qfrm,qto,qind; qlen++ }

/* syncer_log_push: durable WAL record (slots), bump pending_count, enqueue. */
inline syncer_log_push(op, ent, frm, to, id) {
	wal_append(op, ent, frm, to);
	meta_ind_acquire(id);
	sync_enqueue(op, ent, frm, to, id)
}

/* sync_seed: at (re)mount, load the un-applied WAL suffix as ind==NIL nodes. */
inline sync_seed() {
	s_recovery_target = 0;
	k = g_applied;
	do
	:: k < wal_n -> sync_enqueue(wal[k].op, wal[k].ent, wal[k].from, wal[k].to, NIL);
	                s_recovery_target++; k++
	:: else -> break
	od
}

/* ============================== syncer.c (apply) ============================ */

/* apply_structural: one drained record applied to the backend, no lock held. a/b are
 * SLOTs; a slot's dir is slot/NN. A rename OVERWRITES its target slot (POSIX replace). */
inline apply_structural(op, ent, a, b) {
	if
	:: op == MKDIR  -> bdir = true
	:: op == RMDIR  ->
#ifndef CRASH
	   assert(!bpres[SLOT(D,0)] && !bpres[SLOT(D,1)]);   /* dir empty */
#endif
	   bdir = false
	:: op == CREATE ->
	   if
	   :: bpres[a] -> skip                               /* EEXIST: no-op */
	   :: else ->
#ifndef CRASH
	      if :: (a/NN) == D -> assert(bdir) :: else -> skip fi;  /* parent exists */
#endif
	      bpres[a] = true; bver[a] = 0                   /* created empty */
	   fi
	:: op == UNLINKF -> bpres[a] = false; bver[a] = 0    /* idempotent */
	:: op == RENAMEF ->
	   if
	   :: bpres[a] ->
#ifndef CRASH
	      if :: (b/NN) == D -> assert(bdir) :: else -> skip fi;  /* target parent */
#endif
	      bpres[b] = bpres[a]; bver[b] = bver[a];        /* overwrite target (replace) */
	      bpres[a] = false; bver[a] = 0
	   :: else -> skip                                   /* ENOENT (already moved): no-op */
	   fi
	fi
}

/* phase_structural: drain one queue node in order; defer a rename/unlink of a flushing
 * entity; else pop, apply off the lock, settle the ind, advance the checkpoint. */
inline phase_structural() {
	atomic {
		q?<op,ent,a,b,id> ->
		if
		:: FLUSHING_DEFER(op,id) -> skip
		:: else ->
		   q?op,ent,a,b,id; qlen--;
		   apply_structural(op,ent,a,b);
		   meta_ind_settle(id);
		   s_q_drained++;
		   wal_checkpoint()
		fi
	}
}

/* ============================== syncer.c (flush) =========================== */

/* copy_file: write the snapshot bytes to the backend at the snapshot path slot, off the
 * lock. PUBLISH_GUARD makes the publish atomic with a check that the entity wasn't
 * dropped since the claim (epoch unchanged) -- meta_flush_publish: a superseded flush
 * (its entity replaced/unlinked mid-flush) discards instead of clobbering the successor. */
inline copy_file(i) {
#ifdef PUBLISH_GUARD
	atomic {
		if
		:: epoch[i] == sep ->
#ifndef CRASH
		   if :: sp == D -> assert(bdir) :: else -> skip fi;
#endif
		   bpres[SLOT(sp,sn)] = true; bver[SLOT(sp,sn)] = sv
		:: else -> skip                               /* superseded: discard */
		fi
	}
#else
#ifndef CRASH
	if :: sp == D -> assert(bdir) :: else -> skip fi;
#endif
	bpres[SLOT(sp,sn)] = true; bver[SLOT(sp,sn)] = sv
#endif
}

/* flush_entry: meta_flush_claim -> copy_file (off the lock) -> meta_flush_release. */
inline flush_entry(i) {
	meta_flush_claim(i);
	copy_file(i);
	meta_flush_release(i)
}

/* ============================== content.c (read / fetch) ==================== */

inline meta_mark_cached(i) { idx[i].cached = true }

/* open_backend_for_fetch: open BACKEND/<current path>; while ENOENT and a rename is
 * still pending, kick the syncer and retry (READ_RETRY). Then take the bytes, or rho=0. */
inline open_backend_for_fetch(i) {
	do
	:: !bpres[curslot(i)] && wal_has_pending_rename && READ_RETRY ->
	   skip                                       /* syncer_kick + 100ms, then retry */
	:: else -> break
	od;
	if
	:: bpres[curslot(i)] -> rho = bver[curslot(i)]   /* opened: the bytes */
	:: else              -> rho = 0                  /* ENOENT: no bytes  */
	fi
}

inline fetch_thread(i) {
	open_backend_for_fetch(i);
	meta_mark_cached(i)
}

inline content_open(i) {
	if
	:: idx[i].cached -> rho = idx[i].ver          /* warm hit: straight from the cache */
	:: else          -> fetch_thread(i)           /* miss: fetch from the backend      */
	fi
}

inline content_read(i) { assert(rho == idx[i].ver) }

inline do_read(i) {
	content_open(i);
	content_read(i)
}

inline evict_one(i) { idx[i].cached = false }

/* ============================== foreground mutators ========================= */

/* drop an identity: it ceases to exist; bump its epoch so any in-flight flush of it is
 * superseded at publish (the ind detaches in the C), and cancel its un-started flush. */
inline drop(j) { idx[j].live = false; epoch[j]++; idx[j].dirty = false }

inline mk(i, dd, nn) {
	idx[i].live=true; idx[i].dir=dd; idx[i].name=nn; idx[i].ver=1; idx[i].ind_slot=i;
	idx[i].cached=true; syncer_log_push(CREATE,i,SLOT(dd,nn),0,i)
}
inline do_create(i) {
	if
	:: !occ_other(i,ROOT,0)        -> atomic { mk(i,ROOT,0) }
	:: !occ_other(i,ROOT,1)        -> atomic { mk(i,ROOT,1) }
	:: dlive && !occ_other(i,D,0)  -> atomic { mk(i,D,0) }
	:: dlive && !occ_other(i,D,1)  -> atomic { mk(i,D,1) }
	fi;
	idx[i].dirty = true                   /* published AFTER the append + bump */
}

/* content write: bytes land in the cache (cached), entry goes dirty, no WAL record */
inline do_write(i)  { idx[i].ver = 3 - idx[i].ver; idx[i].cached = true; idx[i].dirty = true }

/* rename i onto (dd,nn): if occupied by the other identity, REPLACE it (drop). The WAL
 * rename is logged on i; the occupant's removal is implicit (its path is overwritten),
 * exactly as meta_move_entry drops the destination. */
inline mv(i, dd, nn) {
	if :: occ_other(i,dd,nn) -> drop(OTHER(i)) :: else -> skip fi;
	idx[i].cached = true;     /* content_prefetch(from): cache the bytes FROM THE SOURCE
	                             path before the rename, so a later read is a warm hit and
	                             never fetches the (about-to-be-replaced) destination's
	                             stale bytes -- omdfs_rename, main.c. */
	syncer_log_push(RENAMEF,i,curslot(i),SLOT(dd,nn),i);
	assert(wal_has_pending_rename);
	idx[i].dir=dd; idx[i].name=nn
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
	atomic { idx[i].live=false; syncer_log_push(UNLINKF,i,curslot(i),0,i); epoch[i]++; idx[i].dirty=false }
}
inline do_mkdir() {
	atomic { dlive=true; syncer_log_push(MKDIR,DIRX,0,0,DIRX) }
}
inline do_rmdir() {
	atomic { dlive=false; syncer_log_push(RMDIR,DIRX,0,0,DIRX) }
}

#define live_in_D ( (idx[0].live && idx[0].dir==D) || (idx[1].live && idx[1].dir==D) )

active proctype foreground() {
	byte n = 0;
	byte rho;            /* bytes a read sees (cache hit value / fetched version) */
	(mounted);
	do
	:: n < MAXOPS ->
	   n++;
	   if
	   :: !idx[0].live                             -> do_create(0)
	   :: !idx[1].live                             -> do_create(1)
	   :: idx[0].live                              -> do_write(0)
	   :: idx[1].live                              -> do_write(1)
	   :: idx[0].live                              -> do_rename(0)
	   :: idx[1].live                              -> do_rename(1)
	   :: idx[0].live                              -> do_unlink(0)
	   :: idx[1].live                              -> do_unlink(1)
	   :: idx[0].live                              -> do_read(0)
	   :: idx[1].live                              -> do_read(1)
	   :: evictable(0)                             -> evict_one(0)
	   :: evictable(1)                             -> evict_one(1)
	   :: !dlive                           -> do_mkdir()
	   :: dlive && !live_in_D              -> do_rmdir()
	   fi
	:: break
	od;
	fg_done = true
}

/* ===== syncer.c phase 1: the structural-drain loop ========================= */
active proctype phase1() {
	mtype op; byte ent, a, b, id;
	(mounted);
	do
	:: phase_structural()
	:: atomic { qlen==0 && fg_done -> break }
	od
}

/* ===== syncer.c phase 2: the content/attr flush loop ======================= */
active proctype phase2() {
	byte sp, sn, sv, sep;
	(mounted);
	do
	:: flush_entry(0)
	:: flush_entry(1)
	:: atomic { fg_done && qlen==0 && !idx[0].dirty && !idx[1].dirty -> break }
	od
}

/* ===== mount: syncer_start -> wal_init + sync_seed, then the threads run ===== */
init {
	byte k;
	atomic { wal_init(); sync_seed(); mounted = true }
}

#ifdef CRASH
/* A one-shot crash + remount, after the foreground is done, at a flush-quiescent point. */
active proctype crasher() {
	byte k; mtype op; byte ent, a, b, id;
	atomic {
		fg_done && !crash_used && !inds[0].flushing && !inds[1].flushing ->
		crash_used = true;
		do :: q?op,ent,a,b,id -> qlen-- :: empty(q) -> break od;   /* drop the in-memory queue */
		inds[0].pending_count=0; inds[1].pending_count=0; inds[DIRX].pending_count=0;
		inds[0].flushing=false; inds[1].flushing=false;
		s_q_drained = 0;
		wal_init();                                               /* recover counters */
		sync_seed()                                               /* re-seed the suffix */
	}
}
#endif

/* ============================== correctness ================================= */
#ifdef CRASH
#define POSTCRASH ( crash_used )
#else
#define POSTCRASH ( true )
#endif

#define quiesced ( POSTCRASH && fg_done && qlen==0 && \
	inds[0].pending_count==0 && inds[1].pending_count==0 && inds[DIRX].pending_count==0 && \
	!idx[0].dirty && !idx[1].dirty && !inds[0].flushing && !inds[1].flushing )

/* identity i is the live occupant of slot s (at most one identity per slot) */
#define at(i,s) ( idx[i].live && curslot(i)==(s) )
/* slot s converged: holds its live occupant's bytes, or is empty if no live occupant */
#define slotconv(s) ( \
	( !at(0,s) || (bpres[s] && bver[s]==idx[0].ver) ) && \
	( !at(1,s) || (bpres[s] && bver[s]==idx[1].ver) ) && \
	( at(0,s) || at(1,s) || !bpres[s] ) )

#define converged ( slotconv(0) && slotconv(1) && slotconv(2) && slotconv(3) && \
	(bdir == dlive) && wal_fully_drained )

#ifndef NOLTL
ltl converge { [] (quiesced -> converged) }
#endif
