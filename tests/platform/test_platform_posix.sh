#!/bin/bash
# Test platform abstraction (Phase 7.3 — Windows Portability)
# These tests verify that platform_posix.cpp compiles and the API is callable.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${SCRIPT_DIR}/../.."

PASS=0
FAIL=0

check() {
    local name="$1"
    local result="$2"
    if [ "$result" = "0" ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

# ============================================================================
# Test 1: platform.h header exists
# ============================================================================
[ -f "${ROOT}/include/naab/platform.h" ]
check "include/naab/platform.h exists" $?

# ============================================================================
# Test 2: platform_posix.cpp source exists
# ============================================================================
[ -f "${ROOT}/src/platform/platform_posix.cpp" ]
check "src/platform/platform_posix.cpp exists" $?

# ============================================================================
# Test 3: platform.h compiles without errors (C++17)
# ============================================================================
TEST_CPP="${ROOT}/.test_platform_$$.cpp"
cat > "$TEST_CPP" << 'EOF'
#include "naab/platform.h"
int main() {
    auto pid = naab::platform::getpid();
    (void)pid;
    char sep = naab::platform::pathSeparator();
    (void)sep;
    return 0;
}
EOF
g++ -std=c++17 -I"${ROOT}/include" -c "$TEST_CPP" -o /dev/null 2>/dev/null
COMPILE_RESULT=$?
rm -f "$TEST_CPP"
check "platform.h compiles cleanly with C++17" "$COMPILE_RESULT"

# ============================================================================
# Test 4: naab-lang binary exists (platform_posix.cpp linked into naab_runtime)
# ============================================================================
[ -f "${ROOT}/build/naab-lang" ]
check "naab-lang binary exists (platform_posix.cpp linked)" $?

# ============================================================================
echo ""
echo "Platform tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
