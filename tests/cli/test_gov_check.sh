#!/usr/bin/env bash
# test_gov_check.sh — Test the naab-gov check subcommand (CLI pipe mode)

PASS=0
FAIL=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
GOV="$LANG_DIR/build/naab-gov"
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"

if [ ! -x "$GOV" ]; then
    echo "SKIP: naab-gov not built (run: cd build && cmake .. && make naab-gov)"
    exit 0
fi

# Create a test governance config with enforce mode
TEST_CONFIG="$TMPDIR/test_gov_check_config.json"
cat > "$TEST_CONFIG" << 'EOF'
{
  "version": "3.0",
  "mode": "enforce",
  "restrictions": {
    "dangerous_calls": { "level": "hard" },
    "shell_injection": { "level": "hard" },
    "code_injection": { "level": "hard" }
  },
  "code_quality": {
    "semantic_checks": {
      "level": "hard",
      "check_imports": true,
      "check_dangerous_eval": true
    },
    "no_secrets": { "level": "hard" }
  }
}
EOF

check() {
    local desc="$1" expected_exit="$2" expected_pattern="$3"
    shift 3
    local output exit_code
    output=$("$@" 2>&1)
    exit_code=$?
    if [ "$exit_code" -ne "$expected_exit" ]; then
        echo "  FAIL: $desc (exit=$exit_code, expected=$expected_exit)"
        echo "        Output: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
        return
    fi
    if [ -n "$expected_pattern" ] && ! echo "$output" | grep -q "$expected_pattern"; then
        echo "  FAIL: $desc (pattern '$expected_pattern' not found)"
        echo "        Output: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: $desc"
    PASS=$((PASS + 1))
}

echo "=== naab-gov check tests ==="

# T1: Safe code passes (exit 0)
check "safe python code" 0 '"blocked": false' \
    bash -c "echo 'x = 42; print(x)' | $GOV check --language python --config $TEST_CONFIG"

# T2: Dangerous os.system() blocked (exit 3)
check "os.system blocked" 3 '"blocked": true' \
    bash -c "echo 'import os; os.system(\"rm -rf /\")' | $GOV check --language python --config $TEST_CONFIG"

# T3: eval() blocked (exit 3)
check "eval() blocked" 3 '"blocked": true' \
    bash -c "echo 'eval(input())' | $GOV check --language python --config $TEST_CONFIG"

# T4: Missing --language flag (exit 1)
check "missing --language" 1 "language is required" \
    bash -c "echo 'x = 1' | $GOV check"

# T5: --file flag works
TEST_FILE="$TMPDIR/test_gov_check_safe.py"
echo 'x = 42' > "$TEST_FILE"
check "--file with safe code" 0 '"blocked": false' \
    "$GOV" check --language python --file "$TEST_FILE" --config "$TEST_CONFIG"

# T6: --file with dangerous code
TEST_FILE_BAD="$TMPDIR/test_gov_check_bad.py"
echo 'eval(input())' > "$TEST_FILE_BAD"
check "--file with dangerous code" 3 '"blocked": true' \
    "$GOV" check --language python --file "$TEST_FILE_BAD" --config "$TEST_CONFIG"

# T7: Nonexistent file (exit 1)
check "nonexistent file" 1 "file not found" \
    "$GOV" check --language python --file /nonexistent/file.py

# T8: SARIF output
check "SARIF output format" 0 '"\$schema"' \
    bash -c "echo 'x = 42' | $GOV check --language python --config $TEST_CONFIG --sarif"

# T9: JSON output has violation_count
check "JSON has violation_count" 3 '"violation_count"' \
    bash -c "echo 'eval(input())' | $GOV check --language python --config $TEST_CONFIG"

# T10: Config not found (exit 4)
check "config not found" 4 "govern.json not found" \
    bash -c "echo 'x = 1' | $GOV check --language python --config /nonexistent/govern.json"

# T11: JavaScript code works too
check "javascript eval blocked" 3 '"blocked": true' \
    bash -c "echo 'eval(user_input)' | $GOV check --language javascript --config $TEST_CONFIG"

# T12: Violations contain rule names
check "violations have rule field" 3 '"rule"' \
    bash -c "echo 'eval(input())' | $GOV check --language python --config $TEST_CONFIG"

# Cleanup
rm -f "$TEST_CONFIG" "$TEST_FILE" "$TEST_FILE_BAD"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] || exit 1
