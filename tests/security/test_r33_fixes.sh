#!/usr/bin/env bash
# Security R33 Fix Verification Tests
# V-DOS-014: process.exit() throws ExitException instead of std::exit()
# V-RCE-020: file.create_dir symlink sandbox escape
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
WORK_DIR="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_r33.XXXXXX")"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

cat > "$WORK_DIR/govern.json" << 'JSON'
{ "version": "4.0", "mode": "off" }
JSON

# ── V-DOS-014: process.exit() isolation ────────────────────────────────────

echo "=== V-DOS-014: process.exit() isolation ==="

SRC_PROC="$SCRIPT_DIR/../../src/stdlib/process_impl.cpp"
SRC_MAIN="$SCRIPT_DIR/../../src/cli/main.cpp"

# Test 1: No std::exit() call in process_impl.cpp (comments don't count)
if grep -v '//' "$SRC_PROC" | grep -q 'std::exit('; then
    fail "V-DOS-014 T1: std::exit() call still present in process_impl.cpp"
else
    pass "V-DOS-014 T1: No std::exit() call in process_impl.cpp"
fi

# Test 2: ExitException thrown instead
if grep -q 'ExitException' "$SRC_PROC"; then
    pass "V-DOS-014 T2: ExitException thrown in process_impl.cpp"
else
    fail "V-DOS-014 T2: ExitException not found in process_impl.cpp"
fi

# Test 3: main.cpp catches ExitException
if grep -q 'ExitException' "$SRC_MAIN"; then
    pass "V-DOS-014 T3: main.cpp catches ExitException"
else
    fail "V-DOS-014 T3: main.cpp does NOT catch ExitException"
fi

# Test 4: Runtime — process.exit(42) returns exit code 42
cat > "$WORK_DIR/exit_test.naab" << 'NAAB'
use process
main {
    print("before exit")
    process.exit(42)
    print("after exit")
}
NAAB

"$NAAB" "$WORK_DIR/exit_test.naab" > "$WORK_DIR/exit_out.txt" 2>&1
EXIT_CODE=$?
OUTPUT=$(cat "$WORK_DIR/exit_out.txt")

if [ "$EXIT_CODE" -eq 42 ]; then
    pass "V-DOS-014 T4: process.exit(42) returns exit code 42"
else
    fail "V-DOS-014 T4: Expected exit code 42, got $EXIT_CODE" "$OUTPUT"
fi

# Test 5: "before exit" printed, "after exit" not printed
if echo "$OUTPUT" | grep -q "before exit" && ! echo "$OUTPUT" | grep -q "after exit"; then
    pass "V-DOS-014 T5: Execution stopped at process.exit()"
else
    fail "V-DOS-014 T5: Unexpected output" "$OUTPUT"
fi

# ── V-RCE-020: file.create_dir symlink check ──────────────────────────────

echo "=== V-RCE-020: file.create_dir symlink sandbox escape ==="

SRC_FILE="$SCRIPT_DIR/../../src/stdlib/file_impl.cpp"

# Test 6: create_dir uses resolveCanonical
if grep -A15 'function_name == "create_dir"' "$SRC_FILE" | grep -q 'resolveCanonical'; then
    pass "V-RCE-020 T6: create_dir uses resolveCanonical"
else
    fail "V-RCE-020 T6: create_dir does NOT use resolveCanonical"
fi

# Test 7: create_dir has double checkFileSandbox
COUNT=$(grep -A10 'function_name == "create_dir"' "$SRC_FILE" | grep -c 'checkFileSandbox')
if [ "$COUNT" -ge 2 ]; then
    pass "V-RCE-020 T7: create_dir has double checkFileSandbox ($COUNT calls)"
else
    fail "V-RCE-020 T7: create_dir has only $COUNT checkFileSandbox call(s)"
fi

# ── Summary ────────────────────────────────────────────────────────────────

echo ""
echo "================================"
echo "R33 Results: $PASS passed, $FAIL failed (of $((PASS+FAIL)) tests)"
echo "================================"

[[ $FAIL -eq 0 ]] && exit 0 || exit 1
