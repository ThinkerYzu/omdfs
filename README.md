# omdfs — Only Me Distributed File System

omdfs is a FUSE filesystem that puts a size-bounded, write-back local cache in
front of an existing network-mounted filesystem (such as sshfs or NFS). It gives
you **local-disk speed for your working set** and **the full capacity of the
remote store** for everything else.

It is designed for one specific situation: your local disk is small, your home
NAS is large, and accessing the NAS directly is slow — directory listings cost a
round-trip per file, and every read hits the network even for files you just
used. omdfs caches recently-used file contents and directory metadata locally,
evicts cold data to stay within a budget, and writes everything back to the
remote in the background.

> **Single-writer by design.** omdfs assumes this client is the *only* writer to
> the remote store. Other machines may read it, but must not write it. omdfs does
> not provide cross-client consistency and does not resolve conflicts — it
> optimizes consistency for one writing client only.

## Why omdfs

- **Capacity decoupling** — see the whole remote namespace through a mountpoint
  while keeping only a bounded working set on local disk.
- **Fast metadata** — directory listings and `stat`s for cached directories are
  served from local disk, with no per-file round-trip to the remote.
- **Fast warm reads** — recently-used files are read at local-disk speed.
- **Non-blocking writes** — changes apply locally and immediately; they sync to
  the remote in the background.
- **Crash-safe** — un-synced writes survive a crash and are flushed on restart.

## How it works

omdfs sits between your applications and an already-mounted backend filesystem.
It implements **no network protocol of its own** — it does ordinary POSIX I/O
against the backend mount (sshfs, NFS, …).

```
        applications
            │  POSIX syscalls
            ▼
   ┌──────────────────┐   mountpoint (what you see)
   │  omdfs (FUSE)    │
   │  ┌────────────┐  │     cache miss / build index
   │  │ metadata    │──┼──────────────────────────────┐
   │  │ index       │  │                               │
   │  │ content     │  │     background write-back      │
   │  │ cache       │──┼──────────────────────────────┤
   │  │ journal     │  │                               ▼
   │  └────────────┘  │   ┌─────────────────────────────────────┐
   └──────────────────┘   │  backend: mounted network FS         │
                          │  (sshfs / nfs / …), plain POSIX I/O   │
   data directory:        └─────────────────────────────────────┘
     DATADIR/
       config           configuration
       cache/           mirrors the backend tree (cached file contents +
                        per-directory metadata index files)
       journal/         write-ahead log + checkpoint
       state/           cache size / LRU bookkeeping
```

Three things make it fast and safe:

1. **Per-directory metadata index.** Each cached directory has a local-only index
   file recording the metadata of the directory and its children. Listings and
   stats are served from it, so a cached directory needs zero per-file round-trips
   to the backend. The index is regenerable and is never written to the backend.

2. **Whole-file cache with progressive read.** Files are cached, evicted, and
   synced as whole files. On a miss, the whole file is fetched in the background,
   but reads of the already-fetched portion return immediately — you can start
   reading the head of a large file before the tail arrives.

3. **Write-back with a crash-safe journal.** Structural changes
   (create/mkdir/unlink/rmdir/rename/symlink) are recorded in an ordered
   write-ahead log; content and attribute changes ride as dirty flags on the
   metadata entry (so they survive renames). A background syncer applies the log
   to the backend in order, then flushes dirty contents. Durability is provided
   by periodic checkpoints; the journal is always recoverable after a crash.

When the cache exceeds its size budget, least-recently-used **clean** whole files
are evicted (never dirty, open, or in-flight data) and transparently re-fetched
on next access.

## Build

Requires **libfuse 3** and a C11 toolchain:

```bash
# Debian / Ubuntu
sudo apt install libfuse3-dev pkg-config build-essential

make
```

## Usage

```bash
# Mount: cache the backend at /mnt/nas, store cache/journal under ~/.omdfs/data,
# expose the filesystem at ~/nas
omdfs --backend /mnt/nas --datadir ~/.omdfs/data ~/nas

# Run in the foreground (useful while developing)
omdfs --backend /mnt/nas --datadir ~/.omdfs/data -f ~/nas

# Unmount
fusermount3 -u ~/nas
```

| Option | Meaning |
|--------|---------|
| `--backend <dir>` | the already-mounted network filesystem to cache |
| `--datadir <dir>` | local directory for the cache, journal, and state |
| `<mountpoint>` | where omdfs exposes the filesystem |

### Forcing a resync

If the cache and backend drift, reconcile the whole cache onto the backend
(force-push every cached entry's existence, content, and attributes regardless of
dirty flags):

```bash
# offline — run while UNMOUNTED so the live syncer isn't also writing the backend
omdfs --resync --backend /mnt/nas --datadir ~/.omdfs/data

# live — on a running mount, trigger the same reconcile on the syncer thread:
touch ~/.omdfs/data/state/resync
```

The live trigger is one-shot: the syncer thread consumes the file within one sync
interval, runs the reconcile on its own thread (never concurrently with the live
drain), and reports the outcome in `state/status` (`last-resync`,
`last-resync-result`).

### Reclaiming cold directory metadata

When a cache budget is set, a cold directory whose cache directory holds nothing
but its `.omdfs.dir` index (no cached files, no cached subdirectories) and has no
un-synced children has that index reclaimed and the cache directory removed; the
listing rebuilds from the backend on next access. It runs automatically (about
once a minute, only when the sync is fully caught up) and can be forced:

```bash
touch ~/.omdfs/data/state/cold-evict
```

`OMDFS_COLD_META=0`/`1` disables/forces it regardless of the budget; the count
reclaimed since mount is reported in `state/status` as `cold-meta-evicted`.

### Pausing the background flush

To hold back the dirty content/attribute push to the backend — e.g. during backend
maintenance, or to stop hammering a slow/flaky remote — pause the flush:

```bash
# pause a running mount
touch ~/.omdfs/data/state/flush-off

# resume
rm ~/.omdfs/data/state/flush-off

# or boot a mount already paused
omdfs --no-flush --backend /mnt/nas --datadir ~/.omdfs/data /mnt/omdfs
```

While the file is present the syncer skips phase 2 (the network-heavy per-file
content/attr copy); local writes still succeed and the cache stays authoritative,
so nothing is lost — the backend just lags, and the held-back work drains once the
file is removed. The **cheap structural ops** (create/mkdir/unlink/rmdir/rename/
symlink) keep syncing, so the backend namespace stays coherent. `state/status`
reports `flush: disabled` and an overall `state: flush-paused`, with the still-
accurate `dirty-files`/`dirty-bytes` backlog. `--no-flush` (config key `no-flush =
on`) just pre-creates the control file, which persists across mounts until removed.

## Tests

```bash
make test
```

`tests/run-tests.sh` mounts omdfs over a throwaway backend and asserts real
filesystem behavior (TAP-style output), then unmounts and cleans up. It needs the
FUSE 3 userspace tools (`fusermount3`) and permission to mount FUSE.

### Formal verification

```bash
make verify                  # all SPIN models; non-zero exit on any surprise
make verify VFLAGS=--quick   # skip the largest bound
```

`spin/` holds six SPIN/Promela models of the concurrency-critical core, each modeling
the shipped design; all configurations are expected to pass. Needs `spin` (tested with
6.5.2) and a C compiler; it does not build or mount omdfs.

- `omdfs-flush.pml` — the flush vs. structural-drain **exclusion** (`pending_count`
  gate + `flushing` flag) on the single-entry clobber (cf. `tests/clobber.sh`).
- `omdfs-syncer.pml` — the **whole write-back engine** (create/mkdir/unlink/rmdir/
  rename/write) over *every* op sequence up to a bound: backend converges to the live
  namespace, structural ordering holds, no deadlock.
- `omdfs-recovery.pml` — **crash recovery**: WAL-suffix replay, the stale-high
  checkpoint clamp, and the `s_initial_drained` dry-flush guard.
- `omdfs-impl.pml` — the **function-by-function** model: `journal.c`, `meta.c`,
  `syncer.c`, and `content.c` mirrored as same-named inlines/proctypes over state that
  maps 1:1 to the C structures (the on-disk WAL + checkpoint, the indirection table, the
  in-memory drain queue, the content-cached bit). Runs the full pipeline over every op
  sequence, plus a one-shot crash + remount that drives `wal_init`/`sync_seed`/the
  recovery guard, and the read/fetch path's read-vs-rename race (`readok`).
- `omdfs-rename-replace.pml` — the **cross-identity replacing rename**: a flush of the
  *replaced* entry must not clobber the survivor at the shared path (the publish guard).
- `omdfs-replace.pml` — the write-back engine over the **full rename op-space** (name
  decoupled from identity, so *rename-onto-existing* is generated) over every op sequence.

See the writeup in `proj_docs/omdfs/SPIN-VERIFICATION.md`.

## Limitations

- **Single writer only.** Concurrent writes from other clients are unsupported
  and may corrupt consistency.
- **No cache-staleness detection.** The cache is trusted fully; if the backend is
  modified out-of-band, the result is undefined. That is the user's
  responsibility.
- **Whole-file granularity.** A file must fit within the cache budget to be
  cached; there is no block-level/partial caching.
- **Hard links are not first-class** (the cache is keyed by path).
- Linux + FUSE only.

## Status

Early development. Roadmap:

- [x] Repository scaffold (build + argument parsing)
- [x] Phase 1 — mount skeleton (read-only passthrough to the backend)
- [x] Phase 2 — per-directory metadata index for reads
- [x] Phase 3 — content cache + progressive read
- [x] Phase 4 — write-back (journal + background syncer) + crash recovery
- [x] Phase 5 — eviction (bounded cache), incl. cold-metadata-index eviction
- [x] Phase 6 — hardening (config, status/diagnostics, clean unmount)

## License

MIT — see [LICENSE](LICENSE).
