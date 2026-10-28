#!/usr/bin/env bash
# ============================================================
# test_risk_budget_overshoot.sh -- a risk budget that overshoots zero is
# exhausted, not unlimited (A68, Bundle 3)
#
# consumeRiskBudget() subtracted without a floor, getRemainingBudget()
# returned risk_budget - consumed, -1 is also its "no budget configured"
# sentinel, and checkAdmission() refused only on remaining == 0. Two charges
# can land between two admission checks (a BSD match costs 1, a coherence loss
# costs 1), so a budget of 1 went to -1, which every later admission read as
# "no budget": the agent was admitted for the rest of the run, and
# agent.environment() reported the -1 as unlimited. Traced 2026-10-24; the
# fixture below reproduces it: an advisory BSD pattern completes on each send's
# agent.response (charge 1) and a 2-token reply fires S23 (response_degenerate)
# below a 0.99 coherence threshold, a coherence loss (charge 1), so each send is
# charged 2. Two BSD patterns cannot do it: the detector returns on the first
# pattern an event completes, so one event charges once (observed).
#
#   RB-00  CONTROL: budget 2 -- the first send is charged 2 (reaches 0) and the
#          second is refused; the fixture charges what it claims and the
#          existing remaining == 0 path refuses
#   RB-01  budget 1 -- the second send is refused (the overshoot to -1 read as
#          unlimited before the fix)
#   RB-02  budget 1 -- after the first send risk_budget_remaining reads 0, not -1
#   RB-03  CONTROL: budget 3 -- the second send is admitted (remaining 1 still
#          admits), the third is refused
#   RB-04  CONTROL: no budget -- three sends admitted, risk_budget_remaining -1
#          (the unlimited sentinel stays where no budget is configured)
#   RB-05  budget 1 -- the dashboard prints the exhausted budget as 0 remaining
#
# Kill table (each mutant rebuilt, this suite run):
#   getRemainingBudget() without the floor at 0      RB-01, RB-02, RB-03
#   the dashboard's inline remaining without it      RB-05
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
source "$SCRIPT_DIR/../helpers/stub_platform.sh" 2>/dev/null
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }

echo "=== risk budget: overshoot below zero is exhausted, not unlimited ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi
T="$(mktemp -d "${TMPDIR:-/tmp}/naab-riskbudget-XXXXXX")"
cleanup() { stop_stub 2>/dev/null; teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT
export FAKE_KEY_RB=fake

FIXTURE='{"responses":[
 {"content":"invoice one reviewed","input_tokens":10,"output_tokens":2},
 {"content":"invoice two reviewed","input_tokens":10,"output_tokens":2},
 {"content":"invoice three reviewed","input_tokens":10,"output_tokens":2},
 {"content":"invoice four reviewed","input_tokens":10,"output_tokens":2}]}'

# Three sends; each prints SENT<i> or REFUSED<i>, and the remaining budget after it.
PROG='use agent
main {
  let h = agent.create("a")
  for i in 1..4 {
    let r = "SENT"
    try {
      agent.send(h, "review invoice " + string(i))
    } catch (e) {
      r = "REFUSED"
    }
    let env = agent.environment(h)
    print(r + string(i) + " REMAINING" + string(i) + "=" + string(env["state"]["risk_budget_remaining"]))
  }
}'

# arm NAME BUDGET_JSON_FRAGMENT [extra naab-lang flags]
arm() {
    local d="$T/$1"; mkdir -p "$d"
    printf '%s\n' "$FIXTURE" > "$d/fixture.json"
    OUT="STUBFAIL"; RC=99
    if ! start_stub "$d/fixture.json" "$d" >/dev/null 2>&1; then return; fi
    cat > "$d/govern.json" <<EOF
{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "exposure_tracking":{"enabled":true,"level":"detect","max_autonomous_actions":100},
 "behavioral_sequences":{"enabled":true,"patterns":[
   {"name":"p1","level":"advisory","max_gap":3,"sequence":["agent.send","agent.response"]}]},
 "context_drift":{"enabled":true,"level":"advisory","coherence_threshold":0.99,
   "check_interval_turns":1,"adaptive_baseline_enabled":false,"coherence_natural_healing":0.0,
   "signals":{"response_degenerate":true}},
 "agents":{"a":{"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:$STUB_PORT",
   "api_key_env":"FAKE_KEY_RB","max_tokens":100,"max_turns":30$2,
   "system_prompt":"You review billing invoices for accuracy."}}}
EOF
    if ! python3 -c 'import json,sys; json.load(sys.stdin)' < "$d/govern.json"; then
        OUT="BAD FIXTURE"; stop_stub 2>/dev/null; return
    fi
    printf '%s\n' "$PROG" > "$d/t.naab"
    OUT="$(cd "$d" && timeout 90s "$NAAB" run ${3:-} t.naab 2>&1)"; RC=$?
    stop_stub 2>/dev/null
}
has() { grep -q -- "$1" <<<"$OUT"; }

arm rb00 ',"risk_budget":2'
if [ "$RC" = 0 ] && has "SENT1 REMAINING1=0" && has "REFUSED2"; then
    ok "RB-00" "control: budget 2 is charged 2 by one send, and the next send is refused"
else bad "RB-00" "the fixture does not charge 2 per send, or the exhausted budget is not refused" "rc=$RC $OUT"; fi

arm rb01 ',"risk_budget":1'
if [ "$RC" = 0 ] && has "SENT1 " && has "REFUSED2" && has "REFUSED3"; then
    ok "RB-01" "budget 1: after an overshoot, the next sends are refused"
else bad "RB-01" "an overshot budget admitted the next send" "rc=$RC $OUT"; fi
if has "REMAINING1=0"; then ok "RB-02" "budget 1: risk_budget_remaining reads 0 after the overshoot"
else bad "RB-02" "an overshot budget is reported as something other than 0" "$(grep -o 'REMAINING1=[^ ]*' <<<"$OUT")"; fi

arm rb03 ',"risk_budget":3'
if [ "$RC" = 0 ] && has "SENT1 REMAINING1=1" && has "SENT2 " && has "REFUSED3"; then
    ok "RB-03" "control: budget 3 admits the second send (1 remaining) and refuses the third"
else bad "RB-03" "budget 3 did not admit at 1 remaining, or did not refuse after" "rc=$RC $OUT"; fi

arm rb04 ''
if [ "$RC" = 0 ] && has "SENT1 REMAINING1=-1" && has "SENT2 " && has "SENT3 REMAINING3=-1"; then
    ok "RB-04" "control: with no budget every send is admitted and the sentinel is -1"
else bad "RB-04" "a run with no budget was refused or reported a budget" "rc=$RC $OUT"; fi

arm rb05 ',"risk_budget":1' --governance-dashboard
if has "a: 0 remaining"; then ok "RB-05" "budget 1: the dashboard prints the exhausted budget as 0 remaining"
else bad "RB-05" "the dashboard does not show the exhausted budget" "$(grep -i 'budget' <<<"$OUT" | head -3)"; fi

echo ""
echo "  Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
