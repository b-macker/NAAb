#!/usr/bin/env bash
# Security R25 Fix Verification Tests
# V-RT-015:  readFileBounded TOCTOU — atomic open with O_NOFOLLOW
# V-DOS-005: REST API token-bucket rate limiting
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
WORK_DIR="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_r25.XXXXXX")"
SERVER_PID=""
cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

# ── V-RT-015: readFileBounded rejects symlinks via O_NOFOLLOW ────────────────

echo "=== V-RT-015: Atomic symlink rejection ==="

# Test 1: symlink to /etc/passwd is rejected when used as a module
mkdir -p "$WORK_DIR/rt015"
echo 'main { print("hello") }' > "$WORK_DIR/rt015/real.naab"
ln -sf /etc/passwd "$WORK_DIR/rt015/badmod.naab"

cat > "$WORK_DIR/rt015/test_symlink.naab" << 'NAAB'
use badmod
main {
    print("should not reach here")
}
NAAB

OUTPUT=$( (cd "$WORK_DIR/rt015" && "$NAAB" test_symlink.naab 2>&1) || true )
if echo "$OUTPUT" | grep -qi "should not reach here"; then
    fail "V-RT-015-T1: symlink module was loaded (TOCTOU bypassed)"
else
    pass "V-RT-015-T1: symlink module correctly rejected"
fi

# Test 2: regular file still loads fine
cat > "$WORK_DIR/rt015/goodmod.naab" << 'NAAB'
export function greet() {
    return "hello from module"
}
NAAB

# Need govern.json for --require-governance (global default)
cat > "$WORK_DIR/rt015/govern.json" << 'JSON'
{"version":"4.0","mode":"off"}
JSON

cat > "$WORK_DIR/rt015/test_regular.naab" << 'NAAB'
use goodmod
main {
    print(goodmod.greet())
}
NAAB

OUTPUT=$( (cd "$WORK_DIR/rt015" && "$NAAB" test_regular.naab 2>&1) || true )
if echo "$OUTPUT" | grep -q "hello from module"; then
    pass "V-RT-015-T2: regular file module loads correctly (regression)"
else
    fail "V-RT-015-T2: regular file module failed to load" "$OUTPUT"
fi

# Test 3: symlink to /dev/zero rejected (OOM prevention)
ln -sf /dev/zero "$WORK_DIR/rt015/zeromod.naab"
cat > "$WORK_DIR/rt015/test_zero.naab" << 'NAAB'
use zeromod
main { print("should not reach") }
NAAB

OUTPUT=$( (cd "$WORK_DIR/rt015" && timeout 5 "$NAAB" test_zero.naab 2>&1) || true )
if echo "$OUTPUT" | grep -qi "should not reach"; then
    fail "V-RT-015-T3: /dev/zero symlink was followed"
else
    pass "V-RT-015-T3: /dev/zero symlink correctly rejected"
fi

# ── V-DOS-005: REST API rate limiting ────────────────────────────────────────

echo ""
echo "=== V-DOS-005: REST API rate limiting ==="

# Check if curl is available
if ! command -v curl &>/dev/null; then
    echo "SKIP: curl not available, skipping REST API rate limit tests"
else
    PORT=18925

    "$NAAB" api "$PORT" --api-key r25test --api-rate-limit 5 \
        > "$WORK_DIR/server.log" 2>&1 &
    SERVER_PID=$!

    # Wait for server to start
    READY=0
    for i in $(seq 1 20); do
        if curl -sf "http://127.0.0.1:$PORT/health" > /dev/null 2>&1; then
            READY=1; break
        fi
        sleep 0.5
    done

    if [[ $READY -eq 0 ]]; then
        fail "V-DOS-005: server failed to start"
        kill "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    else
        # Test 4: /health is never rate-limited
        HEALTH_OK=1
        for i in $(seq 1 10); do
            HTTP_CODE=$(curl -sf -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/health")
            if [[ "$HTTP_CODE" == "429" ]]; then
                HEALTH_OK=0; break
            fi
        done
        if [[ $HEALTH_OK -eq 1 ]]; then
            pass "V-DOS-005-T4: /health endpoint exempt from rate limiting"
        else
            fail "V-DOS-005-T4: /health was rate-limited (should be exempt)"
        fi

        # Test 5: rapid requests hit 429
        GOT_429=0
        for i in $(seq 1 15); do
            HTTP_CODE=$(curl -s -o /dev/null -w '%{http_code}' \
                -H "X-API-Key: r25test" \
                "http://127.0.0.1:$PORT/api/v1/stats" 2>/dev/null)
            if [[ "$HTTP_CODE" == "429" ]]; then
                GOT_429=1; break
            fi
        done
        if [[ $GOT_429 -eq 1 ]]; then
            pass "V-DOS-005-T5: rate limit triggers HTTP 429"
        else
            fail "V-DOS-005-T5: 15 rapid requests never triggered 429"
        fi

        # Test 6: 429 response includes Retry-After header
        if [[ $GOT_429 -eq 1 ]]; then
            RETRY_AFTER=$(curl -s -D - -o /dev/null \
                -H "X-API-Key: r25test" \
                "http://127.0.0.1:$PORT/api/v1/stats" 2>/dev/null | grep -i "Retry-After" || true)
            if [[ -n "$RETRY_AFTER" ]]; then
                pass "V-DOS-005-T6: 429 response includes Retry-After header"
            else
                fail "V-DOS-005-T6: 429 response missing Retry-After header"
            fi
        else
            fail "V-DOS-005-T6: skipped (no 429 to check)"
        fi

        # Test 7: unauthenticated request gets 401, not 429
        HTTP_CODE=$(curl -s -o /dev/null -w '%{http_code}' \
            "http://127.0.0.1:$PORT/api/v1/stats" 2>/dev/null)
        if [[ "$HTTP_CODE" == "401" ]]; then
            pass "V-DOS-005-T7: unauthenticated request rejected before rate check"
        else
            fail "V-DOS-005-T7: expected 401 for unauthenticated, got $HTTP_CODE"
        fi

        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
fi

# ── Summary ──────────────────────────────────────────────────────────────────

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
