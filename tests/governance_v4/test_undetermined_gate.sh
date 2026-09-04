#!/usr/bin/env bash
# ============================================================
# test_undetermined_gate.sh — output_admissibility.on_undetermined
#
# THE GAP. While a handle sits inside its adaptive baseline window,
# `in_baseline` suppresses every STATISTICAL signal's penalty, so coherence
# stays at its ceiling whatever the agent is doing. The output-admissibility
# gate compares that ceiling against its threshold and passes. The pass is not
# a judgement — it is the absence of one — and nothing in the telemetry, the
# response dict or the dashboard distinguished the two.
#
# THE SHAPE OF THE FIX. "Undetermined" marks the PASS DIRECTION ONLY. A LOW
# coherence stays meaningful while calibrating, because objective signals
# (byte-identical repetition, S1/S21) are exempt from baseline absorption and
# charge from turn 1 — the gate can still rule OUT, it just cannot rule IN.
# Group D is the positive control for exactly that asymmetry; without it this
# suite would pass for an implementation that labelled every calibrating turn
# undetermined and stopped gating altogether.
#
# Group A: the label appears, and only while calibrating.
#          BOTH directions asserted — a run must produce undetermined turns
#          AND determined ones. An always-undetermined implementation fails.
# Group B: on_undetermined="pass" (the default) changes no outcome. Every
#          response admissible; identical admissible count to the same run with
#          adaptive baselining off.
# Group C: on_undetermined="quarantine" holds EXACTLY the turns Group A
#          labelled — set equality, not "more than before".
# Group D: positive control — the gate still FAILS on low coherence while
#          undetermined (objective signals charge through the baseline).
# Group E: undetermined holds do NOT advance the quarantine streak.
#          The baseline window default (5) and max_quarantine_streak default
#          (5) are the same length, so a streak-advancing undetermined hold
#          terminates every agent on its last calibrating turn regardless of
#          behaviour. This group is the guard against that unconditional kill.
# Group V: vacuity — with adaptive baselining OFF, zero undetermined events and
#          on_undetermined="quarantine" is completely inert.
# ============================================================
# NOTE ON COUNTING: assertions read the OUTPUT_ADMISSIBILITY_EVAL "result"
# field, not the whole telemetry line. CDD_TURN also carries a "baseline_state"
# field with the same vocabulary; a line-wide grep for "calibrating" matches
# events that have nothing to do with the gate.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/undet-gate-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

IS_WINDOWS=false
if [[ "$(uname -s)" == MINGW* ]] || [[ "$(uname -s)" == MSYS* ]] || [[ -n "${WINDIR:-}" ]]; then IS_WINDOWS=true; fi

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_undetermined_gate.sh"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_UNDET="fake-key-undetermined"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

source "$SCRIPT_DIR/../helpers/stub_launch.sh"  # D1: shared hardened launcher

# ------------------------------------------------------------
# Fixtures
# ------------------------------------------------------------
# VARIED: eight substantively different responses on one topic. Nothing here
# should ever fire an objective signal — this is the "agent behaving" arm, and
# it is what makes Group A's undetermined turns a statement about the ENGINE's
# calibration state rather than about the content.
VARIED_FIXTURE='{"responses": [
  {"content": "The calculator module exposes an add method that returns the sum of two operands.", "output_tokens": 40},
  {"content": "Subtraction is implemented by negating the second operand before delegating to add.", "output_tokens": 42},
  {"content": "Multiplication uses repeated addition only for integers; floats take the native operator.", "output_tokens": 44},
  {"content": "Division guards against a zero denominator and raises a descriptive error instead.", "output_tokens": 41},
  {"content": "Each arithmetic operation appends a formatted entry to the calculator history log.", "output_tokens": 43},
  {"content": "The history log is capped at one hundred entries and discards the oldest first.", "output_tokens": 42},
  {"content": "A clear method empties the history log and resets the running accumulator to zero.", "output_tokens": 44},
  {"content": "Unit tests cover each operation plus the zero denominator and history cap paths.", "output_tokens": 43}
]}'

# IDENTICAL: eight byte-identical responses. S21 response_repetition and S1
# circular_actions are OBJECTIVE — exempt from baseline absorption — so this
# arm drives coherence down DURING the baseline window. Group D's control.
IDENTICAL_FIXTURE='{"responses": [
  {"content": "done", "output_tokens": 20},
  {"content": "done", "output_tokens": 20},
  {"content": "done", "output_tokens": 20},
  {"content": "done", "output_tokens": 20},
  {"content": "done", "output_tokens": 20},
  {"content": "done", "output_tokens": 20},
  {"content": "done", "output_tokens": 20},
  {"content": "done", "output_tokens": 20}
]}'

# $1=port $2=adaptive(true/false) $3=on_undetermined line-or-empty $4=max_quarantine_streak
mk_govern() {
cat <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": $2,
    "adaptive_baseline_window": 5,
    "adaptive_baseline_sensitivity": 2.0
  },
  "circuit_breaker": {
    "enabled": true,
    "output_admissibility": {
      "enabled": true, "threshold": 0.70, "action": "quarantine",
      "max_quarantine_streak": $4${3}
    }
  },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$1",
      "api_key_env": "FAKE_KEY_UNDET", "max_tokens": 200, "max_turns": 30 } }
}
EOF
}

EIGHT_SENDS='use agent
main {
    let h = agent.create("worker")
    let i = 0
    let inadm = 0
    let undet = 0
    while i < 8 {
        let r = agent.send(h, "describe the next part of the calculator module")
        let a = r.get("admissibility")
        if a != null {
            if a.get("admissible") == false { inadm = inadm + 1 }
            if a.get("undetermined") == true { undet = undet + 1 }
        }
        i = i + 1
    }
    print("INADMISSIBLE=" + string(inadm))
    print("UNDETERMINED_DICT=" + string(undet))
    print("FINAL_COHERENCE=" + string(agent.coherence(h)))
}'

# $1=name $2=fixture $3=adaptive $4=on_undetermined-json-fragment $5=streak
# sets OUT / WDIR; returns 1 if the stub never came up.
run_case() {
    WDIR="$TEST_TMP/$1"
    mkdir -p "$WDIR"
    printf '%s' "$2" > "$WDIR/fixture.json"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    mk_govern "$STUB_PORT" "$3" "$4" "$5" > "$WDIR/govern.json"; sign_govern "$WDIR"
    printf '%s' "$EIGHT_SENDS" > "$WDIR/test.naab"
    OUT=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
    stop_stub
    return 0
}

# Count OUTPUT_ADMISSIBILITY_EVAL events by their "result" field only.
oa_count() {  # $1=telemetry file $2=result value
    grep '"event_type":"OUTPUT_ADMISSIBILITY_EVAL"' "$1" 2>/dev/null \
        | grep -o '"result":"[^"]*"' | grep -c "\"result\":\"$2\"" || true
}
# Turn numbers carrying a given result, sorted — for set equality in Group C.
oa_turns() {  # $1=telemetry file $2=result value
    python3 - "$1" "$2" <<'PY'
import json, sys
f, want = sys.argv[1], sys.argv[2]
turns = []
try:
    for line in open(f):
        line = line.strip()
        if '"OUTPUT_ADMISSIBILITY_EVAL"' not in line: continue
        try: e = json.loads(line)
        except Exception: continue
        if e.get("event_type") != "OUTPUT_ADMISSIBILITY_EVAL": continue
        d = e.get("fields", e)
        if d.get("result") == want: turns.append(int(d.get("turn", -1)))
except FileNotFoundError:
    pass
print(",".join(str(t) for t in sorted(turns)))
PY
}
val() { echo "$OUT" | grep "^$1=" | sed 's/.*=//'; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  output_admissibility.on_undetermined — the calibration gate  |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A — the label appears, and only while calibrating
# ============================================================
echo -e "${CYAN}--- Group A: undetermined is emitted while the baseline calibrates ---${NC}"
A_UNDET=""; A_TURNS=""; A_ADMISS=""
if run_case "a_default" "$VARIED_FIXTURE" true "" 0; then
    A_TELE="$WDIR/tele.jsonl"
    A_UNDET=$(oa_count "$A_TELE" "undetermined")
    A_PASS=$(oa_count "$A_TELE" "pass")
    A_TURNS=$(oa_turns "$A_TELE" "undetermined")
    A_ADMISS=$(val INADMISSIBLE)
    if [ "${A_UNDET:-0}" -gt 0 ]; then
        pass "A-01" "Calibrating turns labelled undetermined ($A_UNDET events, turns [$A_TURNS])"
    else
        fail "A-01" "No undetermined events emitted" "pass=$A_PASS undet=$A_UNDET"
    fi
    # The other half. Without this an implementation that labels EVERY turn
    # undetermined — i.e. one that never notices the baseline completing —
    # would satisfy A-01.
    if [ "${A_PASS:-0}" -gt 0 ]; then
        pass "A-02" "Post-baseline turns labelled pass, not undetermined ($A_PASS events)"
    else
        fail "A-02" "Every turn undetermined — baseline completion not observed" "pass=$A_PASS undet=$A_UNDET"
    fi
    if grep -q '"baseline_state":"calibrating"' "$A_TELE" 2>/dev/null && \
       grep -q '"baseline_state":"complete"' "$A_TELE" 2>/dev/null; then
        pass "A-03" "baseline_state attributes each verdict (calibrating and complete both present)"
    else
        fail "A-03" "baseline_state missing or single-valued" \
             "$(grep -o '"baseline_state":"[^"]*"' "$A_TELE" 2>/dev/null | sort -u | tr '\n' ' ')"
    fi
    if [ "$(val UNDETERMINED_DICT)" = "${A_UNDET:-x}" ]; then
        pass "A-04" "response.admissibility.undetermined agrees with telemetry ($(val UNDETERMINED_DICT))"
    else
        fail "A-04" "Script-visible flag disagrees with telemetry" \
             "dict=$(val UNDETERMINED_DICT) telemetry=$A_UNDET"
    fi
else skip "A-01" "stub failed"; fi

# ============================================================
# Group D — POSITIVE CONTROL: the gate still rules OUT while undetermined
# ============================================================
echo -e "${CYAN}--- Group D: low coherence still FAILS during calibration ---${NC}"
D_FAIL=""; D_INADM=""
if run_case "d_objective" "$IDENTICAL_FIXTURE" true "" 0; then
    D_TELE="$WDIR/tele.jsonl"
    D_FAIL=$(oa_count "$D_TELE" "fail")
    D_INADM=$(val INADMISSIBLE)
    D_COH=$(val FINAL_COHERENCE)
    if [ "${D_FAIL:-0}" -gt 0 ]; then
        pass "D-01" "Objective signals drove a real fail through the baseline window ($D_FAIL fails, coherence=$D_COH)"
    else
        fail "D-01" "Nothing failed — undetermined swallowed the gate entirely" \
             "coherence=$D_COH undetermined=$(oa_count "$D_TELE" undetermined) pass=$(oa_count "$D_TELE" pass)"
    fi
    # And those fails are NOT labelled undetermined: the label is one-directional.
    if ! grep '"event_type":"OUTPUT_INADMISSIBLE"' "$D_TELE" 2>/dev/null | grep -q '"undetermined":"true"'; then
        pass "D-02" "Real failures are not mislabelled undetermined"
    else
        fail "D-02" "A coherence failure was labelled undetermined"
    fi
else skip "D-01" "stub failed"; fi

# ============================================================
# Group B — the default policy changes nothing
# ============================================================
# WHAT THIS CANNOT BE. The first draft compared this arm against the same
# fixture with adaptive baselining OFF and called equality "no behaviour
# change". That control is invalid, and its failure is the phenomenon under
# study: with baselining off nothing absorbs the statistical signals, the
# varied fixture drops below threshold and SIX turns are genuinely
# inadmissible. Baselining-on and baselining-off are different configurations
# that are SUPPOSED to differ. The comparison that actually pins "no behaviour
# change" is same-config, key-absent vs key="pass" — plus the structural claim
# that no hold ever originates from an undetermined verdict.
echo -e "${CYAN}--- Group B: on_undetermined defaults to pass (no behaviour change) ---${NC}"
if [ -n "$A_ADMISS" ]; then
    if [ "$A_ADMISS" -eq 0 ]; then
        pass "B-01" "Default policy delivered every response (0 inadmissible)"
    else
        fail "B-01" "Default policy held responses" "inadmissible=$A_ADMISS"
    fi
else skip "B-01" "Group A did not run"; fi
# Key absent vs key explicitly "pass". A sweep over explicit values cannot see
# a default, and "key absent" is the condition every existing config is in.
if run_case "b_explicit_pass" "$VARIED_FIXTURE" true ', "on_undetermined": "pass"' 0; then
    B_EXPLICIT=$(val INADMISSIBLE)
    B_TURNS=$(oa_turns "$WDIR/tele.jsonl" "undetermined")
    if [ "$B_EXPLICIT" = "$A_ADMISS" ] && [ "$B_TURNS" = "$A_TURNS" ]; then
        pass "B-02" "Explicit pass identical to key absent (held=$B_EXPLICIT, turns [$B_TURNS])"
    else
        fail "B-02" "Default differs from explicit pass" \
             "absent=[$A_TURNS]/$A_ADMISS explicit=[$B_TURNS]/$B_EXPLICIT"
    fi
else skip "B-02" "stub failed"; fi
# The load-bearing one: with the gate ACTIVE AND FIRING (the identical-response
# arm from Group D, default policy), every hold must trace to a "fail" verdict
# and none to an "undetermined" one. This is what "the label does not leak into
# enforcement" means, and unlike B-01 it is not satisfied by a gate that never
# fires at all.
if [ -n "${D_FAIL:-}" ] && [ -n "${D_INADM:-}" ]; then
    if [ "$D_INADM" = "$D_FAIL" ] && [ "${D_FAIL:-0}" -gt 0 ]; then
        pass "B-03" "Under the default policy every hold traces to a fail verdict ($D_INADM of $D_FAIL)"
    else
        fail "B-03" "Hold count does not match the fail count" "held=$D_INADM fails=$D_FAIL"
    fi
else skip "B-03" "Group D did not run"; fi

# ============================================================
# Group C — quarantine holds exactly the undetermined turns
# ============================================================
echo -e "${CYAN}--- Group C: on_undetermined=quarantine holds exactly those turns ---${NC}"
if run_case "c_quarantine" "$VARIED_FIXTURE" true ', "on_undetermined": "quarantine"' 0; then
    C_TELE="$WDIR/tele.jsonl"
    C_INADM=$(val INADMISSIBLE)
    C_TURNS=$(oa_turns "$C_TELE" "undetermined")
    if [ -n "$A_UNDET" ] && [ "${C_INADM:-x}" = "${A_UNDET:-y}" ] && [ "${C_INADM:-0}" -gt 0 ]; then
        pass "C-01" "Quarantine policy held exactly the undetermined turns ($C_INADM)"
    else
        fail "C-01" "Held count does not match the undetermined count" \
             "held=$C_INADM undetermined_in_A=$A_UNDET"
    fi
    # Set equality on turn numbers, not just cardinality — two runs can hold
    # the same NUMBER of different turns.
    if [ -n "$A_TURNS" ] && [ "$C_TURNS" = "$A_TURNS" ]; then
        pass "C-02" "Same turn set as the default-policy run ([$C_TURNS])"
    else
        fail "C-02" "Different turns held" "A=[$A_TURNS] C=[$C_TURNS]"
    fi
    if grep '"event_type":"OUTPUT_INADMISSIBLE"' "$C_TELE" 2>/dev/null | grep -q '"undetermined":"true"'; then
        pass "C-03" "OUTPUT_INADMISSIBLE distinguishes an undetermined hold from incoherence"
    else
        fail "C-03" "Undetermined holds indistinguishable from real inadmissibility" \
             "$(grep -c '"event_type":"OUTPUT_INADMISSIBLE"' "$C_TELE" 2>/dev/null) events"
    fi
else skip "C-01" "stub failed"; fi

# ============================================================
# Group E — undetermined holds do not advance the quarantine streak
# ============================================================
echo -e "${CYAN}--- Group E: undetermined holds do not advance the kill streak ---${NC}"
# Baseline window 5 and max_quarantine_streak 3: if undetermined holds advanced
# the streak, the run would die on the third calibrating turn with a
# GovernanceHardError and never print. This is not hypothetical — the engine
# DEFAULTS are window 5 and streak 5, so a streak-advancing undetermined hold
# terminates every agent on its last calibrating turn.
if run_case "e_streak" "$VARIED_FIXTURE" true ', "on_undetermined": "quarantine"' 3; then
    E_TELE="$WDIR/tele.jsonl"
    E_INADM=$(val INADMISSIBLE)
    # The holds must OUTNUMBER the streak limit, or survival proves nothing —
    # a build that emits no undetermined verdicts at all survives trivially,
    # and the mutation run confirmed E-01/E-02 pass vacuously without this.
    E_UNDET=$(oa_count "$E_TELE" "undetermined")
    if ! echo "$OUT" | grep -q "FINAL_COHERENCE="; then
        fail "E-01" "Undetermined holds terminated the agent" "$(echo "$OUT" | tail -3)"
        fail "E-02" "Streak fired on undetermined holds" \
             "streak_exceeded=$(grep -c '"event_type":"QUARANTINE_STREAK_EXCEEDED"' "$E_TELE" 2>/dev/null || true)"
    elif [ "${E_UNDET:-0}" -le 3 ]; then
        # Survival proves nothing if fewer holds occurred than the streak limit —
        # a build emitting no undetermined verdicts survives trivially, and the
        # mutation run confirmed E-01/E-02 passed vacuously without this guard.
        fail "E-01" "Too few undetermined holds to exercise the streak limit" "undetermined=$E_UNDET limit=3"
        fail "E-02" "Streak limit never exercised" "undetermined=$E_UNDET limit=3"
    else
        pass "E-01" "Run survived $E_UNDET undetermined holds against streak limit 3"
        if ! grep -q '"event_type":"QUARANTINE_STREAK_EXCEEDED"' "$E_TELE" 2>/dev/null; then
            pass "E-02" "No QUARANTINE_STREAK_EXCEEDED from undetermined holds"
        else
            fail "E-02" "Streak fired on undetermined holds"
        fi
    fi
    # Negative control for E-02: the event must still be reachable. Same limit,
    # same window, but the identical-response fixture makes the holds REAL.
    # Without this, E-02 would pass for a build where the streak never fires.
    if run_case "e_control" "$IDENTICAL_FIXTURE" true "" 3; then
        if grep -q '"event_type":"QUARANTINE_STREAK_EXCEEDED"' "$WDIR/tele.jsonl" 2>/dev/null; then
            pass "E-03" "Control: genuine quarantines still hit the streak limit"
        else
            fail "E-03" "Streak limit unreachable — E-02 proves nothing" \
                 "fails=$(oa_count "$WDIR/tele.jsonl" fail)"
        fi
    else skip "E-03" "stub failed"; fi
else skip "E-01" "stub failed"; fi

# ============================================================
# Group V — vacuity: no baselining means nothing is undetermined
# ============================================================
echo -e "${CYAN}--- Group V: vacuity checks ---${NC}"
# With adaptive baselining off every signal charges from turn 1, so a pass is
# determined. If ANY undetermined event appears here the label is being applied
# on some other basis and Groups A and C mean nothing.
#
# NOTE ON "INERT": this arm is NOT quiet. Without absorption the varied fixture
# drops below threshold and produces genuine holds, so inert cannot mean "zero
# inadmissible" — it means "the same outcomes the policy-free run produced".
# V-03 therefore runs BOTH policies over the identical configuration and
# compares, which is also the only form of the claim that would catch the
# policy firing on determined turns.
V_INADM=""
if run_case "v_no_adaptive_default" "$VARIED_FIXTURE" false "" 0; then
    V_BASE_INADM=$(val INADMISSIBLE)
else V_BASE_INADM=""; fi
if run_case "v_no_adaptive" "$VARIED_FIXTURE" false ', "on_undetermined": "quarantine"' 0; then
    V_TELE="$WDIR/tele.jsonl"
    V_UNDET=$(oa_count "$V_TELE" "undetermined")
    V_INADM=$(val INADMISSIBLE)
    V_FAIL=$(oa_count "$V_TELE" "fail")
    V_EVENTS=$(grep -c '"event_type":"OUTPUT_ADMISSIBILITY_EVAL"' "$V_TELE" 2>/dev/null || true)
    if [ "${V_EVENTS:-0}" -gt 0 ]; then
        pass "V-01" "Gate evaluated ($V_EVENTS events) — the arm is not empty"
    else
        fail "V-01" "No gate evaluations at all — V-02/V-03 would pass vacuously"
    fi
    if [ "${V_UNDET:-1}" -eq 0 ]; then
        pass "V-02" "Zero undetermined events with baselining off"
    else
        fail "V-02" "Undetermined applied without a baseline window" "undetermined=$V_UNDET"
    fi
    if [ -n "$V_BASE_INADM" ] && [ "$V_INADM" = "$V_BASE_INADM" ] && [ "$V_INADM" = "$V_FAIL" ]; then
        pass "V-03" "quarantine policy inert with baselining off (held=$V_INADM, all from fail verdicts)"
    else
        fail "V-03" "Policy changed outcomes with baselining off" \
             "default=$V_BASE_INADM policy=$V_INADM fails=$V_FAIL"
    fi
    if grep -q '"baseline_state":"disabled"' "$V_TELE" 2>/dev/null; then
        pass "V-04" "baseline_state reports disabled rather than defaulting to complete"
    else
        fail "V-04" "baseline_state does not distinguish disabled from complete" \
             "$(grep -o '"baseline_state":"[^"]*"' "$V_TELE" 2>/dev/null | sort -u | tr '\n' ' ')"
    fi
else skip "V-01" "stub failed"; fi

# ============================================================
# Group R — ratchet: relaxing the policy mid-run is a loosening
# ============================================================
echo -e "${CYAN}--- Group R: on_undetermined ratchet ---${NC}"
if $IS_WINDOWS; then
    skip "R-01" "mid-run file swap requires POSIX file semantics"
else
WDIR="$TEST_TMP/r"; mkdir -p "$WDIR"
printf '%s' "$VARIED_FIXTURE" > "$WDIR/fixture.json"
if start_stub "$WDIR/fixture.json" "$WDIR"; then
presign() {
    local pdir="$TEST_TMP/.presign"; mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"; sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""; rm -rf "$pdir"
}
R_BASE="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"tele.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1,\"adaptive_baseline_enabled\":true,\"adaptive_baseline_window\":5},\"circuit_breaker\":{\"enabled\":true,\"output_admissibility\":{\"enabled\":true,\"threshold\":0.70,\"action\":\"quarantine\",\"max_quarantine_streak\":0,\"on_undetermined\":\"ON_UNDET\"}},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"worker\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_UNDET\",\"max_tokens\":200,\"max_turns\":30}}}"
LOOSE="${R_BASE/ON_UNDET/pass}"
LOOSE_SIG=$(presign "$LOOSE")
if [ -z "$LOOSE_SIG" ]; then skip "R-01" "presign failed"; stop_stub; else
export NAAB_LOOSE_JSON="$LOOSE" NAAB_LOOSE_SIG="$LOOSE_SIG"
printf '%s' "${R_BASE/ON_UNDET/quarantine}" > "$WDIR/govern.json"
sign_govern "$WDIR"
cat > "$WDIR/test.naab" <<'EOF'
use agent
main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "describe the calculator module")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "describe the calculator module")
    print("DONE")
}
EOF
OUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub
if grep -qi "ratchet\|loosen" "$WDIR/stderr.txt" 2>/dev/null && grep -q "on_undetermined" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "R-01" "quarantine -> pass rejected as a loosening"
else
    fail "R-01" "mid-run relaxation not rejected" \
         "$(grep -i 'ratchet\|reload\|loosen' "$WDIR/stderr.txt" 2>/dev/null | head -2)"
fi
fi
else skip "R-01" "stub failed"; fi
fi

# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
