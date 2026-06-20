#!/usr/bin/env bash
#
# omdfs flush-vs-rename clobber repro (DESIGN.md § 2026-06-19).
#
# The suspected silent-divergence bug: a dirty entry's content is flushed to its
# *new* backend path before the WAL_RENAME that defines that path has drained, and
# the later structural rename then OVERWRITES the freshly-flushed content with the
# stale file at the old path. The flush succeeded, so the retry contract never
# notices — the backend silently diverges from the cache.
#
# This reproduces it deterministically, without relying on a thread race:
#
#   1. /d/f exists and is flushed clean      -> backend /d/f = v1
#   2. Wedge phase 1 (the structural drain) on a PERMANENT error: a create into a
#      backend directory we make mode 000 fails EACCES and blocks every later
#      structural op behind it (strict in-order, DESIGN Decision 5).
#   3. With the flush paused (state/flush-off): overwrite /d/f with v2 (dirty), then
#      rename /d/f -> /d/g. The WAL_RENAME is appended BEHIND the wedged create, so
#      it cannot drain.
#   4. Resume the flush. Phase 2 pushes the dirty /d/g (v2) to backend /d/g, which
#      succeeds (its parent /d exists) and clears the dirty flag — while the rename
#      is still pending.
#   5. Un-wedge phase 1. It now drains the rename: rename(backend /d/f, backend /d/g)
#      OVERWRITES backend /d/g (v2) with backend /d/f (v1).
#   6. After a clean unmount the backend is fully drained, so backend == cache must
#      hold. It does NOT: backend /d/g is v1, the cache/mount has v2 -> data loss.
#
# Usage: tests/clobber.sh [path-to-omdfs-binary]   (default: ./omdfs)

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
	local m p
	for m in "${MNTS[@]:-}"; do
		[ -n "$m" ] && fusermount3 -u "$m" >/dev/null 2>&1
	done
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

STATUS=""  # set to the datadir status file once the mount is up
sfield() { awk -F': ' -v k="$1" '$1==k{print $2}' "$STATUS" 2>/dev/null; }

# Wait until `cmd` succeeds (up to ~10s), else fail with desc.
wait_until() { # desc cmd...
	local d="$1"; shift; local i
	for i in $(seq 1 100); do
		if "$@" >/dev/null 2>&1; then return 0; fi
		sleep 0.1
	done
	no "$d (timed out)"; return 1
}

# Wait until status field $1 equals $2 (exact), up to ~10s.
wait_field() { # field value desc
	local f="$1" v="$2" d="$3" i
	for i in $(seq 1 100); do
		[ "$(sfield "$f")" = "$v" ] && return 0
		sleep 0.1
	done
	no "$d (timed out; $f='$(sfield "$f")')"; return 1
}

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

B="$WORK/backend"; D="$WORK/data"; M="$WORK/mnt"
mkdir -p "$B" "$D" "$M"

if ! mount_omdfs "$M" "$WORK/log" --backend "$B" --datadir "$D"; then
	echo "Bail out! mount failed" >&2; cat "$WORK/log" >&2; exit 99
fi
STATUS="$D/state/status"

# --- 1. /d/f = v1, fully flushed and clean; /ro exists on the backend ---
mkdir "$M/d" "$M/ro"
printf 'VERSION-ONE\n' > "$M/d/f"
# Let the structural ops + content flush reach the backend.
wait_until "backend /d/f appears"   test -f "$B/d/f"   || { exit 1; }
wait_until "backend /ro appears"    test -d "$B/ro"    || { exit 1; }
wait_field state ok "drained to clean" || true
ok "setup: /d/f flushed (backend has v1), /ro on backend"
[ "$(cat "$B/d/f")" = "VERSION-ONE" ] && ok "backend /d/f == v1" || no "backend /d/f == v1"

# --- 2. Wedge phase 1 on a permanent EACCES, with the flush paused ---
touch "$D/state/flush-off"     # pause phase 2 (content/attr push); phase 1 still runs
chmod 000 "$B/ro"              # a create into backend /ro now fails EACCES (permanent)
touch "$M/ro/x"                # WAL_CREATE /ro/x -> wedges the structural drain
# Confirm phase 1 is actually stuck before proceeding (so the rename queues behind it).
wait_field stuck-op create "phase 1 wedged (stuck)" \
	|| { echo '## status:'; cat "$STATUS" >&2; }
ok "phase 1 wedged on /ro/x (EACCES), flush paused"

# --- 3. Dirty modify + rename, both behind the wedge, with flush still paused ---
printf 'VERSION-TWO-XXL\n' > "$M/d/f"   # /d/f now dirty (v2), not yet pushed
mv "$M/d/f" "$M/d/g"                     # WAL_RENAME behind the wedged create
ok "modified /d/f -> v2 and renamed to /d/g (rename pending behind the wedge)"

# --- 4. Resume the flush. The gate must HOLD BACK the dirty /d/g while its rename
#        is still pending (pending_count > 0), so v2 must NOT reach backend /d/g
#        yet — that is exactly what prevents the clobber. ---
rm -f "$D/state/flush-off"
# Give the syncer several cycles; the dirty entry should stay pending, NOT flush.
sleep 2
if [ -e "$B/d/g" ]; then
	no "gate holds /d/g back while its rename is pending (it flushed early: '$(cat "$B/d/g" 2>/dev/null)')"
else
	ok "gate holds the dirty /d/g back while its rename is pending"
fi
[ "$(sfield dirty-files)" != 0 ] && ok "deferred /d/g still shown in the dirty backlog" \
	|| no "deferred /d/g still shown in the dirty backlog (dirty-files=0)"

# --- 5. Un-wedge phase 1: the rename drains FIRST, then the gate lets the flush
#        land v2 — so backend /d/g ends at v2, never clobbered. ---
chmod 755 "$B/ro"
wait_field pending-structural 0 "phase 1 caught up (rename drained)" \
	|| { echo '## status:'; cat "$STATUS" >&2; }

# --- 6. Clean unmount fully drains; backend must equal the cache (v2) ---
fusermount3 -u "$M" >/dev/null 2>&1
wait_until "unmount" sh -c "! grep -F ' $M fuse' /proc/mounts" || true
wait "$PID" 2>/dev/null

got="$(cat "$B/d/g" 2>/dev/null)"
if [ "$got" = VERSION-TWO-XXL ]; then
	ok "backend /d/g == cache (v2) after drain — no clobber"
else
	no "backend /d/g == cache (v2) after drain — CLOBBERED (got: '${got}', want VERSION-TWO-XXL)"
fi

echo "1..$COUNT"
[ "$FAIL" -eq 0 ] && { echo "# clobber: all $COUNT checks passed (bug NOT reproduced / fixed)"; exit 0; }
echo "# clobber: $FAIL/$COUNT checks failed (bug reproduced)"; exit 1
