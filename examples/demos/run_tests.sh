#!/bin/bash
# NAAb Test Runner
# Executes all test files and reports results

echo "========================================"
echo "NAAb v1.0 Test Suite Runner"
echo "========================================"
echo ""

# Configuration
NAAB_BIN="/data/data/com.termux/files/home/naab-instrumented-new"
TEST_DIR="/storage/emulated/0/Download/.naab/naab_language/tests"
TIMEOUT=10
PASSED=0
FAILED=0
TOTAL=0

# Color codes
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test files to run
tests=(
    "test_hello.naab"
    "test_minimal.naab"
    "test_calculator.naab"
    "test_pipeline.naab"
    "test_data_structures.naab"
    "test_control_flow.naab"
    "test_error_handling_complete.naab"
    "test_higher_order.naab"
)

# Function to run a test
run_test() {
    local test_file=$1
    local test_path="$TEST_DIR/$test_file"

    echo "----------------------------------------"
    echo "Running: $test_file"
    echo "----------------------------------------"

    TOTAL=$((TOTAL + 1))

    # Check if file exists
    if [ ! -f "$test_path" ]; then
        echo -e "${RED}SKIP${NC}: File not found"
        FAILED=$((FAILED + 1))
        return 1
    fi

    # Run test with timeout
    if timeout $TIMEOUT $NAAB_BIN run "$test_path" 2>&1; then
        echo -e "${GREEN}PASS${NC}: Test completed successfully"
        PASSED=$((PASSED + 1))
        return 0
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo -e "${RED}FAIL${NC}: Test timed out after ${TIMEOUT}s"
        else
            echo -e "${RED}FAIL${NC}: Test failed with exit code $exit_code"
        fi
        FAILED=$((FAILED + 1))
        return 1
    fi
}

# Run all tests
for test in "${tests[@]}"; do
    run_test "$test"
    echo ""
done

# Summary
echo "========================================"
echo "Test Summary"
echo "========================================"
echo "Total:  $TOTAL"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ Some tests failed${NC}"
    exit 1
fi
