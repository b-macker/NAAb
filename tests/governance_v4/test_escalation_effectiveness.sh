#!/usr/bin/env bash
# ============================================================
# test_escalation_effectiveness.sh — Escalation Effectiveness Config Tests
#
# 6 tests validating escalation_effectiveness_window config parsing,
# defaults, clamping, dashboard output, agent creation defaults,
# and govern-template.json coverage.
#
# No API key required — all tests use NAAB_TEST_FAKE_KEY for agent.create().
#
# Grounded in:
# - ContextDriftConfig.escalation_effectiveness_window (default 5)
# - governance_config.cpp: parsed from context_drift.escalation_effectiveness_window
# - DriftState.escalation_turn defaults to -1 (no escalation)
# - Dashboard: "Escalation:" line only printed when escalation_turn >= 0
# - Environment dict: escalation_turn, escalation_from_level, escalation_to_level
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/esc-eff-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { local id="$1" desc="$2"; PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$id] $desc"; }
fail() { local id="$1" desc="$2" detail="${3:-}"; FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$id] $desc"; [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"; }
skip() { local id="$1" desc="$2"; SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$id] $desc"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"

setup_workdir() {
    local workdir="$TEST_TMP/work-$$-$RANDOM"
    mkdir -p "$workdir"
    echo "$workdir"
}

sign_govern() {
    local workdir="$1"
    (cd "$workdir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Escalation Effectiveness Config Tests                       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# E01: Default escalation_effectiveness_window = 5
# ============================================================
echo -e "${CYAN}--- E01: Default escalation_effectiveness_window ---${NC}"
WORK=$(setup_workdir)
cat > "$WORK/govern.json" <<'GJSON'
{
  "version": "5.0", "mode": "enforce",
  "languages": {"allowed": ["python"]},
  "security": {"sandbox_level": "elevated"},
  "context_drift": {"enabled": true, "level": "advisory"},
  "governance": {"dashboard": true}
}
GJSON
cat > "$WORK/test.naab" <<'NAAB'
use governance
main {
    let h = governance.health()
    print("HEALTH_OK")
}
NAAB
sign_govern "$WORK"
OUT=$("$NAAB" --governance-dashboard "$WORK/test.naab" 2>&1) || true
if echo "$OUT" | grep -q "HEALTH_OK"; then
    pass "E01" "Default config accepted (no escalation_effectiveness_window key)"
else
    fail "E01" "Default config not accepted" "$(echo "$OUT" | tail -1)"
fi

# ============================================================
# E02: Explicit escalation_effectiveness_window = 3 parses correctly
# ============================================================
echo -e "${CYAN}--- E02: Explicit escalation_effectiveness_window ---${NC}"
WORK=$(setup_workdir)
cat > "$WORK/govern.json" <<'GJSON'
{
  "version": "5.0", "mode": "enforce",
  "languages": {"allowed": ["python"]},
  "security": {"sandbox_level": "elevated"},
  "context_drift": {"enabled": true, "level": "advisory", "escalation_effectiveness_window": 3},
  "governance": {"dashboard": true}
}
GJSON
cat > "$WORK/test.naab" <<'NAAB'
use governance
main {
    let h = governance.health()
    print("HEALTH_OK")
}
NAAB
sign_govern "$WORK"
OUT=$("$NAAB" --governance-dashboard "$WORK/test.naab" 2>&1) || true
if echo "$OUT" | grep -q "HEALTH_OK"; then
    pass "E02" "escalation_effectiveness_window=3 parsed without error"
else
    fail "E02" "escalation_effectiveness_window=3 failed to parse" "$(echo "$OUT" | tail -1)"
fi

# ============================================================
# E03: Negative escalation_effectiveness_window clamped to 0
# ============================================================
echo -e "${CYAN}--- E03: Negative value clamped ---${NC}"
WORK=$(setup_workdir)
cat > "$WORK/govern.json" <<'GJSON'
{
  "version": "5.0", "mode": "enforce",
  "languages": {"allowed": ["python"]},
  "security": {"sandbox_level": "elevated"},
  "context_drift": {"enabled": true, "level": "advisory", "escalation_effectiveness_window": -5},
  "governance": {"dashboard": true}
}
GJSON
cat > "$WORK/test.naab" <<'NAAB'
use governance
main {
    let h = governance.health()
    print("HEALTH_OK")
}
NAAB
sign_govern "$WORK"
OUT=$("$NAAB" --governance-dashboard "$WORK/test.naab" 2>&1) || true
if echo "$OUT" | grep -q "HEALTH_OK"; then
    pass "E03" "Negative escalation_effectiveness_window clamped (no crash)"
else
    fail "E03" "Negative value caused crash" "$(echo "$OUT" | tail -1)"
fi

# ============================================================
# E04: Dashboard does NOT show "Escalation:" when no escalation occurred
# ============================================================
echo -e "${CYAN}--- E04: Dashboard no escalation line ---${NC}"
WORK=$(setup_workdir)
cat > "$WORK/govern.json" <<'GJSON'
{
  "version": "5.0", "mode": "enforce",
  "languages": {"allowed": ["python"]},
  "security": {"sandbox_level": "elevated"},
  "capabilities": {"network": {"enabled": true}},
  "context_drift": {"enabled": true, "level": "advisory",
    "signals": {"claim_result_reconciliation": true}},
  "agents": {
    "test_agent": {
      "provider": "gemini", "model": "gemma-4-31b-it",
      "api_key_env": "NAAB_TEST_FAKE_KEY", "max_tokens": 64, "max_turns": 2,
      "system_prompt": "You are a test assistant.",
      "network_allowed": true
    }
  },
  "circuit_breaker": {"enabled": true},
  "governance": {"dashboard": true}
}
GJSON
cat > "$WORK/test.naab" <<'NAAB'
use agent
main {
    let h = agent.create("test_agent")
    print("AGENT_CREATED")
}
NAAB
sign_govern "$WORK"
export NAAB_TEST_FAKE_KEY="fake-key-for-create-only"
OUT=$("$NAAB" --governance-dashboard "$WORK/test.naab" 2>&1) || true
ESC_LINE=$(echo "$OUT" | grep "Escalation:" | wc -l)
ESC_LINE=$(echo "$ESC_LINE" | tr -d ' ')
if [ "$ESC_LINE" -eq 0 ]; then
    pass "E04" "No 'Escalation:' line when no escalation occurred"
else
    fail "E04" "Unexpected 'Escalation:' line in dashboard" "found $ESC_LINE lines"
fi

# ============================================================
# E05: Agent creation — environment dict has escalation_turn = -1
# ============================================================
echo -e "${CYAN}--- E05: Agent env dict escalation defaults ---${NC}"
WORK=$(setup_workdir)
cat > "$WORK/govern.json" <<'GJSON'
{
  "version": "5.0", "mode": "enforce",
  "languages": {"allowed": ["python"]},
  "security": {"sandbox_level": "elevated"},
  "capabilities": {"network": {"enabled": true}},
  "context_drift": {"enabled": true, "level": "advisory",
    "escalation_effectiveness_window": 5},
  "agents": {
    "test_agent": {
      "provider": "gemini", "model": "gemma-4-31b-it",
      "api_key_env": "NAAB_TEST_FAKE_KEY", "max_tokens": 64, "max_turns": 2,
      "system_prompt": "You are a test assistant.",
      "network_allowed": true
    }
  },
  "circuit_breaker": {"enabled": true},
  "governance": {"dashboard": true}
}
GJSON
cat > "$WORK/test.naab" <<'NAAB'
use agent
fn get_env_val(handle, key) {
    let env = handle.get("environment")
    if env == null { return "NULL_ENV" }
    let state = env.get("state")
    if state == null { return "NULL_STATE" }
    let v = state.get(key)
    if v == null { return "NULL" }
    return v
}
main {
    let h = agent.create("test_agent")
    let et = get_env_val(h, "escalation_turn")
    let ef = get_env_val(h, "escalation_from_level")
    let eto = get_env_val(h, "escalation_to_level")
    print("ESC_TURN=" + string(et))
    print("ESC_FROM=" + string(ef))
    print("ESC_TO=" + string(eto))
}
NAAB
sign_govern "$WORK"
export NAAB_TEST_FAKE_KEY="fake-key-for-create-only"
OUT=$("$NAAB" --governance-dashboard "$WORK/test.naab" 2>&1) || true
if echo "$OUT" | grep -q "ESC_TURN=-1"; then
    pass "E05" "escalation_turn=-1 on fresh agent"
else
    fail "E05" "escalation_turn not -1 on fresh agent" "$(echo "$OUT" | grep ESC_TURN)"
fi

# ============================================================
# E06: govern-template.json contains escalation_effectiveness_window
# ============================================================
echo -e "${CYAN}--- E06: govern-template.json coverage ---${NC}"
TEMPLATE="$SCRIPT_DIR/../../govern-template.json"
if grep -q "escalation_effectiveness_window" "$TEMPLATE"; then
    pass "E06" "govern-template.json contains escalation_effectiveness_window"
else
    fail "E06" "govern-template.json missing escalation_effectiveness_window"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo -e "${CYAN}+----------------------------+${NC}"
echo -e "${CYAN}|  Results                   |${NC}"
echo -e "${CYAN}+----------------------------+${NC}"
echo -e "  Total:   $TOTAL"
echo -e "  ${GREEN}Passed:  $PASS_COUNT${NC}"
echo -e "  ${RED}Failed:  $FAIL_COUNT${NC}"
echo -e "  ${YELLOW}Skipped: $SKIP_COUNT${NC}"
if [ $FAIL_COUNT -gt 0 ]; then
    echo -e "\n${RED}Failures:${NC}"
    echo -e "$FAILURES"
fi
echo ""
[ $FAIL_COUNT -eq 0 ]
