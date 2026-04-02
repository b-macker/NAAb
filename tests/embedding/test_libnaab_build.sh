#!/bin/bash
# Test libnaab shared library build artifacts and public headers
# Phase 8.1 — libnaab embedding API

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
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

echo ""
echo "─── libnaab Build Tests ───────────────────────────────────────────"
echo ""

# Test 1: naab.h public header exists
check "naab.h public header exists" \
    "$([ -f "${ROOT_DIR}/include/naab/public/naab.h" ] && echo 0 || echo 1)"

# Test 2: naab_interpreter.h public header exists
check "naab_interpreter.h public header exists" \
    "$([ -f "${ROOT_DIR}/include/naab/public/naab_interpreter.h" ] && echo 0 || echo 1)"

# Test 3: naab_sandbox.h public header exists
check "naab_sandbox.h public header exists" \
    "$([ -f "${ROOT_DIR}/include/naab/public/naab_sandbox.h" ] && echo 0 || echo 1)"

# Test 4: naab_val.h public header exists
check "naab_val.h public header exists" \
    "$([ -f "${ROOT_DIR}/include/naab/public/naab_val.h" ] && echo 0 || echo 1)"

# Test 5: context.cpp source exists
check "context.cpp source exists" \
    "$([ -f "${ROOT_DIR}/src/interpreter/context.cpp" ] && echo 0 || echo 1)"

# Test 6: libnaab.a (or libnaab.so) was built
check "libnaab.a or libnaab.so was built" \
    "$(( [ -f "${BUILD_DIR}/libnaab.a" ] || [ -f "${BUILD_DIR}/libnaab.so" ] ) && echo 0 || echo 1)"

# Test 7: public headers compile cleanly (compile-only, no link)
EMBED_TEST="${HOME}/.naab_embed_test_$$.cpp"
EMBED_OBJ="${HOME}/.naab_embed_test_$$.o"
cat > "${EMBED_TEST}" << 'CPPEOF'
#include "naab/public/naab.h"
int main() { return 0; }
CPPEOF

COMPILE_EXIT=1
if command -v g++ >/dev/null 2>&1; then
    g++ -c -std=c++17 \
        -I"${ROOT_DIR}/include" \
        -I"${ROOT_DIR}/include/naab/public" \
        "${EMBED_TEST}" \
        -o "${EMBED_OBJ}" 2>/dev/null
    COMPILE_EXIT=$?
fi
rm -f "${EMBED_TEST}" "${EMBED_OBJ}"
check "public headers compile cleanly (g++ -c)" "$COMPILE_EXIT"

echo ""
echo "Results: ${PASS}/${TESTS} passed"
echo ""

[ "$FAIL" -eq 0 ]
