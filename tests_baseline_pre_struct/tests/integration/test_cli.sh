#!/bin/bash

# CLI Integration Tests for NAAb
# Tests all CLI commands and options

NAAB_BIN="${NAAB_BIN:-~/naab-lang}"
TEST_DIR="/storage/emulated/0/Download/.naab/naab_language/tests/integration"
TIMEOUT=10

echo "======================================="
echo "  NAAb CLI Integration Tests"
echo "======================================="
echo ""
echo "Binary: $NAAB_BIN"
echo "Test Dir: $TEST_DIR"
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

passed=0
failed=0
errors=()

# Helper to run CLI command
test_cli() {
    local name="$1"
    shift
    local args=("$@")

    echo -n "Test: $name ... "

    output=$(timeout $TIMEOUT "$NAAB_BIN" "${args[@]}" 2>&1)
    exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}PASS${NC}"
        ((passed++))
        return 0
    elif [ $exit_code -eq 124 ]; then
        echo -e "${RED}TIMEOUT${NC}"
        ((failed++))
        errors+=("$name: Timed out after ${TIMEOUT}s")
        return 1
    else
        echo -e "${RED}FAIL${NC} (exit code $exit_code)"
        ((failed++))
        errors+=("$name: Exit code $exit_code")
        return 1
    fi
}

# Helper to test command output contains string
test_cli_output() {
    local name="$1"
    local expected="$2"
    shift 2
    local args=("$@")

    echo -n "Test: $name ... "

    output=$(timeout $TIMEOUT "$NAAB_BIN" "${args[@]}" 2>&1)
    exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (exit code $exit_code)"
        ((failed++))
        errors+=("$name: Command failed with exit code $exit_code")
        return 1
    fi

    if echo "$output" | grep -q "$expected"; then
        echo -e "${GREEN}PASS${NC}"
        ((passed++))
        return 0
    else
        echo -e "${RED}FAIL${NC} (output doesn't contain '$expected')"
        ((failed++))
        errors+=("$name: Output missing expected string")
        return 1
    fi
}

# Test 1: Version command
test_cli_output "naab-lang version" "NAAb v" version

# Test 2: Help command
test_cli_output "naab-lang help" "Usage:" help

# Test 3: Run simple file
cat > /tmp/test_simple.naab << 'EOF'
print("CLI test")
EOF
test_cli "naab-lang run simple file" run /tmp/test_simple.naab

# Test 4: Parse command
test_cli "naab-lang parse" parse /tmp/test_simple.naab

# Test 5: Check command (type check)
cat > /tmp/test_typecheck.naab << 'EOF'
let x = 42
let y = "hello"
EOF
test_cli "naab-lang check" check /tmp/test_typecheck.naab

# Test 6: Blocks command - list
test_cli_output "naab-lang blocks list" "Total blocks" blocks list

# Test 7: Blocks command - search
test_cli_output "naab-lang blocks search" "Search results" blocks search "email"

# Test 8: Stats command
test_cli_output "naab-lang stats" "Block Statistics" stats

# Test 9: Validate command (block composition)
test_cli "naab-lang validate" validate "BLOCK-PY-001,BLOCK-PY-002"

# Test 10: Run with error (should fail gracefully)
cat > /tmp/test_error.naab << 'EOF'
throw "Intentional error"
EOF
output=$(timeout $TIMEOUT "$NAAB_BIN" run /tmp/test_error.naab 2>&1)
exit_code=$?
if [ $exit_code -ne 0 ]; then
    echo -e "Test: Error handling ... ${GREEN}PASS${NC} (exited with error as expected)"
    ((passed++))
else
    echo -e "Test: Error handling ... ${RED}FAIL${NC} (should have failed)"
    ((failed++))
    errors+=("Error handling: Should have exited with non-zero code")
fi

# Test 11: Invalid command
output=$(timeout $TIMEOUT "$NAAB_BIN" invalid_command 2>&1)
exit_code=$?
if [ $exit_code -ne 0 ]; then
    echo -e "Test: Invalid command ... ${GREEN}PASS${NC} (rejected as expected)"
    ((passed++))
else
    echo -e "Test: Invalid command ... ${RED}FAIL${NC} (should have failed)"
    ((failed++))
    errors+=("Invalid command: Should have exited with non-zero code")
fi

# Test 12: File not found
output=$(timeout $TIMEOUT "$NAAB_BIN" run /nonexistent/file.naab 2>&1)
exit_code=$?
if [ $exit_code -ne 0 ]; then
    echo -e "Test: File not found ... ${GREEN}PASS${NC} (handled gracefully)"
    ((passed++))
else
    echo -e "Test: File not found ... ${RED}FAIL${NC} (should have failed)"
    ((failed++))
    errors+=("File not found: Should have exited with non-zero code")
fi

# Clean up temp files
rm -f /tmp/test_simple.naab /tmp/test_typecheck.naab /tmp/test_error.naab

echo ""
echo "======================================="
echo "  Results"
echo "======================================="
echo "Total:  $((passed + failed))"
echo -e "Passed: ${GREEN}$passed${NC}"
echo -e "Failed: ${RED}$failed${NC}"

if [ ${#errors[@]} -gt 0 ]; then
    echo ""
    echo "Errors:"
    for error in "${errors[@]}"; do
        echo -e "${RED}  - $error${NC}"
    done
fi

echo ""

if [ $failed -eq 0 ]; then
    echo -e "${GREEN}✓ All CLI tests passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ Some tests failed${NC}"
    exit 1
fi
