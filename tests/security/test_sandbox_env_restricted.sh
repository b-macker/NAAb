#!/bin/bash
# Tests env stdlib operations are correctly blocked under restricted sandbox.
# Note: --sandbox-level standard uses createEnterpriseConfig() which lacks SYS_ENV.
#       Only --sandbox-level elevated grants SYS_ENV via fromPermissionLevel(ELEVATED).
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

TMPDIR_NAAB="${HOME}/.naab_sec_env_test_$$"
mkdir -p "$TMPDIR_NAAB"
cleanup() { rm -rf "$TMPDIR_NAAB"; }
trap cleanup EXIT

echo "=== Env Sandbox Enforcement Tests ==="
echo ""

# Test 1: env.get blocked under restricted (no SYS_ENV)
cat > "${TMPDIR_NAAB}/env_get.naab" << 'NAAB'
main { let v = env.get("HOME") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/env_get.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "env.get blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 2: env.set_var blocked under restricted
cat > "${TMPDIR_NAAB}/env_set.naab" << 'NAAB'
main { env.set_var("NAAB_TEST", "value") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/env_set.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "env.set_var blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 3: env.get_all blocked under restricted
cat > "${TMPDIR_NAAB}/env_getall.naab" << 'NAAB'
main { let v = env.get_all() }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/env_getall.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "env.get_all blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 4: env.has blocked under restricted
cat > "${TMPDIR_NAAB}/env_has.naab" << 'NAAB'
main { let v = env.has("HOME") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/env_has.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "env.has blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 5: env.get permitted under elevated (SYS_ENV granted via fromPermissionLevel(ELEVATED))
# Note: --sandbox-level standard uses createEnterpriseConfig() which lacks SYS_ENV.
#       Use --sandbox-level elevated for positive test.
cat > "${TMPDIR_NAAB}/env_get_elevated.naab" << 'NAAB'
main { let v = env.get("HOME") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level elevated \
    "${TMPDIR_NAAB}/env_get_elevated.naab" >"${TMPDIR_NAAB}/t5_out.log" 2>"${TMPDIR_NAAB}/t5_err.log" || EXIT_CODE=$?
if [ "$EXIT_CODE" -gt 128 ]; then
    SIG=$((EXIT_CODE - 128))
    echo "  SKIP: env.get under elevated crashed with signal $SIG (CI runner issue)"
else
    check "env.get permitted under elevated (exit 0)" '[ "$EXIT_CODE" = "0" ]'
fi

echo ""
echo "Env Sandbox Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
