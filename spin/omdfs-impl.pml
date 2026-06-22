/*
 * omdfs-impl.pml -- function-by-function SPIN model of the omdfs write-back core.
 *
 * Where omdfs-syncer.pml models the *algorithm* abstractly, this model mirrors the
 * actual C, function by function, across the three concurrency-critical modules:
 *
 *   journal.c  (the structural write-ahead log: on-disk records + checkpoint)
 *   meta.c     (the per-entry metadata index + indirections that gate the flush)
 *   syncer.c   (the two-phase background syncer + crash-recovery seeding)
 *   content.c  (the read/fetch path: a cold read pulls bytes from the backend)
 *
 * Every significant C function appears below as an inline of the SAME name, and each
 * caller CALLS it -- nothing is merged into its caller. So the call graph reads like
 * the C: a fuse op -> syncer_log_push -> wal_append + meta_ind_acquire + sync_enqueue;
 * phase 1 -> phase_structural -> apply_structural + meta_ind_settle + wal_checkpoint;
 * phase 2 -> flush_entry -> meta_flush_claim + copy_file + meta_flush_release.
 *
 * The shared state maps 1:1 to the C data structures -- the four heap structs are
 * Promela typedefs of the same name, field for field (see the typedef block below):
 *
 *   C                                  model
 *   -------------------------------    -------------------------------------------
 *   struct wal_record (on-disk WAL)    wal_record wal[] + wal_n                (persist)
 *   journal/checkpoint                 ckpt_disk                              (persist)
 *   g_total / g_applied / g_last_..    wal_n / g_applied / g_last_rename       (volatile)
 *   struct ind {pending_count,...}     ind inds[]                             (volatile)
 *   struct omdfs_entry (the index)     omdfs_entry idx[] + dlive             (persist)
 *   struct sync_op (drain queue)       chan q + qlen   (each msg = a flattened sync_op)
 *   backend filesystem                 bpres[]/bver[]/bdir
 *
 * meta_mtx / g_wal critical sections are atomic{}; copy_file and the backend
 * structural ops run with NO lock held (the real concurrency surface).
 *
 * Verified, over EVERY interleaving of EVERY foreground op program up to MAXOPS:
 *   converge -- at quiescence the backend exactly equals the live local namespace
 *               (right bytes at the right path, no orphan copies, deletes gone,
 *               backend dir matches) AND the WAL is fully drained. One property that
 *               subsumes no-clobber, no-lost-update, deletes-propagate, WAL ordering.
 *   ordering -- asserts in apply_structural (parent exists before a create/rename
 *               into it, rename source present, rmdir only on an empty dir).
 *   readok   -- a read of a live file always yields its CURRENT content, never a
 *               spurious ENOENT and never stale bytes -- even when the file was
 *               evicted and then renamed, so the cold fetch must wait out the
 *               not-yet-drained backend rename (content_read's assert). Reads
 *               never touch the WAL/queue/dirty/backend, so they leave converge
 *               untouched -- this checks the fetch's own correctness.
 *   + SPIN invalid-end-state => no deadlock.
 *
 * Crash recovery (-DCRASH): once the foreground is done, a single crash may fire at
 * any flush-quiescent point -- the volatile state (the in-memory queue, pending_count,
 * flushing, s_q_drained) is lost; the on-disk WAL, the checkpoint, the cache (dirty
 * flags + content) and the backend persist. The remount re-runs wal_init + sync_seed
 * and drains. This exercises journal.c's recovery path (the checkpoint clamp, the
 * applied-suffix replay) and syncer.c's recovery guard (a recovered rename carries no
 * pending_count, so the per-entry gate is blind to it -- s_initial_drained holds the
 * content flush until the recovered WAL drains once). converge must still hold.
 *
 * Bounded abstraction (to stay finite): one optional subdir d + NF=2 file identities,
 * each under root or d; content is a version tag; one flusher process (different
 * entries flush independently; the same entry never flushes twice at once -- the claim
 * clears dirty). The async index-writer's persistence timing (meta_schedule_save) is
 * abstracted: a claimed dirty flag is treated as durably synced once the backend copy
 * lands, and crash is modeled only at flush-quiescent points -- the dirty-clear-vs-
 * backend-copy durability window is an async-persist question, out of scope here (the
 * cache stays the truth across a crash). The CRC/torn-tail framing in journal.c is a
 * serialization detail, abstracted as an atomic append. meta_flush_claim's C return
 * code (0/1/2/-errno) is abstracted to its gate: the claim is enabled iff claimable.
 *
 * Switches: -DFIX       model the fixed core (omit for the pre-exclusion buggy one)
 *           -DCRASH     enable the one-shot crash + remount
 *           -DMAXOPS=n  foreground program length (default 3)
 *           -DNOLTL     build the deadlock/assert check with the never-claim off
 *           -DNOREADFIX drop open_backend_for_fetch's retry, so a cold read racing
 *                       a not-yet-drained rename wrongly ENOENTs (breaks readok)
 */

#ifndef MAXOPS
#define MAXOPS 3
#endif

#define NF   2        /* file identities 0,1                        */
#define DIRX 2        /* the subdirectory d's ind index             */
#define NIL  3        /* "no indirection" (a seeded recovery node)  */
#define ROOT 0
#define D    1
#define SLOT(dd,ii) ((dd)*NF + (ii))   /* backend file slot = dir*NF + name */

mtype = { CREATE, MKDIR, UNLINKF, RMDIR, RENAMEF };

/* ===== the four C structs, as Promela typedefs (field-for-field) ============ */

/* struct wal_record { uint8_t op; char *path; char *path2; }
 * A path is modeled as (entity, dir): path = (ent, from), path2 = (ent, to).
 * The record carries no sequence number -- its slot in the WAL is its identity. */
typedef wal_record {
	mtype op;
	byte ent;     /* the entity named by path / path2                          */
	byte from;    /* path's directory (ROOT/D); the source dir for a rename    */
	byte to;      /* path2's directory; the target dir for a rename            */
};

/* struct ind { int pending_count; int flushing; int detached;
 *              struct omdfs_entry *entry; struct cnode *cnode; }
 * The per-entry exclusion object. pending_count + flushing drive the gate and
 * are fully exercised; detached/entry/cnode are the lazy-free flag + the
 * live-entry handle (in the C, the handle is read only by the NDEBUG audit and
 * detached only by the lazy free). The model's ind table is static -- never
 * freed or relocated -- so those three stay 0, present here to match the struct. */
typedef ind {
	byte pending_count;  /* appended-but-undrained structural ops for the entry */
	bool flushing;       /* a flush of this entry is in progress (Change 3)      */
	bool detached;       /* entry detached from the index (unused: static table) */
	byte entry;          /* live-entry handle (unused: static table)             */
	byte cnode;          /* owning dir node    (unused: static table)            */
};

/* struct omdfs_entry { uint8_t type; uint8_t flags; struct stat st;
 *                      char *name; char *link; struct ind *ind; }
 * type/flags/st/name/link are abstracted to: exists + parent dir + a content/
 * mtime version tag + dirty + cached (flags' OMDFS_F_CONTENT_CACHED bit: the whole
 * file's bytes are in the local cache). `ind` is the index of this entry's ind. */
typedef omdfs_entry {
	bool live;    /* entry exists in the namespace                             */
	byte dir;     /* ROOT or D : the entry's current parent                    */
	byte ver;     /* content version (1,2); 0 = empty                          */
	bool dirty;   /* un-synced content/attr change pending                     */
	bool cached;  /* OMDFS_F_CONTENT_CACHED: whole content is in the cache     */
	byte ind_slot;/* struct ind *ind, as a slot index (== the entry's own slot;
	                 named _slot because Promela bars a member named like a type) */
};

/* struct sync_op { struct wal_record *rec; struct ind *ind; struct sync_op *next; }
 * A drain-queue node. The FIFO + `next` are modeled by the channel `q` below; a
 * channel cannot carry a typedef field, so each message is this struct flattened:
 * the owned *rec (op,ent,from,to) inline, then the *ind slot (NIL for recovery). */
typedef sync_op {
	mtype op; byte ent; byte from; byte to;   /* *rec, owned + carried inline   */
	byte ind_slot;                             /* struct ind *ind, or NIL        */
};

/* ===== meta.c : the local index ("the truth") + per-entry indirections ======= */
omdfs_entry idx[NF];  /* struct omdfs_entry index, files 0,1       */
bool dlive;           /* subdirectory d exists (its own dir entry) */
ind inds[4];          /* struct ind table; [DIRX]=d, [NIL]=unused  */

/* ===== backend : the durable mirror ========================================= */
bool bpres[2*NF];     /* file present at slot (dir,name)            */
byte bver[2*NF];      /* its content version                       */
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

/* flushable_locked: the real flush gate -- the entry has no pending structural op,
 * and (if it lives under d) d has none either. */
#define flushable_locked(i) ( inds[i].pending_count==0 && (idx[i].dir!=D || inds[DIRX].pending_count==0) )

/* The fix, isolated to three macros (DESIGN.md changes 2 & 3 + the recovery guard):
 * phase 1 defers a rename/unlink of a flushing entry; the claim consults the gate;
 * the flush waits for the initial recovery drain. The buggy build neuters all three. */
#ifdef FIX
#define FLUSHING_DEFER(op,id) ( ((op)==RENAMEF || (op)==UNLINKF) && inds[id].flushing )
#define CLAIM_GATE(i)         flushable_locked(i)
#define INITIAL_DRAINED       ( s_q_drained >= s_recovery_target )
#else
#define FLUSHING_DEFER(op,id) ( false )
#define CLAIM_GATE(i)         ( true )      /* pre-fix: meta_flush_claim ignored the gate */
#define INITIAL_DRAINED       ( true )
#endif

/* open_backend_for_fetch's retry over a not-yet-drained rename (content.c). Always
 * on in the C; -DNOREADFIX drops it to a single attempt to expose the read race. */
#ifdef NOREADFIX
#define READ_RETRY ( false )
#else
#define READ_RETRY ( true )
#endif

/* evictable: evict.c only reclaims a clean file whose CURRENT content is already
 * durable on the backend at its current path (no structural op in flight). So a
 * later cold read's sole obstacle is locating the bytes after a rename drains --
 * never genuinely-missing data. */
#define evictable(i) ( idx[i].live && idx[i].cached && !idx[i].dirty && \
	inds[i].pending_count==0 && bpres[SLOT(idx[i].dir,i)] && bver[SLOT(idx[i].dir,i)]==idx[i].ver )

/* ============================== journal.c =================================== */

/* wal_append: pack + write the record durably, advance g_total, note a rename.
 * (params spelled w* so they can't textually collide with the same-named struct
 * members on the left-hand sides -- inline args are substituted as raw tokens.) */
inline wal_append(wop, went, wfrm, wto) {
	wal[wal_n].op = wop; wal[wal_n].ent = went; wal[wal_n].from = wfrm; wal[wal_n].to = wto;
	if :: wop == RENAMEF -> g_last_rename = wal_n :: else -> skip fi;
	wal_n++
}

/* wal_checkpoint: advance the checkpoint by one applied record (the C batches per
 * cycle; per-record here covers the batched-replay window too). On full drain,
 * reclaim -- truncate the WAL first, then reset the checkpoint (the ordering that
 * makes a crash in the window clamp safely, see wal_init). */
inline wal_checkpoint() {
	g_applied++;
	if
	:: g_applied >= wal_n -> wal_n = 0; g_applied = 0; g_last_rename = -1; ckpt_disk = 0
	:: else -> ckpt_disk = g_applied
	fi
}

/* wal_init: recover g_applied from the checkpoint file, rescan the WAL for the
 * record total + last rename, and clamp a stale-high checkpoint (a crash in the
 * reclaim window left {empty WAL, stale ckpt} -- provably everything was applied). */
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

/* meta_ind_acquire: bump the entry's pending_count when an op is logged for it. */
inline meta_ind_acquire(id) { inds[id].pending_count++ }

/* meta_ind_settle: a drained op drops the entry's pending_count (NIL = recovery). */
inline meta_ind_settle(id) {
	if
	:: id != NIL -> if :: inds[id].pending_count > 0 -> inds[id].pending_count-- :: else -> skip fi
	:: else -> skip
	fi
}

/* meta_flush_claim: under meta_mtx, snapshot the entry and clear dirty, gated on the
 * entry being dirty, the initial recovery drain done, and the flush gate open. In the
 * fixed build it also sets flushing so phase 1 defers a concurrent rename/unlink. The
 * guarded atomic models the C return code: the claim is enabled iff a flush is owed. */
inline meta_flush_claim(i) {
	atomic {
		idx[i].dirty && INITIAL_DRAINED && CLAIM_GATE(i) ->
		sp = idx[i].dir; sv = idx[i].ver; idx[i].dirty = false;
#ifdef FIX
		inds[i].flushing = true;
#endif
	}
}

/* meta_flush_release: clear the flushing flag once copy_file has landed. */
inline meta_flush_release(i) {
#ifdef FIX
	inds[i].flushing = false;
#else
	skip
#endif
}

/* ============================== syncer.c (queue) ============================ */

/* sync_enqueue: append a node carrying ownership of the record + its ind handle.
 * (formals spelled q* so a member-access actual like wal[k].op can't clash.) */
inline sync_enqueue(qop, qent, qfrm, qto, qind) { q!qop,qent,qfrm,qto,qind; qlen++ }

/* syncer_log_push: the foreground path -- durable WAL record, then bump the entry's
 * pending_count (meta_ind_acquire), then enqueue the drain node. The dirty flag is
 * published by the caller AFTER this returns (Change 1), so the gate sees
 * pending_count before any flush can see dirty. */
inline syncer_log_push(op, ent, frm, to, id) {
	wal_append(op, ent, frm, to);
	meta_ind_acquire(id);
	sync_enqueue(op, ent, frm, to, id)
}

/* sync_seed: at (re)mount, load the un-applied WAL suffix records[g_applied:] as
 * ind==NIL nodes (recovered ops drain by path; no live entry/flush exists yet).
 * Returns, via s_recovery_target, the count seeded. */
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

/* apply_structural: one drained record applied to the backend, no lock held.
 * Idempotent exactly as the C is (EEXIST/ENOENT treated as success) so a recovery
 * replay of an already-applied prefix is a safe no-op. The ordering asserts hold in
 * the live (non-crash) run, where the in-order WAL guarantees them. */
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
	   :: bpres[SLOT(a,ent)] -> skip                     /* EEXIST: no-op */
	   :: else ->
#ifndef CRASH
	      if :: a == D -> assert(bdir) :: else -> skip fi;  /* parent exists */
#endif
	      bpres[SLOT(a,ent)] = true; bver[SLOT(a,ent)] = 0  /* created empty */
	   fi
	:: op == UNLINKF -> bpres[SLOT(a,ent)] = false; bver[SLOT(a,ent)] = 0  /* idempotent */
	:: op == RENAMEF ->
	   if
	   :: bpres[SLOT(a,ent)] ->
#ifndef CRASH
	      if :: b == D -> assert(bdir) :: else -> skip fi;  /* target parent */
#endif
	      bpres[SLOT(b,ent)] = bpres[SLOT(a,ent)]; bver[SLOT(b,ent)] = bver[SLOT(a,ent)];
	      bpres[SLOT(a,ent)] = false; bver[SLOT(a,ent)] = 0
	   :: else -> skip                                   /* ENOENT (already moved): no-op */
	   fi
	fi
}

/* phase_structural: drain one queue node in order. Peek the head (stable: only this
 * proc pops); if it would rename/unlink a flushing entry, defer it; else pop, apply
 * to the backend off the meta lock, settle the ind, and advance the checkpoint. */
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

/* copy_file: write the snapshot bytes to the backend at the snapshot path. Runs with
 * NO lock held -- the real off-lock concurrency surface. */
inline copy_file(i) {
#ifndef CRASH
	if :: sp == D -> assert(bdir) :: else -> skip fi;   /* parent exists at push time */
#endif
	bpres[SLOT(sp,i)] = true; bver[SLOT(sp,i)] = sv;
}

/* flush_entry: meta_flush_claim (gated snapshot under the lock) -> copy_file (off the
 * lock) -> meta_flush_release. As a phase-2 do-branch, the claim's gate is the branch
 * guard, so a non-claimable entry simply isn't selected. */
inline flush_entry(i) {
	meta_flush_claim(i);
	copy_file(i);
	meta_flush_release(i)
}

/* ============================== content.c (read / fetch) ==================== */
/* The read side. A read of an UN-cached (evicted) file fetches it from the backend
 * by the entry's CURRENT path. If a rename was logged in the cache but hasn't
 * drained to the backend yet, the file still sits at its OLD backend path, so the
 * fetch's open() would ENOENT -- open_backend_for_fetch kicks the syncer and retries
 * while a rename is pending, waiting out the drain instead of failing. That's the
 * read-vs-rename race. Abstractions: a single foreground reader (the C's shared
 * in-flight fetch list / FETCHING dedup is for many openers of one path, not this
 * race, so it's out of scope); the bounded 150x100ms retry is abstracted to "block
 * until the pending rename drains" (the backend is up and the rename always
 * eventually drains), as other timing is abstracted. rho is the bytes the read sees. */

/* meta_mark_flags(...,CONTENT_CACHED): the whole content is now local. */
inline meta_mark_cached(i) { idx[i].cached = true }

/* open_backend_for_fetch: open BACKEND/<current path>; while it's ENOENT and a
 * rename is still pending in the WAL, kick the syncer and retry (READ_RETRY). Then
 * take the bytes, or rho=0 (ENOENT, no bytes) if it still isn't there. */
inline open_backend_for_fetch(i) {
	do
	:: !bpres[SLOT(idx[i].dir,i)] && wal_has_pending_rename && READ_RETRY ->
	   skip                                       /* syncer_kick + 100ms, then retry */
	:: else -> break
	od;
	if
	:: bpres[SLOT(idx[i].dir,i)] -> rho = bver[SLOT(idx[i].dir,i)]   /* opened: the bytes */
	:: else                      -> rho = 0                          /* ENOENT: no bytes  */
	fi
}

/* fetch_thread: copy BACKEND/<path> into the cache (off the meta lock), then mark
 * the entry cached so future opens are warm hits. */
inline fetch_thread(i) {
	open_backend_for_fetch(i);
	meta_mark_cached(i)
}

/* content_open (O_RDONLY path): consult the index -- a warm hit serves from the
 * cache file, a miss runs the fetch. */
inline content_open(i) {
	if
	:: idx[i].cached -> rho = idx[i].ver          /* warm hit: straight from the cache */
	:: else          -> fetch_thread(i)           /* miss: fetch from the backend      */
	fi
}

/* content_read: return the bytes for the handle. A read of a live file must yield
 * its CURRENT content -- never a pending-rename ENOENT, never stale. */
inline content_read(i) { assert(rho == idx[i].ver) }

/* do_read: the FUSE read path -- omdfs_open(O_RDONLY)=content_open, then
 * omdfs_read=content_read. Touches no WAL/queue/dirty/backend state. */
inline do_read(i) {
	content_open(i);
	content_read(i)
}

/* evict_one: reclaim a clean, fully-synced file's cache bytes (evict.c trims the
 * cold LRU tail). Guarded by evictable() so only durable-on-the-backend content is
 * dropped -- the precondition that makes a later cold read possible. */
inline evict_one(i) { idx[i].cached = false }

/* ============================== foreground mutators ========================= */
/* Each = local index apply + syncer_log_push (WAL + meta_ind_acquire + enqueue),
 * then publish dirty (for content ops). */
inline do_create(i) {
	if
	:: atomic { idx[i].live=true; idx[i].dir=ROOT; idx[i].ver=1; idx[i].ind_slot=i;
	            idx[i].cached=true; syncer_log_push(CREATE,i,ROOT,0,i) }
	:: dlive ->
	   atomic { idx[i].live=true; idx[i].dir=D; idx[i].ver=1; idx[i].ind_slot=i;
	            idx[i].cached=true; syncer_log_push(CREATE,i,D,0,i) }
	fi;
	idx[i].dirty = true                   /* published AFTER the append + bump */
}
/* content write: bytes land in the cache (cached), entry goes dirty, no WAL record */
inline do_write(i)  { idx[i].ver = 3 - idx[i].ver; idx[i].cached = true; idx[i].dirty = true }
inline do_rename(i) {
	if
	:: idx[i].dir == D ->
	   atomic { idx[i].dir=ROOT; syncer_log_push(RENAMEF,i,D,ROOT,i); assert(wal_has_pending_rename) }
	:: idx[i].dir == ROOT && dlive ->
	   atomic { idx[i].dir=D; syncer_log_push(RENAMEF,i,ROOT,D,i); assert(wal_has_pending_rename) }
	fi
}
inline do_unlink(i) {
	atomic { idx[i].live=false; syncer_log_push(UNLINKF,i,idx[i].dir,0,i); idx[i].dirty=false }
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
	   :: idx[0].live && (idx[0].dir==D || dlive)  -> do_rename(0)
	   :: idx[1].live && (idx[1].dir==D || dlive)  -> do_rename(1)
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
	byte sp, sv;
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
/* A one-shot crash + remount. Fires once, after the foreground is done, at a
 * flush-quiescent point (no flush mid-claim). Volatile state is lost; the WAL,
 * checkpoint, cache (dirty flags survive -- the cache is the truth) and backend
 * persist; the remount re-runs wal_init + sync_seed. */
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

/* a live entry: its bytes are on the backend at its current path, and nowhere else */
#define econv(i) ( !idx[i].live || ( bpres[SLOT(idx[i].dir,i)] && bver[SLOT(idx[i].dir,i)]==idx[i].ver \
	                             && !bpres[SLOT((1-idx[i].dir),i)] ) )
/* a removed entry: gone from the backend entirely */
#define edead(i) ( idx[i].live || ( !bpres[SLOT(ROOT,i)] && !bpres[SLOT(D,i)] ) )

#define converged ( econv(0) && econv(1) && edead(0) && edead(1) && \
	(bdir == dlive) && wal_fully_drained )

#ifndef NOLTL
ltl converge { [] (quiesced -> converged) }
#endif
