#!/usr/bin/env bash
# ============================================================
# test_bsd_cdd_gate.sh — context_drift.enabled does nothing without BSD
#
# The whole CDD block in agentSend() -- checkContextDrift(), all 23 signals,
# and the CDD_TURN emission -- lives inside a guard on
# behavioral_sequences.enabled (agent_impl.cpp:4036, block 4036-4431), which
# defaults FALSE (governance.h:1310). checkOutputAdmissibility() sits at :4443,
# OUTSIDE that block, gated independently on circuit_breaker.*.
#
# So a config saying "context_drift": {"enabled": true} and nothing else gets
# no drift detection at all, silently, while the admissibility gate keeps
# running on a coherence score no signal ever wrote and reports baseline_state
# "calibrating" -- which reads as "window filling", not "nothing was observed".
# A live run was misdiagnosed twice through that gap.
#
# Every in-tree CDD test sets both flags together. That is why the suite never
# caught it, and it is why BC-01 exists: without a control proving this fixture
# CAN produce CDD_TURN, BC-02's zero would be indistinguishable from a broken
# fixture.
#
#   BC-01  control — BSD on  + CDD on  => CDD_TURN > 0
#   BC-02  BSD off + CDD on            => CDD_TURN == 0 (the coupling)
#   BC-03  BSD off + CDD on            => config load warns
#   BC-04  control — BSD on  + CDD on  => no warning
#   BC-05  control — BSD off + CDD off => no warning (nothing was asked for)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_bsd_cdd_gate.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/bsdcdd-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_BSDCDD="fake-key-bsd-cdd-gate"
sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  context_drift.enabled is inert without behavioral_sequences  |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# $1 = arm name, $2 = bsd (true/false), $3 = cdd (true/false)
run_arm() {
    local arm="$1" bsd="$2" cdd="$3"
    local W="$TEST_TMP/$arm"; mkdir -p "$W"
    cat > "$W/fixture.json" << 'EOF'
{"responses": [
  {"content": "a reasoned paragraph on ledger reconciliation and quarterly totals", "output_tokens": 14}
]}
EOF
    start_stub "$W/fixture.json" "$W" || return 1
    cat > "$W/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": $bsd },
    "context_drift": { "enabled": $cdd, "level": "advisory", "check_interval_turns": 1 },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_BSDCDD", "max_tokens": 100, "max_turns": 20
        }
    }
}
GOVEOF
    sign_govern "$W"
    cat > "$W/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    agent.send(h, "reconcile the quarterly ledger totals")
    agent.send(h, "now summarize the discrepancies you found")
    print("DONE")
}
NAABEOF
    ARM_ERR=$(cd "$W" && timeout 60s "$NAAB" test.naab 2>&1 >/dev/null) || true
    stop_stub
    ARM_CDD=$(grep -c '"CDD_TURN"' "$W/telemetry.jsonl" 2>/dev/null || true)
    [ -z "$ARM_CDD" ] && ARM_CDD=0
    return 0
}

# ── BC-01: control — both on, CDD must actually run ──
if run_arm "on_on" true true; then
    if [ "$ARM_CDD" -gt 0 ]; then
        pass "BC-01" "control: BSD on + CDD on emits CDD_TURN ($ARM_CDD)"
    else
        fail "BC-01" "no CDD_TURN even with BSD on — fixture is broken, BC-02 proves nothing" "$ARM_ERR"
    fi
    if echo "$ARM_ERR" | grep -q 'context_drift.enabled.*no effect'; then
        fail "BC-04" "warned about an effective context_drift config" "$ARM_ERR"
    else
        pass "BC-04" "control: no warning when behavioral_sequences is enabled"
    fi
else
    skip "BC-01" "stub failed to start"; skip "BC-04" "stub failed to start"
fi

# ── BC-02/03: CDD asked for, BSD off ──
if run_arm "off_on" false true; then
    if [ "$ARM_CDD" = "0" ]; then
        pass "BC-02" "CONFIRMED: BSD off silently disables all CDD (CDD_TURN=0)"
    else
        pass "BC-02" "CDD now runs without BSD ($ARM_CDD) — coupling was removed, update BC-03"
    fi
    if echo "$ARM_ERR" | grep -q 'context_drift.enabled.*no effect'; then
        pass "BC-03" "config load warns that context_drift.enabled is inert"
    else
        fail "BC-03" "no warning — an inert context_drift config loads silently" \
             "stderr: $(echo "$ARM_ERR" | head -3 | tr '\n' ' ')"
    fi
else
    skip "BC-02" "stub failed to start"; skip "BC-03" "stub failed to start"
fi

# ── BC-05: control — CDD not asked for, so no warning ──
if run_arm "off_off" false false; then
    if echo "$ARM_ERR" | grep -q 'context_drift.enabled.*no effect'; then
        fail "BC-05" "warned when context_drift was never enabled" "$ARM_ERR"
    else
        pass "BC-05" "control: no warning when context_drift is disabled too"
    fi
else
    skip "BC-05" "stub failed to start"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}| Results: ${GREEN}${PASS_COUNT} passed${NC}, ${RED}${FAIL_COUNT} failed${NC}, ${YELLOW}${SKIP_COUNT} skipped${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}${FAILURES}"; exit 1; fi
exit 0
