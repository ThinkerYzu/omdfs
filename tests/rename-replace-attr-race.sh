#!/usr/bin/env bash
# Regression for the ATTR half of the cross-identity rename-replace flush clobber (see
# proj_docs/omdfs/bug-rename-replace-flush.md and spin/omdfs-rename-replace.pml).
#
# Same race as the content version, but for an attr-only flush: B is dirty-attr (a chmod)
# and its flush is about to stamp B's mode onto B's backend path P. Meanwhile A (a clean
# file with a different mode) is renamed onto P, replacing B; the structural rename puts
# A's file (and A's mode) at P. If B's stale attr flush then runs, it chmods A's file to
# B's mode -- an attr clobber. The fix (meta_flush_attr_begin/end: apply only if the
# entry's indirection is still attached, under meta_mtx) skips the superseded apply.
#
# Self-validating: drives the race deterministically (via the OMDFS_TEST_HOOKS attr gate)
# against BOTH the guarded build and an unguarded one (-DOMDFS_NO_ATTR_GUARD). A's mode
# (644) must survive with the guard; the unguarded build MUST clobber it to B's 600 --
# proving the test actually exercises the race.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
command -v fusermount3 >/dev/null 2>&1 || { echo "Bail out! fusermount3 not found"; exit 99; }
CF="$(pkg-config --cflags fuse3) -O2 -std=c11 -Wall -Wextra -Wno-format-truncation -DOMDFS_TEST_HOOKS"

# run_race <extra-cflags> -> prints the backend mode of target_b after the race
run_race() {
	local extra="$1"
	local W BK DT MT GT LG BIN pid i
	W="$(mktemp -d)"; BK="$W/backend"; DT="$W/data"; MT="$W/mnt"; GT="$W/gate"; LG="$W/log"; BIN="$W/bin"
	mkdir -p "$BK" "$DT" "$MT" "$GT"
	if ! cc $CF $extra -o "$BIN" "$ROOT"/src/*.c $(pkg-config --libs fuse3) >"$W/b.log" 2>&1; then
		echo "BUILDFAIL"; cat "$W/b.log" >&2; rm -rf "$W"; return
	fi
	OMDFS_TEST_ATTR_MATCH="target_b" OMDFS_TEST_GATE_DIR="$GT" OMDFS_SYNC_BACKOFF_MS=100 \
		"$BIN" --backend "$BK" --datadir "$DT" -f "$MT" >"$LG" 2>&1 &
	pid=$!
	for i in $(seq 1 50); do mountpoint -q "$MT" && break; sleep 0.1; done
	if ! mountpoint -q "$MT"; then echo "MOUNTFAIL"; cat "$LG" >&2; kill "$pid" 2>/dev/null; rm -rf "$W"; return; fi

	wf() { for i in $(seq 1 100); do [ -e "$1" ] && return 0; sleep 0.1; done; return 1; }
	bw() { for i in $(seq 1 80); do [ "$(cat "$2" 2>/dev/null)" = "$1" ] && return 0; sleep 0.1; done; return 1; }

	printf 'AAAA' > "$MT/source_a"; chmod 644 "$MT/source_a"; bw "AAAA" "$BK/source_a"
	printf 'BBBB' > "$MT/target_b"; bw "BBBB" "$BK/target_b"
	chmod 600 "$MT/target_b"                          # B dirty-attr -> parks at the gate
	wf "$GT/attr_ready" || { echo "NOGATE"; fusermount3 -u "$MT" 2>/dev/null; kill "$pid" 2>/dev/null; rm -rf "$W"; return; }
	python3 -c 'import os,sys; os.rename(sys.argv[1], sys.argv[2])' "$MT/source_a" "$MT/target_b"
	bw "AAAA" "$BK/target_b"                          # structural rename drained (A's bytes)
	touch "$GT/attr_go"; sleep 1.2                    # release B's superseded attr flush

	stat -c '%a' "$BK/target_b" 2>/dev/null
	fusermount3 -u "$MT" >/dev/null 2>&1; kill "$pid" 2>/dev/null; rm -rf "$W"
}

fail=0
ok() { echo "ok - $1"; }
no() { echo "not ok - $1"; fail=1; }

guarded="$(run_race "")"
unguarded="$(run_race "-DOMDFS_NO_ATTR_GUARD")"

[ "$guarded" = "644" ] && ok "guarded build: A's mode 644 survives (superseded attr flush discarded)" \
	|| no "guarded build clobbered A's mode (got [$guarded], want 644)"
[ "$unguarded" = "600" ] && ok "unguarded build clobbers to B's 600 (the race is real / test has teeth)" \
	|| no "unguarded build did not reproduce the clobber (got [$unguarded], want 600)"

[ $fail = 0 ] && echo "# rename-replace attr race: PASS" || echo "# rename-replace attr race: FAIL"
exit $fail
