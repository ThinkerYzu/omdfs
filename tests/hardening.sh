#!/usr/bin/env bash
#
# omdfs hardening tests (Phase 6).
#
# Exercises the production-readiness features added in Phase 6:
#   A. status file   — DATADIR/state/status appears and reports a clean idle mount.
#   B. backlog       — with the backend gone, a mutation shows up as pending in
#                      the status file (sync progress is surfaced, never silent).
#   C. stuck         — a permanent backend failure (EACCES) is reported as
#                      state: stuck with the errno, and the mutation is not dropped.
#   D. backpressure  — with the backend gone and a bounded cache, dirty writes are
#                      allowed to overshoot the budget up to a hard limit, then get
#                      ENOSPC instead of filling the disk; once the backend returns
#                      and the dirty data drains, writes are accepted again.
#   E. config file   — --config supplies backend/datadir/cache-size from a file.
#   F. clean unmount — a clean fusermount3 -u fully drains pending writes to the
#                      backend (final-drain), leaving an idle status.
#   G. resync        — the offline `--resync` tool force-reconciles the whole cache
#                      (content + metadata + namespace) onto the backend, even over
#                      a read-only existing target, after a crash left work unsynced.
#   H. live resync   — on a *running* mount, creating DATADIR/state/resync makes the
#                      syncer thread run the same reconcile: a clean cached file whose
#                      backend copy drifted (something the normal syncer never re-pushes)
#                      is forced back to the cache, the trigger is consumed once, and the
#                      result is surfaced in the status file.
#   I/J. mark-dirty  — offline --mark-dirty and the live state/mark-dirty trigger flag
#                      the cache dirty so the background syncer re-pushes it.
#   K. cold metadata — a directory whose cached content has all been evicted (its cache
#                      dir holds only .omdfs.dir) has its index reclaimed by a cold-evict
#                      sweep (state/cold-evict): the on-disk .omdfs.dir is removed and the
#                      cache dir rmdir'd, yet the directory stays listable (rebuilt from
#                      the backend) and its files re-fetch correctly. A directory with a
#                      dirty (un-synced) child keeps its index.
#
# Usage: tests/hardening.sh [path-to-omdfs-binary]   (default: ./omdfs)

set -u

OMDFS="${1:-./omdfs}"
OMDFS="$(cd "$(dirname "$OMDFS")" && pwd)/$(basename "$OMDFS")"

if [ ! -x "$OMDFS" ]; then
	echo "Bail out! omdfs binary not found/executable: $OMDFS" >&2
	exit 99
fi
if ! command -v fusermount3 >/dev/null 2>&1; then
	echo "Bail out! fusermount3 not found (install fuse3)" >&2
	exit 99
fi

WORK="$(mktemp -d)"
declare -a MNTS PIDS

cleanup() {
	local m
	for m in "${MNTS[@]:-}"; do
		[ -n "$m" ] && fusermount3 -u "$m" >/dev/null 2>&1
	done
	local p
	for p in "${PIDS[@]:-}"; do
		[ -n "$p" ] && kill "$p" >/dev/null 2>&1
	done
	chmod -R u+rwx "$WORK" >/dev/null 2>&1
	rm -rf "$WORK"
}
trap cleanup EXIT

COUNT=0; FAIL=0
ok() { COUNT=$((COUNT+1)); printf 'ok %d - %s\n' "$COUNT" "$1"; }
no() { COUNT=$((COUNT+1)); FAIL=$((FAIL+1)); printf 'not ok %d - %s\n' "$COUNT" "$1"; }
wait_for() { # desc cmd...
	local d="$1"; shift; local i
	for i in $(seq 1 50); do
		if "$@" >/dev/null 2>&1; then ok "$d"; return 0; fi
		sleep 0.1
	done
	no "$d (timed out)"; return 1
}

INDEX=".omdfs.dir"
content_bytes() { # datadir
	find "$1/cache" -type f ! -name "$INDEX" ! -name "$INDEX".* \
		-printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}'
}
status_field() { # datadir field
	awk -F': ' -v k="$2" '$1==k{print $2}' "$1/state/status" 2>/dev/null
}

# Mount omdfs at MNT (global) given explicit args; sets PID (global) and registers
# both for cleanup. args... are passed verbatim before the mountpoint.
mount_omdfs() { # mnt logfile args...
	local mnt="$1" log="$2"; shift 2
	"$OMDFS" "$@" -f "$mnt" >"$log" 2>&1 &
	local pid=$!
	MNTS+=("$mnt"); PIDS+=("$pid")
	PID="$pid"
	local i
	for i in $(seq 1 50); do
		grep -F " $mnt fuse" /proc/mounts >/dev/null 2>&1 && return 0
		kill -0 "$pid" 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}

# ============================================================
# A. status file appears and reports a clean idle mount
# ============================================================
A="$WORK/a"; mkdir -p "$A/backend" "$A/data" "$A/mnt"
echo hello > "$A/backend/f"
if ! mount_omdfs "$A/mnt" "$A/omdfs.log" --backend "$A/backend" --datadir "$A/data"; then
	echo "Bail out! omdfs failed to mount (A); log follows:" >&2; cat "$A/omdfs.log" >&2; exit 98
fi
wait_for "status file is created" test -f "$A/data/state/status"
cat "$A/mnt/f" >/dev/null                       # touch the fs
sleep 1                                          # let a sync cycle run
state="$(status_field "$A/data" state)"
[ "$state" = ok ] && ok "idle mount reports state: ok" \
	|| no "idle mount reports state: ok (got '$state')"
pend="$(status_field "$A/data" pending-structural)"
[ "$pend" = 0 ] && ok "idle mount has zero pending-structural" \
	|| no "idle mount has zero pending-structural (got '$pend')"

# ============================================================
# B. a mutation with the backend gone surfaces as pending
# ============================================================
mv "$A/backend" "$A/backend.gone"
echo data > "$A/mnt/newfile"                     # structural create can't reach backend
isyncing() { [ "$(status_field "$A/data" pending-structural)" -ge 1 ] 2>/dev/null; }
wait_for "pending-structural rises when backend is gone" isyncing
# The local write still succeeded (cache-as-truth).
[ "$(cat "$A/mnt/newfile")" = data ] && ok "write succeeds locally though backend is gone" \
	|| no "write succeeds locally though backend is gone"
mv "$A/backend.gone" "$A/backend"                # restore: should drain
backend_has() { [ -f "$A/backend/newfile" ]; }
wait_for "pending drains to the backend once it returns" backend_has

# ============================================================
# C. a permanent failure (EACCES) is reported as state: stuck
# ============================================================
C="$WORK/c"; mkdir -p "$C/backend/ro" "$C/data" "$C/mnt"
if ! mount_omdfs "$C/mnt" "$C/omdfs.log" --backend "$C/backend" --datadir "$C/data"; then
	echo "Bail out! omdfs failed to mount (C); log follows:" >&2; cat "$C/omdfs.log" >&2; exit 98
fi
ls "$C/mnt/ro" >/dev/null                         # build the index for ro/
chmod 0500 "$C/backend/ro"                         # owner can't create inside anymore
echo x > "$C/mnt/ro/blocked"                       # local ok; backend create -> EACCES
isstuck() { [ "$(status_field "$C/data" stuck)" = yes ]; }
if wait_for "permanent backend failure reported as stuck" isstuck; then
	errno="$(status_field "$C/data" stuck-errno)"
	case "$errno" in
		13*) ok "stuck-errno is EACCES (13): $errno" ;;
		*)   no "stuck-errno is EACCES (13) (got '$errno')" ;;
	esac
	op="$(status_field "$C/data" stuck-op)"
	[ "$op" = create ] && ok "stuck-op identifies the blocked op (create)" \
		|| no "stuck-op identifies the blocked op (got '$op')"
fi
# The blocked mutation is not dropped: lift the permission and it drains.
chmod 0700 "$C/backend/ro"
cdrains() { [ -f "$C/backend/ro/blocked" ]; }
wait_for "blocked mutation drains after permission is restored" cdrains

# ============================================================
# D. backpressure: dirty writes get ENOSPC past the hard limit
# ============================================================
D="$WORK/d"; mkdir -p "$D/backend" "$D/data" "$D/mnt"
DBUDGET=262144                                     # 256 KiB budget -> 512 KiB hard limit
if ! mount_omdfs "$D/mnt" "$D/omdfs.log" --backend "$D/backend" --datadir "$D/data" --cache-size "$DBUDGET"; then
	echo "Bail out! omdfs failed to mount (D); log follows:" >&2; cat "$D/omdfs.log" >&2; exit 98
fi
mv "$D/backend" "$D/backend.gone"                  # dirty data can't sync or be evicted
# Write 200 KiB files until one fails. With a 512 KiB hard limit we expect ~2 to
# fit and a later one to be refused with ENOSPC.
enospc=0; wrote=0
for i in $(seq 0 7); do
	if head -c 204800 /dev/urandom > "$D/mnt/d$i" 2>"$D/werr"; then
		wrote=$((wrote+1))
	else
		grep -qiE 'no space|ENOSPC' "$D/werr" && enospc=1
		break
	fi
done
[ "$enospc" = 1 ] && ok "dirty write past the hard limit fails with ENOSPC" \
	|| no "dirty write past the hard limit fails with ENOSPC (wrote=$wrote)"
hard=$((DBUDGET*2))
db="$(content_bytes "$D/data")"
# Cache is bounded near the hard limit (one in-flight file of slack tolerated).
[ "$db" -le $((hard + 204800)) ] && ok "cache bounded under backpressure ($db <= ~$hard)" \
	|| no "cache bounded under backpressure ($db > ~$hard)"
dbp() { [ "$(status_field "$D/data" backpressure)" = yes ]; }
wait_for "status reports backpressure: yes" dbp
# Files that were accepted are intact.
intact=1
for i in $(seq 0 $((wrote-1))); do [ -s "$D/mnt/d$i" ] || intact=0; done
[ "$intact" = 1 ] && ok "accepted dirty files intact under backpressure" \
	|| no "accepted dirty files intact under backpressure"
# Restore the backend: dirty data drains, eviction reclaims, pressure clears,
# and a new write is accepted again.
mv "$D/backend.gone" "$D/backend"
dclear() { [ "$(status_field "$D/data" backpressure)" = no ]; }
wait_for "backpressure clears after the backend returns" dclear
if head -c 204800 /dev/urandom > "$D/mnt/after" 2>/dev/null; then
	ok "writes accepted again after backpressure clears"
else
	no "writes accepted again after backpressure clears"
fi

# ============================================================
# E. config file supplies backend/datadir/cache-size
# ============================================================
E="$WORK/e"; mkdir -p "$E/backend" "$E/data" "$E/mnt"
echo conf > "$E/backend/cf"
cat > "$E/omdfs.conf" <<EOF
# omdfs config
backend = $E/backend
datadir = $E/data
cache-size = 4M
EOF
if mount_omdfs "$E/mnt" "$E/omdfs.log" --config "$E/omdfs.conf"; then
	ok "mounts from a --config file with no CLI backend/datadir"
	[ "$(cat "$E/mnt/cf" 2>/dev/null)" = conf ] && ok "config-mounted fs serves files" \
		|| no "config-mounted fs serves files"
	hb="$(status_field "$E/data" cache-hard-limit)"
	[ "$hb" = $((4*1024*1024*2)) ] && ok "config cache-size applied (hard-limit $hb)" \
		|| no "config cache-size applied (got hard-limit '$hb')"
else
	no "mounts from a --config file with no CLI backend/datadir"
	no "config-mounted fs serves files (skipped)"
	no "config cache-size applied (skipped)"
fi

# ============================================================
# F. clean unmount drains pending writes to the backend
# ============================================================
F="$WORK/f"; mkdir -p "$F/backend" "$F/data" "$F/mnt"
if ! mount_omdfs "$F/mnt" "$F/omdfs.log" --backend "$F/backend" --datadir "$F/data"; then
	echo "Bail out! omdfs failed to mount (F); log follows:" >&2; cat "$F/omdfs.log" >&2; exit 98
fi
mkdir "$F/mnt/sub"
echo persist > "$F/mnt/sub/file"
fusermount3 -u "$F/mnt"                            # clean unmount -> final drain
# After a clean unmount the data must be on the backend (drained, not just cached).
[ "$(cat "$F/backend/sub/file" 2>/dev/null)" = persist ] \
	&& ok "clean unmount drained pending writes to the backend" \
	|| no "clean unmount drained pending writes to the backend"
fstate="$(status_field "$F/data" state)"
fpend="$(status_field "$F/data" pending-structural)"
[ "$fstate" = ok ] && [ "$fpend" = 0 ] \
	&& ok "post-unmount status is idle (state ok, 0 pending)" \
	|| no "post-unmount status is idle (state '$fstate', pending '$fpend')"

# ============================================================
# G. --resync force-reconciles the cache onto the backend
# ============================================================
# Build a tree online, prime the cache, then take the backend away and make a
# spread of offline changes (modify, create, mkdir, symlink, chmod) that can't
# sync. Crash (kill -9) so nothing drains, restore the backend, mark an existing
# target read-only (the stuck-class case), and run the offline tool. The backend
# must end up matching the cache.
G="$WORK/g"; mkdir -p "$G/backend/sub" "$G/data" "$G/mnt"
printf 'orig-inner\n' > "$G/backend/sub/inner.txt"
printf 'orig-top\n'   > "$G/backend/top.txt"
if ! mount_omdfs "$G/mnt" "$G/omdfs.log" --backend "$G/backend" --datadir "$G/data"; then
	echo "Bail out! omdfs failed to mount (G); log follows:" >&2; cat "$G/omdfs.log" >&2; exit 98
fi
gpid="$PID"
cat "$G/mnt/sub/inner.txt" >/dev/null; cat "$G/mnt/top.txt" >/dev/null   # cache content
ls "$G/mnt" "$G/mnt/sub" >/dev/null                                       # build indexes
mv "$G/backend" "$G/backend.gone"                                         # backend offline
echo "NEW inner"  > "$G/mnt/sub/inner.txt"     # modify existing (dirty content)
echo "brand new"  > "$G/mnt/sub/created.txt"   # new file...
chmod 0600 "$G/mnt/sub/created.txt"            # ...with a custom mode
mkdir "$G/mnt/newdir"                          # new dir
echo deep > "$G/mnt/newdir/deep.txt"           # new nested file
ln -s targetpath "$G/mnt/sub/alink"            # new symlink
chmod 0640 "$G/mnt/top.txt"                    # dirty attr on an existing file
sleep 0.3
kill -9 "$gpid" 2>/dev/null; wait "$gpid" 2>/dev/null
fusermount3 -u "$G/mnt" >/dev/null 2>&1
mv "$G/backend.gone" "$G/backend"              # backend returns
chmod 0444 "$G/backend/top.txt"                # existing target read-only (stuck-class)
"$OMDFS" --resync --backend "$G/backend" --datadir "$G/data" >"$G/resync.log" 2>&1 \
	&& ok "resync exits clean (0 failures)" \
	|| { no "resync exits clean (0 failures)"; cat "$G/resync.log" >&2; }
[ "$(cat "$G/backend/sub/inner.txt" 2>/dev/null)" = "NEW inner" ] \
	&& ok "resync pushes a modified file's content" \
	|| no "resync pushes a modified file's content"
[ "$(cat "$G/backend/sub/created.txt" 2>/dev/null)" = "brand new" ] \
	&& [ "$(stat -c %a "$G/backend/sub/created.txt" 2>/dev/null)" = 600 ] \
	&& ok "resync pushes a new file with its mode" \
	|| no "resync pushes a new file with its mode"
[ "$(cat "$G/backend/newdir/deep.txt" 2>/dev/null)" = deep ] \
	&& ok "resync pushes a new directory and its contents" \
	|| no "resync pushes a new directory and its contents"
[ "$(readlink "$G/backend/sub/alink" 2>/dev/null)" = targetpath ] \
	&& ok "resync pushes a new symlink" \
	|| no "resync pushes a new symlink"
[ "$(cat "$G/backend/top.txt" 2>/dev/null)" = "orig-top" ] \
	&& [ "$(stat -c %a "$G/backend/top.txt" 2>/dev/null)" = 640 ] \
	&& ok "resync reconciles a read-only backend target (content + mode)" \
	|| no "resync reconciles a read-only backend target (content + mode)"

# ============================================================
# H. live resync trigger reconciles a running mount
# ============================================================
# A clean (non-dirty) cached file whose backend copy drifts out-of-band is never
# re-pushed by the normal syncer (nothing marks it dirty). An operator triggers a
# reconcile on the running mount by creating state/resync; the syncer thread runs
# the full reconcile on its own thread and force-pushes the cache onto the backend.
H="$WORK/h"; mkdir -p "$H/backend" "$H/data" "$H/mnt"
printf 'cache-truth\n' > "$H/backend/drift.txt"
if ! mount_omdfs "$H/mnt" "$H/omdfs.log" --backend "$H/backend" --datadir "$H/data"; then
	echo "Bail out! omdfs failed to mount (H); log follows:" >&2; cat "$H/omdfs.log" >&2; exit 98
fi
cat "$H/mnt/drift.txt" >/dev/null              # prime the cache (CONTENT_CACHED, clean)
ls "$H/mnt" >/dev/null                          # build the index
sleep 0.5                                        # let the background fetch finalize
printf 'BACKEND DRIFTED\n' > "$H/backend/drift.txt"   # out-of-band change to the backend
sleep 2                                          # a couple of normal sync cycles
[ "$(cat "$H/backend/drift.txt")" = "BACKEND DRIFTED" ] \
	&& ok "normal sync leaves a clean file's backend drift untouched" \
	|| no "normal sync leaves a clean file's backend drift untouched"
touch "$H/data/state/resync"                     # operator triggers a live reconcile
hreconciled() { [ "$(cat "$H/backend/drift.txt" 2>/dev/null)" = "cache-truth" ]; }
wait_for "live resync reconciles the drifted backend file to the cache" hreconciled
htrigger_gone() { [ ! -e "$H/data/state/resync" ]; }
wait_for "live resync consumes the trigger file (one-shot)" htrigger_gone
hres="$(status_field "$H/data" last-resync-result)"
{ [ -n "$hres" ] && [ "$hres" != "-" ]; } \
	&& ok "status reports the last-resync result ($hres)" \
	|| no "status reports the last-resync result (got '$hres')"

# ============================================================
# I. offline --mark-dirty re-pushes via the next mount's syncer
# ============================================================
# --mark-dirty is the non-blocking cousin of --resync: it only flags the cache
# dirty (local-only, fast) and returns; the actual push happens incrementally on
# the next mount's background syncer. Prime a clean cached file, drift its backend
# copy out-of-band, unmount, mark dirty offline, then remount and confirm the
# background syncer reconciles the drift back to cache-truth.
I="$WORK/i"; mkdir -p "$I/backend/sub" "$I/data" "$I/mnt"
printf 'cache-truth\n' > "$I/backend/sub/inner.txt"
if ! mount_omdfs "$I/mnt" "$I/omdfs.log" --backend "$I/backend" --datadir "$I/data"; then
	echo "Bail out! omdfs failed to mount (I); log follows:" >&2; cat "$I/omdfs.log" >&2; exit 98
fi
cat "$I/mnt/sub/inner.txt" >/dev/null            # prime the cache (CONTENT_CACHED, clean)
ls "$I/mnt" "$I/mnt/sub" >/dev/null              # build indexes
sleep 0.5
fusermount3 -u "$I/mnt" >/dev/null 2>&1          # clean unmount; clean file => nothing pushed
wait "$PID" 2>/dev/null
printf 'BACKEND DRIFTED\n' > "$I/backend/sub/inner.txt"   # out-of-band drift while unmounted
"$OMDFS" --mark-dirty --backend "$I/backend" --datadir "$I/data" >"$I/mark.log" 2>&1 \
	&& ok "mark-dirty exits clean" \
	|| { no "mark-dirty exits clean"; cat "$I/mark.log" >&2; }
grep -q 'marked dirty' "$I/mark.log" \
	&& ok "mark-dirty prints a summary" \
	|| no "mark-dirty prints a summary"
[ "$(cat "$I/backend/sub/inner.txt")" = "BACKEND DRIFTED" ] \
	&& ok "mark-dirty does not itself push to the backend" \
	|| no "mark-dirty does not itself push to the backend"
if ! mount_omdfs "$I/mnt" "$I/omdfs2.log" --backend "$I/backend" --datadir "$I/data"; then
	echo "Bail out! omdfs failed to remount (I); log follows:" >&2; cat "$I/omdfs2.log" >&2; exit 98
fi
ireconciled() { [ "$(cat "$I/backend/sub/inner.txt" 2>/dev/null)" = "cache-truth" ]; }
wait_for "next mount's syncer re-pushes the marked-dirty file" ireconciled
fusermount3 -u "$I/mnt" >/dev/null 2>&1; wait "$PID" 2>/dev/null

# ============================================================
# J. live mark-dirty trigger re-pushes on a running mount
# ============================================================
# Same as H, but via the non-blocking path: a clean cached file's backend copy
# drifts; the operator creates state/mark-dirty; the syncer flags the tree dirty
# and the very next flush drains it back to cache-truth — without the operator
# waiting on the push.
J="$WORK/j"; mkdir -p "$J/backend" "$J/data" "$J/mnt"
printf 'cache-truth\n' > "$J/backend/drift.txt"
if ! mount_omdfs "$J/mnt" "$J/omdfs.log" --backend "$J/backend" --datadir "$J/data"; then
	echo "Bail out! omdfs failed to mount (J); log follows:" >&2; cat "$J/omdfs.log" >&2; exit 98
fi
cat "$J/mnt/drift.txt" >/dev/null                # prime the cache (CONTENT_CACHED, clean)
ls "$J/mnt" >/dev/null                            # build the index
sleep 0.5
printf 'BACKEND DRIFTED\n' > "$J/backend/drift.txt"   # out-of-band change to the backend
sleep 2                                           # a couple of normal sync cycles
[ "$(cat "$J/backend/drift.txt")" = "BACKEND DRIFTED" ] \
	&& ok "normal sync leaves a clean file's backend drift untouched (mark path)" \
	|| no "normal sync leaves a clean file's backend drift untouched (mark path)"
touch "$J/data/state/mark-dirty"                  # operator triggers a live mark-dirty
jreconciled() { [ "$(cat "$J/backend/drift.txt" 2>/dev/null)" = "cache-truth" ]; }
wait_for "live mark-dirty re-pushes the drifted file via the syncer" jreconciled
jtrigger_gone() { [ ! -e "$J/data/state/mark-dirty" ]; }
wait_for "live mark-dirty consumes the trigger file (one-shot)" jtrigger_gone
jres="$(status_field "$J/data" last-mark-dirty-result)"
{ [ -n "$jres" ] && [ "$jres" != "-" ]; } \
	&& ok "status reports the last-mark-dirty result ($jres)" \
	|| no "status reports the last-mark-dirty result (got '$jres')"
fusermount3 -u "$J/mnt" >/dev/null 2>&1; wait "$PID" 2>/dev/null

# ============================================================
# K. cold-metadata-index eviction reclaims a cold directory's index
# ============================================================
# A subdirectory whose cached content is all evicted (LRU pressure) ends up with a
# cache dir holding only .omdfs.dir. A cold-evict sweep removes that index and
# rmdir's the cache dir, while the directory stays listable (rebuilt from the
# backend) and its files re-fetch. A directory that still holds cached content (and
# the root) keep their index.
K="$WORK/k"; mkdir -p "$K/backend/sub" "$K/backend/other" "$K/data" "$K/mnt"
KFSZ=204800; KBUDGET=524288       # 200 KiB files, 512 KiB cache
for i in $(seq 0 4); do head -c "$KFSZ" /dev/urandom > "$K/backend/sub/f$i"; done
for i in $(seq 0 9); do head -c "$KFSZ" /dev/urandom > "$K/backend/other/g$i"; done
if ! mount_omdfs "$K/mnt" "$K/omdfs.log" --backend "$K/backend" --datadir "$K/data" --cache-size "$KBUDGET"; then
	echo "Bail out! omdfs failed to mount (K); log follows:" >&2; cat "$K/omdfs.log" >&2; exit 98
fi
# Build sub's index + cache its files, then read 'other' so LRU evicts sub's content.
for i in $(seq 0 4); do cat "$K/mnt/sub/f$i" >/dev/null; done
for i in $(seq 0 9); do cat "$K/mnt/other/g$i" >/dev/null; done
ksub_content() { find "$K/data/cache/sub" -type f ! -name "$INDEX" ! -name "$INDEX".* 2>/dev/null | wc -l; }
ksub_empty() { [ "$(ksub_content)" -eq 0 ]; }
wait_for "cold dir's cached content is fully evicted" ksub_empty
[ -f "$K/data/cache/sub/$INDEX" ] \
	&& ok "cold dir still has its on-disk index before the sweep" \
	|| no "cold dir still has its on-disk index before the sweep"

touch "$K/data/state/cold-evict"          # operator forces a cold-metadata sweep
kindex_gone() { [ ! -e "$K/data/cache/sub/$INDEX" ]; }
wait_for "cold-evict removes the cold directory's .omdfs.dir" kindex_gone
[ ! -e "$K/data/cache/sub" ] \
	&& ok "cold-evict rmdir's the now-empty cache directory" \
	|| no "cold-evict rmdir's the now-empty cache directory"
ktrigger_gone() { [ ! -e "$K/data/state/cold-evict" ]; }
wait_for "cold-evict consumes the trigger file (one-shot)" ktrigger_gone

# Still listable (rebuilt from the backend) and the files re-fetch byte-correct.
kn="$(ls "$K/mnt/sub" 2>/dev/null | wc -l)"
[ "$kn" -eq 5 ] \
	&& ok "cold dir still listable after index eviction (rebuilt from backend)" \
	|| no "cold dir still listable after index eviction (got $kn of 5)"
cmp -s "$K/mnt/sub/f0" "$K/backend/sub/f0" \
	&& ok "evicted-index dir's file re-fetches byte-correct" \
	|| no "evicted-index dir's file re-fetches byte-correct"
# A directory still holding cached content keeps its index; the root always does.
{ [ -f "$K/data/cache/other/$INDEX" ] && [ -f "$K/data/cache/$INDEX" ]; } \
	&& ok "a content-holding directory (and the root) keep their index" \
	|| no "a content-holding directory (and the root) keep their index"
kswept="$(status_field "$K/data" cold-meta-evicted)"
{ [ -n "$kswept" ] && [ "$kswept" -ge 1 ]; } \
	&& ok "status reports the cold-meta-evicted count ($kswept)" \
	|| no "status reports the cold-meta-evicted count (got '$kswept')"
fusermount3 -u "$K/mnt" >/dev/null 2>&1; wait "$PID" 2>/dev/null

# ---- summary ----
echo "1..$COUNT"
if [ "$FAIL" -ne 0 ]; then
	echo "# FAILED: $FAIL of $COUNT"
	exit 1
fi
echo "# All $COUNT hardening tests passed"
exit 0
