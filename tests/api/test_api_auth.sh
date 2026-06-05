#!/usr/bin/env bash
# test_api_auth.sh — Test REST API multi-key authentication with scoped permissions
#
# Phase 3: Enterprise readiness — multi-key auth, scoped access, 401/403 enforcement.
# Starts naab-lang API server with govern.json multi-key config, runs auth tests.

PASS=0
FAIL=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$LANG_DIR/build/naab-lang"
PORT=18931  # Unlikely to conflict
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
WORKDIR="$TMPDIR/naab_api_auth_$$"

if [ ! -x "$NAAB" ]; then
    echo "SKIP: naab-lang not built"
    exit 0
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "SKIP: curl not available"
    exit 0
fi

mkdir -p "$WORKDIR"

# --- Helper: sign govern.json if trusted keys exist ---
SIGNING_KEY="${HOME}/.naab/keys/signing.pem"
sign_gov() {
    local dir="$1"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
}

# --- Create govern.json with multi-key config ---
cat > "$WORKDIR/govern.json" << 'GOVEOF'
{
  "mode": "off",
  "api": {
    "keys": [
      { "key": "full-access-key-001", "name": "Admin", "scopes": [] },
      { "key": "exec-check-key-002", "name": "CI Pipeline", "scopes": ["execute", "check"] },
      { "key": "readonly-key-003", "name": "Dashboard", "scopes": ["blocks", "stats"] }
    ],
    "rate_limit": 0,
    "timeout": 10
  }
}
GOVEOF
sign_gov "$WORKDIR"

# Start API server from workdir (picks up govern.json)
cd "$WORKDIR"
"$NAAB" api "$PORT" >"$WORKDIR/server.log" 2>&1 &
API_PID=$!

cleanup() {
    kill "$API_PID" 2>/dev/null
    wait "$API_PID" 2>/dev/null
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

# Wait for server to start
for i in $(seq 1 50); do
    if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

if ! curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
    echo "SKIP: API server failed to start"
    cat "$WORKDIR/server.log" 2>/dev/null | tail -10
    exit 0
fi

# --- Test helper ---
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

echo "=== REST API Multi-Key Auth Tests ==="

# ---- T1: /health always public (no auth required) ----
check "T1: /health without auth" "200" "healthy" \
    "http://127.0.0.1:$PORT/health"

# ---- T2: No key → 401 on protected endpoints ----
check "T2: /execute without key → 401" "401" "Unauthorized" \
    -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
    -H "Content-Type: application/json" \
    -d '{"code":"main { print(1) }"}'

check "T3: /check without key → 401" "401" "Unauthorized" \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Content-Type: application/json" \
    -d '{"code":"main { print(1) }","language":"naab"}'

check "T4: /stats without key → 401" "401" "Unauthorized" \
    "http://127.0.0.1:$PORT/api/v1/stats"

# ---- T5: Invalid key → 401 ----
check "T5: wrong key → 401" "401" "Unauthorized" \
    -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer wrong-key-999" \
    -d '{"code":"main { print(1) }"}'

# ---- T6-T8: Full-access key (empty scopes) can reach all endpoints ----
check "T6: admin key → /execute 200" "200" "" \
    -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer full-access-key-001" \
    -d '{"code":"main { print(1) }"}'

check "T7: admin key → /check 200" "200" "" \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer full-access-key-001" \
    -d '{"code":"main { print(1) }","language":"naab"}'

check "T8: admin key → /stats 200" "200" "" \
    -H "Authorization: Bearer full-access-key-001" \
    "http://127.0.0.1:$PORT/api/v1/stats"

# ---- T9-T11: CI key (execute+check scopes) ----
check "T9: CI key → /execute 200" "200" "" \
    -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer exec-check-key-002" \
    -d '{"code":"main { print(1) }"}'

check "T10: CI key → /check 200" "200" "" \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer exec-check-key-002" \
    -d '{"code":"main { print(1) }","language":"naab"}'

check "T11: CI key → /stats 403 (no stats scope)" "403" "Forbidden" \
    -H "Authorization: Bearer exec-check-key-002" \
    "http://127.0.0.1:$PORT/api/v1/stats"

# ---- T12-T14: Dashboard key (blocks+stats scopes) ----
check "T12: dashboard key → /stats 200" "200" "" \
    -H "Authorization: Bearer readonly-key-003" \
    "http://127.0.0.1:$PORT/api/v1/stats"

check "T13: dashboard key → /execute 403 (no execute scope)" "403" "Forbidden" \
    -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer readonly-key-003" \
    -d '{"code":"main { print(1) }"}'

check "T14: dashboard key → /check 403 (no check scope)" "403" "Forbidden" \
    -X POST "http://127.0.0.1:$PORT/api/v1/check" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer readonly-key-003" \
    -d '{"code":"main { print(1) }","language":"naab"}'

# ---- T15: X-API-Key header works (alternate auth) ----
check "T15: X-API-Key header → /execute 200" "200" "" \
    -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
    -H "Content-Type: application/json" \
    -H "X-API-Key: full-access-key-001" \
    -d '{"code":"main { print(1) }"}'

# ---- T16: 403 response includes required_scope field ----
check "T16: 403 includes required_scope" "403" "required_scope" \
    -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer readonly-key-003" \
    -d '{"code":"main { print(1) }"}'

echo ""
echo "Results: $PASS passed, $FAIL failed out of $((PASS + FAIL))"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
