#!/usr/bin/env bash
# ============================================================
# test_thinking_reported.sh — can thinking_collapse tell a collapsed agent
# from an unreported measurement, and does it matter?
#
# Gemini omits thoughtsTokenCount from usageMetadata on some responses. The
# parser leaves thinking_tokens at its initialiser 0, so "the model did no
# thinking" and "the API did not mention thinking" arrive as the same value.
# Live run 21: the developer reported 451, 386, 342, 429 and then zero for
# sixteen straight turns while its output grew eightfold. Run 22 showed a lone
# 570 in the middle of the zeros, which no account of agent behaviour explains.
# The agent paid 0.64 of coherence and three OA quarantines for a missing field.
#
# Three scenarios, identical in every other respect:
#
#   collapse    thinking reported, and it genuinely falls to near zero
#   unreported  thinking reported at first, then the field disappears
#   steady      thinking reported at a constant healthy level throughout
#
# TR-01 is the anti-regression assertion: skipping unmeasured turns must not
# cost the signal its actual job. A real collapse still has to fire.
#
# LIVE STATUS (run 23, commit dbf55a4): the reported-check is confirmed —
# thinking_collapse went from the largest drain in the run (0.64 over 11 turns,
# three OA quarantines) to ZERO firings on any agent, and the developer's
# coherence floor rose from 0.5175 to 0.83. The ANNOUNCEMENT is still unproven
# live: the developer's unreported streaks ran to 7, below the
# thinking_history_window/2 threshold of 10, so THINKING_UNREPORTED correctly
# declined to fire and has never fired outside this test. Run 21's sixteen-zero
# tail would have crossed it. The live pattern is intermittent reporting, not
# the permanent step change the fix was first described as chasing.
#
# TR-04 is the part that grounds the design question rather than assuming it.
# The proposal was to give unreported thinking its own visibility, and the
# argument for it is only worth anything if silence is genuinely ambiguous. So
# TR-04 measures that directly: it compares the CDD-visible evidence of the
# `unreported` run against the `steady` run. If those two are indistinguishable
# on signals alone, then a run where the instrumentation quietly died looks
# exactly like a run where the agent was fine, and the announcement is carrying
# real information. If they differ anyway, the announcement is redundant and
# should be dropped rather than kept on faith.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_thinking_reported.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${THINKREP_TMP:-${_SYSTMP}/thinkrep-$$}"

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
export FAKE_KEY_THINKREP="fake-key-thinkrep"

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

# Response CONTENT is identical across all three scenarios and varies per turn,
# so no other CDD signal has cause to fire and any divergence is attributable to
# the thinking field alone.
make_fixture() {  # $1=mode
    python3 - "$1" <<'PY'
import json, sys
mode = sys.argv[1]
N = 24
topics = [
    "ingestion adapters normalise inbound records before the schema gate",
    "the schema gate rejects malformed payloads and emits a rejection receipt",
    "checkpointing writes offsets so a restart resumes without replay",
    "backpressure throttles producers when the sink queue depth climbs",
    "the dead letter queue retains poison messages for manual triage",
    "partition assignment rebalances when a consumer leaves the group",
]
out = []
for i in range(N):
    if mode == "collapse":
        think = 400 if i < 6 else 3          # reported, genuinely collapsed
    elif mode == "unreported":
        think = 400 if i < 6 else None       # reported, then the field vanishes
    else:
        think = 400                          # steady throughout
    out.append({
        "content": "Batch %d: %s. Throughput nominal, sink acknowledged every "
                   "committed offset in this window." % (i, topics[i % len(topics)]),
        "input_tokens": 140,
        "output_tokens": 60,
        "thinking_tokens": think,
    })
json.dump({"responses": out}, sys.stdout)
PY
}

write_config() {  # $1=workdir $2=port
    cat > "$1/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": false,
    "thresholds": { "thinking_history_window": 8, "thinking_collapse_ratio": 0.5 },
    "signals": {
      "circular_actions": false, "repeated_failures": false, "scope_creep": false,
      "intent_contradictions": false, "vocabulary_contraction": false,
      "coherence_velocity": false, "capability_underutilization": false,
      "response_quality": false, "thinking_collapse": true,
      "semantic_stability": false, "mandate_alignment": false,
      "context_growth": false, "instruction_recall": false, "plan_drift": false,
      "entity_consistency": false, "instruction_conflict": false,
      "persona_fingerprint": false, "tool_chain_integrity": false,
      "claim_result_reconciliation": false, "prompt_compliance": false,
      "response_repetition": false, "validation_outcome": false,
      "response_degenerate": false
    },
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": { "enabled": true, "step_up_enabled": false },
  "agents": {
    "worker": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$2",
      "api_key_env": "FAKE_KEY_THINKREP",
      "max_tokens": 400, "max_turns": 60,
      "thinking_budget": 1024,
      "system_prompt": "You maintain a streaming data pipeline. Report on its stages, throughput and failure handling."
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
    while i < 24 {
        i = i + 1
        agent.send(h, "Report on pipeline batch " + string(i) + ".")
    }
    print("FINAL_COHERENCE=" + string(agent.coherence(h)))
}
NAABEOF
}

# -> "<collapse_firings> <final_coherence> <unreported_events>"
measure() {  # $1=mode
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    make_fixture "$1" > "$d/fixture.json"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "ERR ERR ERR"; return; }
    write_config "$d" "$STUB_PORT"
    write_script "$d"
    OUT=$(cd "$d" && timeout 120s "$NAAB" test.naab 2>/dev/null)
    stop_stub
    local coh
    coh=$(echo "$OUT" | grep -oP 'FINAL_COHERENCE=\K[0-9.]+' | tail -1)
    python3 - "$d/tele.jsonl" "${coh:-1.0}" <<'PY'
import json, sys, os, re
p, coh = sys.argv[1], sys.argv[2]
fires = 0; unrep = 0
if os.path.exists(p):
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        t = e.get("event_type")
        if t == "CDD_TURN" and str(e.get("analyzed")).lower() == "true":
            if re.search(r'thinking_collapse=', str(e.get("penalties_detail") or "")):
                fires += 1
        elif t == "THINKING_UNREPORTED":
            unrep += 1
print("%d %s %d" % (fires, coh, unrep))
PY
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  thinking_collapse: measurement or missing field?             |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

read -r C_FIRE C_COH C_UNREP <<< "$(measure collapse)"
read -r U_FIRE U_COH U_UNREP <<< "$(measure unreported)"
read -r S_FIRE S_COH S_UNREP <<< "$(measure steady)"

printf "  %-12s %-14s %-14s %s\n" "SCENARIO" "S9 firings" "final coherence" "THINKING_UNREPORTED"
echo "  ---------------------------------------------------------------------"
printf "  %-12s %-14s %-14s %s\n" "collapse"   "$C_FIRE" "$C_COH" "$C_UNREP"
printf "  %-12s %-14s %-14s %s\n" "unreported" "$U_FIRE" "$U_COH" "$U_UNREP"
printf "  %-12s %-14s %-14s %s\n" "steady"     "$S_FIRE" "$S_COH" "$S_UNREP"
echo ""

# TR-01 — the anti-regression. Skipping unmeasured turns must not disarm the
# signal against the failure it exists to detect.
if [ "${C_FIRE:-0}" -gt 0 ]; then
    pass "TR-01" "a genuine reported collapse still fires ($C_FIRE turns, coherence $C_COH)"
else
    fail "TR-01" "reported collapse no longer detected" \
         "the reported-check disarmed the signal instead of narrowing it"
fi

# TR-02 — the defect. An absent field must not be scored as behaviour.
if [ "${U_FIRE:-0}" -eq 0 ]; then
    pass "TR-02" "an unreported thinking count fires nothing ($U_FIRE turns)"
else
    fail "TR-02" "missing field still scored as collapse ($U_FIRE turns, coherence $U_COH)" \
         "the agent is paying coherence for a field the provider omitted"
fi

# TR-03 — the control. Steady healthy thinking must be silent either way.
if [ "${S_FIRE:-0}" -eq 0 ]; then
    pass "TR-03" "steady thinking fires nothing"
else
    fail "TR-03" "steady thinking fired $S_FIRE times" "false positive on the control"
fi

# TR-04 — is the announcement carrying information, or is it decoration?
# Compare the unreported run against the steady run on CDD evidence alone. If
# they are identical there, the announcement is the only thing separating a dead
# instrument from a healthy agent, and it earns its place. If they already
# differ, it does not.
if [ "${U_FIRE:-0}" -eq "${S_FIRE:-0}" ] && [ "${U_COH:-x}" = "${S_COH:-y}" ]; then
    if [ "${U_UNREP:-0}" -gt 0 ] && [ "${S_UNREP:-0}" -eq 0 ]; then
        pass "TR-04" "dead instrument and healthy agent are identical on CDD evidence — only THINKING_UNREPORTED separates them"
    else
        fail "TR-04" "dead instrument is indistinguishable from a healthy agent" \
             "unreported=$U_UNREP steady=$S_UNREP — nothing marks the difference"
    fi
else
    skip "TR-04" "the two runs already differ on CDD evidence (S9 $U_FIRE vs $S_FIRE, coherence $U_COH vs $S_COH) — announcement may be redundant"
fi

# TR-05 — the announcement must not fire on a healthy run.
if [ "${S_UNREP:-0}" -eq 0 ] && [ "${C_UNREP:-0}" -eq 0 ]; then
    pass "TR-05" "no spurious THINKING_UNREPORTED on reported runs"
else
    fail "TR-05" "THINKING_UNREPORTED fired on a run where thinking was reported" \
         "steady=$S_UNREP collapse=$C_UNREP"
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
