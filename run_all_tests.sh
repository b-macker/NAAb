#!/bin/bash
# Comprehensive test runner for all NAAb tests

set -e

NAAB_BIN="./build/naab-lang"
TEST_DIRS=(
    "tests/bugs"
    "tests/comprehensive"
    "docs/book/verification"
    "tests"
)

PASSED=0
FAILED=0
SKIPPED=0
TOTAL=0

declare -a FAILED_TESTS
declare -a PASSED_TESTS

echo "═══════════════════════════════════════════════════════════"
echo "  NAAb Language - Comprehensive Test Suite Runner"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Check if naab-lang exists
if [ ! -f "$NAAB_BIN" ]; then
    echo "❌ Error: naab-lang binary not found at $NAAB_BIN"
    echo "Run 'cd build && make naab-lang' first"
    exit 1
fi

# Function to run a single test
run_test() {
    local test_file="$1"
    local test_name=$(basename "$test_file")
    local timeout_duration="${2:-10s}"  # Default 10s, or use provided timeout

    TOTAL=$((TOTAL + 1))

    # Run the test with timeout
    local output_file="$HOME/.naab_test_output_$$.txt"
    if timeout "$timeout_duration" "$NAAB_BIN" run "$test_file" > "$output_file" 2>&1; then
        PASSED=$((PASSED + 1))
        PASSED_TESTS+=("$test_name")
        echo "  ✅ PASS: $test_name"
    else
        local exit_code=$?
        if [ $exit_code -eq 124 ]; then
            echo "  ⏱️  TIMEOUT: $test_name (>10s)"
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_name (timeout)")
        else
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_name")
            echo "  ❌ FAIL: $test_name"
            # Show first line of error
            if [ -f "$output_file" ]; then
                head -1 "$output_file" | sed 's/^/       /'
            fi
        fi
    fi
    rm -f "$output_file"
}

# Run tests from each directory
for dir in "${TEST_DIRS[@]}"; do
    if [ ! -d "$dir" ]; then
        continue
    fi

    echo ""
    echo "─────────────────────────────────────────────────────────"
    echo "📁 Testing: $dir"
    echo "─────────────────────────────────────────────────────────"

    # Set timeout based on directory
    timeout="10s"
    if [ "$dir" = "tests/comprehensive" ]; then
        timeout="60s"  # Comprehensive tests need more time
    fi

    # Find all .naab files in this directory (not subdirectories for tests/)
    if [ "$dir" = "tests" ]; then
        # For tests/ root, only get direct .naab files, not subdirs
        while IFS= read -r -d '' test_file; do
            run_test "$test_file" "$timeout"
        done < <(find "$dir" -maxdepth 1 -name "*.naab" -type f -print0 | sort -z)
    else
        # For subdirectories, get all .naab files
        while IFS= read -r -d '' test_file; do
            run_test "$test_file" "$timeout"
        done < <(find "$dir" -name "*.naab" -type f -print0 | sort -z)
    fi
done

# Print summary
echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  TEST SUMMARY"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "Total Tests:    $TOTAL"
echo "Passed:         $PASSED"
echo "Failed:         $FAILED"
echo "Success Rate:   $(awk "BEGIN {printf \"%.1f\", ($PASSED/$TOTAL)*100}")%"
echo ""

if [ $FAILED -gt 0 ]; then
    echo "Failed Tests:"
    for test in "${FAILED_TESTS[@]}"; do
        echo "  ❌ $test"
    done
    echo ""
    echo "❌ Some tests failed!"
    exit 1
else
    echo "✅✅✅  ALL $TOTAL TESTS PASSED  ✅✅✅"
    echo ""
    exit 0
fi
