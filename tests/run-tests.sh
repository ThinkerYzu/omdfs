#!/usr/bin/env bash
#
# omdfs integration tests.
#
# Mounts omdfs over a throwaway backend and asserts real filesystem behavior.
# Covers Phase 1 (read-only passthrough) and Phase 2 (metadata index). Output
# is TAP-ish ("ok N - desc" / "not ok N - desc"); exits non-zero on any failure.
#
# Usage: tests/run-tests.sh [path-to-omdfs-binary]   (default: ./omdfs)

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
OMDFS_PID=""

cleanup() {
	fusermount3 -u "$MNT" >/dev/null 2>&1
	[ -n "$OMDFS_PID" ] && kill "$OMDFS_PID" >/dev/null 2>&1
	rm -rf "$WORK"
}
trap cleanup EXIT

# ---- tiny TAP assertion framework ----
COUNT=0
FAIL=0
ok()  { COUNT=$((COUNT+1)); printf 'ok %d - %s\n' "$COUNT" "$1"; }
no()  { COUNT=$((COUNT+1)); FAIL=$((FAIL+1)); printf 'not ok %d - %s\n' "$COUNT" "$1"; }

assert_eq() { # desc expected actual
	if [ "$2" = "$3" ]; then ok "$1"; else no "$1 (expected [$2], got [$3])"; fi
}
assert_success() { # desc cmd...
	local d="$1"; shift
	if "$@" >/dev/null 2>&1; then ok "$d"; else no "$d (command failed)"; fi
}
assert_failure() { # desc cmd...
	local d="$1"; shift
	if "$@" >/dev/null 2>&1; then no "$d (expected failure)"; else ok "$d"; fi
}
# Poll a command until it succeeds (write-back is asynchronous). ~4s budget.
wait_for() { # desc cmd...
	local d="$1"; shift
	local i
	for i in $(seq 1 40); do
		if "$@" >/dev/null 2>&1; then ok "$d"; return; fi
		sleep 0.1
	done
	no "$d (timed out)"
}
# Predicates for backend state (used with wait_for).
bcontent_eq() { [ "$(cat "$2" 2>/dev/null)" = "$1" ]; } # bcontent_eq val file
bmode_eq()    { [ "$(stat -c '%a' "$2" 2>/dev/null)" = "$1" ]; }
bsize_eq()    { [ "$(stat -c '%s' "$2" 2>/dev/null)" = "$1" ]; }
bgone()       { ! test -e "$1"; }

# ---- backend fixture ----
mkdir -p "$BACKEND/sub" "$BACKEND/cdir" "$BACKEND/p3x" "$DATA" "$MNT"
printf 'hello\n'           > "$BACKEND/file.txt"
: > "$BACKEND/empty.txt"
printf 'nested content\n'  > "$BACKEND/sub/inner.txt"
printf 'a\n'               > "$BACKEND/sub/another.txt"
ln -sf inner.txt             "$BACKEND/sub/link.txt"
printf 'rebuilt\n'         > "$BACKEND/cdir/x.txt"
printf 'rebuilt2\n'        > "$BACKEND/cdir/y.txt"
# A large file (> the fetch buffer) to exercise progressive, multi-chunk fetch
# and whole-file caching. Deterministic content so we can checksum it.
head -c 3145728 /dev/zero | tr '\0' 'X' > "$BACKEND/sub/big.bin"   # 3 MiB
BIGSUM="$(md5sum < "$BACKEND/sub/big.bin" | cut -d' ' -f1)"
# Files kept out of the diff -r sweep so their content stays uncached until the
# specific Phase 3 test that needs a genuine miss. (Random content so a wrong
# offset / short read is actually detectable.)
printf 'cold\n'            > "$BACKEND/p3x/cold.txt"        # never content-read
head -c 4194304 /dev/urandom > "$BACKEND/p3x/conc.bin"      # 4 MiB, shared fetch
head -c 4194304 /dev/urandom > "$BACKEND/p3x/prog.bin"      # 4 MiB, deep offset
CONCSUM="$(md5sum < "$BACKEND/p3x/conc.bin" | cut -d' ' -f1)"
PROGBLK="$(dd if="$BACKEND/p3x/prog.bin" bs=4096 skip=1023 count=1 2>/dev/null \
	| md5sum | cut -d' ' -f1)"

# Pre-seed a CORRUPT index for cdir so the first access must rebuild it.
mkdir -p "$DATA/cache/cdir"
printf 'this is not a valid omdfs index' > "$DATA/cache/cdir/.omdfs.dir"

# ---- mount ----
"$OMDFS" --backend "$BACKEND" --datadir "$DATA" -f "$MNT" >"$LOG" 2>&1 &
OMDFS_PID=$!
mounted=0
for _ in $(seq 1 50); do
	if grep -F " $MNT fuse" /proc/mounts >/dev/null 2>&1; then mounted=1; break; fi
	kill -0 "$OMDFS_PID" 2>/dev/null || break
	sleep 0.1
done
if [ "$mounted" != 1 ]; then
	echo "Bail out! omdfs failed to mount; log follows:" >&2
	cat "$LOG" >&2
	exit 98
fi

# ============================================================
# Phase 1 — read-only passthrough
# ============================================================
# Exclude p3cold so its file is never content-read (kept uncached for a Phase 3
# miss test); diff still validates the rest of the tree byte-for-byte.
assert_success "mount view matches backend (diff -r)" \
	diff -r --exclude=p3x "$MNT" "$BACKEND"

assert_eq "read file content" "hello" "$(cat "$MNT/file.txt")"
assert_eq "read nested file content" "nested content" "$(cat "$MNT/sub/inner.txt")"
assert_eq "empty file reads empty" "" "$(cat "$MNT/empty.txt")"
assert_eq "empty file size is 0" "0" "$(stat -c '%s' "$MNT/empty.txt")"
assert_eq "regular file size" "6" "$(stat -c '%s' "$MNT/file.txt")"
assert_eq "readlink target" "inner.txt" "$(readlink "$MNT/sub/link.txt")"
assert_eq "directory listing (sorted)" \
	"another.txt big.bin inner.txt link.txt" \
	"$(ls "$MNT/sub" | sort | tr '\n' ' ' | sed 's/ $//')"

assert_failure "ENOENT for missing path" cat "$MNT/does-not-exist"

# ============================================================
# Phase 2 — metadata index (.omdfs.dir)
# ============================================================
# Listing a directory builds and persists its index under the cache tree.
ls "$MNT" >/dev/null; ls "$MNT/sub" >/dev/null
assert_success "root index persisted" test -f "$DATA/cache/.omdfs.dir"
assert_success "sub index persisted"  test -f "$DATA/cache/sub/.omdfs.dir"

assert_eq "index file is hidden from listings" "" \
	"$(ls -a "$MNT" | grep -F '.omdfs.dir')"

# A corrupt pre-seeded index must be transparently rebuilt from the backend.
assert_eq "corrupt index is rebuilt (listing correct)" \
	"x.txt y.txt" \
	"$(ls "$MNT/cdir" | sort | tr '\n' ' ' | sed 's/ $//')"
assert_eq "corrupt index rebuild serves content" "rebuilt" "$(cat "$MNT/cdir/x.txt")"

# ============================================================
# Phase 3 — content cache + progressive read
# ============================================================
# Reading a file warms the cache: content is copied into the mirrored cache path.
# (inner.txt / big.bin were already read by the diff -r sweep above.)
assert_success "small file content cached on local disk" \
	test -f "$DATA/cache/file.txt"
assert_eq "cached file bytes match the backend" "hello" \
	"$(cat "$DATA/cache/file.txt")"
assert_success "large file content cached on local disk" \
	test -s "$DATA/cache/sub/big.bin"
assert_eq "large file reads correctly through the mount" "$BIGSUM" \
	"$(md5sum < "$MNT/sub/big.bin" | cut -d' ' -f1)"

# Concurrent opens of one *uncached* file must share a single background fetch
# (not each start their own) and all return correct bytes.
CN=8
CRES="$WORK/conc.out"; : > "$CRES"
cpids=""
for _ in $(seq 1 "$CN"); do
	( md5sum < "$MNT/p3x/conc.bin" | cut -d' ' -f1 >> "$CRES" ) &
	cpids="$cpids $!"
done
wait $cpids
assert_eq "concurrent readers of one file all get correct bytes" \
	"$CN" "$(grep -c "^$CONCSUM\$" "$CRES")"

# A read at a deep offset of a fresh uncached file must block on the fetched-len
# watermark until the fetch reaches that offset, then return the correct bytes —
# this is the progressive-read mechanism.
assert_eq "deep-offset read waits for the watermark and returns correct bytes" \
	"$PROGBLK" \
	"$(dd if="$MNT/p3x/prog.bin" bs=4096 skip=1023 count=1 2>/dev/null \
		| md5sum | cut -d' ' -f1)"

# Build p3x's index (metadata) while the backend is present; cold.txt's content
# is still never read, so it stays an uncached miss.
ls "$MNT/p3x" >/dev/null

# With the backend gone: cached *metadata* is still served (Phase 2) AND cached
# *content* is still served (Phase 3, SPEC success criterion 2); only a genuine
# content miss now needs the backend.
mv "$BACKEND" "$WORK/backend.gone"

assert_success "cached dir lists with backend gone" ls -l "$MNT/sub"
assert_eq "cached attrs served with backend gone" "15" \
	"$(stat -c '%s' "$MNT/sub/inner.txt")"
assert_eq "cached small content served with backend gone" \
	"nested content" "$(cat "$MNT/sub/inner.txt")"
assert_eq "cached large content served with backend gone (whole-file)" \
	"$BIGSUM" "$(md5sum < "$MNT/sub/big.bin" | cut -d' ' -f1)"
assert_failure "uncached content read fails with backend gone" \
	cat "$MNT/p3x/cold.txt"

mv "$WORK/backend.gone" "$BACKEND"
assert_eq "previously-uncached content reads after backend returns" \
	"cold" "$(cat "$MNT/p3x/cold.txt")"

# ============================================================
# Phase 4 — write-back (structural WAL + dirty flags + syncer)
# ============================================================
# Mutations apply locally and immediately; the background syncer pushes them to
# the backend shortly after (we poll for the backend side with wait_for).

# --- create a new file ---
echo "new content" > "$MNT/new.txt"
assert_eq "created file readable through mount" \
	"new content" "$(cat "$MNT/new.txt")"
wait_for "created file content reaches backend" \
	bcontent_eq "new content" "$BACKEND/new.txt"

# --- modify an existing (previously cached) file ---
echo "changed" > "$MNT/file.txt"
assert_eq "modified file readable through mount" \
	"changed" "$(cat "$MNT/file.txt")"
wait_for "modification reaches backend" \
	bcontent_eq "changed" "$BACKEND/file.txt"

# --- append via a second open keeps prior bytes ---
printf 'more\n' >> "$MNT/new.txt"
assert_eq "append visible through mount" \
	"$(printf 'new content\nmore')" "$(cat "$MNT/new.txt")"

# --- mkdir + nested create ---
mkdir "$MNT/d4"
assert_success "mkdir visible in mount" test -d "$MNT/d4"
wait_for "mkdir reaches backend" test -d "$BACKEND/d4"
echo "nested" > "$MNT/d4/inner.txt"
wait_for "nested file reaches backend" \
	bcontent_eq "nested" "$BACKEND/d4/inner.txt"

# --- chmod ---
chmod 0741 "$MNT/new.txt"
assert_eq "chmod reflected in mount" "741" "$(stat -c '%a' "$MNT/new.txt")"
wait_for "chmod reaches backend" bmode_eq "741" "$BACKEND/new.txt"

# --- truncate ---
truncate -s 3 "$MNT/new.txt"
assert_eq "truncate reflected in mount" "3" "$(stat -c '%s' "$MNT/new.txt")"
wait_for "truncate reaches backend" bsize_eq "3" "$BACKEND/new.txt"

# --- rename (file) ---
mv "$MNT/new.txt" "$MNT/renamed.txt"
assert_success "renamed file visible in mount" test -f "$MNT/renamed.txt"
assert_failure "old name gone from mount" test -e "$MNT/new.txt"
wait_for "rename target reaches backend" test -f "$BACKEND/renamed.txt"
wait_for "rename source gone from backend" bgone "$BACKEND/new.txt"

# --- symlink ---
ln -s renamed.txt "$MNT/link4.txt"
assert_eq "symlink readlink via mount" "renamed.txt" "$(readlink "$MNT/link4.txt")"
wait_for "symlink reaches backend" test -L "$BACKEND/link4.txt"

# --- unlink ---
rm "$MNT/renamed.txt"
assert_failure "unlinked file gone from mount" test -e "$MNT/renamed.txt"
wait_for "unlink reaches backend" bgone "$BACKEND/renamed.txt"

# --- rmdir ---
rm "$MNT/d4/inner.txt"
rmdir "$MNT/d4"
assert_failure "rmdir gone from mount" test -d "$MNT/d4"
wait_for "rmdir reaches backend" bgone "$BACKEND/d4"

# --- rename of a DIRTY directory while the backend is down ---
# The syncer flushes only the directories recorded as dirty (it no longer walks
# the whole cached tree each cycle). Renaming a directory before its descendants
# have synced moves them to new backend paths, so their dirty-set membership must
# be re-keyed or the writes would never flush. Pre-create the tree while online,
# then take the backend down so the write stays dirty across the rename, and
# confirm the content lands under the new name once the backend returns.
mkdir -p "$MNT/dd/sub"
wait_for "pre-created nested dir reaches backend" test -d "$BACKEND/dd/sub"
mv "$BACKEND" "$WORK/backend.down"      # backend offline: write can't flush
echo "deep dirty" > "$MNT/dd/sub/f.txt"
mv "$MNT/dd" "$MNT/dd2"                  # rename the dir while its file is dirty
assert_success "renamed dirty dir visible in mount" test -f "$MNT/dd2/sub/f.txt"
mv "$WORK/backend.down" "$BACKEND"       # backend returns
wait_for "dirty file under renamed dir reaches new backend path" \
	bcontent_eq "deep dirty" "$BACKEND/dd2/sub/f.txt"
wait_for "renamed-away source dir is gone from backend" bgone "$BACKEND/dd"

# --- rename onto an EXISTING directory (replace semantics) ---
# rename(2) of a directory onto an existing directory must refuse a NON-EMPTY
# target (ENOTEMPTY, like rmdir) and, for an EMPTY target, replace it -- carrying
# the source's children to the new path, not dropping them. Regression for the bug
# where meta_move_entry dropped the target's index entry while its cnode (and cache
# dir) lived on and shadowed the move: silent data loss, cache vs backend divergence.
# mv(1) would move the source INTO the dir, so call rename(2) directly.
ren() { python3 -c 'import os,sys; os.rename(sys.argv[1], sys.argv[2])' "$1" "$2"; }
mkdir "$MNT/rr_src" "$MNT/rr_dstne"
echo src  > "$MNT/rr_src/a.txt"
echo keep > "$MNT/rr_dstne/keep.txt"            # non-empty target
assert_failure "rename onto non-empty dir is ENOTEMPTY" ren "$MNT/rr_src" "$MNT/rr_dstne"
assert_success "rejected rename leaves source intact"   test -f "$MNT/rr_src/a.txt"
assert_success "rejected rename leaves target intact"   test -f "$MNT/rr_dstne/keep.txt"
mkdir "$MNT/rr_dste"                            # empty target
assert_success "rename onto empty dir succeeds"         ren "$MNT/rr_src" "$MNT/rr_dste"
assert_failure "replaced-rename source gone from mount" test -e "$MNT/rr_src"
assert_eq "moved dir's child preserved at new path" "src" "$(cat "$MNT/rr_dste/a.txt" 2>/dev/null)"
wait_for "replaced-rename child reaches new backend path" bcontent_eq "src" "$BACKEND/rr_dste/a.txt"
wait_for "replaced-rename source gone from backend"       bgone "$BACKEND/rr_src"

# --- root getattr is served from the local index, not the backend ---
# stat($MNT) (path "/") must answer from the cached root index (its dir_st, the
# same value readdir fills for "."), so the mount root remains stat-able with the
# backend gone — consistent with every other path. Regression for the old quirk
# where omdfs_getattr("/") did an unconditional backend lstat.
mv "$BACKEND" "$WORK/backend.gone2"
assert_success "root getattr served locally with backend gone" stat "$MNT"
mv "$WORK/backend.gone2" "$BACKEND"

# --- metadata-index cache coherence ---
# The per-directory index is cached parsed-in-memory; these check the cache stays
# coherent across the namespace ops that re-key or drop it. All ops run purely
# through the local cache (no backend round-trip needed for the assertion).
#
# Path reuse after rename is the case a stale cache would get WRONG: list a dir
# (caching its index under "/ca"), rename it away, then move a DIFFERENT dir onto
# the now-freed "/ca". Listing "/ca" must show the new occupant's children, not
# the cached old ones — i.e. rename must drop the stale-keyed cached subtree.
mkdir -p "$MNT/ca" "$MNT/cb"
echo old > "$MNT/ca/inca.txt"
echo new > "$MNT/cb/incb.txt"
ls "$MNT/ca" >/dev/null                  # populate the "/ca" cached index
mv "$MNT/ca" "$MNT/ca_old"               # rename away -> drop stale "/ca" subtree
mv "$MNT/cb" "$MNT/ca"                    # reuse the freed path with new contents
assert_success "reused path shows the new dir's child" test -f "$MNT/ca/incb.txt"
assert_failure "reused path does not show the old cached child" \
	test -e "$MNT/ca/inca.txt"

# rmdir then recreate the same path must yield a fresh, empty directory (the
# removed dir's cached index is dropped; the new one is published empty).
mkdir "$MNT/rc"
echo a > "$MNT/rc/a.txt"
ls "$MNT/rc" >/dev/null                   # cache "/rc" = {a.txt}
rm "$MNT/rc/a.txt"
rmdir "$MNT/rc"
mkdir "$MNT/rc"
assert_eq "recreated dir is empty (no stale cached child)" "" "$(ls -A "$MNT/rc")"

# ============================================================
# Large-directory lookup — exercise the O(1) name-hash path (>= META_HASH_MIN
# entries) on the single-entry read APIs, and its invalidation on mutation.
# ============================================================
mkdir "$MNT/big"
N=200
for i in $(seq 1 $N); do echo "v$i" > "$MNT/big/f$i.txt"; done
assert_eq "large dir lists all entries" "$N" "$(ls "$MNT/big" | wc -l)"
# Hashed single-entry getattr/read at the ends and middle of the table.
assert_eq "hashed lookup: first entry"  "v1"   "$(cat "$MNT/big/f1.txt")"
assert_eq "hashed lookup: middle entry" "v100" "$(cat "$MNT/big/f100.txt")"
assert_eq "hashed lookup: last entry"   "v200" "$(cat "$MNT/big/f200.txt")"
assert_failure "hashed lookup: absent entry is ENOENT" test -e "$MNT/big/nope.txt"
# Remove -> hash invalidated + rebuilt without the victim; neighbors still hit.
rm "$MNT/big/f100.txt"
assert_failure "removed entry gone after hash rebuild" test -e "$MNT/big/f100.txt"
assert_eq "neighbor still resolves after removal" "v101" "$(cat "$MNT/big/f101.txt")"
# In-dir rename -> in-place name change invalidates the hash.
mv "$MNT/big/f200.txt" "$MNT/big/renamed.txt"
assert_failure "old name gone after in-dir rename" test -e "$MNT/big/f200.txt"
assert_eq "new name resolves after in-dir rename" "v200" "$(cat "$MNT/big/renamed.txt")"
# Add -> hash invalidated; the new and the existing keys both resolve.
echo fresh > "$MNT/big/added.txt"
assert_eq "added entry resolves" "fresh" "$(cat "$MNT/big/added.txt")"
assert_eq "existing entry still resolves after add" "v1" "$(cat "$MNT/big/f1.txt")"
# Hashed readlink in a large directory.
ln -sf f1.txt "$MNT/big/sl"
assert_eq "hashed readlink target" "f1.txt" "$(readlink "$MNT/big/sl")"

# ---- summary ----
echo "1..$COUNT"
if [ "$FAIL" -ne 0 ]; then
	echo "# FAILED: $FAIL of $COUNT"
	exit 1
fi
echo "# All $COUNT tests passed"
exit 0
