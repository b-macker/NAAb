#!/usr/bin/env bash
# ============================================================
# test_escalation_scope.sh — what advisory escalation counts, measured
#
# advisory_escalation exists so that "persistent issues must be addressed"
# (3cd561e): the soft_after-th repeat of an advisory becomes an uncatchable
# block. Persistence is a property of ONE subject repeating. Two behaviours
# decouple the count from its subject, and were accepted without being
# reasoned about (docs/governance-campaign-findings.md, round 7):
#
#   (A) the count is keyed by RULE NAME for the whole engine, so every agent
#       shares it — "repeated" means "happened N times anywhere";
#   (B) the count is halved at every evidence-epoch boundary, INCLUDING a
#       governance-level change caused by the very drift being counted.
#
# THIS SUITE PINS CURRENT BEHAVIOUR. It is a measurement, not an endorsement:
# ES-02, ES-04 and ES-06 assert what the engine does today. If (A) or (B) is
# changed deliberately, the arm that flips is the record of that decision — flip
# it in the same commit, never relax it to pass.
#
# Ground truth is set by construction: the stub serves distinct on-topic
# responses, and the script decides each agent's validation results. A
# "transient" agent fails once and then passes; a "persistent" agent fails every
# turn. coherence_threshold 0.9 makes one S22 failure (0.15) a coherence_loss
# occurrence and one recovery (+0.075) clear it.
#
#   ES-01  control: 2 agents, one transient dip each -> completes
#   ES-02  (A) 3 agents, one transient dip each -> KILLED today, each counted x1
#          (no agent repeated anything; the kill scales with team size)
#   ES-03  control: 1 persistent agent, level static -> killed on its 3rd
#          occurrence (analyzed turn 3)
#   ES-04  (B) the same agent with circuit-breaker thresholds low enough that
#          its drift moves the level -> killed one analyzed turn LATER today
#          (NORMAL->ELEVATED halved its first occurrence away)
#   ES-05  control: fail/pass alternating agent, level static -> killed after
#          3 firings
#   ES-06  (B) the same alternating agent whose drift moves the level both
#          ways -> today survives far longer (>= 2x the firings), because each
#          level change halves its record
#   ES-07  every agent persistent, shared count -> killed by analyzed turn 2
#   ES-08  load-bearing context for (A): every agent persistent, escalation
#          OFF, no admissibility gate -> the run COMPLETES at coherence 0 under
#          "Governance: PASS". In such a config escalation is the only thing
#          that stops drift, so any change to (A) must keep this case killed.
#   ES-09  the same with escalation OFF but the quarantine streak configured
#          (repo-sentinel shape) -> killed by QUARANTINE_STREAK_EXCEEDED: the
#          other mechanism that covers system-wide drift, and when it fires.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/escalation-scope-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2"; }

echo "=== advisory escalation: what the count is keyed on, and what erases it ==="

IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=1
if [ "$IS_WINDOWS" -eq 1 ] || ! command -v python3 >/dev/null 2>&1; then
    for id in ES-01 ES-02 ES-03 ES-04 ES-05 ES-06 ES-07 ES-08 ES-09; do
        skip "$id" "agent stub unsupported here (UNMEASURABLE)"
    done
    echo ""; echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"; exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "${TEST_TMP:?}"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
export FAKE_KEY_ESCOPE="fake-key-escalation-scope"

CB_MOVES=',"circuit_breaker":{"enabled":true,"elevated_threshold":0.05,"elevated_sustained":1,"high_threshold":0.15,"high_sustained":1,"critical_threshold":0.99,"critical_sustained":50,"deescalate_sustained":1}'
OA_STREAK=',"circuit_breaker":{"enabled":true,"output_admissibility":{"enabled":true,"threshold":0.6,"action":"quarantine","max_quarantine_streak":3,"require_corroboration":2}}'

# run_case DIR NAGENTS PATTERN ESCALATION(true|false) EXTRA_JSON
# PATTERN: one char per round after the opening send; f = failed validation
# before the next send, p = passed. Every agent follows the same pattern.
# Sets RESULT (completed|killed), LAST_TURN (max analyzed CDD turn), FIRINGS
# (coherence_loss warnings + escalation), LEVEL_CHANGES, ATTRIB, EVENTS.
run_case() {
    local d="$1" n="$2" pat="$3" esc="$4" extra="${5:-}"
    mkdir -p "$d"
    python3 -c '
import json, sys
topics = ["input validation in the request handler", "error handling in the parser",
          "bounds checks in the tokenizer", "null checks in the config loader",
          "timeout handling in the client", "logging in the scheduler",
          "retry logic in the uploader", "escaping in the template renderer",
          "locking in the cache", "pagination in the listing endpoint",
          "rounding in the billing module", "encoding in the exporter"]
r = [{"content": "Reviewed %s: found one defect, proposed a fix, and the related tests now cover it." % t,
      "output_tokens": 30} for t in topics * 6]
json.dump({"responses": r}, open(sys.argv[1], "w"))
' "$d/fixture.json"
    STUB_PID=""; start_stub "$d/fixture.json" "$d" || return 1
    local ag='"provider":"gemini","model":"stub-model","api_base":"http://127.0.0.1:'"$STUB_PORT"'","api_key_env":"FAKE_KEY_ESCOPE","system_prompt":"Review code for defects and propose fixes.","max_tokens":100,"max_turns":40,"retry":{"max_attempts":1,"backoff_ms":0}'
    local agents="" i k c
    for i in $(seq 1 "$n"); do agents="$agents${agents:+,}\"a$i\":{$ag}"; done
    cat > "$d/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "coherence_threshold": 0.9, "check_interval_turns": 1 },
  "advisory_escalation": { "enabled": $esc, "soft_after": 3 },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" }
  $extra,
  "agents": { $agents } }
EOF
    {
        echo "use agent"; echo "main {"
        for i in $(seq 1 "$n"); do echo "    let h$i = agent.create(\"a$i\")"; done
        echo '    let r = ""'
        for i in $(seq 1 "$n"); do echo "    r = agent.send(h$i, \"review module $i part 0\")"; done
        for k in $(seq 0 $((${#pat} - 1))); do
            c="${pat:$k:1}"
            for i in $(seq 1 "$n"); do
                if [ "$c" = "f" ]; then
                    echo "    agent.record_validation(h$i, false, \"test_$i failed\")"
                else
                    echo "    agent.record_validation(h$i, true)"
                fi
                echo "    r = agent.send(h$i, \"review module $i part $((k + 1))\")"
            done
        done
        echo '    print("REACHED_END")'; echo "}"
    } > "$d/t.naab"
    (cd "$d" && timeout 120 "$NAAB" t.naab > out.txt 2>&1)
    kill "$STUB_PID" 2>/dev/null; STUB_PID=""
    local out; out="$(cat "$d/out.txt")"
    case "$out" in *REACHED_END*) RESULT=completed ;; *) RESULT=killed ;; esac
    FIRINGS=$(grep -cE '^\[governance\] (WARNING|ESCALATED) context_drift\.coherence_loss' <<<"$out")
    ATTRIB="$(grep -o 'Occurrences counted:.*' <<<"$out" | head -1)"
    read -r LAST_TURN LEVEL_CHANGES EVENTS < <(python3 -c '
import json, sys, collections
turn, lc, ev = 0, 0, collections.Counter()
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    try: e = json.loads(l)
    except ValueError: continue
    t = e.get("event_type")
    if t == "CDD_TURN" and e.get("analyzed") == "true":
        turn = max(turn, int(e.get("turn", 0)))
    if t == "GOVERNANCE_LEVEL_CHANGE": lc += 1
    if t in ("QUARANTINE_STREAK_EXCEEDED", "OUTPUT_INADMISSIBLE"): ev[t] += 1
print(turn, lc, ",".join("%s=%d" % kv for kv in sorted(ev.items())) or "none")
' "$d/tele.jsonl" 2>/dev/null || echo "0 0 none")
    return 0
}

need() { # $1=id: a stub that never came up is UNMEASURABLE, not a verdict
    skip "$1" "stub failed to start (UNMEASURABLE)"
}

# ---- (A) one count shared across agents ----
if run_case "$TEST_TMP/es01" 2 fppppp true; then
    if [ "$RESULT" = completed ]; then
        pass "ES-01" "control: 2 agents with one transient dip each complete"
    else
        fail "ES-01" "2 transient agents were killed -- the fixture is not transient" "$ATTRIB"
    fi
else need ES-01; fi

if run_case "$TEST_TMP/es02" 3 fppppp true; then
    if [ "$RESULT" = killed ] && [ "$ATTRIB" = "Occurrences counted: <agent:a1> x1, <agent:a2> x1, <agent:a3> x1" ]; then
        pass "ES-02" "(A) pinned: 3 agents with ONE transient dip each kill the run (each counted x1)"
    else
        fail "ES-02" "(A) changed: 3 single-dip agents no longer killed this way (result=$RESULT)" "${ATTRIB:-no attribution}"
    fi
else need ES-02; fi

# ---- (B) halving at a level change the drift itself caused ----
S3_TURN=""
if run_case "$TEST_TMP/es03" 1 ffffff true; then
    if [ "$RESULT" = killed ] && [ "$LEVEL_CHANGES" -eq 0 ] && [ "$LAST_TURN" -eq 3 ]; then
        S3_TURN=$LAST_TURN
        pass "ES-03" "control: a persistent drifter, level static, is killed at analyzed turn 3"
    else
        fail "ES-03" "control drifted (result=$RESULT turn=$LAST_TURN level_changes=$LEVEL_CHANGES)" "$ATTRIB"
    fi
else need ES-03; fi

if run_case "$TEST_TMP/es04" 1 ffffffffff true "$CB_MOVES"; then
    if [ "$RESULT" = killed ] && [ "$LEVEL_CHANGES" -ge 1 ] && [ -n "$S3_TURN" ] && [ "$LAST_TURN" -gt "$S3_TURN" ]; then
        pass "ES-04" "(B) pinned: the same drifter moving the level is killed later (turn $LAST_TURN vs $S3_TURN, $LEVEL_CHANGES level changes)"
    else
        fail "ES-04" "(B) changed or fixture inert (result=$RESULT turn=$LAST_TURN vs ${S3_TURN:-?}, level_changes=$LEVEL_CHANGES)"
    fi
else need ES-04; fi

S5_FIRINGS=""
if run_case "$TEST_TMP/es05" 1 fpfpfpfpfpfpfp true; then
    if [ "$RESULT" = killed ] && [ "$LEVEL_CHANGES" -eq 0 ] && [ "$FIRINGS" -eq 3 ]; then
        S5_FIRINGS=$FIRINGS
        pass "ES-05" "control: an alternating drifter, level static, is killed after 3 firings"
    else
        fail "ES-05" "control drifted (result=$RESULT firings=$FIRINGS level_changes=$LEVEL_CHANGES)"
    fi
else need ES-05; fi

if run_case "$TEST_TMP/es06" 1 fpfpfpfpfpfpfp true "$CB_MOVES"; then
    if [ -n "$S5_FIRINGS" ] && [ "$LEVEL_CHANGES" -ge 2 ] && [ "$FIRINGS" -ge $((S5_FIRINGS * 2)) ]; then
        pass "ES-06" "(B) pinned: when its drift moves the level, the same agent takes $FIRINGS firings to stop (vs $S5_FIRINGS; $LEVEL_CHANGES level changes, result=$RESULT)"
    else
        fail "ES-06" "(B) changed or fixture inert (firings=$FIRINGS vs ${S5_FIRINGS:-?}, level_changes=$LEVEL_CHANGES, result=$RESULT)"
    fi
else need ES-06; fi

# ---- system-wide drift: what stops it, and when ----
if run_case "$TEST_TMP/es07" 3 ffffffffff true; then
    if [ "$RESULT" = killed ] && [ "$LAST_TURN" -le 2 ]; then
        pass "ES-07" "every agent persistent, shared count: killed by analyzed turn $LAST_TURN"
    else
        fail "ES-07" "(result=$RESULT turn=$LAST_TURN)" "$ATTRIB"
    fi
else need ES-07; fi

if run_case "$TEST_TMP/es08" 3 ffffffffff false; then
    if [ "$RESULT" = completed ]; then
        pass "ES-08" "context: escalation off, no admissibility gate -> all agents drift to the end (turn $LAST_TURN) and the run completes"
    else
        fail "ES-08" "something else now stops system-wide drift here (result=$RESULT events=$EVENTS) -- update the (A) analysis"
    fi
else need ES-08; fi

if run_case "$TEST_TMP/es09" 3 ffffffffff false "$OA_STREAK"; then
    case "$EVENTS" in
        *QUARANTINE_STREAK_EXCEEDED=1*)
            pass "ES-09" "context: escalation off, quarantine streak on -> killed by the streak at analyzed turn $LAST_TURN" ;;
        *) fail "ES-09" "the streak did not stop system-wide drift (result=$RESULT events=$EVENTS)" ;;
    esac
else need ES-09; fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
