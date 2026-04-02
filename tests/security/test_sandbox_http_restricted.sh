#!/bin/bash
# Tests HTTP stdlib operations are correctly blocked under restricted/standard sandbox
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0

check() {
    local desc="$1"
    local condition="$2"
    if eval "$condition"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

TMPDIR_NAAB="${HOME}/.naab_sec_http_test_$$"
mkdir -p "$TMPDIR_NAAB"
cleanup() { rm -rf "$TMPDIR_NAAB"; }
trap cleanup EXIT

echo "=== HTTP Sandbox Enforcement Tests ==="
echo ""

# Test 1: http.get blocked under restricted (NET_CONNECT not granted)
cat > "${TMPDIR_NAAB}/get_restricted.naab" << 'NAAB'
main { let r = http.get("https://example.com") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/get_restricted.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "http.get blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 2: http.post blocked under restricted
cat > "${TMPDIR_NAAB}/post_restricted.naab" << 'NAAB'
main { let r = http.post("https://example.com", "data") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/post_restricted.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "http.post blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 3: http.get blocked under standard (network_enabled=false in createEnterpriseConfig)
cat > "${TMPDIR_NAAB}/get_standard.naab" << 'NAAB'
main { let r = http.get("https://example.com") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level standard \
    "${TMPDIR_NAAB}/get_standard.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "http.get blocked under standard (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 4: file:// URL scheme rejected regardless of sandbox level
# http_impl.cpp has a hard-coded scheme check before sandbox check
cat > "${TMPDIR_NAAB}/file_url.naab" << 'NAAB'
main { let r = http.get("file:///proc/version") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level unrestricted \
    "${TMPDIR_NAAB}/file_url.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "file:// URL rejected even under unrestricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 5: --sandbox-level unrestricted does not produce config error (exit 4)
# We can't test live network on CI, but we can verify the flag is not rejected
cat > "${TMPDIR_NAAB}/get_unrestricted.naab" << 'NAAB'
main { let r = http.get("https://example.com") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level unrestricted \
    "${TMPDIR_NAAB}/get_unrestricted.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "unrestricted sandbox flag not rejected as config error (not exit 4)" '[ "$EXIT_CODE" != "4" ]'

echo ""
echo "HTTP Sandbox Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
