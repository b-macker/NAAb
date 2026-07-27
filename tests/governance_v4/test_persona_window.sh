#!/usr/bin/env bash
# ============================================================
# test_persona_window.sh — persona_fingerprint must not be retuned by a
# setting that belongs to a different signal.
#
# S17 had no window of its own. It read config_->thresholds.thinking_history_window,
# under a comment claiming that value was "the coherence history" — which it is
# not either. That window controls how many response keyword counts back the
# persona baseline mean and stddev, so changing the THINKING signal's window
# silently moved the persona verdict on identical responses. Both default to 20,
# so nothing visible depended on it until someone tuned thinking_history_window,
# at which point a second signal quietly changed behaviour with no mention of it
# anywhere.
#
# output_admissibility is on with threshold 0.0 purely as a carrier: the CDD
# decision snapshot rides on OUTPUT_ADMISSIBILITY_EVAL, and at threshold 0.0
# nothing is ever ruled inadmissible, so the gate observes without acting.
#
# Adaptive baselining is ON because S17 cannot fire without it: persona_baseline_mean
# is gated on state.baseline_complete, which is only ever set inside the
# `if (config_->adaptive_baseline_enabled)` block. With the engine default of
# adaptive_baseline_enabled=false the signal is silently inert — worth knowing
# separately from the window question this test is about.
#
# Two runs over a byte-identical response stream. The only difference is
# thinking_history_window, and thinking_collapse is DISABLED in both, so that
# key has no legitimate reason to affect anything measured here.
#
#   PW-01  the persona baseline is identical across two thinking-window settings
#   PW-02  persona_history_window still governs that baseline — moving IT does
#          change it, so the signal is decoupled rather than pinned
#
# PW-01 fails against the pre-fix binary. PW-02 guards the fix from being a
# stub: a window that no longer responds to its own key would also pass PW-01.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${PERSONAWIN_TMP:-${_SYSTMP}/personawin-$$}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

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

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not available — skipping"; exit 0
fi

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_PERSONAWIN="fake-key-personawin"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

start_stub() {  # $1=fixture $2=statedir
    local attempt
    for attempt in 1 2 3 4 5; do
        STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
        : > "$2/stub.log"
        python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" >> "$2/stub.log" 2>&1 &
        STUB_PID=$!
        for _ in $(seq 1 50); do
            grep -q READY "$2/stub.log" 2>/dev/null && return 0
            kill -0 "$STUB_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# Response length swings from turn one, so which samples the window retains
# decides the baseline mean and stddev the rest of the run is judged against.
make_fixture() {  # $1=path
    python3 - "$1" <<'PY'
import json, sys
words = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu "
         "nu xi omicron pi rho sigma tau upsilon phi chi psi omega").split()
resps = []
# Lengths swing hard from the first turn. The persona baseline is computed once,
# over whatever the window is holding when adaptive baselining completes — so
# which samples the window keeps has to change the mean and stddev, or the
# setting is unobservable and the test proves nothing.
for i in range(34):
    n = 4 + (i * 17) % 70
    body = " ".join(words[(i + k) % len(words)] + str(k) for k in range(n))
    resps.append({"content": "Stage %d report: %s." % (i, body),
                  "input_tokens": 120, "output_tokens": 40 + n,
                  "thinking_tokens": 200})
json.dump({"responses": resps}, open(sys.argv[1], "w"))
PY
}

# $1=workdir $2=port $3=thinking_history_window $4=persona_history_window
write_config() {
    cat > "$1/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl", "decision_snapshots": true },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": true, "adaptive_baseline_window": 14,
    "adaptive_baseline_sensitivity": 0.0,
    "thresholds": {
      "thinking_history_window": $3,
      "persona_history_window": $4,
      "persona_deviation_factor": 2.0
    },
    "signals": {
      "circular_actions": false, "repeated_failures": false, "scope_creep": false,
      "intent_contradictions": false, "vocabulary_contraction": false,
      "coherence_velocity": false, "capability_underutilization": false,
      "response_quality": false, "thinking_collapse": false,
      "semantic_stability": false, "mandate_alignment": false,
      "context_growth": false, "instruction_recall": false, "plan_drift": false,
      "entity_consistency": false, "instruction_conflict": false,
      "persona_fingerprint": true, "tool_chain_integrity": false,
      "claim_result_reconciliation": false, "prompt_compliance": false,
      "response_repetition": false, "validation_outcome": false,
      "response_degenerate": false
    },
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": {
    "enabled": true, "step_up_enabled": false,
    "output_admissibility": { "enabled": true, "threshold": 0.0, "action": "quarantine" }
  },
  "agents": {
    "worker": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$2",
      "api_key_env": "FAKE_KEY_PERSONAWIN",
      "max_tokens": 800, "max_turns": 60,
      "system_prompt": "You report on pipeline stages."
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
    let h = agent.create("worker")
    let i = 0
    while i < 34 {
        i = i + 1
        agent.send(h, "Report on stage " + string(i) + ".")
    }
    print("DONE")
}
NAABEOF
}

# -> persona baseline mean established under this window pair
measure() {  # $1=tag $2=thinking_window $3=persona_window
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    make_fixture "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "ERR"; return; }
    write_config "$d" "$STUB_PORT" "$2" "$3"
    write_script "$d"
    (cd "$d" && timeout 120s "$NAAB" test.naab >/dev/null 2>&1)
    stop_stub
    python3 - "$d/tele.jsonl" <<'SNAP'
# Read the persona baseline DIRECTLY out of the decision snapshot instead of
# inferring it from firing counts. The window's only job is deciding which
# samples feed that one-time baseline computation, so the baseline IS the
# measurement. Firing counts are a lossy proxy that also moves with the
# deviation factor, the adaptive penalty and the response mix — chasing them
# meant retuning the fixture until the signal fired, which is fitting a test to
# its own conclusion.
import json, sys, os
p = sys.argv[1]
mean = None
if os.path.exists(p):
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        snap = e.get("cdd_snapshot")
        if isinstance(snap, str):
            try: snap = json.loads(snap)
            except Exception: snap = None
        if isinstance(snap, dict) and snap.get("persona_baseline_mean") is not None:
            m = float(snap["persona_baseline_mean"])
            if m >= 0.0: mean = m
print("NONE" if mean is None else "%.4f" % mean)
SNAP
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  persona_fingerprint: whose window is it?                     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# thinking_collapse is OFF in both, so thinking_history_window has no legitimate
# route to the result. Only the borrowed read could carry it.
A=$(measure think20 20 20)
B=$(measure think4  4  20)
# And S17 must still answer to its own key, or the fix is a pin rather than a
# decoupling.
C=$(measure persona3 20 3)

printf "  %-34s %s\n" "thinking_window=20 persona_window=20" "baseline_mean=$A"
printf "  %-34s %s\n" "thinking_window=4  persona_window=20" "baseline_mean=$B"
printf "  %-34s %s\n" "thinking_window=20 persona_window=3"  "baseline_mean=$C"
echo ""

if [ "${A:-x}" = "ERR" ] || [ "${B:-x}" = "ERR" ]; then
    fail "PW-01" "a scenario failed to run" "stub or binary error"
elif [ "${A:-x}" = "NONE" ]; then
    fail "PW-01" "no persona baseline established" "A=$A — nothing was measured"
elif [ "$A" = "$B" ]; then
    pass "PW-01" "the thinking window no longer moves the persona baseline ($A = $B)"
else
    fail "PW-01" "changing thinking_history_window moved the persona baseline ($A vs $B)" \
         "S17 is reading a setting that belongs to a different signal"
fi

if [ "${C:-x}" = "ERR" ] || [ "${C:-x}" = "NONE" ]; then
    fail "PW-02" "no persona baseline under the narrow window" "C=$C"
elif [ "$C" != "$A" ]; then
    pass "PW-02" "persona_history_window governs the baseline ($A at 20, $C at 3)"
else
    fail "PW-02" "persona_history_window has no effect ($C = $A)" \
         "the window was pinned rather than decoupled — S17 answers to nothing"
fi

echo ""
echo "────────────────────────────────────────"
echo -e "Passed:  ${GREEN}${PASS_COUNT}${NC}"
echo -e "Failed:  ${RED}${FAIL_COUNT}${NC}"
echo -e "Skipped: ${YELLOW}${SKIP_COUNT}${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}${FAILURES}"
    exit 1
fi
exit 0
