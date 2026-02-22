#!/bin/bash

# Integration Test Runner for NAAb
# Runs all integration tests and reports results

NAAB_BIN="${NAAB_BIN:-~/naab-lang}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_DIR="$SCRIPT_DIR"
TIMEOUT=30

echo "======================================="
echo "  NAAb Integration Test Suite"
echo "======================================="
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test arrays
declare -a tests=(
    # Multi-file tests
    "test_multifile_math.naab"
    "test_multifile_wildcard.naab"
    "test_multifile_aliases.naab"
    "test_multifile_strings.naab"

    # Pipeline tests
    "test_pipeline_basic.naab"
    "test_pipeline_arrays.naab"
    "test_pipeline_imports.naab"

    # Composition tests
    "test_composition_validation.naab"

    # Error handling tests
    "test_error_propagation.naab"
)

passed=0
failed=0
errors=()

# Run each test
for test in "${tests[@]}"; do
    test_path="$TEST_DIR/$test"

    if [ ! -f "$test_path" ]; then
        echo -e "${YELLOW}SKIP${NC}: $test (file not found)"
        continue
    fi

    echo -n "Running $test ... "

    # Run test with timeout
    output=$(timeout $TIMEOUT "$NAAB_BIN" run "$test_path" 2>&1)
    exit_code=$?

    if [ $exit_code -eq 0 ]; then
        echo -e "${GREEN}PASS${NC}"
        ((passed++))
    elif [ $exit_code -eq 124 ]; then
        echo -e "${RED}TIMEOUT${NC}"
        ((failed++))
        errors+=("$test: Test timed out after ${TIMEOUT}s")
    else
        echo -e "${RED}FAIL${NC}"
        ((failed++))
        errors+=("$test: Exit code $exit_code")
        # Optionally show error output
        # echo "$output"
    fi
done

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

# Exit with appropriate code
if [ $failed -eq 0 ]; then
    echo -e "${GREEN}✓ All integration tests passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ Some tests failed${NC}"
    exit 1
fi
