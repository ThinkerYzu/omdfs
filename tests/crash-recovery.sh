#!/usr/bin/env bash
#
# omdfs crash-recovery test (Phase 4, SPEC criteria 5 & 7).
#
# Make the backend unreachable, perform mutations through the mount (they apply
# locally and queue in the WAL + dirty flags but cannot sync), kill omdfs with
# -9 (no clean unmount, no final drain), then restore the backend and remount the
# SAME datadir. The recovery syncer must replay the journal and flush dirty
# content so every change reaches the backend.
#
# Usage: tests/crash-recovery.sh [path-to-omdfs-binary]   (default: ./omdfs)

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
BACKEND="$WORK/backend"
DATA="$WORK/data"
MNT="$WORK/mnt"
PID=""

cleanup() {
	fusermount3 -u "$MNT" >/dev/null 2>&1
	[ -n "$PID" ] && kill "$PID" >/dev/null 2>&1
	rm -rf "$WORK"
}
trap cleanup EXIT

COUNT=0; FAIL=0
ok() { COUNT=$((COUNT+1)); printf 'ok %d - %s\n' "$COUNT" "$1"; }
no() { COUNT=$((COUNT+1)); FAIL=$((FAIL+1)); printf 'not ok %d - %s\n' "$COUNT" "$1"; }
wait_for() { # desc cmd...
	local d="$1"; shift; local i
	for i in $(seq 1 50); do
		if "$@" >/dev/null 2>&1; then ok "$d"; return; fi
		sleep 0.1
	done
	no "$d (timed out)"
}
bcontent_eq() { [ "$(cat "$2" 2>/dev/null)" = "$1" ]; }

mount_omdfs() { # waits until mounted
	"$OMDFS" --backend "$BACKEND" --datadir "$DATA" -f "$MNT" \
		>"$WORK/omdfs.log" 2>&1 &
	PID=$!
	local i
	for i in $(seq 1 50); do
		grep -F " $MNT fuse" /proc/mounts >/dev/null 2>&1 && return 0
		kill -0 "$PID" 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}

# ---- fixture + first mount ----
mkdir -p "$BACKEND/existing" "$DATA" "$MNT"
printf 'original\n' > "$BACKEND/existing/keep.txt"

if ! mount_omdfs; then
	echo "Bail out! omdfs failed first mount; log follows:" >&2
	cat "$WORK/omdfs.log" >&2
	exit 98
fi

# Prime the cache for the dirs we'll mutate (so creates don't need the backend).
ls "$MNT" >/dev/null
ls "$MNT/existing" >/dev/null
cat "$MNT/existing/keep.txt" >/dev/null   # cache its content (we'll modify it)

# ---- make the backend unreachable, then mutate ----
mv "$BACKEND" "$WORK/backend.gone"

mkdir "$MNT/fresh"                         # new dir (empty index, no backend)
echo "persist me" > "$MNT/fresh/f.txt"     # new file in the new dir
echo "top level"  > "$MNT/top.txt"         # new file at root
echo "rewritten"  > "$MNT/existing/keep.txt"  # modify an existing file

# Confirm the mutations are visible locally even with the backend gone.
[ "$(cat "$MNT/fresh/f.txt")" = "persist me" ] && ok "new file readable with backend gone" \
	|| no "new file readable with backend gone"
[ "$(cat "$MNT/existing/keep.txt")" = "rewritten" ] && ok "modified file readable with backend gone" \
	|| no "modified file readable with backend gone"

# Give the syncer a moment to *try* (and fail) to drain — nothing should sync.
sleep 1

# ---- crash: kill -9, no clean unmount ----
kill -9 "$PID" 2>/dev/null
wait "$PID" 2>/dev/null
PID=""
fusermount3 -u "$MNT" >/dev/null 2>&1     # clear the stale mount

# ---- restore the backend and remount the SAME datadir ----
mv "$WORK/backend.gone" "$BACKEND"
# Sanity: the backend must NOT yet have the un-synced changes.
[ ! -e "$BACKEND/top.txt" ] && ok "un-synced change absent from backend pre-recovery" \
	|| no "un-synced change absent from backend pre-recovery"

if ! mount_omdfs; then
	echo "Bail out! omdfs failed to remount; log follows:" >&2
	cat "$WORK/omdfs.log" >&2
	exit 98
fi

# ---- recovery: the syncer must replay the WAL + flush dirty content ----
wait_for "recovered new dir on backend"        test -d "$BACKEND/fresh"
wait_for "recovered new file content"          bcontent_eq "persist me" "$BACKEND/fresh/f.txt"
wait_for "recovered top-level file content"    bcontent_eq "top level"  "$BACKEND/top.txt"
wait_for "recovered modification to existing"  bcontent_eq "rewritten"  "$BACKEND/existing/keep.txt"

# Mount still serves everything correctly after recovery.
[ "$(cat "$MNT/fresh/f.txt")" = "persist me" ] && ok "mount serves recovered file" \
	|| no "mount serves recovered file"

# ---- a torn WAL tail (partial write at a crash) must be tolerated ----
# Clean-unmount so the syncer drains, then append garbage to the WAL and remount:
# the trailing record fails its length/CRC check and is discarded, not fatal.
fusermount3 -u "$MNT" >/dev/null 2>&1
wait "$PID" 2>/dev/null
PID=""
printf 'garbage-torn-tail-not-a-valid-record' >> "$DATA/journal/wal"
if mount_omdfs; then ok "mounts with a torn WAL tail"; else no "mounts with a torn WAL tail"; fi
[ "$(cat "$MNT/top.txt")" = "top level" ] && ok "data intact after torn-tail recovery" \
	|| no "data intact after torn-tail recovery"

# ---- summary ----
echo "1..$COUNT"
if [ "$FAIL" -ne 0 ]; then
	echo "# FAILED: $FAIL of $COUNT"
	exit 1
fi
echo "# All $COUNT crash-recovery tests passed"
exit 0
