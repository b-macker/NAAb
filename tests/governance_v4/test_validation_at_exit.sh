#!/usr/bin/env bash
# ============================================================
# test_validation_at_exit.sh — a validation recorded after the last turn is scored
#
# THE FAILURE THIS CATCHES
#
# agent.record_validation() latches its result and the NEXT recordTurn scores
# it -- and recordTurn runs only on an agent response. A pipeline checks a
# handle's FINAL answer after that handle's last send, so the last result for
# every handle was never scored: coherence stayed 1.0000 through a recorded
# ground-truth failure. Found by an outside dogfood run (Gemini, release-notes
# pipeline, F-02: 5/5 adversarial runs), and present in examples/agent_harness
# too (its planner and its final worker step).
#
#   VX-01  failure recorded after the last send -> scored at exit:
#          VALIDATION_SCORED_AT_EXIT with coherence falling, a failed
#          context_drift.validation_outcome finding in the report
#   VX-02  CONTROL: a failure followed by another send is scored by that turn
#          (CDD_TURN validation_outcome) and NOT again at exit -- no double
#          charge
#   VX-03  CONTROL: a PASS recorded after the last send is not scored
#   VX-04  the finding has teeth: a quality gate on advisory findings fails
#          the run (exit 2)
#   VX-05  CONTROL: with the S22 signal switched off nothing is scored
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-vexit-$$"
STUB_PID=""
cleanup(){ [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

echo "=== Validation recorded after a handle's last turn ==="

ALL="VX-01 VX-02 VX-03 VX-04 VX-05"
IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
if [ "$IS_WINDOWS" = 1 ] || ! command -v python3 >/dev/null 2>&1; then
    for id in $ALL; do skip "$id" "agent stub unavailable (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

printf '{"routes": {"VEXIT": {"responses": [{"content": "the release notes list every commit", "output_tokens": 30}]}}}\n' > "$W/stub.json"
start_stub "$W/stub.json" "$W" || { for id in $ALL; do skip "$id" "stub failed (UNMEASURABLE)"; done; exit 0; }

cfg() {  # $1 = extra top-level JSON members (may be empty), $2 = S22 on/off
    cat > "$W/govern.json" <<EOF
{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"},
 "telemetry":{"enabled":true,"output_file":"t.jsonl"},
 "governance":{"report_json":"report.json"},
 "behavioral_sequences":{"enabled":true},
 "context_drift":{"enabled":true,"check_interval_turns":1,"signals":{"validation_outcome":${2:-true}}},
 "agents":{"writer":{"provider":"gemini","model":"gemini-2.5-flash","api_key_env":"GEMINI_API_KEY",
   "api_base":"http://127.0.0.1:$STUB_PORT","system_prompt":"VEXIT release notes writer","max_tokens":100}}${1:+,$1}}
EOF
}
prog() {  # $1 = body lines after create
    printf 'use agent\nmain {\n  let h = agent.create("writer")\n%s\n  print("done")\n}\n' "$1" > "$W/p.naab"
}
run() { rm -f "$W/t.jsonl" "$W/report.json"; ( cd "$W" && GEMINI_API_KEY=stub timeout 60 "$NAAB" p.naab 2>&1 ); }
events() {  # $1 = event_type -> count
    python3 - "$W/t.jsonl" "$1" <<'PY'
import json, sys
n = 0
try:
    for l in open(sys.argv[1]):
        e = json.loads(l)
        if e.get("event_type") == sys.argv[2]:
            n += 1
except OSError:
    pass
print(n)
PY
}
exit_drop() {  # prints "before after" of the first VALIDATION_SCORED_AT_EXIT
    python3 - "$W/t.jsonl" <<'PY'
import json, sys
for l in open(sys.argv[1]):
    e = json.loads(l)
    if e.get("event_type") == "VALIDATION_SCORED_AT_EXIT":
        print(e.get("coherence_before"), e.get("coherence_after")); break
PY
}

cfg ""
prog '  agent.send(h, "write notes")
  agent.record_validation(h, false, "notes announce v9.0 but truth says v1.0.0")'
out=$(run); rc=$?
read -r before after <<<"$(exit_drop)"
finding=$(python3 -c "import json,sys;r=json.load(sys.stdin);print(sum(1 for c in (r.get('results') or []) if c.get('rule')=='context_drift.validation_outcome' or c.get('rule_name')=='context_drift.validation_outcome'))" < "$W/report.json" 2>/dev/null)
if [ "$(events VALIDATION_SCORED_AT_EXIT)" = 1 ] && python3 -c "import sys; sys.exit(0 if float('$after') < float('$before') else 1)" 2>/dev/null \
   && [ "${finding:-0}" -ge 1 ] && grep -q 'scored at exit' <<<"$out"; then
    ok "VX-01" "a failure after the last send is scored at exit ($before -> $after) and reported"
else
    bad "VX-01" "unscored (rc=$rc events=$(events VALIDATION_SCORED_AT_EXIT) before=$before after=$after finding=$finding)"
fi

prog '  agent.send(h, "write notes")
  agent.record_validation(h, false, "wrong version")
  agent.send(h, "fix the version")'
out=$(run)
turn_scored=$(python3 - "$W/t.jsonl" <<'PY'
import json, sys
print(sum(1 for l in open(sys.argv[1]) if (lambda e: e.get("event_type")=="CDD_TURN" and "validation_outcome" in (e.get("penalties_detail") or ""))(json.loads(l))))
PY
)
if [ "${turn_scored:-0}" -ge 1 ] && [ "$(events VALIDATION_SCORED_AT_EXIT)" = 0 ]; then
    ok "VX-02" "CONTROL: a failure followed by a send is scored by that turn, not again at exit"
else bad "VX-02" "double charge or not scored (turn=$turn_scored exit=$(events VALIDATION_SCORED_AT_EXIT))"; fi

prog '  agent.send(h, "write notes")
  agent.record_validation(h, true, "")'
out=$(run)
if [ "$(events VALIDATION_SCORED_AT_EXIT)" = 0 ] && ! grep -q 'scored at exit' <<<"$out"; then
    ok "VX-03" "CONTROL: a final PASS is not scored"
else bad "VX-03" "a final pass was scored"; fi

cfg '"quality_gate":{"enabled":true,"conditions":[{"metric":"advisory_violations","operator":">","threshold":0}]}'
prog '  agent.send(h, "write notes")
  agent.record_validation(h, false, "wrong version")'
out=$(run); rc=$?
if [ $rc -eq 2 ]; then ok "VX-04" "a quality gate on advisory findings fails the run (exit 2)"
else bad "VX-04" "quality gate did not see the exit-scored failure (rc=$rc)" "$(grep -m2 -i 'gate' <<<"$out")"; fi

cfg "" false
prog '  agent.send(h, "write notes")
  agent.record_validation(h, false, "wrong version")'
out=$(run)
if [ "$(events VALIDATION_SCORED_AT_EXIT)" = 0 ]; then ok "VX-05" "CONTROL: with S22 off nothing is scored at exit"
else bad "VX-05" "scored with S22 disabled"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
