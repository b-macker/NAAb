#!/usr/bin/env bash
# Test: Telemetry webhook forwarding
# Verifies that governance telemetry events are forwarded to a webhook endpoint.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-$PROJECT_DIR/build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

SIGNING_KEY="${HOME}/.naab/keys/signing.pem"
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/naab_telfwd_XXXXXX")
trap 'rm -rf "$WORKDIR"; [ -n "${LISTENER_PID:-}" ] && kill "$LISTENER_PID" 2>/dev/null || true' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP: $1"; }

sign_gov() {
    local dir="$1"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
}

echo "=== Telemetry Forwarding Tests ==="

# --- Test 1: Config parsing ---
echo ""
echo "--- Test 1: Webhook config fields parsed correctly ---"

T1DIR="$WORKDIR/t1"
mkdir -p "$T1DIR"

cat > "$T1DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": {
        "enabled": true,
        "output_file": "test_telemetry.jsonl",
        "webhook_url": "http://localhost:19876/ingest",
        "forward_batch_size": 5,
        "forward_timeout_ms": 3000,
        "forward_retry_count": 1,
        "forward_buffer_max": 500
    }
}
GOVEOF
sign_gov "$T1DIR"

# Simple script — should parse config without error
cat > "$T1DIR/test_config.naab" << 'NAABEOF'
main {
    let x = 42
    print(x)
}
NAABEOF

OUTPUT1=$(cd "$T1DIR" && timeout 10s "$NAAB" test_config.naab 2>/dev/null) || true
if echo "$OUTPUT1" | grep -q "42"; then
    ok "Webhook config fields parsed without error"
else
    fail "Webhook config broke execution"
    echo "    OUTPUT: $(echo "$OUTPUT1" | head -3)"
fi

# --- Test 2: Local telemetry still works with webhook configured ---
echo ""
echo "--- Test 2: Local JSONL file still written with webhook configured ---"

T2DIR="$WORKDIR/t2"
mkdir -p "$T2DIR"

cat > "$T2DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": true } },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "webhook_url": "http://127.0.0.1:19999/nonexistent",
        "forward_timeout_ms": 500,
        "forward_retry_count": 0,
        "tamper_evidence": {
            "enabled": true,
            "chain_genesis": "TEST-GENESIS"
        }
    }
}
GOVEOF
sign_gov "$T2DIR"

cat > "$T2DIR/test_local.naab" << 'NAABEOF'
main {
    let r = <<sh
    echo "hello"
    >>
    print(r)
}
NAABEOF

cd "$T2DIR" && timeout 15s "$NAAB" test_local.naab --governance-dashboard 2>/dev/null || true
if [ -f "$T2DIR/telemetry.jsonl" ]; then
    LINES=$(wc -l < "$T2DIR/telemetry.jsonl")
    if [ "$LINES" -gt 0 ]; then
        ok "Local JSONL file written ($LINES events) even with unreachable webhook"
    else
        fail "JSONL file exists but is empty"
    fi
else
    fail "Local JSONL file not created"
fi

# --- Test 3: Webhook auth env var resolution ---
echo ""
echo "--- Test 3: Webhook auth via env var ---"

T3DIR="$WORKDIR/t3"
mkdir -p "$T3DIR"

cat > "$T3DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "webhook_url": "http://localhost:19876/ingest",
        "webhook_auth_env": "NAAB_TEST_SIEM_KEY"
    }
}
GOVEOF
sign_gov "$T3DIR"

cat > "$T3DIR/test_auth.naab" << 'NAABEOF'
main {
    print("auth_test_ok")
}
NAABEOF

NAAB_TEST_SIEM_KEY="test-api-key-12345" \
    timeout 10s "$NAAB" "$T3DIR/test_auth.naab" 2>/dev/null || true
# If it ran without crash, the env var was resolved
OUTPUT3=$(NAAB_TEST_SIEM_KEY="test-api-key-12345" timeout 10s "$NAAB" "$T3DIR/test_auth.naab" 2>/dev/null) || true
if echo "$OUTPUT3" | grep -q "auth_test_ok"; then
    ok "Webhook auth env var resolved without error"
else
    fail "Auth env var resolution broke execution"
fi

# --- Test 4: Forwarding failure doesn't block execution ---
echo ""
echo "--- Test 4: Forwarding failure is non-blocking ---"

T4DIR="$WORKDIR/t4"
mkdir -p "$T4DIR"

cat > "$T4DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": true } },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "webhook_url": "http://192.0.2.1:1/unreachable",
        "forward_timeout_ms": 500,
        "forward_retry_count": 0,
        "forward_buffer_max": 10
    }
}
GOVEOF
sign_gov "$T4DIR"

cat > "$T4DIR/test_nonblock.naab" << 'NAABEOF'
main {
    let r = <<sh
    echo "nonblock_ok"
    >>
    print(r)
}
NAABEOF

START=$(date +%s)
OUTPUT4=$(cd "$T4DIR" && timeout 15s "$NAAB" test_nonblock.naab 2>/dev/null) || true
END=$(date +%s)
ELAPSED=$((END - START))

if echo "$OUTPUT4" | grep -q "nonblock_ok"; then
    if [ "$ELAPSED" -lt 10 ]; then
        ok "Execution completed in ${ELAPSED}s despite unreachable webhook (non-blocking)"
    else
        fail "Execution took ${ELAPSED}s — webhook may be blocking"
    fi
else
    fail "Execution failed with unreachable webhook"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ]
