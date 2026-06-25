#!/usr/bin/env bash
# run-clang-tidy.sh — Run clang-tidy on governance-critical files
#
# Usage:
#   bash scripts/run-clang-tidy.sh          # governance-critical files only
#   bash scripts/run-clang-tidy.sh --all    # entire codebase (slow)
#
# Requires: clang-tidy, build/compile_commands.json (cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LANG_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$LANG_DIR/build"
COMPILE_DB="$BUILD_DIR/compile_commands.json"

if ! command -v clang-tidy >/dev/null 2>&1; then
    echo "clang-tidy not found — install with: pkg install clang-tools-extra"
    exit 1
fi

if [ ! -f "$COMPILE_DB" ]; then
    echo "compile_commands.json not found at $COMPILE_DB"
    echo "Run: cd build && cmake .. && make naab-lang -j4"
    exit 1
fi

# Governance-critical files — ordered by risk
CRITICAL_FILES=(
    "$LANG_DIR/src/runtime/governance_engine.cpp"
    "$LANG_DIR/src/runtime/governance_checks.cpp"
    "$LANG_DIR/src/runtime/governance_config.cpp"
    "$LANG_DIR/src/interpreter/interpreter.cpp"
    "$LANG_DIR/src/vm/vm.cpp"
    "$LANG_DIR/src/vm/compiler.cpp"
    "$LANG_DIR/src/stdlib/agent_impl.cpp"
    "$LANG_DIR/src/runtime/trust_store.cpp"
    "$LANG_DIR/src/runtime/crypto_utils.cpp"
    "$LANG_DIR/src/cli/main.cpp"
)

FINDINGS=0
ERRORS=0

run_tidy() {
    local file="$1"
    local relfile="${file#$LANG_DIR/}"
    if [ ! -f "$file" ]; then
        echo "  SKIP $relfile (not found)"
        return
    fi
    echo "  Checking $relfile..."
    local output
    output=$(clang-tidy -p "$BUILD_DIR" "$file" 2>&1) || true
    local count
    count=$(echo "$output" | grep -c 'warning:\|error:' 2>/dev/null || echo "0")
    local err_count
    err_count=$(echo "$output" | grep -c 'error:' 2>/dev/null || echo "0")
    if [ "$count" -gt 0 ]; then
        echo "$output" | grep -E 'warning:|error:' | head -20
        FINDINGS=$((FINDINGS + count))
        ERRORS=$((ERRORS + err_count))
    fi
}

echo "=== clang-tidy: NAAb Governance Analysis ==="
echo "  clang-tidy: $(clang-tidy --version 2>&1 | head -1)"
echo "  compile_commands: $COMPILE_DB"
echo ""

if [ "${1:-}" = "--all" ]; then
    echo "--- Full codebase scan ---"
    for f in "$LANG_DIR"/src/**/*.cpp; do
        [ -f "$f" ] || continue
        run_tidy "$f"
    done
else
    echo "--- Governance-critical files ---"
    for f in "${CRITICAL_FILES[@]}"; do
        run_tidy "$f"
    done
fi

echo ""
echo "=== clang-tidy Results: $FINDINGS findings, $ERRORS errors ==="

if [ $ERRORS -gt 0 ]; then
    echo "  FAILED: $ERRORS errors (WarningsAsErrors)"
    exit 1
fi

if [ $FINDINGS -gt 0 ]; then
    echo "  $FINDINGS warnings (non-blocking)"
fi

exit 0
