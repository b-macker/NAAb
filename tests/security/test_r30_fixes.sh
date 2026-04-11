#!/usr/bin/env bash
# Security R30 Fix Verification Tests
# V-SSRF-001: HTTP redirect protocol restriction
# V-DOS-010:  HTTP response size cap
# V-DOS-009:  JSON depth limit + cycle detection
# V-API-005:  CSPRNG for crypto tokens
# V-API-003:  compare_digest length-independent timing
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
WORK_DIR="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_r30.XXXXXX")"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

cat > "$WORK_DIR/govern.json" << 'JSON'
{ "version": "4.0", "mode": "off" }
JSON

# ── V-SSRF-001: Source verification — protocol restriction ─────────────────

echo "=== V-SSRF-001: HTTP redirect protocol restriction ==="

SRC_HTTP="$SCRIPT_DIR/../../src/stdlib/http_impl.cpp"

# Test 1: CURLOPT_PROTOCOLS_STR is set
if grep -q 'CURLOPT_PROTOCOLS_STR.*"http,https"' "$SRC_HTTP"; then
    pass "V-SSRF-001 T1: CURLOPT_PROTOCOLS_STR restricts to http,https"
else
    fail "V-SSRF-001 T1: CURLOPT_PROTOCOLS_STR not found"
fi

# Test 2: CURLOPT_REDIR_PROTOCOLS_STR is set
if grep -q 'CURLOPT_REDIR_PROTOCOLS_STR.*"http,https"' "$SRC_HTTP"; then
    pass "V-SSRF-001 T2: CURLOPT_REDIR_PROTOCOLS_STR restricts to http,https"
else
    fail "V-SSRF-001 T2: CURLOPT_REDIR_PROTOCOLS_STR not found"
fi

# ── V-DOS-010: Source verification — bounded response ──────────────────────

echo "=== V-DOS-010: HTTP response size cap ==="

# Test 3: BoundedResponseSink struct exists
if grep -q 'BoundedResponseSink' "$SRC_HTTP"; then
    pass "V-DOS-010 T3: BoundedResponseSink implemented"
else
    fail "V-DOS-010 T3: BoundedResponseSink not found"
fi

# Test 4: MAX_HTTP_RESPONSE_BYTES defined
if grep -q 'MAX_HTTP_RESPONSE_BYTES' "$SRC_HTTP"; then
    pass "V-DOS-010 T4: MAX_HTTP_RESPONSE_BYTES defined"
else
    fail "V-DOS-010 T4: MAX_HTTP_RESPONSE_BYTES not found"
fi

# ── V-DOS-009: JSON depth + cycle detection ────────────────────────────────

echo "=== V-DOS-009: JSON depth + cycle detection ==="

SRC_JSON="$SCRIPT_DIR/../../src/stdlib/json_impl.cpp"

# Test 5: valueToJson has depth parameter
if grep -q 'valueToJson.*int depth' "$SRC_JSON"; then
    pass "V-DOS-009 T5: valueToJson has depth parameter"
else
    fail "V-DOS-009 T5: valueToJson missing depth parameter"
fi

# Test 6: Cycle detection with visited set
if grep -q 'circular reference detected' "$SRC_JSON"; then
    pass "V-DOS-009 T6: Circular reference detection implemented"
else
    fail "V-DOS-009 T6: Circular reference detection not found"
fi

# Test 7: json.parse() depth guard
if grep -q 'nesting depth exceeded' "$SRC_JSON"; then
    pass "V-DOS-009 T7: json.parse() depth guard implemented"
else
    fail "V-DOS-009 T7: json.parse() depth guard not found"
fi

# Test 8: Runtime test — deeply nested JSON parse rejection
cat > "$WORK_DIR/json_depth.naab" << 'NAAB'
main {
    // Build a deeply nested JSON string: 200 levels of [
    let s = ""
    let i = 0
    while (i < 200) {
        s = s + "["
        i = i + 1
    }
    i = 0
    while (i < 200) {
        s = s + "]"
        i = i + 1
    }
    try {
        let parsed = json.parse(s)
        print("ERROR: should have thrown")
    } catch (e) {
        print("CAUGHT: " + e)
    }
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/json_depth.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "depth\|nesting"; then
    pass "V-DOS-009 T8: Deeply nested JSON parse rejected at runtime"
else
    fail "V-DOS-009 T8: Deeply nested JSON NOT rejected" "$OUTPUT"
fi

# ── V-API-005: CSPRNG verification ─────────────────────────────────────────

echo "=== V-API-005: CSPRNG for crypto tokens ==="

SRC_CRYPTO="$SCRIPT_DIR/../../src/stdlib/crypto_impl.cpp"

# Test 9: generate_random_bytes does NOT use mt19937
if grep -A20 'static std::string generate_random_bytes' "$SRC_CRYPTO" | grep -q 'mt19937'; then
    fail "V-API-005 T9: generate_random_bytes still uses mt19937"
else
    pass "V-API-005 T9: generate_random_bytes does NOT use mt19937"
fi

# Test 10: RAND_bytes or /dev/urandom used
if grep -A20 'static std::string generate_random_bytes' "$SRC_CRYPTO" | grep -q 'RAND_bytes\|urandom'; then
    pass "V-API-005 T10: CSPRNG (RAND_bytes or /dev/urandom) used"
else
    fail "V-API-005 T10: No CSPRNG source found"
fi

# Test 11: random_string does NOT use mt19937
if grep -B2 -A15 'Function 10: random_string' "$SRC_CRYPTO" | grep -q 'mt19937'; then
    fail "V-API-005 T11: random_string still uses mt19937"
else
    pass "V-API-005 T11: random_string does NOT use mt19937"
fi

# ── V-API-003: compare_digest timing fix ───────────────────────────────────

echo "=== V-API-003: compare_digest length-independent ==="

# Test 12: No early return on length mismatch
if grep -A15 'Function 12: compare_digest' "$SRC_CRYPTO" | grep -q 'a.length() != b.length().*return'; then
    fail "V-API-003 T12: compare_digest still has early-return length check"
else
    pass "V-API-003 T12: compare_digest has no early-return length check"
fi

# Test 13: Branchless length mismatch
if grep -A15 'Function 12: compare_digest' "$SRC_CRYPTO" | grep -q 'a.length() != len'; then
    pass "V-API-003 T13: Branchless length mismatch recording"
else
    fail "V-API-003 T13: Branchless length mismatch not found"
fi

# Test 14: Runtime test — compare_digest works correctly
cat > "$WORK_DIR/compare_test.naab" << 'NAAB'
main {
    let r1 = crypto.compare_digest("abc", "abc")
    let r2 = crypto.compare_digest("abc", "def")
    let r3 = crypto.compare_digest("abc", "abcd")
    let r4 = crypto.compare_digest("abcd", "abc")
    print(r1)
    print(r2)
    print(r3)
    print(r4)
}
NAAB

OUTPUT=$("$NAAB" "$WORK_DIR/compare_test.naab" 2>&1 || true)
EXPECTED=$'true\nfalse\nfalse\nfalse'
if echo "$OUTPUT" | grep -q 'true' && echo "$OUTPUT" | grep -c 'false' | grep -q '3'; then
    pass "V-API-003 T14: compare_digest returns correct results"
else
    fail "V-API-003 T14: compare_digest incorrect" "$OUTPUT"
fi

# ── Summary ────────────────────────────────────────────────────────────────

echo ""
echo "================================"
echo "R30 Results: $PASS passed, $FAIL failed (of $((PASS+FAIL)) tests)"
echo "================================"

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
