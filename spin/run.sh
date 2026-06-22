#!/usr/bin/env bash
# Exhaustively verify the omdfs SPIN models. For each configuration it runs a full
# state-space search and reports PASS (no error) or FOUND BUG. The buggy configs
# are EXPECTED to fail; the fixed configs are EXPECTED to pass. `--check` exits
# non-zero if any config deviates, so it can gate CI. `--quick` skips the largest
# bounds.
#
# Models:
#   omdfs-flush.pml     focused clobber proof: pending_count gate + flushing flag.
#   omdfs-syncer.pml    comprehensive write-back engine: convergence + ordering +
#                       deadlock over every op sequence up to MAXOPS.
#   omdfs-recovery.pml  crash recovery: WAL-suffix replay, stale-high checkpoint
#                       clamp, and the s_initial_drained dry-flush guard.
#
# Usage: ./run.sh [--check] [--quick]
set -u
cd "$(dirname "$0")"
CHECK=0; QUICK=0
for a in "$@"; do
	[ "$a" = "--check" ] && CHECK=1
	[ "$a" = "--quick" ] && QUICK=1
done
rc=0

# run_cfg <label> <expect pass|bug> <model> <pan-claim-args> -- <spin -a defines...>
run_cfg() {
	local label="$1" expect="$2" model="$3" claim="$4"; shift 4
	[ "$1" = "--" ] && shift
	local defs="$*"
	rm -f pan pan.* _spin_nvr.tmp "$model".trail
	if ! spin -a $defs "$model" >/dev/null 2>spin.err; then
		echo "  $label: spin -a FAILED"; cat spin.err; rc=1; return; fi
	cc -O2 -o pan pan.c 2>cc.err || { echo "  $label: cc FAILED"; cat cc.err; rc=1; return; }
	local out errs states depth got
	out=$(./pan $claim -m8000000 2>&1)
	errs=$(echo "$out" | sed -n 's/.*errors: \([0-9]*\).*/\1/p')
	states=$(echo "$out" | sed -n 's/ *\([0-9]*\) states, stored.*/\1/p' | head -1)
	depth=$(echo "$out"  | sed -n 's/.*depth reached \([0-9]*\).*/\1/p')
	if [ "${errs:-1}" = "0" ]; then got=pass; else got=bug; fi
	printf '  %-30s -> %-9s (%s states, depth %s)\n' \
	       "$label" "$([ $got = bug ] && echo 'FOUND BUG' || echo PASS)" "${states:-?}" "${depth:-?}"
	[ $got = bug ] && [ -f "$model".trail ] && mv "$model".trail "trail.${label// /_}"
	[ "$expect" != "$got" ] && { echo "      !! expected $expect, got $got"; rc=1; }
}

echo "============================================================"
echo " omdfs SPIN verification"
echo "============================================================"

echo
echo "[1] omdfs-flush.pml -- clobber proof (gate + flushing flag)"
echo "    property no_clobber; buggy MUST reach the clobber, fixed MUST be safe"
run_cfg "buggy gate" bug  omdfs-flush.pml "-a -N no_clobber" -- -DSCENARIO=1
run_cfg "buggy flag" bug  omdfs-flush.pml "-a -N no_clobber" -- -DSCENARIO=2
run_cfg "fixed gate" pass omdfs-flush.pml "-a -N no_clobber" -- -DFIX -DSCENARIO=1
run_cfg "fixed flag" pass omdfs-flush.pml "-a -N no_clobber" -- -DFIX -DSCENARIO=2

echo
echo "[2] omdfs-syncer.pml -- write-back engine over all op sequences"
echo "    convergence: buggy MUST diverge, fixed MUST converge"
run_cfg "buggy converge MAXOPS=3" bug  omdfs-syncer.pml "-a -N converge" -- -DMAXOPS=3
run_cfg "fixed converge MAXOPS=3" pass omdfs-syncer.pml "-a -N converge" -- -DFIX -DMAXOPS=3
run_cfg "fixed converge MAXOPS=4" pass omdfs-syncer.pml "-a -N converge" -- -DFIX -DMAXOPS=4
if [ $QUICK = 0 ]; then
	run_cfg "fixed converge MAXOPS=5" pass omdfs-syncer.pml "-a -N converge" -- -DFIX -DMAXOPS=5
fi
echo "    ordering asserts + deadlock-freedom (no never-claim):"
run_cfg "fixed safety  MAXOPS=4" pass omdfs-syncer.pml "" -- -DNOLTL -DFIX -DMAXOPS=4

echo
echo "[3] omdfs-recovery.pml -- crash recovery"
echo "    1=suffix replay  2=stale-high clamp  3=dry-flush guard"
for s in 1 2 3; do
	run_cfg "buggy scenario $s" bug  omdfs-recovery.pml "" -- -DSCENARIO=$s
	run_cfg "fixed scenario $s" pass omdfs-recovery.pml "" -- -DFIX -DSCENARIO=$s
done

echo
rm -f pan pan.* _spin_nvr.tmp spin.err cc.err
if [ $CHECK = 1 ]; then
	[ $rc = 0 ] && echo "RESULT: all configurations matched expectation." \
	            || echo "RESULT: a configuration deviated from expectation (see above)."
	exit $rc
fi
