#!/usr/bin/env bash
# Agent Governance Unification Tests
# Fix 1: agent.send as taint SINK — bidirectional taint tracking
# Fix 2: checkInfoDisclosure on outbound prompts
# Fix 3: json_valid failure → CDD coherence signal
# Fix 4: Scanner agent-aware checks (agent_unchecked_send, agent_prompt_from_file)
# Fix 5: BSD default pattern — AGENT_RESPONSE → FILE_WRITE
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
SRC_AGENT="$SCRIPT_DIR/../../src/stdlib/agent_impl.cpp"
SRC_GOVINIT="$SCRIPT_DIR/../../src/cli/governance_init.cpp"
SRC_SCANNER="$SCRIPT_DIR/../../src/scanner/checks_lang_naab.cpp"

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

# ── Source Verification Tests ─────────────────────────────────────────────────

echo "=== Source Verification ==="

# T1: agent.send in taint sinks (governance_init.cpp)
if grep -q '"agent\.send"' "$SRC_GOVINIT" && grep -q 'taint_sinks.*agent\.send\|agent\.send.*sink' "$SRC_GOVINIT"; then
    pass "T1: governance_init.cpp has agent.send as taint sink"
else
    # Fallback: check for agent.send in sinks JSON array
    if grep -A2 'taint_sinks' "$SRC_GOVINIT" | grep -q 'agent\.send'; then
        pass "T1: governance_init.cpp has agent.send as taint sink (in sinks array)"
    elif grep 'agent\.send' "$SRC_GOVINIT" | grep -q 'sink'; then
        pass "T1: governance_init.cpp has agent.send as taint sink (contextual)"
    else
        fail "T1: governance_init.cpp missing agent.send as taint sink"
    fi
fi

# T2: checkInfoDisclosure on outbound prompts
if grep -q 'checkInfoDisclosure' "$SRC_AGENT"; then
    pass "T2: agent_impl.cpp has checkInfoDisclosure on outbound prompts"
else
    fail "T2: agent_impl.cpp missing checkInfoDisclosure call"
fi

# T3: json_error_signal for CDD integration
if grep -q 'json_error_signal' "$SRC_AGENT"; then
    pass "T3: agent_impl.cpp has json_error_signal (CDD integration)"
else
    fail "T3: agent_impl.cpp missing json_error_signal for CDD"
fi

# T4: agent_unchecked_send scanner check
if grep -q 'agent_unchecked_send' "$SRC_SCANNER"; then
    pass "T4: checks_lang_naab.cpp has agent_unchecked_send check"
else
    fail "T4: checks_lang_naab.cpp missing agent_unchecked_send check"
fi

# T5: agent_prompt_from_file scanner check
if grep -q 'agent_prompt_from_file' "$SRC_SCANNER"; then
    pass "T5: checks_lang_naab.cpp has agent_prompt_from_file check"
else
    fail "T5: checks_lang_naab.cpp missing agent_prompt_from_file check"
fi

# T6: agent_output_to_file BSD pattern
if grep -q 'agent_output_to_file' "$SRC_GOVINIT"; then
    pass "T6: governance_init.cpp has agent_output_to_file BSD pattern"
else
    fail "T6: governance_init.cpp missing agent_output_to_file BSD pattern"
fi

# ── Structural Verification Tests ─────────────────────────────────────────────

echo ""
echo "=== Structural Verification ==="

# T7: json_valid check appears BEFORE checkContextDrift
JSON_CHECK_LINE=$(grep -n 'json_valid_result' "$SRC_AGENT" | head -1 | cut -d: -f1)
CDD_LINE=$(grep -n 'checkContextDrift' "$SRC_AGENT" | head -1 | cut -d: -f1)
if [[ -n "$JSON_CHECK_LINE" && -n "$CDD_LINE" ]] && (( JSON_CHECK_LINE < CDD_LINE )); then
    pass "T7: json_valid check (line $JSON_CHECK_LINE) is before CDD (line $CDD_LINE)"
else
    fail "T7: json_valid check must be before checkContextDrift" \
         "json=$JSON_CHECK_LINE cdd=$CDD_LINE"
fi

# T8: checkInfoDisclosure appears BEFORE callAgentMultiTurn
INFO_LINE=$(grep -n 'checkInfoDisclosure' "$SRC_AGENT" | head -1 | cut -d: -f1)
API_LINE=$(grep -n 'callAgentMultiTurn' "$SRC_AGENT" | head -1 | cut -d: -f1)
if [[ -n "$INFO_LINE" && -n "$API_LINE" ]] && (( INFO_LINE < API_LINE )); then
    pass "T8: checkInfoDisclosure (line $INFO_LINE) is before API call (line $API_LINE)"
else
    fail "T8: checkInfoDisclosure must be before callAgentMultiTurn" \
         "info=$INFO_LINE api=$API_LINE"
fi

# ── Behavioral Tests (Scanner) ────────────────────────────────────────────────

echo ""
echo "=== Behavioral Tests ==="

# Create temp test files for scanner
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
TEST_UNCHECKED="$TMPDIR/test_unchecked_send.naab"
TEST_FROMFILE="$TMPDIR/test_prompt_from_file.naab"

# T9: Scanner detects unchecked agent.send
cat > "$TEST_UNCHECKED" << 'NAAB'
use agent

main {
    let handle = agent.create("test")
    let resp = agent.send(handle, "hello")
    print(resp.get("content"))
}
NAAB

SCAN_OUT=$("$NAAB" --scan "$TEST_UNCHECKED" naab 2>&1 || true)
if echo "$SCAN_OUT" | grep -q 'agent_unchecked_send'; then
    pass "T9: scanner detects unchecked agent.send"
else
    fail "T9: scanner did not detect unchecked agent.send" "$SCAN_OUT"
fi

# T10: Scanner detects unsanitized file.read → agent.send
cat > "$TEST_FROMFILE" << 'NAAB'
use agent
use file

main {
    let handle = agent.create("test")
    let data = file.read("secrets.txt")
    let resp = agent.send(handle, data)
    print(resp.get("content"))
}
NAAB

SCAN_OUT2=$("$NAAB" --scan "$TEST_FROMFILE" naab 2>&1 || true)
if echo "$SCAN_OUT2" | grep -q 'agent_prompt_from_file'; then
    pass "T10: scanner detects unsanitized file.read → agent.send"
else
    fail "T10: scanner did not detect file.read → agent.send" "$SCAN_OUT2"
fi

# Cleanup
rm -f "$TEST_UNCHECKED" "$TEST_FROMFILE"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
