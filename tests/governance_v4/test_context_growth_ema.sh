#!/usr/bin/env bash
# ============================================================
# test_context_growth_ema.sh — does context_growth (S12) still fire forever?
#
# open-investigations row B5 recorded S12 firing on 29 of 29 turns from turn 8
# on a long single-handle conversation, and reasoned that a signal which cannot
# stop firing is a permanent coherence tax rather than a drift detector. The
# mechanism it named is real: `input_tokens_baseline_mean` is set once and then
# updated by a rolling EMA ONLY under
#
#     else if (config_->adaptive_baseline_enabled && state.baseline_complete)
#
# in behavioral_sequence.cpp. With that flag off the baseline freezes at the
# opening window and any conversation whose history grows past 3x its own
# opening turns fires from then on.
#
# The row's premise has since expired: `adaptive_baseline_enabled` defaulted
# false when it was measured and defaults TRUE now, so the EMA the row says is
# unreachable is live on shipped defaults. The row was never re-measured. This
# file is that measurement.
#
# PROVENANCE. The input-token series is AUTHORED, not observed: the stub reports
# whatever `input_tokens` the fixture names, so the growth curve is stipulated
# rather than produced by real prompts. Linear growth is the right stipulation —
# a full-history conversation appends roughly a constant per turn, so input
# tokens grow linearly in the turn number — but it is a model of the workload,
# not a sample of one. What is OBSERVED is the engine's response to that curve.
#
#   CG-01  POSITIVE CONTROL. Adaptive OFF (the pre-flip default) reproduces the
#          row: linear growth fires S12 on most turns and never stops. Without
#          this, CG-02's silence cannot be told apart from a fixture whose
#          growth never reaches the trip point at all.
#   CG-02  Adaptive ON (shipped default) over the SAME series. This is the
#          question the row leaves open.
#   CG-03  DETECTION CONTROL. Adaptive ON, flat context that then jumps ~6x.
#          The EMA must not have bought CG-02's quiet by blinding the signal to
#          the bloat it exists to catch. Without this, "S12 stopped firing"
#          reads as a fix when it could be a silencing.
#
# S12 is the only CDD signal enabled, so the coherence figures are its alone.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_context_growth_ema.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${CTXGROW_TMP:-${_SYSTMP}/ctxgrow-$$}"

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
export FAKE_KEY_CTXGROW="fake-key-ctxgrow"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

source "$SCRIPT_DIR/../helpers/stub_launch.sh"

TURNS=40

# $1=path $2=shape (linear|step)
# linear: 200 + 150*i  -- a full-history conversation appending a constant per turn.
# step:   flat 400 for 25 turns, then 2400 -- a 6x bloat arriving at once, which is
#         the failure S12 is named for. Responses are byte-identical across shapes
#         so nothing but the token series can move the result.
make_fixture() {
    python3 - "$1" "$2" "$TURNS" <<'PY'
import json, sys
path, shape, turns = sys.argv[1], sys.argv[2], int(sys.argv[3])
resps = []
for i in range(turns):
    if shape == "linear":
        tok = 200 + 150 * i
    else:
        tok = 400 if i < 25 else 2400
    resps.append({"content": "Stage %d handled; proceeding to the next item." % i,
                  "input_tokens": tok, "output_tokens": 40})
json.dump({"responses": resps}, open(path, "w"))
PY
}

# $1=workdir $2=port $3=adaptive(true|false)
write_config() {
    cat > "$1/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": $3, "adaptive_baseline_window": 5,
    "thresholds": { "context_growth_factor": 3.0 },
    "signals": {
      "circular_actions": false, "repeated_failures": false, "scope_creep": false,
      "intent_contradictions": false, "vocabulary_contraction": false,
      "coherence_velocity": false, "capability_underutilization": false,
      "response_quality": false, "thinking_collapse": false,
      "semantic_stability": false, "mandate_alignment": false,
      "context_growth": true, "instruction_recall": false, "plan_drift": false,
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
      "api_key_env": "FAKE_KEY_CTXGROW",
      "max_tokens": 800, "max_turns": 80,
      "_note_max_total_tokens": "raised off the 100000 default: the linear series crosses it at turn 36 and truncates the run",
      "max_total_tokens": 5000000,
      "system_prompt": "You work through a queue of pipeline stages."
    }
  }
}
GOVEOF
    sign_govern "$1"
}

write_script() {
    cat > "$1/test.naab" <<NAABEOF
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < $TURNS {
        i = i + 1
        agent.send(h, "Handle stage " + string(i) + ".")
    }
    print("DONE")
}
NAABEOF
}

# -> "<fired>/<analyzed> <first_fire_turn> <final_coherence>"
measure() {  # $1=tag $2=shape $3=adaptive
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    make_fixture "$d/fixture.json" "$2"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "ERR"; return; }
    write_config "$d" "$STUB_PORT" "$3"
    write_script "$d"
    (cd "$d" && timeout 180s "$NAAB" test.naab >/dev/null 2>&1)
    stop_stub
    python3 - "$d/tele.jsonl" <<'SNAP'
import json, sys, os
# Only analyzed turns carry a live verdict; an interval-skipped CDD_TURN
# re-shows the previous check's signals_detail and would be counted twice.
p = sys.argv[1]
fired = analyzed = 0; first = -1; coh = None
if os.path.exists(p):
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        if e.get("event_type") != "CDD_TURN": continue
        if str(e.get("analyzed", "true")).lower() != "true": continue
        analyzed += 1
        try: coh = float(e.get("coherence", coh if coh is not None else 1.0))
        except Exception: pass
        if "context_growth" in str(e.get("signals_detail", "")):
            fired += 1
            if first < 0:
                try: first = int(e.get("turn", -1))
                except Exception: first = -1
print("%d/%d %s %s" % (fired, analyzed, first,
                       "?" if coh is None else "%.4f" % coh))
SNAP
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  context_growth: does the EMA stop the permanent firing?      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

A=$(measure off_linear linear false)
B=$(measure on_linear  linear true)
C=$(measure on_step    step   true)

printf "  %-40s fired=%s first=%s coherence=%s\n" "adaptive OFF, linear growth"  $A
printf "  %-40s fired=%s first=%s coherence=%s\n" "adaptive ON,  linear growth"  $B
printf "  %-40s fired=%s first=%s coherence=%s\n" "adaptive ON,  flat then 6x"   $C
echo ""

a_fired=$(echo "$A" | cut -d' ' -f1 | cut -d/ -f1)
a_seen=$(echo "$A" | cut -d' ' -f1 | cut -d/ -f2)
b_fired=$(echo "$B" | cut -d' ' -f1 | cut -d/ -f1)
b_seen=$(echo "$B" | cut -d' ' -f1 | cut -d/ -f2)
c_fired=$(echo "$C" | cut -d' ' -f1 | cut -d/ -f1)

if [ "$A" = "ERR" ] || [ "$B" = "ERR" ] || [ "$C" = "ERR" ]; then
    fail "CG-01" "a scenario failed to run" "stub or binary error"
elif [ "${a_seen:-0}" -ne "$TURNS" ] || [ "${b_seen:-0}" -ne "$TURNS" ]; then
    fail "CG-01" "an arm did not run its full length" \
        "A=$A B=$B against TURNS=$TURNS — a truncated arm cannot support \"never stops firing\" (the per-agent max_total_tokens default of 100000 truncates this series at turn 36)"
elif [ "${a_fired:-0}" -ge 15 ]; then
    pass "CG-01" "positive control: adaptive OFF fires $a_fired of $a_seen turns"
else
    fail "CG-01" "positive control did not reproduce the permanent firing" \
        "adaptive OFF fired only $a_fired of $a_seen — the fixture never reaches the trip point, so CG-02 proves nothing"
fi

if [ "${a_seen:-0}" -eq "$TURNS" ] && [ "${a_fired:-0}" -ge 15 ]; then
    if [ "${b_fired:-0}" -lt "${a_fired:-0}" ]; then
        pass "CG-02" "adaptive ON fires $b_fired of $b_seen on the same series (was $a_fired)"
    else
        fail "CG-02" "the EMA did not reduce the firing rate" \
            "OFF=$a_fired ON=$b_fired — B5's finding survives the default flip"
    fi
else
    skip "CG-02" "positive control failed — nothing to compare against"
fi

if [ "${c_fired:-0}" -ge 1 ]; then
    pass "CG-03" "detection control: a 6x jump still fires with adaptive ON ($c_fired turns)"
else
    fail "CG-03" "adaptive ON never fired on a 6x context jump" \
        "the EMA bought CG-02's quiet by blinding S12 to the bloat it exists to catch"
fi

echo ""
if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}All $PASS_COUNT checks passed${NC} (${SKIP_COUNT} skipped)"
else
    echo -e "${RED}$FAIL_COUNT failed${NC}, $PASS_COUNT passed"
    echo -e "$FAILURES"
    exit 1
fi
