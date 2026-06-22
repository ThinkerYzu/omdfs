#!/usr/bin/env bash
# Exhaustively verify the omdfs flush/structural-drain exclusion model with SPIN.
#
# For each of the four configurations it runs a full state-space search of the
# `no_clobber` safety property and reports PASS (no error) or FOUND BUG (the
# clobber). The buggy configs are EXPECTED to fail; the fixed configs are
# EXPECTED to pass. `--check` makes the script exit non-zero if any config
# deviates from that expectation, so it can gate CI.
#
# Usage: ./run.sh [--check]
set -u
cd "$(dirname "$0")"
MODEL=omdfs-flush.pml
PROP=no_clobber
CHECK=0; [ "${1:-}" = "--check" ] && CHECK=1
rc=0

run_one() {
	local name="$1" expect="$2" defs="$3"
	rm -f pan pan.* _spin_nvr.tmp "$MODEL".trail
	spin -a $defs "$MODEL" >/dev/null 2>spin.err || { echo "  $name: spin -a FAILED"; cat spin.err; rc=1; return; }
	cc -O2 -o pan pan.c 2>cc.err          || { echo "  $name: cc FAILED";     cat cc.err;   rc=1; return; }
	local out errs got
	out=$(./pan -a -N "$PROP" -m200000 2>&1)
	errs=$(echo "$out" | sed -n 's/.*errors: \([0-9]*\).*/\1/p')
	local states depth
	states=$(echo "$out" | sed -n 's/ *\([0-9]*\) states, stored.*/\1/p' | head -1)
	depth=$(echo "$out"  | sed -n 's/.*depth reached \([0-9]*\).*/\1/p')
	if [ "${errs:-0}" = "0" ]; then got=pass; else got=bug; fi
	printf '  %-28s -> %-8s (%s states, depth %s)\n' \
	       "$name" "$([ $got = bug ] && echo 'FOUND BUG' || echo PASS)" "${states:-?}" "${depth:-?}"
	# preserve the counter-example trail for the buggy configs
	if [ $got = bug ] && [ -f "$MODEL".trail ]; then
		mv "$MODEL".trail "trail.$name"
	fi
	if [ "$expect" != "$got" ]; then
		echo "      !! expected $expect, got $got"
		rc=1
	fi
}

echo "omdfs flush-exclusion :: SPIN exhaustive verification of $PROP"
echo
echo "BUGGY syncer (no per-entry exclusion) -- clobber MUST be reachable:"
run_one buggy-gate bug "-DSCENARIO=1"
run_one buggy-flag bug "-DSCENARIO=2"
echo
echo "FIXED syncer (pending_count gate + flushing flag) -- MUST be clobber-free:"
run_one fixed-gate pass "-DFIX -DSCENARIO=1"
run_one fixed-flag pass "-DFIX -DSCENARIO=2"
echo
rm -f pan pan.* _spin_nvr.tmp spin.err cc.err
if [ $CHECK = 1 ]; then
	[ $rc = 0 ] && echo "RESULT: all four configurations matched expectation." \
	            || echo "RESULT: a configuration deviated from expectation (see above)."
	exit $rc
fi
