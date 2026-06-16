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

# ---- summary ----
echo "1..$COUNT"
if [ "$FAIL" -ne 0 ]; then
	echo "# FAILED: $FAIL of $COUNT"
	exit 1
fi
echo "# All $COUNT hardening tests passed"
exit 0
