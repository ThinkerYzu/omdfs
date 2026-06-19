#!/usr/bin/env bash
#
# omdfs differential consistency stress test.
#
# Applies the SAME seeded sequence of filesystem operations to two roots:
#   target  = the omdfs mount
#   control = a plain directory that starts as a copy of the backend
# then checks three invariants:
#   1. live:           mount view  == control          (cache-as-truth is correct)
#   2. after unmount:  backend     == control          (write-back drains correctly)
#   3. after --resync: backend     == control          (force-reconcile is correct)
#
# The op engine (tests/stress.py) does a per-op differential check (matching
# success/errno; for reads, matching bytes) so the FIRST divergence is reported
# with the exact op and seed for reproduction.
#
# Usage: tests/stress.sh [path-to-omdfs-binary]
# Knobs (env): STRESS_SEED (default 1), STRESS_OPS (2000), STRESS_ROUNDS (1),
#              STRESS_CACHE (default 0 = unlimited), STRESS_MAXSZ (131072).
#
# The default runs with an UNLIMITED cache, which cleanly verifies the three
# consistency invariants above. Setting STRESS_CACHE to a byte budget (e.g.
# STRESS_CACHE=8M) turns on bounded-cache + eviction stress; that mode currently
# surfaces a KNOWN open eviction-safety bug (a clean file content-evicted while a
# directory-rename structural op affecting its path is still draining makes a
# mid-sequence refetch read the not-yet-updated backend path and fail ENOENT —
# the final drained state is still consistent). Repro: STRESS_CACHE=8M
# STRESS_SEED=100 STRESS_OPS=3000. See HANDOFF.md "Deferred".

set -u

OMDFS="${1:-./omdfs}"
OMDFS="$(cd "$(dirname "$OMDFS")" && pwd)/$(basename "$OMDFS")"
HERE="$(cd "$(dirname "$0")" && pwd)"
PY="${PYTHON:-python3}"

SEED="${STRESS_SEED:-1}"
OPS="${STRESS_OPS:-2000}"
ROUNDS="${STRESS_ROUNDS:-1}"
CACHE="${STRESS_CACHE:-0}"
MAXSZ="${STRESS_MAXSZ:-131072}"

if [ ! -x "$OMDFS" ]; then
	echo "Bail out! omdfs binary not found/executable: $OMDFS" >&2; exit 99
fi
if ! command -v fusermount3 >/dev/null 2>&1; then
	echo "Bail out! fusermount3 not found (install fuse3)" >&2; exit 99
fi
if ! command -v "$PY" >/dev/null 2>&1; then
	echo "Bail out! python3 not found" >&2; exit 99
fi

WORK="$(mktemp -d)"
MNTS=()
cleanup() {
	local m
	for m in "${MNTS[@]:-}"; do
		[ -n "$m" ] && fusermount3 -u "$m" >/dev/null 2>&1
	done
	rm -rf "$WORK"
}
trap cleanup EXIT

COUNT=0; FAIL=0
ok()  { COUNT=$((COUNT+1)); printf 'ok %d - %s\n' "$COUNT" "$1"; }
no()  { COUNT=$((COUNT+1)); FAIL=$((FAIL+1)); printf 'not ok %d - %s\n' "$COUNT" "$1"; }

# Mount omdfs at $1 with args $2.., poll /proc/mounts. Sets PID.
mount_omdfs() { # mnt log args...
	local mnt="$1" log="$2"; shift 2
	"$OMDFS" "$@" -f "$mnt" >"$log" 2>&1 &
	PID=$!
	MNTS+=("$mnt")
	local i
	for i in $(seq 1 50); do
		grep -F " $mnt fuse" /proc/mounts >/dev/null 2>&1 && return 0
		kill -0 "$PID" 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}

# Build a non-trivial backend fixture: nested dirs, files of various sizes,
# symlinks. Some files will be read (cached) by the stress, some never touched.
seed_backend() { # dir
	local b="$1"
	mkdir -p "$b/a/b/c" "$b/d" "$b/e/f"
	printf 'hello\n'           > "$b/top.txt"
	printf 'nested\n'          > "$b/a/x.txt"
	printf 'deep content\n'    > "$b/a/b/c/deep.txt"
	head -c 65537 /dev/urandom > "$b/d/rand.bin"     # > a page, odd size
	head -c 200000 /dev/urandom > "$b/e/big.bin"     # multi-chunk fetch
	: > "$b/e/empty.txt"
	ln -s x.txt                  "$b/a/link"
	ln -s ../../top.txt          "$b/a/b/up"
	ln -s nowhere                "$b/d/dangling"
	chmod 0600 "$b/a/x.txt"
	chmod 0444 "$b/a/b/c/deep.txt"
}

run_round() { # seed
	local seed="$1"
	local R="$WORK/r$seed"
	local B="$R/backend" D="$R/data" M="$R/mnt" C="$R/control"
	mkdir -p "$B" "$D" "$M"
	seed_backend "$B"
	cp -a "$B" "$C"          # control starts as an exact copy of the backend

	local margs=(--backend "$B" --datadir "$D")
	[ "$CACHE" != "0" ] && margs+=(--cache-size "$CACHE")
	if ! mount_omdfs "$M" "$R/omdfs.log" "${margs[@]}"; then
		no "round $seed: mount (log follows)"; cat "$R/omdfs.log" >&2; return
	fi

	# 1. apply the op sequence to both roots with per-op differential checks
	if "$PY" "$HERE/stress.py" apply --target "$M" --control "$C" \
			--seed "$seed" --ops "$OPS" --max-file-size "$MAXSZ" \
			>"$R/apply.log" 2>&1; then
		ok "round $seed: $OPS ops, no live target/control divergence"
	else
		no "round $seed: op divergence (see below)"; cat "$R/apply.log" >&2
	fi

	# 2. live: the mount view must equal control (cache-as-truth)
	if "$PY" "$HERE/stress.py" compare "$M" "$C" >"$R/cmp-mount.log" 2>&1; then
		ok "round $seed: mount view matches control"
	else
		no "round $seed: mount view != control"; cat "$R/cmp-mount.log" >&2
	fi

	# clean unmount -> final drain pushes everything pending to the backend
	fusermount3 -u "$M" >/dev/null 2>&1
	wait "$PID" 2>/dev/null

	# 3. after the drain, the backend must equal control
	if "$PY" "$HERE/stress.py" compare "$B" "$C" >"$R/cmp-drain.log" 2>&1; then
		ok "round $seed: backend matches control after clean-unmount drain"
	else
		no "round $seed: backend != control after drain"; cat "$R/cmp-drain.log" >&2
	fi

	# 4. --resync force-reconciles the cache onto the backend; still equal
	if "$OMDFS" --resync "${margs[@]}" >"$R/resync.log" 2>&1; then
		if "$PY" "$HERE/stress.py" compare "$B" "$C" >"$R/cmp-resync.log" 2>&1; then
			ok "round $seed: backend matches control after --resync"
		else
			no "round $seed: backend != control after --resync"
			cat "$R/cmp-resync.log" >&2
		fi
	else
		no "round $seed: --resync failed"; cat "$R/resync.log" >&2
	fi
}

echo "# omdfs stress: rounds=$ROUNDS ops=$OPS cache=$CACHE maxsz=$MAXSZ base-seed=$SEED"
r=0
while [ "$r" -lt "$ROUNDS" ]; do
	run_round "$((SEED + r))"
	r=$((r + 1))
done

printf '1..%d\n' "$COUNT"
if [ "$FAIL" -eq 0 ]; then
	echo "# All $COUNT stress checks passed"
	exit 0
else
	echo "# $FAIL of $COUNT stress checks FAILED"
	exit 1
fi
