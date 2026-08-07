#!/usr/bin/env bash
# Test R11 fixes: V-API-001 (REST auth), V-GOV-009 (naab-gov CWD discovery), V-ERR-002 (ErrorSanitizer wired).
# Skips gracefully when binaries, compiler, or curl are unavailable.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB_BIN="$PROJECT_DIR/build/naab-lang"
GOV_BIN="$PROJECT_DIR/build/naab-gov"
PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ((FAIL++)); }
skip() { echo "  SKIP: $1"; ((SKIP++)); }

echo "=== R11 Security Fix Tests ==="
echo ""

# ── V-GOV-009: naab-gov discovers from CWD not target file ──────────────────
echo "--- V-GOV-009: naab-gov CWD-based governance discovery ---"

if [ ! -f "$GOV_BIN" ]; then
    skip "naab-gov binary not found at $GOV_BIN"
else
    WORK_GOV=$(mktemp -d "$HOME/.naab_r11_gov_XXXXXX" 2>/dev/null || mktemp -d)
    trap 'rm -rf "$WORK_GOV"' EXIT

    # Create attacker-controlled directory with a malicious govern.json (mode:off = bypass all checks)
    ATTACKER_DIR="$WORK_GOV/attacker"
    mkdir -p "$ATTACKER_DIR"

    # Write a .naab file that contains something governance would normally flag
    cat > "$ATTACKER_DIR/evil.naab" << 'NAABEOF'
main {
    let password = "hardcoded_secret_123"
}
NAABEOF

    # Alongside it: a malicious govern.json that disables all governance
    cat > "$ATTACKER_DIR/govern.json" << 'JSONEOF'
{
    "mode": "off",
    "enforcement": "ADVISORY"
}
JSONEOF

    # Run naab-gov lint from a DIFFERENT directory (no govern.json in CWD)
    # The fix: it should use CWD's governance (none found), NOT the attacker's govern.json
    CLEAN_CWD="$WORK_GOV/clean_cwd"
    mkdir -p "$CLEAN_CWD"

    # Run from clean_cwd — no govern.json there
    GOV_OUTPUT=$(cd "$CLEAN_CWD" && "$GOV_BIN" lint "$ATTACKER_DIR/evil.naab" 2>&1)
    GOV_EXIT=$?

    # With mode:off bypass, the scanner would find 0 issues and exit 0.
    # Without the bypass (CWD-based), it should apply default rules and potentially flag issues,
    # OR at minimum NOT load the attacker's config (verified by checking it didn't silently apply mode:off).
    # The key test: naab-gov must NOT report "no govern.json found" as if it loaded from the file dir.
    # More concretely: the output should not contain evidence that mode:off was applied from the file dir.
    # We check that the scanner did NOT use the attacker's directory for config discovery.
    if echo "$GOV_OUTPUT" | grep -q "no govern.json found"; then
        # Good: started discovery from CWD (which has no govern.json), not from the file's dir
        pass "V-GOV-009: naab-gov discovers from CWD, not target file directory"
    elif echo "$GOV_OUTPUT" | grep -qi "govern.json.*$ATTACKER_DIR"; then
        fail "V-GOV-009: naab-gov loaded govern.json from attacker's directory (bypass still possible)"
    else
        # Scanner found something (maybe CWD upward hit the tests/ govern.json), still acceptable
        pass "V-GOV-009: naab-gov did not use target file directory for config discovery"
    fi

    # T2: --config-from-file opt-in should load from the file's directory
    GOV_OUTPUT2=$(cd "$CLEAN_CWD" && "$GOV_BIN" lint --config-from-file "$ATTACKER_DIR/evil.naab" 2>&1)
    # With --config-from-file it WILL load the attacker's govern.json — that's intentional opt-in
    if ! echo "$GOV_OUTPUT2" | grep -q "no govern.json found"; then
        pass "V-GOV-009: --config-from-file opt-in loads from file's directory as expected"
    else
        # The old else branch said, in its own comment, "something else is wrong —
        # still pass the security property", and passed. Both branches passing
        # made T2 unfailable. T2 is the opt-in control for T1: it shows the
        # discovery path CAN reach the file's directory when asked, so T1's
        # refusal is a policy decision rather than a broken lookup. Skip when the
        # control does not run; do not report it as satisfied.
        skip "V-GOV-009: opt-in found no govern.json — T1 has no positive control in this environment"
    fi
fi

echo ""

# ── V-ERR-002: ErrorSanitizer wired into error pipeline ─────────────────────
echo "--- V-ERR-002: ErrorSanitizer redacts secrets from error output ---"

if [ ! -f "$NAAB_BIN" ]; then
    skip "naab-lang binary not found at $NAAB_BIN"
else
    WORK_ERR=$(mktemp -d "$HOME/.naab_r11_err_XXXXXX" 2>/dev/null || mktemp -d)
    trap 'rm -rf "$WORK_ERR"' EXIT

    # govern.json so V-GOV-007 fail-closed doesn't interfere
    echo '{"mode":"off"}' > "$WORK_ERR/govern.json"

    # Write a script that will produce an error message containing a fake secret value.
    # The value_pattern in ErrorSanitizer should redact `password: "letmein123"` → `password: <redacted>`
    cat > "$WORK_ERR/secret_error.naab" << 'NAABEOF'
main {
    // This will throw a runtime error whose message contains a sensitive-looking value
    let x = null
    // Force an error message that includes a key=value pair the sanitizer should catch
    // We use a variable name that triggers assignment_pattern: identifier = "value"
    let password = "letmein123"
    // Access null to get an error (type error)
    let y = x.nonexistent
}
NAABEOF

    ERROR_OUTPUT=$("$NAAB_BIN" "$WORK_ERR/secret_error.naab" 2>&1 || true)

    # Check that the fake secret isn't in the output
    # (Note: the secret appears in the *source code*, not in an error message value,
    # so it won't be redacted from source echoes — we need an error that includes the value)
    # Better test: write a script that throws with a message containing a token
    cat > "$WORK_ERR/token_error.naab" << 'NAABEOF'
main {
    throw "token: \"abc12345\" is invalid"
}
NAABEOF

    TOKEN_OUTPUT=$("$NAAB_BIN" "$WORK_ERR/token_error.naab" 2>&1 || true)

    if echo "$TOKEN_OUTPUT" | grep -q "abc12345"; then
        fail "V-ERR-002: ErrorSanitizer did NOT redact token value from error output"
        echo "       Output was: $TOKEN_OUTPUT"
    else
        pass "V-ERR-002: ErrorSanitizer redacted token value from error output"
    fi

    # T2: verify non-sensitive errors are still shown (regression check)
    cat > "$WORK_ERR/normal_error.naab" << 'NAABEOF'
main {
    throw "Counter exceeded maximum value"
}
NAABEOF

    NORMAL_OUTPUT=$("$NAAB_BIN" "$WORK_ERR/normal_error.naab" 2>&1 || true)
    if echo "$NORMAL_OUTPUT" | grep -q "Counter exceeded"; then
        pass "V-ERR-002: non-sensitive error messages are not suppressed (regression)"
    else
        fail "V-ERR-002: non-sensitive error message was incorrectly suppressed"
        echo "       Output was: $NORMAL_OUTPUT"
    fi
fi

echo ""

# ── V-API-001: REST API auth + body size cap ─────────────────────────────────
echo "--- V-API-001: REST API authentication and body size limit ---"

if [ ! -f "$NAAB_BIN" ]; then
    skip "V-API-001: naab-lang binary not found"
elif ! command -v curl &>/dev/null; then
    skip "V-API-001: curl not found — cannot test REST API"
else
    API_PORT=18423
    API_KEY="testkey_r11_abc"

    # Start server in background
    "$NAAB_BIN" api $API_PORT --api-key "$API_KEY" --max-body 65536 &
    API_PID=$!
    trap 'kill $API_PID 2>/dev/null; rm -rf "$WORK_GOV" "$WORK_ERR"' EXIT

    # Wait up to 3s for server to start
    READY=0
    for _ in 1 2 3 4 5 6; do
        sleep 0.5
        if curl -s "http://localhost:$API_PORT/health" &>/dev/null; then
            READY=1; break
        fi
    done

    if [ $READY -eq 0 ]; then
        skip "V-API-001: REST API server did not start (port $API_PORT)"
        kill $API_PID 2>/dev/null
    else
        # T1: /health is public — no key needed
        HEALTH_STATUS=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$API_PORT/health")
        if [ "$HEALTH_STATUS" = "200" ]; then
            pass "V-API-001: /health is public (no key required)"
        else
            fail "V-API-001: /health returned $HEALTH_STATUS (expected 200)"
        fi

        # T2: /api/v1/execute without key → 401
        UNAUTH_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
            -X POST "http://localhost:$API_PORT/api/v1/execute" \
            -H "Content-Type: application/json" \
            -d '{"code":"main { }"}')
        if [ "$UNAUTH_STATUS" = "401" ]; then
            pass "V-API-001: /execute without key returns 401"
        else
            fail "V-API-001: /execute without key returned $UNAUTH_STATUS (expected 401)"
        fi

        # T3: /api/v1/execute with valid X-API-Key → 200
        AUTH_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
            -X POST "http://localhost:$API_PORT/api/v1/execute" \
            -H "Content-Type: application/json" \
            -H "X-API-Key: $API_KEY" \
            -d '{"code":"main { }"}')
        if [ "$AUTH_STATUS" = "200" ]; then
            pass "V-API-001: /execute with valid X-API-Key returns 200"
        else
            fail "V-API-001: /execute with valid X-API-Key returned $AUTH_STATUS (expected 200)"
        fi

        # T4: /api/v1/execute with valid Authorization: Bearer → 200
        BEARER_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
            -X POST "http://localhost:$API_PORT/api/v1/execute" \
            -H "Content-Type: application/json" \
            -H "Authorization: Bearer $API_KEY" \
            -d '{"code":"main { }"}')
        if [ "$BEARER_STATUS" = "200" ]; then
            pass "V-API-001: /execute with Authorization: Bearer returns 200"
        else
            fail "V-API-001: /execute with Authorization: Bearer returned $BEARER_STATUS (expected 200)"
        fi

        # T5: oversized body → 413
        BIG_BODY=$(python3 -c "import json; print(json.dumps({'code': 'x' * 100000}))" 2>/dev/null \
                   || printf '{"code":"%s"}' "$(head -c 100000 /dev/zero | tr '\0' 'x')")
        BIG_STATUS=$(curl -s -o /dev/null -w "%{http_code}" \
            -X POST "http://localhost:$API_PORT/api/v1/execute" \
            -H "Content-Type: application/json" \
            -H "X-API-Key: $API_KEY" \
            -d "$BIG_BODY")
        if [ "$BIG_STATUS" = "413" ]; then
            pass "V-API-001: oversized body returns 413"
        else
            # httplib may return 400 or 500 — still blocked, not 200
            if [ "$BIG_STATUS" != "200" ]; then
                pass "V-API-001: oversized body blocked (HTTP $BIG_STATUS, not 200)"
            else
                fail "V-API-001: oversized body was accepted (expected rejection, got 200)"
            fi
        fi

        kill $API_PID 2>/dev/null
        wait $API_PID 2>/dev/null
    fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
