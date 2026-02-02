#!/bin/bash
# Test Security Modules Integration
# Phase 1 Day 1: Safe Time and Secure String

set -e  # Exit on error

echo "========================================="
echo "Security Module Integration Test"
echo "========================================="
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Track results
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Function to run a test
run_test() {
    local test_name="$1"
    local test_command="$2"

    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    echo -n "Testing: $test_name... "

    if eval "$test_command" &> test_output.log; then
        echo -e "${GREEN}✓ PASS${NC}"
        PASSED_TESTS=$((PASSED_TESTS + 1))
        return 0
    else
        echo -e "${RED}✗ FAIL${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        echo "  Error output:"
        cat test_output.log | head -20 | sed 's/^/    /'
        return 1
    fi
}

echo "Step 1: Build with Hardening Flags"
echo "-----------------------------------"

# Clean previous build
if [ -d "build" ]; then
    echo "Cleaning previous build..."
    rm -rf build
fi

# Build with hardening
echo "Building with ENABLE_HARDENING=ON..."
run_test "CMake Configure" "cmake -B build -DENABLE_HARDENING=ON -DCMAKE_BUILD_TYPE=Debug 2>&1"

run_test "CMake Build" "cmake --build build -j4 2>&1"

echo ""
echo "Step 2: Verify Hardening Flags"
echo "-------------------------------"

# Check if hardening flags were applied
if [ -f "build/CMakeCache.txt" ]; then
    run_test "Stack Protection Flag" "grep -q 'fstack-protector-strong' build/CMakeCache.txt"
    run_test "PIE Flag" "grep -q 'fPIE' build/CMakeCache.txt"
fi

echo ""
echo "Step 3: Run Unit Tests"
echo "----------------------"

if [ -f "build/naab_unit_tests" ]; then
    run_test "All Unit Tests" "cd build && ./naab_unit_tests --gtest_brief=1"

    # Run specific security test suites
    run_test "Safe Time Tests" "cd build && ./naab_unit_tests --gtest_filter='SafeTimeTest.*' --gtest_brief=1"
    run_test "Secure String Tests" "cd build && ./naab_unit_tests --gtest_filter='SecureStringTest.*' --gtest_brief=1"
    run_test "Secure Buffer Tests" "cd build && ./naab_unit_tests --gtest_filter='SecureBufferTest.*' --gtest_brief=1"
else
    echo -e "${YELLOW}⚠ naab_unit_tests not found, skipping unit tests${NC}"
fi

echo ""
echo "Step 4: Test NAAb Security Tests"
echo "---------------------------------"

if [ -f "build/naab-lang" ]; then
    if [ -f "tests/security/safe_time_test.naab" ]; then
        run_test "NAAb Safe Time Test" "./build/naab-lang run tests/security/safe_time_test.naab"
    fi
else
    echo -e "${YELLOW}⚠ naab-lang not found, skipping NAAb tests${NC}"
fi

echo ""
echo "Step 5: Build with Sanitizers"
echo "------------------------------"

# Build with ASan
echo "Building with AddressSanitizer..."
if command -v clang++ &> /dev/null; then
    run_test "ASan Build" "cmake -B build-asan -DENABLE_ASAN=ON -DENABLE_HARDENING=ON -DCMAKE_CXX_COMPILER=clang++ 2>&1"
    run_test "ASan Compile" "cmake --build build-asan -j4 2>&1"

    if [ -f "build-asan/naab_unit_tests" ]; then
        run_test "ASan Tests" "cd build-asan && ./naab_unit_tests --gtest_brief=1"
    fi
else
    echo -e "${YELLOW}⚠ Clang not found, skipping sanitizer builds${NC}"
fi

echo ""
echo "Step 6: Build with CFI (if Clang available)"
echo "--------------------------------------------"

if command -v clang++ &> /dev/null; then
    run_test "CFI Build" "cmake -B build-cfi -DENABLE_CFI=ON -DENABLE_HARDENING=ON -DCMAKE_CXX_COMPILER=clang++ 2>&1"
    run_test "CFI Compile" "cmake --build build-cfi -j4 2>&1"
else
    echo -e "${YELLOW}⚠ Clang not found, skipping CFI build${NC}"
fi

echo ""
echo "========================================="
echo "Test Summary"
echo "========================================="
echo "Total Tests:  $TOTAL_TESTS"
echo -e "Passed:       ${GREEN}$PASSED_TESTS${NC}"
echo -e "Failed:       ${RED}$FAILED_TESTS${NC}"

if [ $FAILED_TESTS -eq 0 ]; then
    echo ""
    echo -e "${GREEN}✅ All tests passed!${NC}"
    echo ""
    echo "Next Steps:"
    echo "  1. Review integration guide: docs/SECURITY_INTEGRATION.md"
    echo "  2. Update existing code to use safe_time.h and secure_string.h"
    echo "  3. Continue with Phase 1 remaining tasks"
    exit 0
else
    echo ""
    echo -e "${RED}❌ Some tests failed${NC}"
    echo ""
    echo "Check the error output above for details."
    exit 1
fi
