#!/usr/bin/env bash
# test_governance_statemachine.sh -- CI smoke for the bounded governance
#   state-machine explorer (Bundle 10.2; A113)
#
# The full exploration is a report-only tool (tools/governance_explore/explore.py)
# run by hand, like schedule_perturb -- minutes at depth 4-5. This is the thin
# depth-3 smoke that runs per PR (~10s): it guards the two safety invariants
# against regression over every bounded ordering of propose / commit / sibling /
# proposer-send, and it checks the TOOL itself can fail.
#
#   SM-00   the checker selftest: the invariant checker flags a synthesized
#           violation and passes a clean stream (explore.py --selftest)
#   SM-01   depth-3 exploration is CLEAN: no commit lands at CRITICAL (INV1) and
#           the level never de-escalates (INV2). The tool exits 2 (UNMEASURABLE),
#           not 0, if no commit ever succeeded or CRITICAL was never reached, so
#           a pass here is not vacuous.
#   SM-02   NEGATIVE CONTROL: with --plant-violation the same run exits 1, so
#           SM-01's "exit 0" is a real signal, not a tool that cannot fail.
#
# Not a mutant of the engine: a genuine INV1 regression (a commit landing at
# CRITICAL) surfaces as explore.py exit 1, exactly as SM-02 forces. The engine
# gate itself (checkCriticalSuspension) is pinned by test_propose_commit.sh J-01.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$ROOT/build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
EXPLORE="$ROOT/tools/governance_explore/explore.py"

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

command -v python3 >/dev/null 2>&1 || { echo "python3 not available -- skipping"; exit 0; }
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) echo "stub-driven explorer skipped on Windows"; exit 0 ;; esac
[ -f "$EXPLORE" ] || { fail SM-00 "explore.py missing"; echo "Results: 0 passed, 1 failed, 0 unmeasurable"; exit 1; }

echo "=== governance state-machine smoke (A113) ==="

if python3 "$EXPLORE" --selftest >/tmp/sm_self.$$ 2>&1; then pass SM-00 "checker selftest: flags a violation, passes a clean stream"
else fail SM-00 "checker selftest failed" "$(cat /tmp/sm_self.$$)"; fi
rm -f /tmp/sm_self.$$

OUT=$(python3 "$EXPLORE" --smoke --naab "$NAAB" 2>&1); RC=$?
case "$RC" in
    0) if grep -q "no invariant violated" <<<"$OUT"; then pass SM-01 "depth-3 exploration clean ($(grep -oE 'run: [0-9]+' <<<"$OUT" | head -1))"
       else fail SM-01 "exit 0 without the clean marker" "$OUT"; fi ;;
    2) unm SM-01 "exploration UNMEASURABLE (staging/positive control not met)" "$OUT" ;;
    1) fail SM-01 "an invariant was violated" "$OUT" ;;
    *) fail SM-01 "explorer errored (rc=$RC)" "$OUT" ;;
esac

OUT2=$(python3 "$EXPLORE" --smoke --plant-violation --naab "$NAAB" 2>&1); RC2=$?
if [ "$RC2" -eq 1 ] && grep -q "PLANT" <<<"$OUT2"; then pass SM-02 "NEGATIVE CONTROL: a planted violation makes the tool exit 1"
elif [ "$RC2" -eq 2 ]; then unm SM-02 "plant run UNMEASURABLE (staging not met)" "$OUT2"
else fail SM-02 "a planted violation did not fail the tool (rc=$RC2)" "$OUT2"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
exit 0
