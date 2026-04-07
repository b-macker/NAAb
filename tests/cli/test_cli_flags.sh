#!/bin/bash
# Tests CLI flags: --version, --tree-walk, --sandbox-level, exit codes
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

# Create temp files in home (Termux has no /tmp)
TMPDIR_NAAB="${HOME}/.naab_cli_test_$$"
mkdir -p "$TMPDIR_NAAB"
cleanup() { rm -rf "$TMPDIR_NAAB"; }
trap cleanup EXIT
# V-GOV-007: provide govern.json so fail-closed default doesn't block these CLI tests
echo '{"mode":"off"}' > "$TMPDIR_NAAB/govern.json"

TMPFILE="${TMPDIR_NAAB}/ok.naab"
cat > "$TMPFILE" << 'NAAB'
main { print("ok") }
NAAB

ERRFILE="${TMPDIR_NAAB}/err.naab"
cat > "$ERRFILE" << 'NAAB'
main { throw "runtime error" }
NAAB

echo "=== CLI Flag Tests ==="
echo ""

# --version
VERSION=$("$NAAB_BIN" --version 2>&1)
check "--version shows 0.8.1" 'echo "$VERSION" | grep -q "0.8.1"'

# Default VM execution
OUTPUT=$("$NAAB_BIN" "$TMPFILE" 2>/dev/null)
check "Default VM execution" '[ "$OUTPUT" = "ok" ]'

# --tree-walk execution
OUTPUT=$("$NAAB_BIN" "$TMPFILE" --tree-walk 2>/dev/null)
check "--tree-walk executes correctly" '[ "$OUTPUT" = "ok" ]'

# --sandbox-level unrestricted (explicit)
OUTPUT=$("$NAAB_BIN" "$TMPFILE" --sandbox-level unrestricted 2>/dev/null)
check "--sandbox-level unrestricted works" '[ "$OUTPUT" = "ok" ]'

# --sandbox-level restricted (valid, script has no restricted ops so exit 0)
EXIT_CODE=0
"$NAAB_BIN" "$TMPFILE" --sandbox-level restricted > /dev/null 2>&1 || EXIT_CODE=$?
check "--sandbox-level restricted exits 0 for safe script" '[ "$EXIT_CODE" = "0" ]'

# --sandbox-level standard
EXIT_CODE=0
"$NAAB_BIN" "$TMPFILE" --sandbox-level standard > /dev/null 2>&1 || EXIT_CODE=$?
check "--sandbox-level standard exits 0" '[ "$EXIT_CODE" = "0" ]'

# --sandbox-level elevated
EXIT_CODE=0
"$NAAB_BIN" "$TMPFILE" --sandbox-level elevated > /dev/null 2>&1 || EXIT_CODE=$?
check "--sandbox-level elevated exits 0" '[ "$EXIT_CODE" = "0" ]'

# --sandbox-level invalid → exit code 4 (config error)
EXIT_CODE=0
"$NAAB_BIN" "$TMPFILE" --sandbox-level bogus > /dev/null 2>&1 || EXIT_CODE=$?
check "--sandbox-level invalid gives exit code 4" '[ "$EXIT_CODE" = "4" ]'

# Exit code 0 on success
EXIT_CODE=0
"$NAAB_BIN" "$TMPFILE" > /dev/null 2>&1 || EXIT_CODE=$?
check "Exit code 0 on success" '[ "$EXIT_CODE" = "0" ]'

# Exit code 1 on runtime error
EXIT_CODE=0
"$NAAB_BIN" "$ERRFILE" > /dev/null 2>&1 || EXIT_CODE=$?
check "Exit code 1 on runtime error" '[ "$EXIT_CODE" = "1" ]'

# --no-governance flag accepted
EXIT_CODE=0
"$NAAB_BIN" "$TMPFILE" --no-governance > /dev/null 2>&1 || EXIT_CODE=$?
check "--no-governance flag accepted" '[ "$EXIT_CODE" = "0" ]'

# --verbose flag accepted
EXIT_CODE=0
"$NAAB_BIN" "$TMPFILE" --verbose > /dev/null 2>&1 || EXIT_CODE=$?
check "--verbose flag accepted" '[ "$EXIT_CODE" = "0" ]'

echo ""
echo "CLI flag tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
