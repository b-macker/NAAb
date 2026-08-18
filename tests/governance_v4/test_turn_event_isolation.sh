#!/usr/bin/env bash
# ============================================================
# test_turn_event_isolation.sh — a turn's events belong to one handle
#
# checkContextDrift() gathers its events with getEventsForTurn(turn), which
# selects on turn number ALONE. RuntimeEvent.turn is stamped from the global
# current_agent_turn_, set by setAgentContext() to the SENDING agent's
# per-handle turn — so two agents on the same turn number share one bucket, and
# whichever sends second sees the first's AGENT_RESPONSE as well as its own.
# Most CDD signal loops take the first matching event and break, so that agent
# was scored against a response it never produced.
#
# WHY THE CONTROL IS THE WHOLE TEST
#
# "Coherence looks plausible in a two-agent run" proves nothing — the number is
# plausible either way. The only thing that distinguishes a contaminated engine
# from a clean one is running the SAME agent against the SAME fixture twice,
# changing nothing but whether a sibling sends ahead of it in the turn. Clean:
# identical trajectories. Contaminated: they diverge.
#
# The sibling is deliberately the opposite shape — byte-identical responses
# against the subject's all-distinct ones — so contamination cannot be subtle.
# TEI-02 checks the direction as well as the difference, because the pre-fix
# behaviour made the subject look BETTER, and a test that only asserted
# "different" would pass on a fix that broke it the other way.
#
# The subject sends SECOND on purpose. The first sender is unaffected by
# construction, so a test that put the subject first would pass with or without
# the fix — verified: it does.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_turn_event_isolation.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/tei-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup() { stop_stub; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
export FAKE_KEY_TEI="fake-key-turn-isolation"

if [ ! -x "$NAAB" ]; then
    skip "TEI-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Turn event isolation: whose response am I being scored on?   |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

MANDATE="Build a Calculator class with add, subtract, multiply and divide methods, each logged in a history."

# $1 = tag ("solo" | "pair"). Emits the subject's coherence trajectory.
run_case() {
    local tag="$1"
    local w="$TEST_TMP/$tag"; mkdir -p "$w"
    python3 - "$w/fixture.json" << 'TEIFIX'
import json, sys
# SIBLING: byte-identical every turn -> fires circular + response_repetition.
same = [{"content": "Implemented the Calculator add method and logged the operation in history.",
         "output_tokens": 60, "input_tokens": 200} for _ in range(10)]
# SUBJECT: all distinct, unrelated topics -> fires the variation signals.
vary = [{"content": c, "output_tokens": 60, "input_tokens": 200} for c in (
    "Ocean tides recede beneath basalt cliffs while gulls circle the headland.",
    "Quarterly amortisation schedules require reconciliation against ledger entries.",
    "The kiln reached cone six before the glaze began to craze along the rim.",
    "Migratory terns span pole to pole across a single year of travel.",
    "Cartographers once drew sea serpents where soundings ran out entirely.",
    "Fermentation stalls below twelve degrees regardless of yeast pitching rate.",
    "Basalt columns fracture hexagonally as the lava sheet cools from above.",
    "Telemetry backhaul saturates when the sample interval drops under a second.",
    "The archive's oldest ledger predates the founding charter by two decades.",
    "Solar maxima correlate loosely with observed shortwave propagation gains.")]
json.dump({"routes": {"SIBLING": {"responses": same},
                      "SUBJECT": {"responses": vary}}}, open(sys.argv[1], "w"))
TEIFIX
    start_stub "$w/fixture.json" "$w" || return 1
    cat > "$w/govern.json" << TEIGOV
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "behavioral_sequences": { "enabled": true },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl", "decision_snapshots": true },
  "context_drift": { "enabled": true, "check_interval_turns": 1 },
  "agents": {
    "sibling": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_TEI",
      "max_tokens": 1024, "max_turns": 30,
      "system_prompt": "SIBLING $MANDATE"
    },
    "subject": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_TEI",
      "max_tokens": 1024, "max_turns": 30,
      "system_prompt": "SUBJECT $MANDATE"
    }
  }
}
TEIGOV
    # Sign only if this suite installed a signing key; set -u would abort the
    # subshell otherwise, and the isolated store here holds no trusted keys.
    if [ -n "${NAAB_SIGNING_KEY:-}" ]; then
        (cd "$w" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1)
    fi
    if [ "$tag" = "solo" ]; then
        cat > "$w/t.naab" << 'TEISOLO'
use agent
main {
    let s = agent.create("subject")
    let i = 0
    while i < 8 { let r = agent.send(s, "Continue"); i = i + 1 }
    print("DONE")
}
TEISOLO
    else
        # The sibling sends FIRST, so the subject is the second sender in the
        # turn — the only position the defect can reach.
        cat > "$w/t.naab" << 'TEIPAIR'
use agent
main {
    let b = agent.create("sibling")
    let s = agent.create("subject")
    let i = 0
    while i < 8 {
        let rb = agent.send(b, "Continue")
        let r  = agent.send(s, "Continue")
        i = i + 1
    }
    print("DONE")
}
TEIPAIR
    fi
    (cd "$w" && timeout 180s "$NAAB" t.naab) > "$w/out.txt" 2>&1
    stop_stub
    python3 - "$w/telemetry.jsonl" << 'TEIREAD'
import json, sys
out = []
try:
    for line in open(sys.argv[1]):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("event_type") != "SEMANTIC_TURN": continue
        if d.get("config_name") != "subject": continue
        sn = d.get("cdd_snapshot")
        if isinstance(sn, str):
            try: sn = json.loads(sn)
            except Exception: continue
        if isinstance(sn, dict) and "coherence_score" in sn:
            out.append("%.4f" % float(sn["coherence_score"]))
except OSError:
    pass
print(",".join(out))
TEIREAD
}

SOLO=$(run_case solo)
PAIR=$(run_case pair)

echo "  subject alone            : ${SOLO:-<none>}"
echo "  subject after a sibling  : ${PAIR:-<none>}"
echo ""

# --- TEI-01: both runs produced data (positive control) -------------------
# Without this, two empty strings compare equal and TEI-02 passes vacuously on
# a run that never happened.
N_SOLO=$(awk -F, '{print NF}' <<< "$SOLO"); [ -z "$SOLO" ] && N_SOLO=0
N_PAIR=$(awk -F, '{print NF}' <<< "$PAIR"); [ -z "$PAIR" ] && N_PAIR=0
if [ "$N_SOLO" -ge 5 ] && [ "$N_PAIR" -ge 5 ]; then
    pass "TEI-01" "both runs scored the subject ($N_SOLO solo, $N_PAIR paired turns)"
else
    fail "TEI-01" "a run produced too few scored turns" \
         "solo=$N_SOLO paired=$N_PAIR — equality below would be vacuous"
fi

# --- TEI-02: the sibling does not change the subject's scoring ------------
if [ "$N_SOLO" -ge 5 ] && [ "$N_PAIR" -ge 5 ]; then
    if [ "$SOLO" = "$PAIR" ]; then
        pass "TEI-02" "a sibling sending first does not alter the subject's coherence"
    else
        FIRST_SOLO=${SOLO%%,*}; FIRST_PAIR=${PAIR%%,*}
        DIR="differs"
        awk -v a="$FIRST_PAIR" -v b="$FIRST_SOLO" 'BEGIN{exit !(a>b)}' && \
            DIR="the subject scored BETTER for being graded on the sibling's output"
        fail "TEI-02" "the subject's coherence depends on a sibling's turn" \
             "$DIR — turn events are not filtered by agent_handle, so the second sender is scored against the first's response"
    fi
else
    skip "TEI-02" "insufficient data (see TEI-01)"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
