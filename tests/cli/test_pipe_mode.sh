#!/bin/bash
# Tests for --pipe mode: io.write() → stderr, io.output() → stdout

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

# Temp directory (Termux: no /tmp)
TEST_DIR="${HOME}/.naab_pipe_test_$$"
mkdir -p "$TEST_DIR"
cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

echo "=== Pipe Mode Tests ==="
echo ""

# ============================================================================
# Test setup: scripts using io module
# ============================================================================
WRITE_SCRIPT="${TEST_DIR}/write.naab"
cat > "$WRITE_SCRIPT" << 'NAAB'
use io
main {
    io.write("write-output")
    io.output("output-result")
}
NAAB

OUTPUT_ONLY="${TEST_DIR}/output_only.naab"
cat > "$OUTPUT_ONLY" << 'NAAB'
use io
main {
    io.output("machine-result")
}
NAAB

WRITE_ONLY="${TEST_DIR}/write_only.naab"
cat > "$WRITE_ONLY" << 'NAAB'
use io
main {
    io.write("human-log")
}
NAAB

# ============================================================================
# Test 1: --pipe flag accepted, exits 0
# ============================================================================
EXIT_CODE=0
"$NAAB_BIN" "$OUTPUT_ONLY" --pipe > /dev/null 2>&1 || EXIT_CODE=$?
check "--pipe flag accepted, exits 0" '[ "$EXIT_CODE" = "0" ]'

# ============================================================================
# Test 2: Without --pipe, io.write() goes to stdout
# ============================================================================
STDOUT_OUT=$("$NAAB_BIN" "$WRITE_ONLY" 2>/dev/null)
check "Without --pipe: io.write() appears on stdout" '[ "$STDOUT_OUT" = "human-log" ]'

# ============================================================================
# Test 3: With --pipe, io.write() goes to stderr (stdout is empty)
# ============================================================================
STDOUT_PIPE=$("$NAAB_BIN" "$WRITE_ONLY" --pipe 2>/dev/null)
check "With --pipe: io.write() NOT on stdout (redirected to stderr)" '[ -z "$STDOUT_PIPE" ]'

# ============================================================================
# Test 4: With --pipe, io.write() content appears on stderr
# ============================================================================
STDERR_PIPE=$("$NAAB_BIN" "$WRITE_ONLY" --pipe 2>&1 >/dev/null)
check "With --pipe: io.write() content appears on stderr" '[ -n "$STDERR_PIPE" ]'

# ============================================================================
# Test 5: io.output() always goes to stdout regardless of --pipe
# ============================================================================
STDOUT_OUTPUT=$("$NAAB_BIN" "$OUTPUT_ONLY" --pipe 2>/dev/null)
check "With --pipe: io.output() still on stdout" '[ "$STDOUT_OUTPUT" = "machine-result" ]'

# ============================================================================
# Test 6: Without --pipe, io.output() also goes to stdout
# ============================================================================
STDOUT_OUTPUT_NORMAL=$("$NAAB_BIN" "$OUTPUT_ONLY" 2>/dev/null)
check "Without --pipe: io.output() on stdout" '[ "$STDOUT_OUTPUT_NORMAL" = "machine-result" ]'

# ============================================================================
# Test 7: With --pipe, combined script — stdout has only io.output content
# ============================================================================
STDOUT_COMBINED=$("$NAAB_BIN" "$WRITE_SCRIPT" --pipe 2>/dev/null)
check "With --pipe: stdout has only io.output content" '[ "$STDOUT_COMBINED" = "output-result" ]'

# ============================================================================
echo ""
echo "Pipe mode tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
