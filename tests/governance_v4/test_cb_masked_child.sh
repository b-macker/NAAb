#!/usr/bin/env bash
# ============================================================
# test_cb_masked_child.sh — CONTRA-014: a circuit_breaker child the parent masks
#
# THE FAILURE THIS CATCHES
#
# circuit_breaker.enabled defaults to false, and output admissibility and
# governance-level escalation both sit behind it. A config that sets only
#   "circuit_breaker": {"output_admissibility": {"enabled": true}}
# got no gate, no OUTPUT_ADMISSIBILITY_EVAL event and no warning. An outside
# dogfood run (Gemini, F-01) lost the gate in all ten runs and found the cause
# only by reading agent_impl.cpp.
#
#   CM-01  output_admissibility on, breaker off -> CONTRA-014 names it
#   CM-02  step_up_enabled on, breaker off -> CONTRA-014 names it, and says the
#          lease trigger still works (the half that is NOT masked)
#   CM-03  CONTROL: the same children with the breaker on -> silent
#   CM-04  CONTROL: breaker off and no dependent child -> silent
#   CM-05  it takes contradiction_detection.max_level: at hard the run stops
#          (exit 3) -- it names two keys that disagree, which the operator CAN fix
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

W="${TMPDIR:-/tmp}/naab-cbmask-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }
printf 'main { print("ran") }\n' > "$W/p.naab"

echo "=== CONTRA-014: circuit_breaker children masked by the parent ==="

cfg() {  # $1 = circuit_breaker object, $2 = contradiction max_level
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"},"contradiction_detection":{"enabled":true,"max_level":"%s"},"circuit_breaker":%s}\n' "${2:-advisory}" "$1" > "$W/govern.json"
}
run() { ( cd "$W" && timeout 30 "$NAAB" p.naab 2>&1 ); }

cfg '{"output_admissibility":{"enabled":true,"threshold":0.6}}'
out=$(run); rc=$?
if [ $rc -eq 0 ] && grep -q 'CONTRA-014' <<<"$out" && grep -q 'output_admissibility (never evaluated)' <<<"$out"; then
    ok "CM-01" "output_admissibility without the breaker is reported"
else bad "CM-01" "masked output_admissibility not reported (rc=$rc)" "$(grep -m2 CONTRA <<<"$out")"; fi

cfg '{"step_up_enabled":true}'
out=$(run)
if grep -q 'CONTRA-014' <<<"$out" && grep -q 'only an expired lease triggers one' <<<"$out"; then
    ok "CM-02" "step_up_enabled without the breaker is reported, naming the half that still works"
else bad "CM-02" "masked step_up not reported" "$(grep -m2 CONTRA <<<"$out")"; fi

cfg '{"enabled":true,"step_up_enabled":true,"output_admissibility":{"enabled":true,"threshold":0.6}}'
out=$(run)
if ! grep -q 'CONTRA-014' <<<"$out" && grep -q '^ran' <<<"$out"; then ok "CM-03" "CONTROL: silent when the breaker is on"
else bad "CM-03" "CONTRA-014 fired with the breaker on"; fi

cfg '{"elevated_threshold":0.4}'
out=$(run)
if ! grep -q 'CONTRA-014' <<<"$out"; then ok "CM-04" "CONTROL: silent when no dependent child is enabled"
else bad "CM-04" "CONTRA-014 fired with nothing masked"; fi

cfg '{"output_admissibility":{"enabled":true}}' hard
out=$(run); rc=$?
if [ $rc -eq 3 ] && grep -q 'CONTRA-014' <<<"$out" && ! grep -q '^ran' <<<"$out"; then
    ok "CM-05" "at max_level hard the masked child stops the run"
else bad "CM-05" "max_level hard did not stop the run (rc=$rc)"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
