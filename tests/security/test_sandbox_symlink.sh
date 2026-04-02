#!/bin/bash
# Tests that symlink traversal and path canonicalization are enforced under restricted sandbox.
# normalizePath() calls realpath() which resolves symlinks before sandbox checks.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0
SKIP=0

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

TMPDIR_NAAB="${HOME}/.naab_sec_sym_test_$$"
mkdir -p "$TMPDIR_NAAB"
cleanup() { rm -rf "$TMPDIR_NAAB"; }
trap cleanup EXIT

echo "=== Symlink / Path Traversal Enforcement Tests ==="
echo ""

# Test 1: Double path traversal (no symlink needed)
# ../../proc/version resolves to /proc/version — outside any allowed path under restricted
TRAVERSAL_PATH="${TMPDIR_NAAB}/../../proc/version"
cat > "${TMPDIR_NAAB}/traversal.naab" << NAAB
main { let x = file.read("${TRAVERSAL_PATH}") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/traversal.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "double path traversal blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 2: Deep traversal attempt
DEEP_TRAVERSAL="../../../../../../../../proc/version"
cat > "${TMPDIR_NAAB}/deep_traversal.naab" << NAAB
main { let x = file.read("${DEEP_TRAVERSAL}") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/deep_traversal.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "deep path traversal blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 3: Symlink pointing to /proc/version (resolved by realpath before sandbox check)
SYMLINK_PATH="${TMPDIR_NAAB}/evil_link"
if ln -s /proc/version "$SYMLINK_PATH" 2>/dev/null && [ -L "$SYMLINK_PATH" ]; then
    cat > "${TMPDIR_NAAB}/symlink_read.naab" << NAAB
main { let x = file.read("${SYMLINK_PATH}") }
NAAB
    EXIT_CODE=0
    "$NAAB_BIN" --no-governance --sandbox-level restricted \
        "${TMPDIR_NAAB}/symlink_read.naab" > /dev/null 2>&1 || EXIT_CODE=$?
    check "symlink to /proc/version blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'
else
    echo "  SKIP: symlink creation not supported on this platform"
    SKIP=$((SKIP + 1))
fi

# Test 4: Nested path with embedded .. components resolved before check
# e.g., /home/user/.naab_test/../../../proc/version → /proc/version
NESTED_PATH="${TMPDIR_NAAB}/../../../proc/version"
cat > "${TMPDIR_NAAB}/nested.naab" << NAAB
main { let x = file.read("${NESTED_PATH}") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/nested.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "nested .. components blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

echo ""
echo "Symlink/Traversal Tests: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
