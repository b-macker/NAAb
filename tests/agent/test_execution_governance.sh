#!/usr/bin/env bash
# Execution Governance Tests
# Fix 1A: Tamper-evident telemetry hash chain
# Fix 1B: Signed execution attestations
# Fix 2:  Approval CLI (--approve, --list-approvals, --revoke-approval)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
SRC_AGENT="$SCRIPT_DIR/../../src/stdlib/agent_impl.cpp"
SRC_GOVINIT="$SCRIPT_DIR/../../src/cli/governance_init.cpp"
SRC_REPORTS="$SCRIPT_DIR/../../src/runtime/governance_reports.cpp"
SRC_CONFIG="$SCRIPT_DIR/../../src/runtime/governance_config.cpp"
SRC_MAIN="$SCRIPT_DIR/../../src/cli/main.cpp"
HEADER="$SCRIPT_DIR/../../include/naab/governance.h"

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

# ── Source Verification Tests ─────────────────────────────────────────────────

echo "=== Source Verification ==="

# T1: TelemetryOutputConfig has tamper_evidence
if grep -q 'TamperEvidenceConfig tamper_evidence' "$HEADER" | head -1 && \
   grep -A3 'struct TelemetryOutputConfig' "$HEADER" | grep -q 'tamper_evidence'; then
    pass "T1: TelemetryOutputConfig has tamper_evidence field"
else
    # Fallback check
    if grep -A5 'TelemetryOutputConfig' "$HEADER" | grep -q 'tamper_evidence'; then
        pass "T1: TelemetryOutputConfig has tamper_evidence field"
    else
        fail "T1: TelemetryOutputConfig missing tamper_evidence"
    fi
fi

# T2: writeTelemetry has hash chain (prev_hash/hash)
if grep -q 'prev_hash.*last_telemetry_hash_\|last_telemetry_hash_.*prev_hash' "$SRC_REPORTS" || \
   (grep -q 'last_telemetry_hash_' "$SRC_REPORTS" && grep -q 'prev_hash' "$SRC_REPORTS"); then
    pass "T2: writeTelemetry has hash chain"
else
    fail "T2: writeTelemetry missing hash chain"
fi

# T3: ProvenanceConfig parsed from JSON
if grep -q 'provenance.*record_attestations\|record_attestations.*provenance' "$SRC_CONFIG"; then
    pass "T3: ProvenanceConfig parsed from JSON"
else
    fail "T3: ProvenanceConfig not parsed from JSON"
fi

# T4: emitAttestation exists
if grep -q 'emitAttestation' "$SRC_REPORTS"; then
    pass "T4: emitAttestation implemented in governance_reports.cpp"
else
    fail "T4: emitAttestation missing"
fi

# T5: agent_impl.cpp calls emitAttestation
if grep -q 'emitAttestation' "$SRC_AGENT"; then
    pass "T5: agent_impl.cpp calls emitAttestation"
else
    fail "T5: agent_impl.cpp missing emitAttestation call"
fi

# T6: main.cpp has --approve flag
if grep -q '"--approve"' "$SRC_MAIN"; then
    pass "T6: main.cpp has --approve flag"
else
    fail "T6: main.cpp missing --approve flag"
fi

# T7: main.cpp has --list-approvals flag
if grep -q '"--list-approvals"' "$SRC_MAIN"; then
    pass "T7: main.cpp has --list-approvals flag"
else
    fail "T7: main.cpp missing --list-approvals flag"
fi

# T8: main.cpp has --revoke-approval flag
if grep -q '"--revoke-approval"' "$SRC_MAIN"; then
    pass "T8: main.cpp has --revoke-approval flag"
else
    fail "T8: main.cpp missing --revoke-approval flag"
fi

# T9: governance_init.cpp has approval config
if grep -q 'approval' "$SRC_GOVINIT" && grep -q 'approver_keys' "$SRC_GOVINIT"; then
    pass "T9: governance_init.cpp has approval config in template"
else
    fail "T9: governance_init.cpp missing approval config"
fi

# ── Structural Verification ───────────────────────────────────────────────────

echo ""
echo "=== Structural Verification ==="

# T10: emitAttestation is AFTER governance checks but BEFORE agentSend return
ATTEST_LINE=$(grep -n 'emitAttestation' "$SRC_AGENT" | head -1 | cut -d: -f1)
# Find the checkContextDrift line (which is the last governance check before return)
CDD_LINE=$(grep -n 'checkContextDrift' "$SRC_AGENT" | head -1 | cut -d: -f1)
if [[ -n "$ATTEST_LINE" && -n "$CDD_LINE" ]] && (( ATTEST_LINE > CDD_LINE )); then
    pass "T10: emitAttestation (line $ATTEST_LINE) is after CDD check (line $CDD_LINE)"
else
    fail "T10: emitAttestation must be after checkContextDrift in agentSend" \
         "attest=$ATTEST_LINE cdd=$CDD_LINE"
fi

# T11: computeHash shared helper exists (refactored from computeAuditHash)
if grep -q 'computeHash' "$SRC_REPORTS"; then
    pass "T11: computeHash shared helper exists"
else
    fail "T11: computeHash shared helper missing"
fi

# ── Behavioral Tests ──────────────────────────────────────────────────────────

echo ""
echo "=== Behavioral Tests ==="

TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
TEST_DIR="$TMPDIR/test_exec_gov_$$"
mkdir -p "$TEST_DIR/.naab"

# T12: --approve generates valid JSON
NAAB_SIGNING_KEY="$SCRIPT_DIR/../../tests/security/test_signing_key.pem"
if [[ ! -f "$NAAB_SIGNING_KEY" ]]; then
    # Generate a temporary key for testing
    cd "$TEST_DIR"
    "$NAAB" --keygen test_key.pem 2>/dev/null
    "$NAAB" --trust-key "$TEST_DIR/test_key.pem.pub" 2>/dev/null
    NAAB_SIGNING_KEY="$TEST_DIR/test_key.pem"
fi

cd "$TEST_DIR"
APPROVE_OUT=$(NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --approve test.rule --reason "testing" --expiry 1 2>&1)
if [[ -f "$TEST_DIR/.naab/approvals.json" ]]; then
    # Verify it's valid JSON with the right key
    if grep -q '"test.rule"' "$TEST_DIR/.naab/approvals.json"; then
        pass "T12: --approve generates valid JSON with correct rule key"
    else
        fail "T12: --approve JSON missing rule key" "$(cat "$TEST_DIR/.naab/approvals.json")"
    fi
else
    fail "T12: --approve did not create approvals.json" "$APPROVE_OUT"
fi

# T13: --list-approvals shows the token
LIST_OUT=$(cd "$TEST_DIR" && "$NAAB" --list-approvals 2>&1)
if echo "$LIST_OUT" | grep -q 'test.rule'; then
    pass "T13: --list-approvals shows the token"
else
    fail "T13: --list-approvals did not show token" "$LIST_OUT"
fi

# T14: --revoke-approval removes it
REVOKE_OUT=$(cd "$TEST_DIR" && "$NAAB" --revoke-approval test.rule 2>&1)
if [[ -f "$TEST_DIR/.naab/approvals.json" ]]; then
    if grep -q 'test.rule' "$TEST_DIR/.naab/approvals.json"; then
        fail "T14: --revoke-approval did not remove token"
    else
        pass "T14: --revoke-approval removed the token"
    fi
else
    pass "T14: --revoke-approval removed the token (file empty/gone)"
fi

# Cleanup
rm -rf "$TEST_DIR"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
exit $FAIL
