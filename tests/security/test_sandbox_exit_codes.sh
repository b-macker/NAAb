#!/bin/bash
# Tests that sandbox violations produce the correct exit codes.
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

TMPDIR_NAAB=$(mktemp -d "${TMPDIR:-/tmp}/naab_sec_exit_test_XXXXXX")
mkdir -p "$TMPDIR_NAAB"
cleanup() { rm -rf "$TMPDIR_NAAB"; }
trap cleanup EXIT

echo "=== Sandbox Exit Code Tests ==="
echo ""

# Test 1: Sandbox violation → exit code 1 (runtime error)
cat > "${TMPDIR_NAAB}/violation.naab" << 'NAAB'
main { file.write("/proc/test_naab", "bad") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/violation.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "sandbox violation produces exit code 1" '[ "$EXIT_CODE" = "1" ]'

# Test 2: Clean script under restricted exits 0
cat > "${TMPDIR_NAAB}/clean.naab" << 'NAAB'
main { let x = 42 }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/clean.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "clean script under restricted exits 0" '[ "$EXIT_CODE" = "0" ]'

# Test 3: Invalid --sandbox-level value → exit code 4 (config error)
cat > "${TMPDIR_NAAB}/any.naab" << 'NAAB'
main { let x = 1 }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level invalid_level \
    "${TMPDIR_NAAB}/any.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "invalid sandbox level produces exit code 4" '[ "$EXIT_CODE" = "4" ]'

# Test 4: Multiple sandbox violations in one script — first one aborts, exits 1
cat > "${TMPDIR_NAAB}/multi_violation.naab" << 'NAAB'
main {
    file.write("/proc/test1", "bad1")
    file.write("/proc/test2", "bad2")
    file.write("/proc/test3", "bad3")
}
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/multi_violation.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "multiple violations still exit 1 (not 2 or 4)" '[ "$EXIT_CODE" = "1" ]'

echo ""
echo "Exit Code Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
