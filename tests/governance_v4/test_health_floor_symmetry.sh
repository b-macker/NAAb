#!/usr/bin/env bash
# ============================================================
# test_health_floor_symmetry.sh — the health check's missing direction
#
# checkGovernanceHealth() had three checks and all of them asked the same
# question: is DETECTION being bypassed?
#
#   1. BSD received 0 events            -> instrumentation disconnected
#   2. CDD analyzed 0 turns             -> instrumentation disconnected
#   3. coherence perfect after 10 turns -> agent may be evading detection
#
# Nothing asked whether governance is scoring a COMPLIANT agent into the floor.
# Every keyed run of living-script_v3 floored coherence by turn 5-8 with no agent
# misbehaving, and the engine said nothing — the check is built to catch an agent
# evading detection and is blind to governance evading correctness.
#
# Check 3b is the mirror. It is a WARNING only: checkGovernanceHealth never calls
# enforce(), so this reaches stderr and GOVERNANCE_HEALTH_WARNING telemetry and
# can never block a run. That is deliberate — the campaign's evidence names
# threshold suspects but no values, so this reports a suspicion and moves nothing.
#
# HF-03 and HF-04 are what make HF-01 mean anything:
#   HF-03  a healthy agent must stay silent — without it, a warning hardcoded to
#          always fire passes HF-01.
#   HF-04  setting coherence_floor_warning: 0 must silence it — the removal test,
#          proving the new gate is load-bearing rather than incidental.
#   HF-02  one-shot: the condition persists for the rest of the run and the check
#          runs EVERY turn, so an unlatched warning prints on every remaining turn.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_health_floor_symmetry.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/hfloor-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup() { stop_stub; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

if [ ! -x "$NAAB" ]; then
    skip "HF-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Governance health: the false-positive direction              |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# Drifting responses: no vocabulary carried between turns and nothing echoing the
# instruction, so semantic_stability and instruction_recall fire every turn and
# coherence walks to the floor.
gen_drift_fixture() {
    python3 - "$1" <<'PY'
import json, sys
topics = [
    "quantum lattice vibrations in cryogenic superconductors",
    "medieval falconry husbandry and glove stitching",
    "tidal turbine corrosion in brackish estuaries",
    "baroque counterpoint voice leading rules",
    "mycorrhizal nutrient exchange under drought",
    "orbital debris conjunction screening windows",
    "sourdough hydration ratios at altitude",
    "pangolin scale keratin microstructure",
    "harbour dredging sediment plume modelling",
    "typeface hinting for subpixel rendering",
    "volcanic tephra layer geochronology",
    "railway catenary tension compensation",
    "coral spawning lunar synchronisation",
    "loom shedding mechanisms in jacquard weaving",
]
json.dump({"responses": [
    {"content": t, "output_tokens": 40} for t in topics
]}, open(sys.argv[1], "w"))
PY
}

# Healthy responses: stable vocabulary that keeps echoing the instruction.
gen_healthy_fixture() {
    python3 - "$1" <<'PY'
import json, sys
base = ("inventory service report: the inventory service processes inventory "
        "records and returns inventory totals for the inventory report")
json.dump({"responses": [
    {"content": base + " (section %d)" % i, "output_tokens": 40} for i in range(14)
]}, open(sys.argv[1], "w"))
PY
}

# $1 workdir, $2 fixture-kind, $3 extra governance_health JSON body
run_case() {
    local w="$1" kind="$2" gh="$3"
    mkdir -p "$w"
    if [ "$kind" = "drift" ]; then gen_drift_fixture "$w/fixture.json"
    else gen_healthy_fixture "$w/fixture.json"; fi
    start_stub "$w/fixture.json" "$w" || return 1
    cat > "$w/govern.json" <<GOVEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "check_interval_turns": 1 },
  "governance_health": { $gh },
  "agents": { "worker": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_HFLOOR",
      "max_tokens": 200, "max_turns": 40,
      "system_prompt": "Report on the inventory service and its inventory records." } } }
GOVEOF
    cat > "$w/t.naab" <<'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < 13 {
        let r = agent.send(h, "continue the inventory service report")
        i = i + 1
    }
    print("DONE")
}
NAABEOF
    (cd "$w" && FAKE_HFLOOR=x "$NAAB" t.naab > out.txt 2> err.txt)
    stop_stub
}

# --- HF-01: floored coherence is reported ---------------------------------
run_case "$TEST_TMP/drift" "drift" '"enabled": true, "check_after_turns": 10'
DRIFT_ERR="$TEST_TMP/drift/err.txt"
N_FLOOR=$(grep -c "Coherence floored" "$DRIFT_ERR" 2>/dev/null || echo 0)

if [ "$N_FLOOR" -ge 1 ]; then
    pass "HF-01" "floored coherence warns ($(grep -m1 -o "Coherence floored at [0-9.]*" "$DRIFT_ERR"))"
else
    COH=$(grep -o '"coherence":"[0-9.]*"' "$TEST_TMP/drift/telemetry.jsonl" 2>/dev/null | tail -1)
    fail "HF-01" "coherence floored but no warning emitted" "last coherence: ${COH:-none}"
fi

# --- HF-02: exactly once, not once per turn -------------------------------
if [ "$N_FLOOR" -eq 1 ]; then
    pass "HF-02" "warning is one-shot (1 occurrence across 13 turns)"
elif [ "$N_FLOOR" -gt 1 ]; then
    fail "HF-02" "warning repeated $N_FLOOR times — latch not working" \
         "it would print every remaining turn of a real run"
else
    fail "HF-02" "no warning to count (HF-01 already failed)"
fi

# --- HF-03: healthy agent stays silent (positive control) -----------------
run_case "$TEST_TMP/healthy" "healthy" '"enabled": true, "check_after_turns": 10'
if grep -q "Coherence floored" "$TEST_TMP/healthy/err.txt" 2>/dev/null; then
    fail "HF-03" "healthy agent wrongly reported as floored" \
         "the warning fires regardless of coherence; HF-01 proves nothing"
else
    pass "HF-03" "healthy agent produces no floor warning (control)"
fi

# --- HF-04: removal test — the gate must be load-bearing -------------------
run_case "$TEST_TMP/off" "drift" '"enabled": true, "check_after_turns": 10, "coherence_floor_warning": 0'
if grep -q "Coherence floored" "$TEST_TMP/off/err.txt" 2>/dev/null; then
    fail "HF-04" "warning still fires with coherence_floor_warning: 0" \
         "the config gate does not disable the check"
else
    pass "HF-04" "coherence_floor_warning: 0 disables it (removal test)"
fi

# --- HF-05: it is a warning, never an enforcement -------------------------
# checkGovernanceHealth() must never block: the drift run has to complete.
if grep -q "DONE" "$TEST_TMP/drift/out.txt" 2>/dev/null; then
    pass "HF-05" "run completes — warning never blocks"
else
    fail "HF-05" "drift run did not complete" \
         "the health warning must not reach an enforcement path"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
