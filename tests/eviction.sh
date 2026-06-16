#!/usr/bin/env bash
#
# omdfs eviction test (Phase 5, SPEC criterion 6).
#
# Mount with a small --cache-size, then drive an over-budget workload and verify:
#   A. read pressure  — reading more data than the budget keeps DATADIR/cache
#                       at/under the limit (LRU clean files are evicted) while
#                       every file stays readable (evicted files re-fetch).
#   B. dirty pinning  — with the backend unreachable, un-synced (dirty) files are
#                       never evicted even when they exceed the budget, so no
#                       un-synced data is lost; they flush once the backend is back.
#
# Usage: tests/eviction.sh [path-to-omdfs-binary]   (default: ./omdfs)

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
LOG="$WORK/omdfs.log"
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

# Total bytes of cached CONTENT files (exclude the local-only .omdfs.dir indexes
# and their mkstemp temporaries).
content_bytes() {
	find "$DATA/cache" -type f \
		! -name "$INDEX" ! -name "$INDEX".* \
		-printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}'
}
content_files() {
	find "$DATA/cache" -type f \
		! -name "$INDEX" ! -name "$INDEX".* 2>/dev/null | wc -l
}
INDEX=".omdfs.dir"

mount_omdfs() { # args: --cache-size value
	"$OMDFS" --backend "$BACKEND" --datadir "$DATA" --cache-size "$1" -f "$MNT" \
		>"$LOG" 2>&1 &
	PID=$!
	local i
	for i in $(seq 1 50); do
		grep -F " $MNT fuse" /proc/mounts >/dev/null 2>&1 && return 0
		kill -0 "$PID" 2>/dev/null || return 1
		sleep 0.1
	done
	return 1
}

# ---- fixture: 10 x 200 KiB files = ~2 MiB, with a 1 MiB budget ----
BUDGET=1048576          # 1 MiB
FILESZ=204800           # 200 KiB
NFILES=10               # ~2 MiB total backend content
mkdir -p "$BACKEND" "$DATA" "$MNT"
declare -a SUM
for i in $(seq 0 $((NFILES-1))); do
	head -c "$FILESZ" /dev/urandom > "$BACKEND/f$i"
	SUM[$i]="$(md5sum < "$BACKEND/f$i" | cut -d' ' -f1)"
done

if ! mount_omdfs "$BUDGET"; then
	echo "Bail out! omdfs failed to mount; log follows:" >&2
	cat "$LOG" >&2
	exit 98
fi

# ============================================================
# A. Read pressure keeps the cache bounded; data stays accessible
# ============================================================
for i in $(seq 0 $((NFILES-1))); do cat "$MNT/f$i" >/dev/null; done

# After reading 2 MiB through a 1 MiB cache, the on-disk content must be bounded.
bounded() { [ "$(content_bytes)" -le "$BUDGET" ]; }
wait_for "cache content stays within budget after read pressure" bounded
if bounded; then ok "cache content <= budget ($(content_bytes) <= $BUDGET)"
else no "cache content <= budget ($(content_bytes) > $BUDGET)"; fi

# Eviction actually happened: fewer than all files remain cached.
if [ "$(content_files)" -lt "$NFILES" ]; then
	ok "eviction occurred ($(content_files) of $NFILES files cached)"
else
	no "eviction occurred (all $NFILES files still cached)"
fi

# Every file is still readable and byte-correct (evicted ones re-fetch).
allgood=1
for i in $(seq 0 $((NFILES-1))); do
	got="$(md5sum < "$MNT/f$i" | cut -d' ' -f1)"
	[ "$got" = "${SUM[$i]}" ] || { allgood=0; break; }
done
[ "$allgood" = 1 ] && ok "all files readable + byte-correct after eviction" \
	|| no "all files readable + byte-correct after eviction"

# Re-reading naturally re-evicts; the cache must still be bounded.
if bounded; then ok "cache still bounded after re-reads"
else no "cache still bounded after re-reads ($(content_bytes) > $BUDGET)"; fi

# ============================================================
# B. Dirty (un-synced) files are never evicted, even over budget
# ============================================================
# Let read-pressure dirt settle, then cut the backend off so new writes can't
# sync and must stay dirty in the cache.
mv "$BACKEND" "$WORK/backend.gone"

NDIRTY=8                # 8 x 200 KiB = ~1.6 MiB of dirty data > 1 MiB budget
for i in $(seq 0 $((NDIRTY-1))); do
	head -c "$FILESZ" /dev/urandom > "$WORK/d$i.src"
	cp "$WORK/d$i.src" "$MNT/d$i"          # create + write; dirty, cannot sync
done

# Dirty data exceeds the budget but must NOT have been evicted.
if [ "$(content_bytes)" -gt "$BUDGET" ]; then
	ok "dirty data retained above budget ($(content_bytes) > $BUDGET)"
else
	no "dirty data retained above budget ($(content_bytes) <= $BUDGET)"
fi

# Every dirty file is intact and readable with the backend gone.
allgood=1
for i in $(seq 0 $((NDIRTY-1))); do
	if ! cmp -s "$MNT/d$i" "$WORK/d$i.src"; then allgood=0; break; fi
done
[ "$allgood" = 1 ] && ok "all dirty files intact with backend gone (none evicted)" \
	|| no "all dirty files intact with backend gone (none evicted)"

# Restore the backend; the dirty files must now flush through.
mv "$WORK/backend.gone" "$BACKEND"
bmatch() { cmp -s "$BACKEND/$1" "$WORK/$1.src"; }
wait_for "dirty file d0 reaches backend" bmatch d0
wait_for "dirty file d7 reaches backend" bmatch d7

# ---- summary ----
echo "1..$COUNT"
if [ "$FAIL" -ne 0 ]; then
	echo "# FAILED: $FAIL of $COUNT"
	exit 1
fi
echo "# All $COUNT eviction tests passed"
exit 0
