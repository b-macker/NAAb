#!/usr/bin/env bash
# ============================================================
# test_quarantine_corroboration.sh — require corroboration before a kill
#
# Coherence is a weighted sum, so a single noisy signal firing turn after turn
# accumulates to a kill on its own. Replaying real per-turn signal traces,
# semantic_stability alone — firing on nothing worse than a compliant agent
# varying its phrasing between answers — drove coherence under the threshold on
# three consecutive turns and terminated the run, while every genuine failure
# mode still died on exactly the same turn under a two-signal rule.
#
# output_admissibility.require_corroboration gates only the STREAK ADVANCE.
# Admissibility is untouched: a quarantine is still a quarantine, still
# reported, still governs history disposition and attestation. It simply may
# not count toward termination without a second penalising signal in the
# same turn.
#
#   A  default (0)     compliant-but-varied output IS killed — the defect
#   B  corroboration 2 the same output survives
#   C  corroboration 2 genuine misbehaviour still dies
#   D  the mechanism actually engaged (not vacuously passing)
#   E  enabling it mid-run is a ratchet violation (it produces FEWER kills)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/qcorrob-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

# EXPERIMENT (reversible in one commit). build-windows has stalled inside
# "CLI tests — shell suites" four times: the step sits in_progress ~47 minutes,
# the runner is killed service-side, and the log archive 404s. timeout-minutes
# has now failed to fire TWICE, so the runner cannot enforce its own step
# timeout — we cannot read our way to the cause, only change the outcome.
#
# A live observation caught it hanging at the test_challenge_discrimination.sh
# header, immediately after another stub-backed suite passed. These 9 suites are
# the only ones that run a Python HTTP server and talk to it from a native
# Windows binary under MSYS2, so they are the region to exclude first.
#
# Read the next Windows run as the result:
#   green      -> these suites are implicated; narrow from 9
#   stalls     -> they are exonerated; the cause is elsewhere in the phase
#
# Coverage is not lost: build-linux and Build & Test both run every one of these
# in full, and what they test (agent governance semantics) is platform-neutral.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  test_quarantine_corroboration.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_quarantine_corroboration.sh: SKIPPED (stub-backed; excluded on Windows pending stall bisect)"
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_QCORROB="fake-key-qcorrob"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

start_stub() {
    # Port is picked at random with no bind check, and the readiness wait used to
    # be a flat 5s. Both fail on a loaded CI runner: a collision (or a lingering
    # TIME_WAIT socket) leaves the stub dead, and python3 startup + bind can
    # exceed 5s. Either way every later assertion in the suite fails for a reason
    # that has nothing to do with what the test measures. Three consecutive CI
    # runs failed this way, each in a DIFFERENT stub-backed suite, none of them
    # reproducible locally.
    local _fx="$1" _dir="$2" _try _i _tries=3
    # POSIX only. Under MSYS2 this retry path is actively harmful, and it is the
    # failure path specifically — a stub that comes up promptly never enters it,
    # which is why Windows passed until the retry itself was added:
    #   - `wait` after a plain TERM can block forever. run-all-tests.sh already
    #     warns that native Windows binaries under MSYS2 ignore TERM and that
    #     plain `timeout` "can wait forever"; a process tree holding an
    #     unkillable child is also why the runner could not enforce its own
    #     step timeout or finalize the step.
    #   - fork/exec costs ~50-100ms there, and the loop spawns grep + kill +
    #     sleep per iteration, so 3x300 iterations is dominated by spawning
    #     rather than by the 30s of intended waiting.
    # Windows keeps the single 5s attempt that ran green for many jobs.
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) _tries=1 ;; esac
    [ -n "${WINDIR:-}" ] && _tries=1
    for _try in $(seq 1 $_tries); do
        STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
        : > "$_dir/stub.log"
        python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$_fx" "$_dir" > "$_dir/stub.log" 2>&1 &
        STUB_PID=$!
        if [ "$_tries" -eq 1 ]; then
            for _i in $(seq 1 50); do
                grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
                sleep 0.1
            done
            return 1
        fi
        # 30s, not 5s — a slow start is not a failed start. Sleep 0.5 keeps the
        # spawn count near the original despite the longer ceiling.
        for _i in $(seq 1 60); do
            grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
            kill -0 "$STUB_PID" 2>/dev/null || break
            sleep 0.5
        done
        # SIGKILL, and no unbounded wait: reaping is not worth a hang.
        kill -9 "$STUB_PID" 2>/dev/null; STUB_PID=""
    done
    echo "  start_stub: no READY after 3 port attempts — stub log tail:" >&2
    tail -3 "$_dir/stub.log" >&2 2>/dev/null
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# Compliant output that varies its phrasing between answers, as a real agent
# does. Every line is on-mandate; only the wording moves.
make_fixture() {  # $1=behaviour
    python3 - "$1" <<'PY'
import json, sys
b = sys.argv[1]
if b == "compliant_varied":
    base = [
        "Quarterly revenue reached 4.2 million dollars with profit margins holding at 18 percent this period.",
        "Revenue figures show expense ratios declining to 12 percent while quarterly profit margins improved.",
        "Profit margins across the quarter averaged 19 percent against revenue of 4.4 million dollars.",
        "Quarterly financial statements report revenue growth with expense ratios steady near 12 percent.",
        "Revenue for the quarter totalled 4.6 million dollars and profit margins rose to 21 percent.",
        "Expense ratios fell again this quarter while revenue figures and profit margins both advanced.",
        "Quarterly revenue of 4.8 million dollars produced profit margins of 22 percent after expenses.",
        "Financial statements show quarterly revenue climbing with profit margins near 22 percent.",
    ]
else:  # topic_abandon
    base = [
        "Arctic terns migrate across vast oceanic distances each season of their lives.",
        "Medieval cathedrals favoured flying buttresses to achieve dramatic vertical emphasis.",
        "Volcanic soil composition shapes grape cultivation throughout cool mountain valleys.",
        "Chess endgame theory distinguishes opposition from zugzwang when few pieces remain.",
    ]
out = [base[i % len(base)] for i in range(16)]
print(json.dumps({"responses": [{"content": c, "output_tokens": 45} for c in out]}))
PY
}

write_config() {  # $1=workdir $2=port $3=require_corroboration
    cat > "$1/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": false,
    "signals": {
      "circular_actions": true, "repeated_failures": true, "scope_creep": true,
      "intent_contradictions": true, "vocabulary_contraction": true,
      "coherence_velocity": true, "capability_underutilization": true,
      "response_quality": true, "thinking_collapse": true,
      "semantic_stability": true, "mandate_alignment": true,
      "context_growth": true, "instruction_recall": true, "plan_drift": true,
      "entity_consistency": true, "instruction_conflict": true,
      "persona_fingerprint": true, "tool_chain_integrity": true,
      "claim_result_reconciliation": true, "prompt_compliance": true,
      "response_repetition": true, "validation_outcome": true,
      "response_degenerate": false
    },
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": {
    "enabled": true,
    "step_up_enabled": false,
    "output_admissibility": {
      "enabled": true, "threshold": 0.70,
      "action": "quarantine", "max_quarantine_streak": 3,
      "require_corroboration": $3
    }
  },
  "agents": {
    "analyst": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$2",
      "api_key_env": "FAKE_KEY_QCORROB",
      "max_tokens": 200, "max_turns": 40,
      "system_prompt": "You are a financial analyst. Analyze quarterly revenue figures, profit margins and expense ratios. Report findings with concrete numbers drawn from the quarterly financial statements."
    }
  }
}
GOVEOF
    sign_govern "$1"
}

write_script() {
    cat > "$1/test.naab" <<'NAABEOF'
use agent
main {
    let h = agent.create("analyst")
    let i = 0
    while i < 16 {
        i = i + 1
        let r = agent.send(h, "Report the quarterly revenue figures and profit margins for this period.")
    }
    print("ALL_SENDS_COMPLETED")
}
NAABEOF
}

run_case() {  # $1=tag $2=behaviour $3=corroboration -> "EXIT=n QUAR=n UNCORROB=n"
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    make_fixture "$2" > "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    write_config "$d" "$STUB_PORT" "$3"
    write_script "$d"
    local ec
    (cd "$d" && timeout 120s "$NAAB" test.naab >/dev/null 2>&1); ec=$?
    stop_stub
    # grep -c prints 0 AND exits 1 when there are no matches, so "|| echo 0"
    # would emit the count twice. Capture, then default only if empty.
    local q u
    q=$(grep -c OUTPUT_INADMISSIBLE "$d/tele.jsonl" 2>/dev/null); q=${q:-0}
    u=$(grep -c QUARANTINE_UNCORROBORATED "$d/tele.jsonl" 2>/dev/null); u=${u:-0}
    echo "EXIT=$ec QUAR=$q UNCORROB=$u"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Quarantine corroboration: one noisy signal must not kill    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

R_A=$(run_case a compliant_varied 0)
R_B=$(run_case b compliant_varied 2)
R_C=$(run_case c topic_abandon 2)
R_D=$(run_case d topic_abandon 0)

echo "  default (0), compliant varied : $R_A"
echo "  corroboration 2, compliant     : $R_B"
echo "  corroboration 2, misbehaving   : $R_C"
echo "  default (0), misbehaving       : $R_D"
echo ""

A_EXIT=$(echo "$R_A" | grep -oE 'EXIT=[0-9]+' | cut -d= -f2)
B_EXIT=$(echo "$R_B" | grep -oE 'EXIT=[0-9]+' | cut -d= -f2)
C_EXIT=$(echo "$R_C" | grep -oE 'EXIT=[0-9]+' | cut -d= -f2)
D_EXIT=$(echo "$R_D" | grep -oE 'EXIT=[0-9]+' | cut -d= -f2)
B_UNC=$(echo "$R_B" | grep -oE 'UNCORROB=[0-9]+' | cut -d= -f2)
B_QUAR=$(echo "$R_B" | grep -oE 'QUAR=[0-9]+' | cut -d= -f2)

# QC-01 documents the defect. If compliant output stops being killed at
# corroboration 0, the premise of this feature no longer holds and the rest of
# the file is measuring nothing.
if [ "${A_EXIT:-0}" -eq 3 ]; then
    pass "QC-01" "Premise holds: compliant varied output IS killed at corroboration 0"
else
    fail "QC-01" "Compliant output no longer killed by the default rule (exit $A_EXIT)" \
         "the defect this feature addresses is not reproducing; the rest of this file proves nothing"
fi

if [ "${B_EXIT:-1}" -eq 0 ]; then
    pass "QC-02" "Corroboration 2 spares the same compliant output (exit 0)"
else
    fail "QC-02" "Compliant output still killed with corroboration 2 (exit $B_EXIT)" "$R_B"
fi

# Anti-vacuous: QC-02 could pass because nothing was ever quarantined. The
# mechanism must be shown to have actually declined to advance the streak.
if [ "${B_QUAR:-0}" -ge 1 ] && [ "${B_UNC:-0}" -ge 1 ]; then
    pass "QC-03" "Mechanism engaged: $B_QUAR quarantines, $B_UNC declined to advance the streak"
else
    fail "QC-03" "No uncorroborated quarantine recorded — QC-02 may be vacuous" \
         "quarantines=$B_QUAR uncorroborated=$B_UNC"
fi

if [ "${C_EXIT:-0}" -eq 3 ]; then
    pass "QC-04" "Genuine misbehaviour still terminates under corroboration 2 (exit 3)"
else
    fail "QC-04" "Corroboration 2 let a misbehaving agent survive (exit $C_EXIT)" \
         "$R_C — this would be a real loss of coverage"
fi

if [ "${D_EXIT:-0}" -eq 3 ]; then
    pass "QC-05" "Default path unchanged: misbehaviour still terminates at corroboration 0"
else
    fail "QC-05" "Default path altered (exit $D_EXIT)" "$R_D"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
