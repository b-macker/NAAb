#!/usr/bin/env bash
# test_rest_governance.sh — Test POST /api/v1/check REST endpoint
#
# Starts naab-lang API server in background, runs tests, then kills it.
# Requires: naab-lang built with REST API support.

PASS=0
FAIL=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$LANG_DIR/build/naab-lang"
PORT=18923  # Unlikely to conflict
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"

if [ ! -x "$NAAB" ]; then
    echo "SKIP: naab-lang not built"
    exit 0
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "SKIP: curl not available"
    exit 0
fi

# Start API server in background (naab-lang api <port> --api-key <key>)
"$NAAB" api "$PORT" --api-key "test-key-123" >"$TMPDIR/naab_api.log" 2>&1 &
API_PID=$!

cleanup() {
    kill "$API_PID" 2>/dev/null
    wait "$API_PID" 2>/dev/null
    rm -f "$TMPDIR/naab_api.log"
}
trap cleanup EXIT

# Wait for server to start (up to 5 seconds)
for i in $(seq 1 50); do
    if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

# Verify server is running
if ! curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
    echo "SKIP: API server failed to start"
    cat "$TMPDIR/naab_api.log" 2>/dev/null | tail -5
    exit 0
fi

AUTH="-H 'Authorization: Bearer test-key-123'"

check() {
    local desc="$1" expected_status="$2" expected_pattern="$3"
    shift 3
    local output http_code body
    output=$(curl -s -w "\n%{http_code}" "$@" 2>&1)
    http_code=$(echo "$output" | tail -1)
    body=$(echo "$output" | sed '$d')

    if [ "$http_code" != "$expected_status" ]; then
        echo "  FAIL: $desc (HTTP $http_code, expected $expected_status)"
        echo "        Body: $(echo "$body" | head -2)"
        FAIL=$((FAIL + 1))
        return
    fi
    if [ -n "$expected_pattern" ] && ! echo "$body" | grep -q "$expected_pattern"; then
        echo "  FAIL: $desc (pattern '$expected_pattern' not found)"
        echo "        Body: $(echo "$body" | head -3)"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: $desc"
    PASS=$((PASS + 1))
}

echo "=== REST API /api/v1/check tests ==="

# T1: Safe code passes
check "safe code returns not blocked" "200" '"blocked": false' \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Authorization: Bearer test-key-123" \
    -H "Content-Type: application/json" \
    -d '{"code": "x = 42", "language": "python"}'

# T2: Dangerous code with inline config
check "dangerous code with enforce config" "200" '"blocked": true' \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Authorization: Bearer test-key-123" \
    -H "Content-Type: application/json" \
    -d '{
        "code": "import os; os.system(\"rm -rf /\")",
        "language": "python",
        "config": {
            "version": "3.0",
            "mode": "enforce",
            "restrictions": {"dangerous_calls": {"level": "hard"}}
        }
    }'

# T3: Missing required fields
check "missing code field" "400" '"error"' \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Authorization: Bearer test-key-123" \
    -H "Content-Type: application/json" \
    -d '{"language": "python"}'

# T4: Missing language field
check "missing language field" "400" '"error"' \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Authorization: Bearer test-key-123" \
    -H "Content-Type: application/json" \
    -d '{"code": "x = 1"}'

# T5: Invalid JSON body (server may return 400 or reject at transport level)
check "invalid JSON body" "400" '"error"' \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Authorization: Bearer test-key-123" \
    -H "Content-Type: application/json" \
    -d '{"invalid": true'

# T6: Response has violation_count
check "response has violation_count" "200" '"violation_count"' \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Authorization: Bearer test-key-123" \
    -H "Content-Type: application/json" \
    -d '{"code": "x = 1", "language": "python"}'

# T7: Violations include rule names
check "violations include rule names" "200" '"rule"' \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Authorization: Bearer test-key-123" \
    -H "Content-Type: application/json" \
    -d '{
        "code": "eval(input())",
        "language": "python",
        "config": {
            "version": "3.0",
            "mode": "enforce",
            "restrictions": {"dangerous_calls": {"level": "hard"}}
        }
    }'

# T8: JavaScript works too
check "javascript eval detected" "200" '"blocked": true' \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Authorization: Bearer test-key-123" \
    -H "Content-Type: application/json" \
    -d '{
        "code": "eval(userInput)",
        "language": "javascript",
        "config": {
            "version": "3.0",
            "mode": "enforce",
            "restrictions": {"code_injection": {"level": "hard"}}
        }
    }'

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] || exit 1
