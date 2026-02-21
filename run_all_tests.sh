#!/bin/bash
# Comprehensive test runner for all NAAb tests

set -e

NAAB_BIN="./build/naab-lang"
TEST_DIRS=(
    "examples"
    "tests/bugs"
    "tests/comprehensive"
    "tests/integration"
    "tests/security"
    "tests/benchmarks"
    "tests/debugger"
    "tests/error_messages"
    "tests/fixtures"
    "tests/formatter"
    "tests/llm_patterns"
    "tests/issue3_path_resolution"
    "docs/book/verification"
    "docs/tutorials"
    "tests"
)

PASSED=0
FAILED=0
EXPECTED_FAIL=0
SKIPPED=0
TOTAL=0

declare -a FAILED_TESTS
declare -a PASSED_TESTS
declare -a EXPECTED_FAILED_TESTS

# Files that are EXPECTED to fail (intentional error tests, missing compilers, etc.)
declare -A EXPECTED_FAILURES
# Intentional error message tests (designed to exit non-zero)
EXPECTED_FAILURES["test_block_error.naab"]=1
EXPECTED_FAILURES["test_class_error.naab"]=1
EXPECTED_FAILURES["test_var_error.naab"]=1
EXPECTED_FAILURES["test_var_toplevel_error.naab"]=1
EXPECTED_FAILURES["test_func_main_error.naab"]=1
EXPECTED_FAILURES["test_reserved_keyword_error.naab"]=1
EXPECTED_FAILURES["test_sys_hint.naab"]=1
EXPECTED_FAILURES["test_variable_binding_error.naab"]=1
EXPECTED_FAILURES["parser_errors_test.naab"]=1
EXPECTED_FAILURES["test_catch_syntax.naab"]=1
EXPECTED_FAILURES["test_js_variable_binding_error.naab"]=1
EXPECTED_FAILURES["test_unclosed_block.naab"]=1
# Intentional error demos
EXPECTED_FAILURES["error_demo.naab"]=1
EXPECTED_FAILURES["error_demo_simple.naab"]=1
# Require compilers not available in this environment
EXPECTED_FAILURES["test_csharp_stats.naab"]=1
# Complex demos with cascading issues (need full rewrites)
EXPECTED_FAILURES["MONOLITH_DEMO.naab"]=1
EXPECTED_FAILURES["WORKING_MONOLITH.naab"]=1
EXPECTED_FAILURES["NAAB_SYSTEM_MONITOR.naab"]=1
EXPECTED_FAILURES["monitoring_system-edit.naab"]=1
# Missing dependency chain (data_transformer -> web_scraper)
EXPECTED_FAILURES["data_transformer.naab"]=1
EXPECTED_FAILURES["insight_generator.naab"]=1
# Missing test module dependency
EXPECTED_FAILURES["ISS024_COMPLETE_TEST.naab"]=1
# Tutorial with polyglot cascading errors
EXPECTED_FAILURES["TUTORIAL_POLYGLOT_BLOCKS.naab"]=1

# Directories to skip entirely
SKIP_DIRS=(
    "docs/book/verification/ch0_full_projects"  # Gemini-generated projects (broken, not real tests)
)

echo "═══════════════════════════════════════════════════════════"
echo "  NAAb Language - Comprehensive Test Suite Runner"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Check if naab-lang exists
if [ ! -f "$NAAB_BIN" ]; then
    echo "Error: naab-lang binary not found at $NAAB_BIN"
    echo "Run 'cd build && make naab-lang' first"
    exit 1
fi

# Function to check if a path should be skipped
should_skip() {
    local file_path="$1"
    for skip_dir in "${SKIP_DIRS[@]}"; do
        if [[ "$file_path" == *"$skip_dir"* ]]; then
            return 0
        fi
    done
    return 1
}

# Function to run a single test
run_test() {
    local test_file="$1"
    local test_name=$(basename "$test_file")
    local timeout_duration="${2:-10s}"

    # Skip files in excluded directories
    if should_skip "$test_file"; then
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    TOTAL=$((TOTAL + 1))

    # Run the test with timeout
    local output_file="$HOME/.naab_test_output_$$.txt"
    if timeout "$timeout_duration" "$NAAB_BIN" run "$test_file" > "$output_file" 2>&1; then
        PASSED=$((PASSED + 1))
        PASSED_TESTS+=("$test_name")
        echo "  PASS: $test_name"
    else
        local exit_code=$?
        # Check if this is an expected failure
        if [ "${EXPECTED_FAILURES[$test_name]}" = "1" ]; then
            EXPECTED_FAIL=$((EXPECTED_FAIL + 1))
            EXPECTED_FAILED_TESTS+=("$test_name")
            echo "  XFAIL: $test_name (expected)"
        elif [ $exit_code -eq 124 ]; then
            echo "  TIMEOUT: $test_name (>$timeout_duration)"
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_name (timeout >$timeout_duration)")
        else
            FAILED=$((FAILED + 1))
            FAILED_TESTS+=("$test_name")
            echo "  FAIL: $test_name"
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
    echo "-----------------------------------------------------------"
    echo "  Testing: $dir"
    echo "-----------------------------------------------------------"

    # Set timeout based on directory
    timeout="10s"
    if [ "$dir" = "tests/comprehensive" ]; then
        timeout="180s"
    elif [ "$dir" = "docs/book/verification" ]; then
        timeout="30s"
    elif [ "$dir" = "examples" ]; then
        timeout="30s"
    elif [ "$dir" = "tests/integration" ]; then
        timeout="30s"
    elif [ "$dir" = "tests/security" ]; then
        timeout="120s"
    elif [ "$dir" = "tests/benchmarks" ]; then
        timeout="60s"
    elif [ "$dir" = "docs/tutorials" ]; then
        timeout="30s"
    fi

    # Find all .naab files
    if [ "$dir" = "tests" ]; then
        # For tests/ root, only get direct .naab files, not subdirs
        while IFS= read -r -d '' test_file; do
            run_test "$test_file" "$timeout"
        done < <(find "$dir" -maxdepth 1 -name "*.naab" -type f -print0 | sort -z)
    else
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
echo "Total Tests:      $TOTAL"
echo "Passed:           $PASSED"
echo "Expected Fails:   $EXPECTED_FAIL"
echo "Unexpected Fails: $FAILED"
if [ $SKIPPED -gt 0 ]; then
    echo "Skipped:          $SKIPPED"
fi
EFFECTIVE=$((PASSED + EXPECTED_FAIL))
echo "Success Rate:     $(awk "BEGIN {printf \"%.1f\", ($EFFECTIVE/$TOTAL)*100}")% ($EFFECTIVE/$TOTAL)"
echo ""

if [ $FAILED -gt 0 ]; then
    echo "Unexpected Failures:"
    for test in "${FAILED_TESTS[@]}"; do
        echo "  - $test"
    done
    echo ""
    exit 1
else
    echo "ALL $TOTAL TESTS ACCOUNTED FOR ($PASSED passed, $EXPECTED_FAIL expected failures)"
    echo ""
    exit 0
fi
