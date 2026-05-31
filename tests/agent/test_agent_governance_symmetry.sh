#!/usr/bin/env bash
# Agent Governance Symmetry Tests
# Fix 1: Prompt-side scanning — checkSecrets/checkPii on outbound messages
# Fix 2: Agent output as taint source — agent.send/agent.run in default sources
# Fix 3: Pipeline stage metadata — structured header between stages
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
SRC_AGENT="$SCRIPT_DIR/../../src/stdlib/agent_impl.cpp"
SRC_GOVINIT="$SCRIPT_DIR/../../src/cli/governance_init.cpp"

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

# ── Source Verification Tests ─────────────────────────────────────────────────

echo "=== Source Verification ==="

# T1: Prompt-side secrets scan exists
if grep -q 'checkSecrets(message' "$SRC_AGENT"; then
    pass "T1: agentSend has outbound checkSecrets(message)"
else
    fail "T1: agentSend missing outbound checkSecrets(message)"
fi

# T2: Prompt-side PII scan exists
if grep -q 'checkPii(message' "$SRC_AGENT"; then
    pass "T2: agentSend has outbound checkPii(message)"
else
    fail "T2: agentSend missing outbound checkPii(message)"
fi

# T3: Outbound error message pattern (not inbound "Response from")
if grep -q '"Prompt for '"'"'"' "$SRC_AGENT"; then
    pass "T3: outbound error uses 'Prompt for' (distinct from inbound 'Response from')"
else
    # Fallback: check for the format string pattern
    if grep -q 'Prompt for' "$SRC_AGENT"; then
        pass "T3: outbound error uses 'Prompt for' pattern"
    else
        fail "T3: missing 'Prompt for' error message pattern"
    fi
fi

# T4: agent.send in default taint sources
if grep -q '"agent\.send"' "$SRC_GOVINIT"; then
    pass "T4: governance_init.cpp includes agent.send as taint source"
else
    fail "T4: governance_init.cpp missing agent.send taint source"
fi

# T5: agent.run in default taint sources
if grep -q '"agent\.run"' "$SRC_GOVINIT"; then
    pass "T5: governance_init.cpp includes agent.run as taint source"
else
    fail "T5: governance_init.cpp missing agent.run taint source"
fi

# T6: Pipeline stage metadata header
if grep -q 'Pipeline Stage' "$SRC_AGENT"; then
    pass "T6: agentPipeline has stage metadata header"
else
    fail "T6: agentPipeline missing stage metadata header"
fi

# T7: Pipeline extracts token usage for metadata
if grep -q 'output_tokens' "$SRC_AGENT" && grep -q 'Tokens:' "$SRC_AGENT"; then
    pass "T7: pipeline metadata includes token usage"
else
    fail "T7: pipeline metadata missing token usage"
fi

# T8: Pipeline includes governance pressure in metadata
if grep -q 'Governance pressure' "$SRC_AGENT"; then
    pass "T8: pipeline metadata includes governance pressure"
else
    fail "T8: pipeline metadata missing governance pressure"
fi

# ── Structural Verification Tests ─────────────────────────────────────────────

echo ""
echo "=== Structural Verification ==="

# T9: Prompt scanning is BEFORE the API call (checkSecrets(message before callAgentMultiTurn)
SECRETS_LINE=$(grep -n 'checkSecrets(message' "$SRC_AGENT" | head -1 | cut -d: -f1)
API_CALL_LINE=$(grep -n 'callAgentMultiTurn' "$SRC_AGENT" | head -1 | cut -d: -f1)
if [[ -n "$SECRETS_LINE" && -n "$API_CALL_LINE" ]] && (( SECRETS_LINE < API_CALL_LINE )); then
    pass "T9: outbound checkSecrets (line $SECRETS_LINE) is before API call (line $API_CALL_LINE)"
else
    fail "T9: outbound checkSecrets must be before callAgentMultiTurn" \
         "secrets=$SECRETS_LINE api=$API_CALL_LINE"
fi

# T10: Both inbound AND outbound scanning exist (symmetry check)
OUTBOUND_COUNT=$(grep -c 'checkSecrets(message' "$SRC_AGENT" || true)
INBOUND_COUNT=$(grep -c 'checkSecrets(content' "$SRC_AGENT" || true)
if (( OUTBOUND_COUNT >= 1 && INBOUND_COUNT >= 1 )); then
    pass "T10: both outbound ($OUTBOUND_COUNT) and inbound ($INBOUND_COUNT) secrets checks present"
else
    fail "T10: asymmetric scanning — outbound=$OUTBOUND_COUNT inbound=$INBOUND_COUNT"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
