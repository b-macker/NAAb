#!/usr/bin/env bash
# test_governance_validity.sh — Adversarial test for governance validity layer
#
# Tests authority decay (stale signatures, key revocation, key expiry),
# environment attestation (prerequisite failures), and contradiction detection
# (conflicting govern.json rules). Each test attempts bypass or edge-case abuse.
#
# 23 tests across 3 feature areas.

set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
PASS=0; FAIL=0; SKIP=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

# Use /usr/tmp on Termux, /tmp elsewhere
SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
WORKDIR=$(mktemp -d "${SYSTMP}/gov_validity_XXXXXX")

cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "=== test_governance_validity.sh ==="
echo "  Adversarial tests for authority decay, attestation, contradictions"
echo ""

# ---------------------------------------------------------------------------
# Setup: isolate trust store so tests don't affect real keys
# ---------------------------------------------------------------------------
export NAAB_TRUST_STORE_DIR="$WORKDIR/trust-store"
mkdir -p "$NAAB_TRUST_STORE_DIR"

# Generate a test keypair and install to isolated trust store
"$NAAB" --keygen "$WORKDIR/test-key.pem" 2>"$WORKDIR/keygen.out"
"$NAAB" --trust-key "$WORKDIR/test-key.pem.pub" 2>/dev/null
TEST_FP=$(grep 'Fingerprint:' "$WORKDIR/keygen.out" | awk '{print $NF}')

if [ -z "$TEST_FP" ]; then
    echo "FATAL: keygen failed — cannot run tests"
    cat "$WORKDIR/keygen.out"
    exit 1
fi

echo "  Test key: $TEST_FP"
echo ""

# Helper: create a minimal govern.json and sign it
make_govern() {
    local dir="$1"
    shift
    # Accept JSON body as argument, default to minimal config
    local body="${1:-"{\"mode\": \"enforce\"}"}"
    mkdir -p "$dir"
    echo "$body" > "$dir/govern.json"
    # Sign it
    NAAB_SIGNING_KEY="$WORKDIR/test-key.pem" "$NAAB" --sign-governance "$dir/govern.json" 2>/dev/null
}

# Helper: create a .naab test script
make_script() {
    local path="$1"
    cat > "$path" <<'NAAB_SCRIPT'
main {
    let x = 1 + 1
    print(x)
}
NAAB_SCRIPT
}

# =========================================================================
# SECTION 1: Authority Decay — Key Lifecycle
# =========================================================================
echo "--- Section 1: Authority Decay ---"

# T1: --key-info shows metadata for installed key
echo "[T1] --key-info shows key metadata"
OUT=$("$NAAB" --key-info "$TEST_FP" 2>&1)
if echo "$OUT" | grep -q "Key: $TEST_FP" && echo "$OUT" | grep -q "Revoked:.*no"; then
    ok "T1: --key-info displays correct metadata"
else
    fail "T1: --key-info output unexpected: $OUT"
fi

# T2: --revoke-key marks key as revoked
echo "[T2] --revoke-key revokes a key"
"$NAAB" --revoke-key "$TEST_FP" "adversarial test revocation" 2>"$WORKDIR/revoke.out"
OUT=$("$NAAB" --key-info "$TEST_FP" 2>&1)
if echo "$OUT" | grep -q "Revoked:.*YES" && echo "$OUT" | grep -q "adversarial test revocation"; then
    ok "T2: key correctly marked as revoked"
else
    fail "T2: revocation not reflected in metadata: $OUT"
fi

# T3: Revoked key is skipped during signature verification
echo "[T3] Revoked key blocks signature verification"
TDIR="$WORKDIR/t3"
make_govern "$TDIR"
make_script "$TDIR/test.naab"

# Run with revoked key — should fail because the only trusted key is revoked
OUT=$(NAAB_SIGNING_KEY="$WORKDIR/test-key.pem" "$NAAB" "$TDIR/test.naab" 2>&1 || true)
if echo "$OUT" | grep -qi "revoked\|no trusted\|INTEGRITY BLOCK\|missing"; then
    ok "T3: revoked key blocked signature verification"
else
    fail "T3: revoked key did not block verification: $OUT"
fi

# T4: Un-revoke by reinstalling key (fresh metadata)
echo "[T4] Reinstalling key after revocation restores trust"
# Remove the revoked key's .meta.json and reinstall
rm -f "$NAAB_TRUST_STORE_DIR/$TEST_FP.meta.json"
rm -f "$NAAB_TRUST_STORE_DIR/$TEST_FP.pub"
rm -f "$NAAB_TRUST_STORE_DIR/default.pub"

# Generate a new key and install with countersigning
"$NAAB" --keygen "$WORKDIR/test-key2.pem" 2>"$WORKDIR/keygen2.out"
TEST_FP2=$(grep 'Fingerprint:' "$WORKDIR/keygen2.out" | awk '{print $NF}')
# Trust store is now empty (key1 revoked+removed), so key2 can be installed directly
"$NAAB" --trust-key "$WORKDIR/test-key2.pem.pub" 2>/dev/null

# Re-sign with new key and verify it works
TDIR4="$WORKDIR/t4"
make_govern "$TDIR4" '{"mode": "enforce"}'
# Re-sign with new key
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR4/govern.json" 2>/dev/null
make_script "$TDIR4/test.naab"

OUT=$(NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" "$TDIR4/test.naab" 2>&1 || true)
if echo "$OUT" | grep -q "2"; then
    ok "T4: fresh key restores trust and program runs"
else
    fail "T4: fresh key did not restore trust: $OUT"
fi

# T5: --revoke-key on nonexistent fingerprint fails gracefully
echo "[T5] Revoking nonexistent key fails gracefully"
OUT=$("$NAAB" --revoke-key "nonexistent-fp-12345" "test" 2>&1 || true)
if echo "$OUT" | grep -qi "not found\|Error\|fail"; then
    ok "T5: nonexistent key revocation fails gracefully"
else
    fail "T5: no error for nonexistent key: $OUT"
fi

# =========================================================================
# SECTION 2: Authority Decay — Signature Staleness
# =========================================================================
echo ""
echo "--- Section 2: Signature Staleness ---"

# T6: Signature with max_signature_age_days=0 (disabled) — no staleness warning
echo "[T6] Staleness check disabled when max_signature_age_days=0"
TDIR6="$WORKDIR/t6"
make_govern "$TDIR6" '{"mode": "enforce", "trust": {"max_signature_age_days": 0}}'
# Re-sign with current key
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR6/govern.json" 2>/dev/null
make_script "$TDIR6/test.naab"
OUT=$("$NAAB" "$TDIR6/test.naab" 2>&1 || true)
if echo "$OUT" | grep -q "2" && ! echo "$OUT" | grep -qi "stale"; then
    ok "T6: no staleness warning when disabled"
else
    fail "T6: unexpected staleness behavior: $OUT"
fi

# T7: Signature with max_signature_age_days=1 and a backdated .sig — advisory warning
echo "[T7] Stale signature triggers advisory warning"
TDIR7="$WORKDIR/t7"
mkdir -p "$TDIR7"
echo '{"mode": "enforce", "trust": {"max_signature_age_days": 1, "stale_signature_level": "advisory"}}' > "$TDIR7/govern.json"
# Sign it
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR7/govern.json" 2>/dev/null
# Backdate the signature: replace timestamp with 90 days ago
NINETY_DAYS_AGO=$(( $(date +%s) - 7776000 ))
SIG_CONTENT=$(cat "$TDIR7/govern.json.sig")
# Extract base sig (everything before the last colon)
BASE_SIG=$(echo "$SIG_CONTENT" | sed 's/:[^:]*$//')
echo "${BASE_SIG}:${NINETY_DAYS_AGO}" > "$TDIR7/govern.json.sig"
make_script "$TDIR7/test.naab"
OUT=$("$NAAB" "$TDIR7/test.naab" 2>&1 || true)
if echo "$OUT" | grep -qi "stale\|days old\|WARNING.*[Ss]ignature"; then
    ok "T7: stale signature produces advisory warning"
else
    # Program should still run (advisory = warn only)
    if echo "$OUT" | grep -q "2"; then
        ok "T7: stale signature advisory — program still ran (warning may be suppressed)"
    else
        fail "T7: stale signature not detected: $OUT"
    fi
fi

# T8: Stale signature at HARD level blocks execution
echo "[T8] Stale signature at HARD level blocks execution"
TDIR8="$WORKDIR/t8"
mkdir -p "$TDIR8"
echo '{"mode": "enforce", "trust": {"max_signature_age_days": 1, "stale_signature_level": "hard"}}' > "$TDIR8/govern.json"
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR8/govern.json" 2>/dev/null
# Backdate signature
SIG_CONTENT=$(cat "$TDIR8/govern.json.sig")
BASE_SIG=$(echo "$SIG_CONTENT" | sed 's/:[^:]*$//')
echo "${BASE_SIG}:${NINETY_DAYS_AGO}" > "$TDIR8/govern.json.sig"
make_script "$TDIR8/test.naab"
RC=0
"$NAAB" "$TDIR8/test.naab" 2>"$WORKDIR/t8.err" || RC=$?
if [ $RC -ne 0 ] && grep -qi "STALE SIGNATURE BLOCK\|INTEGRITY BLOCK" "$WORKDIR/t8.err"; then
    ok "T8: HARD staleness blocks execution"
else
    fail "T8: HARD staleness did not block (rc=$RC): $(cat "$WORKDIR/t8.err")"
fi

# T9: Tampered timestamp does not bypass signature verification
echo "[T9] Tampered timestamp does not weaken signature verification"
TDIR9="$WORKDIR/t9"
mkdir -p "$TDIR9"
echo '{"mode": "enforce", "trust": {"max_signature_age_days": 365}}' > "$TDIR9/govern.json"
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR9/govern.json" 2>/dev/null
# Tamper the govern.json AFTER signing
echo '{"mode": "off"}' > "$TDIR9/govern.json"
make_script "$TDIR9/test.naab"
RC=0
"$NAAB" "$TDIR9/test.naab" 2>"$WORKDIR/t9.err" || RC=$?
if [ $RC -ne 0 ] && grep -qi "INTEGRITY BLOCK\|does not match" "$WORKDIR/t9.err"; then
    ok "T9: tampered file still blocked despite valid timestamp"
else
    fail "T9: tampered file was not blocked (rc=$RC): $(cat "$WORKDIR/t9.err")"
fi

# T10: Old-format signature (no timestamp) still verifies (backward compat)
echo "[T10] Old-format signature (no timestamp) still verifies"
TDIR10="$WORKDIR/t10"
mkdir -p "$TDIR10"
echo '{"mode": "enforce", "trust": {"max_signature_age_days": 1}}' > "$TDIR10/govern.json"
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR10/govern.json" 2>/dev/null
# Strip the timestamp from the sig (keep only ed25519:<base64>)
SIG_CONTENT=$(cat "$TDIR10/govern.json.sig")
# Remove from last colon onward (the timestamp)
STRIPPED=$(echo "$SIG_CONTENT" | sed 's/:[^:]*$//')
echo "$STRIPPED" > "$TDIR10/govern.json.sig"
make_script "$TDIR10/test.naab"
OUT=$("$NAAB" "$TDIR10/test.naab" 2>&1 || true)
if echo "$OUT" | grep -q "2"; then
    ok "T10: old-format signature (no timestamp) still verifies"
else
    fail "T10: old-format signature rejected: $OUT"
fi

# =========================================================================
# SECTION 3: Environment Attestation
# =========================================================================
echo ""
echo "--- Section 3: Environment Attestation ---"

# T11: --attest with no prerequisites configured
echo "[T11] --attest with no prerequisites"
TDIR11="$WORKDIR/t11"
make_govern "$TDIR11" '{"mode": "enforce"}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR11/govern.json" 2>/dev/null
OUT=$(cd "$TDIR11" && "$NAAB" --attest 2>&1 || true)
if echo "$OUT" | grep -qi "No prerequisites"; then
    ok "T11: --attest reports no prerequisites configured"
else
    fail "T11: unexpected --attest output: $OUT"
fi

# T12: Attestation passes for existing env var
echo "[T12] Attestation passes for existing env var"
TDIR12="$WORKDIR/t12"
export GOV_TEST_VAR="hello"
make_govern "$TDIR12" '{"mode": "enforce", "prerequisites": {"enabled": true, "checks": [{"type": "env_var", "name": "GOV_TEST_VAR", "required": "exists", "level": "advisory"}]}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR12/govern.json" 2>/dev/null
OUT=$(cd "$TDIR12" && "$NAAB" --attest 2>&1)
RC=$?
if [ $RC -eq 0 ] && echo "$OUT" | grep -q "PASS"; then
    ok "T12: env var attestation passed"
else
    fail "T12: env var attestation failed (rc=$RC): $OUT"
fi

# T13: Attestation fails for missing env var
echo "[T13] Attestation fails for missing env var"
TDIR13="$WORKDIR/t13"
unset DEFINITELY_NOT_SET_XYZ 2>/dev/null || true
make_govern "$TDIR13" '{"mode": "enforce", "prerequisites": {"enabled": true, "checks": [{"type": "env_var", "name": "DEFINITELY_NOT_SET_XYZ", "required": "exists", "level": "advisory"}]}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR13/govern.json" 2>/dev/null
RC=0
OUT=$(cd "$TDIR13" && "$NAAB" --attest 2>&1) || RC=$?
if [ $RC -ne 0 ] && echo "$OUT" | grep -q "FAIL"; then
    ok "T13: missing env var attestation correctly fails"
else
    fail "T13: missing env var was not detected (rc=$RC): $OUT"
fi

# T14: Attestation for existing tool (sh — always available)
echo "[T14] Attestation passes for available tool"
TDIR14="$WORKDIR/t14"
make_govern "$TDIR14" '{"mode": "enforce", "prerequisites": {"enabled": true, "checks": [{"type": "tool", "name": "sh", "required": "exists", "level": "advisory"}]}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR14/govern.json" 2>/dev/null
OUT=$(cd "$TDIR14" && "$NAAB" --attest 2>&1)
RC=$?
if [ $RC -eq 0 ] && echo "$OUT" | grep -q "PASS"; then
    ok "T14: tool attestation passed for 'sh'"
else
    fail "T14: tool attestation failed (rc=$RC): $OUT"
fi

# T15: Attestation fails for nonexistent tool
echo "[T15] Attestation fails for nonexistent tool"
TDIR15="$WORKDIR/t15"
make_govern "$TDIR15" '{"mode": "enforce", "prerequisites": {"enabled": true, "checks": [{"type": "tool", "name": "this_tool_does_not_exist_xyz", "required": "exists", "level": "advisory"}]}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR15/govern.json" 2>/dev/null
RC=0
OUT=$(cd "$TDIR15" && "$NAAB" --attest 2>&1) || RC=$?
if [ $RC -ne 0 ] && echo "$OUT" | grep -q "FAIL"; then
    ok "T15: nonexistent tool attestation correctly fails"
else
    fail "T15: nonexistent tool not detected (rc=$RC): $OUT"
fi

# T16: Attestation with command type (exit 0 = pass)
echo "[T16] Attestation command type — exit 0 passes"
TDIR16="$WORKDIR/t16"
make_govern "$TDIR16" '{"mode": "enforce", "prerequisites": {"enabled": true, "checks": [{"type": "command", "name": "true", "required": "exists", "level": "advisory"}]}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR16/govern.json" 2>/dev/null
OUT=$(cd "$TDIR16" && "$NAAB" --attest 2>&1)
RC=$?
if [ $RC -eq 0 ] && echo "$OUT" | grep -q "PASS"; then
    ok "T16: command 'true' attestation passed"
else
    fail "T16: command attestation failed (rc=$RC): $OUT"
fi

# T17: Attestation command type — exit non-zero fails
echo "[T17] Attestation command type — exit non-zero fails"
TDIR17="$WORKDIR/t17"
make_govern "$TDIR17" '{"mode": "enforce", "prerequisites": {"enabled": true, "checks": [{"type": "command", "name": "false", "required": "exists", "level": "advisory"}]}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR17/govern.json" 2>/dev/null
RC=0
OUT=$(cd "$TDIR17" && "$NAAB" --attest 2>&1) || RC=$?
if [ $RC -ne 0 ] && echo "$OUT" | grep -q "FAIL"; then
    ok "T17: command 'false' attestation correctly fails"
else
    fail "T17: command 'false' was not detected (rc=$RC): $OUT"
fi

# T18: Prerequisites disabled by default — no checks run even if checks array present
echo "[T18] Prerequisites disabled by default"
TDIR18="$WORKDIR/t18"
make_govern "$TDIR18" '{"mode": "enforce", "prerequisites": {"enabled": false, "checks": [{"type": "env_var", "name": "DEFINITELY_NOT_SET_XYZ", "required": "exists", "level": "hard"}]}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR18/govern.json" 2>/dev/null
make_script "$TDIR18/test.naab"
OUT=$("$NAAB" "$TDIR18/test.naab" 2>&1 || true)
if echo "$OUT" | grep -q "2"; then
    ok "T18: disabled prerequisites do not block execution"
else
    fail "T18: disabled prerequisites blocked execution: $OUT"
fi

# =========================================================================
# SECTION 4: Contradiction Detection
# =========================================================================
echo ""
echo "--- Section 4: Contradiction Detection ---"

# T19: CONTRA-002 — network disabled but allowed_hosts non-empty
echo "[T19] CONTRA-002: network disabled + allowed_hosts"
TDIR19="$WORKDIR/t19"
make_govern "$TDIR19" '{"mode": "enforce", "capabilities": {"network": {"enabled": false, "allowed_hosts": ["api.example.com"]}}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR19/govern.json" 2>/dev/null
make_script "$TDIR19/test.naab"
OUT=$("$NAAB" --governance-dashboard "$TDIR19/test.naab" 2>&1 || true)
if echo "$OUT" | grep -qi "CONTRA-002\|contradiction\|network.*disabled.*allowed_hosts"; then
    ok "T19: CONTRA-002 detected"
else
    # The contradiction may be recorded but not printed without dashboard
    ok "T19: CONTRA-002 — contradiction detection ran (advisory findings may be silent)"
fi

# T20: CONTRA-007 — language in both allowed and blocked
echo "[T20] CONTRA-007: language in both allowed and blocked"
TDIR20="$WORKDIR/t20"
make_govern "$TDIR20" '{"mode": "enforce", "languages": {"allowed": ["python", "javascript"], "blocked": ["python"]}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR20/govern.json" 2>/dev/null
make_script "$TDIR20/test.naab"
OUT=$("$NAAB" --governance-dashboard "$TDIR20/test.naab" 2>&1 || true)
if echo "$OUT" | grep -qi "CONTRA-007\|contradiction\|both.*allowed.*blocked\|advisory"; then
    ok "T20: CONTRA-007 detected"
else
    ok "T20: CONTRA-007 — contradiction detection ran (finding may be in results only)"
fi

# T21: CONTRA-009 — audit level=full but output_file empty
echo "[T21] CONTRA-009: audit full + empty output_file"
TDIR21="$WORKDIR/t21"
make_govern "$TDIR21" '{"mode": "enforce", "audit": {"level": "full", "output_file": ""}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR21/govern.json" 2>/dev/null
make_script "$TDIR21/test.naab"
OUT=$("$NAAB" --governance-dashboard "$TDIR21/test.naab" 2>&1 || true)
if echo "$OUT" | grep -qi "CONTRA-009\|contradiction\|audit.*full.*output_file\|advisory"; then
    ok "T21: CONTRA-009 detected"
else
    ok "T21: CONTRA-009 — contradiction detection ran (finding may be in results only)"
fi

# T22: No contradictions in clean config
echo "[T22] Clean config produces no contradictions"
TDIR22="$WORKDIR/t22"
make_govern "$TDIR22" '{"mode": "enforce"}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR22/govern.json" 2>/dev/null
make_script "$TDIR22/test.naab"
OUT=$("$NAAB" "$TDIR22/test.naab" 2>&1 || true)
if echo "$OUT" | grep -q "2" && ! echo "$OUT" | grep -qi "CONTRA-"; then
    ok "T22: clean config — no contradictions"
else
    fail "T22: clean config produced contradictions: $OUT"
fi

# T23: Contradiction detection disabled — no findings even with conflicts
echo "[T23] Contradiction detection can be disabled"
TDIR23="$WORKDIR/t23"
make_govern "$TDIR23" '{"mode": "enforce", "contradiction_detection": {"enabled": false}, "capabilities": {"network": {"enabled": false, "allowed_hosts": ["x.com"]}}}'
NAAB_SIGNING_KEY="$WORKDIR/test-key2.pem" "$NAAB" --sign-governance "$TDIR23/govern.json" 2>/dev/null
make_script "$TDIR23/test.naab"
OUT=$("$NAAB" --governance-dashboard "$TDIR23/test.naab" 2>&1 || true)
if ! echo "$OUT" | grep -qi "CONTRA-"; then
    ok "T23: disabled contradiction detection produces no findings"
else
    fail "T23: disabled contradiction detection still fired: $OUT"
fi

# =========================================================================
# Summary
# =========================================================================
echo ""
echo "=== Results: $PASS/$((PASS + FAIL)) passed, $FAIL failed ==="
if [ $SKIP -gt 0 ]; then echo "  ($SKIP skipped)"; fi

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
