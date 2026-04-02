#!/bin/bash
# Tests for naab-gov standalone governance CLI
# Phase 8.2 — naab-gov

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GOV="${ROOT_DIR}/build/naab-gov"
PASS=0
FAIL=0
TESTS=0

check() {
    local name="$1"
    local result="$2"
    TESTS=$((TESTS + 1))
    if [ "$result" = "0" ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name"
    fi
}

# Skip all tests if naab-gov was not built
if [ ! -f "${GOV}" ]; then
    echo ""
    echo "─── naab-gov CLI Tests ────────────────────────────────────────────"
    echo ""
    echo "  SKIP: naab-gov not built (${GOV} not found)"
    echo "        Build with: cd build && cmake --build . --target naab-gov"
    echo ""
    exit 0
fi

echo ""
echo "─── naab-gov CLI Tests ────────────────────────────────────────────"
echo ""

# Test 1: --version exits 0 and prints version
VERSION_OUT=$("${GOV}" --version 2>&1)
VERSION_EXIT=$?
check "--version exits 0" "$VERSION_EXIT"
check "--version prints 0.7.0" "$(echo "${VERSION_OUT}" | grep -q "0.7.0" && echo 0 || echo 1)"

# Test 2: --help exits non-zero (usage) OR zero (acceptable either way)
HELP_OUT=$("${GOV}" --help 2>&1)
HELP_HAS_USAGE=$(echo "${HELP_OUT}" | grep -qi "usage" && echo 0 || echo 1)
check "--help prints usage" "$HELP_HAS_USAGE"

# Test 3: lint a known-good NAAb stdlib test file exits 0
# (No govern.json in tests/stdlib/ → governance not loaded → lint is advisory only)
STDLIB_TEST="${ROOT_DIR}/tests/stdlib/test_string.naab"
if [ -f "${STDLIB_TEST}" ]; then
    "${GOV}" lint "${STDLIB_TEST}" 2>/dev/null
    LINT_EXIT=$?
    # Exit 0 or 1 (no govern.json found) is acceptable — 3 would mean HARD violation
    check "lint known-good file exits 0 or 1 (not 3)" \
        "$([ "${LINT_EXIT}" -ne 3 ] && echo 0 || echo 1)"
else
    check "lint known-good file (skipped — test_string.naab not found)" "0"
fi

# Test 4: scan tests/stdlib/ path exits 0 (known-good NAAb code)
"${GOV}" scan "${ROOT_DIR}/tests/stdlib/" 2>/dev/null
SCAN_EXIT=$?
# Exit 0 (no issues) or 2 (issues but scanned OK) are both acceptable
# Exit 1 or 4 means an error
check "scan tests/stdlib/ exits 0 or 2 (not error)" \
    "$([ "${SCAN_EXIT}" -eq 0 ] || [ "${SCAN_EXIT}" -eq 2 ] && echo 0 || echo 1)"

# Test 5: lint a non-existent file returns non-zero exit
"${GOV}" lint "/nonexistent/path/file.naab" 2>/dev/null
MISSING_EXIT=$?
check "lint non-existent file returns non-zero" \
    "$([ "${MISSING_EXIT}" -ne 0 ] && echo 0 || echo 1)"

echo ""
echo "Results: ${PASS}/${TESTS} passed"
echo ""

[ "$FAIL" -eq 0 ]
