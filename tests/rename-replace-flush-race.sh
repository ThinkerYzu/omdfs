#!/usr/bin/env bash
# Regression for the cross-identity rename-replace flush clobber (see
# proj_docs/omdfs/bug-rename-replace-flush.md and spin/omdfs-rename-replace.pml).
#
# The race: B is dirty and a phase-2 flush of B is in flight to B's backend path P
# (its copy_file has written the temp but not yet renamed it onto P). Meanwhile the
# user renames a clean A onto P, REPLACING B; the structural rename drains, putting
# A's bytes at P. If B's now-stale flush then publishes (rename(tmp,P)), it clobbers
# A with B's discarded bytes -- and A is clean, so nothing repairs it.
#
# This drives the window deterministically via the OMDFS_TEST_HOOKS publish gate
# (compiled out of normal builds): the gate pauses B's flush right before its publish
# rename, signalling readiness; we replace B and drain, then release the flush. The
# fix (meta_flush_publish: publish only if the entry's indirection is still attached)
# must discard B's superseded flush, leaving backend P holding A's bytes.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"

command -v fusermount3 >/dev/null 2>&1 || { echo "Bail out! fusermount3 not found"; exit 99; }

WORK="$(mktemp -d)"
BIN="$WORK/omdfs-th"
BACKEND="$WORK/backend"; DATA="$WORK/data"; MNT="$WORK/mnt"; GATE="$WORK/gate"; LOG="$WORK/omdfs.log"
PID=""
cleanup() { fusermount3 -u "$MNT" >/dev/null 2>&1; [ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT
mkdir -p "$BACKEND" "$DATA" "$MNT" "$GATE"

# Build a test-hook binary (the publish gate is behind -DOMDFS_TEST_HOOKS). A direct
# compile keeps it independent of the Makefile's object cache, which is built without
# the flag for the normal binary.
CF="$(pkg-config --cflags fuse3) -O2 -std=c11 -Wall -Wextra -Wno-format-truncation -DOMDFS_TEST_HOOKS"
if ! cc $CF -o "$BIN" "$ROOT"/src/*.c $(pkg-config --libs fuse3) >"$WORK/build.log" 2>&1; then
	echo "Bail out! test-hook build failed"; cat "$WORK/build.log"; exit 99
fi

OMDFS_TEST_GATE_MATCH="target_b" OMDFS_TEST_GATE_DIR="$GATE" OMDFS_SYNC_BACKOFF_MS=100 \
	"$BIN" --backend "$BACKEND" --datadir "$DATA" -f "$MNT" >"$LOG" 2>&1 &
PID=$!
for i in $(seq 1 50); do mountpoint -q "$MNT" && break; sleep 0.1; done
mountpoint -q "$MNT" || { echo "Bail out! mount failed"; cat "$LOG"; exit 99; }

waitf() { for i in $(seq 1 100); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }
bwait() { for i in $(seq 1 80); do [ "$(cat "$2" 2>/dev/null)" = "$1" ] && return 0; sleep 0.1; done; return 1; }

fail=0
ok() { echo "ok - $1"; }
no() { echo "not ok - $1"; fail=1; }

printf 'AAAA' > "$MNT/source_a"
bwait "AAAA" "$BACKEND/source_a" || no "A synced to backend (precondition)"

printf 'BBBB' > "$MNT/target_b"
waitf "$GATE/ready" || { no "B's flush reached the publish gate"; echo "Bail out! gate never engaged"; exit 1; }

python3 -c 'import os,sys; os.rename(sys.argv[1], sys.argv[2])' "$MNT/source_a" "$MNT/target_b" \
	|| no "rename A onto B's path"
bwait "AAAA" "$BACKEND/target_b" || no "structural rename drained to backend"

touch "$GATE/go"      # release B's now-superseded flush
sleep 1.5

got="$(cat "$BACKEND/target_b" 2>/dev/null)"
[ "$got" = "AAAA" ] && ok "backend holds A's bytes (B's superseded flush discarded)" \
	|| no "backend clobbered by B's stale flush (got [$got], want [AAAA])"
[ "$(cat "$MNT/target_b" 2>/dev/null)" = "AAAA" ] && ok "mount view is A's bytes" \
	|| no "mount view wrong"

[ $fail = 0 ] && echo "# rename-replace flush race: PASS" || echo "# rename-replace flush race: FAIL"
exit $fail
