#!/bin/bash
# Tests file stdlib operations are correctly blocked under restricted sandbox
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

TMPDIR_NAAB=$(mktemp -d "$TMPDIR/naab_sec_file_test_XXXXXX")
mkdir -p "$TMPDIR_NAAB"
cleanup() { rm -rf "$TMPDIR_NAAB"; }
trap cleanup EXIT

# On MSYS2/Windows, convert the temp dir to a Windows path for use inside .naab scripts
# (naab-lang.exe is a native Windows binary that can't resolve MSYS2 POSIX paths)
TMPDIR_NAAB_WIN="$TMPDIR_NAAB"
if command -v cygpath &>/dev/null; then
    TMPDIR_NAAB_WIN=$(cygpath -m "$TMPDIR_NAAB")
fi

echo "=== File Sandbox Enforcement Tests ==="
echo ""

# Test 1: file.write blocked under restricted
cat > "${TMPDIR_NAAB}/write_test.naab" << 'NAAB'
main { file.write("/data/data/com.termux/files/home/naab_bad.txt", "bad") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/write_test.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "file.write blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 2: file.delete blocked under restricted
cat > "${TMPDIR_NAAB}/delete_test.naab" << 'NAAB'
main { file.delete("/data/data/com.termux/files/home/naab_bad.txt") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/delete_test.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "file.delete blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 3: file.create_dir blocked under restricted
cat > "${TMPDIR_NAAB}/mkdir_test.naab" << 'NAAB'
main { file.create_dir("/data/data/com.termux/files/home/naab_bad_dir") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/mkdir_test.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "file.create_dir blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 4: file.append blocked under restricted
cat > "${TMPDIR_NAAB}/append_test.naab" << 'NAAB'
main { file.append("/data/data/com.termux/files/home/naab_bad.txt", "data") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/append_test.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "file.append blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 5: file.read with absolute path blocked under restricted
# Use a hardcoded absolute path that doesn't exist on any platform —
# the sandbox should block it before any filesystem access.
cat > "${TMPDIR_NAAB}/absread_test.naab" << 'NAAB'
main { let x = file.read("/etc/naab_nonexistent_test_file") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/absread_test.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "file.read absolute path blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 6: path traversal attempt blocked under restricted
# Use a relative traversal from the script dir that resolves outside it.
# The sandbox resolves ../../.. before checking, hitting the absolute path block.
cat > "${TMPDIR_NAAB}/traversal_test.naab" << 'NAAB'
main { let x = file.read("../../etc/naab_nonexistent_test_file") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level restricted \
    "${TMPDIR_NAAB}/traversal_test.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "path traversal blocked under restricted (exit 1)" '[ "$EXIT_CODE" = "1" ]'

# Test 7: file.read on local file succeeds under elevated (positive test)
# Use Windows-compatible path so naab-lang.exe can find the file on MSYS2
echo "test content" > "${TMPDIR_NAAB}/testdata.txt"
cat > "${TMPDIR_NAAB}/localread_test.naab" << NAAB
main { let x = file.read("${TMPDIR_NAAB_WIN}/testdata.txt") }
NAAB
EXIT_CODE=0
"$NAAB_BIN" --no-governance --sandbox-level elevated \
    "${TMPDIR_NAAB}/localread_test.naab" > /dev/null 2>&1 || EXIT_CODE=$?
check "file.read local path permitted under elevated (exit 0)" '[ "$EXIT_CODE" = "0" ]'

echo ""
echo "File Sandbox Tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
