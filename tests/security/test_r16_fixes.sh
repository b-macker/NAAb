#!/usr/bin/env bash
# Security regression tests for R16 fixes:
#   V-RCE-004 — polyglot compiler temp files now use mkdtemp (exclusive, unpredictable dirs)
#   V-RCE-005 — pre-compilation source scanner rejects dangerous C++/Rust directives
#   V-LSP-006 — LSP server JSON depth guard prevents stack overflow DoS
#   V-GOV-011 — project context loader JSON depth guard prevents stack overflow DoS

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$ROOT/build/naab-lang"

PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1"; }

# ============================================================================
# T-RCE4 — V-RCE-004: mkdtemp used in compiled executors
# ============================================================================

echo ""
echo "=== T-RCE4: V-RCE-004 mkdtemp in compiled executors ==="

if grep -q 'mkdtemp' "$ROOT/src/runtime/cpp_executor_adapter.cpp" 2>/dev/null; then
    pass "T-RCE4-1: cpp_executor_adapter.cpp uses mkdtemp"
else
    fail "T-RCE4-1: mkdtemp not found in cpp_executor_adapter.cpp"
fi

if grep -q 'mkdtemp' "$ROOT/src/runtime/rust_executor.cpp" 2>/dev/null; then
    pass "T-RCE4-2: rust_executor.cpp uses mkdtemp"
else
    fail "T-RCE4-2: mkdtemp not found in rust_executor.cpp"
fi

if grep -q 'mkdtemp' "$ROOT/src/runtime/go_executor.cpp" 2>/dev/null; then
    pass "T-RCE4-3: go_executor.cpp uses mkdtemp"
else
    fail "T-RCE4-3: mkdtemp not found in go_executor.cpp"
fi

# ============================================================================
# T-RCE5 — V-RCE-005: pre-compilation source scanner
# ============================================================================

echo ""
echo "=== T-RCE5: V-RCE-005 pre-compilation source scanner ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-RCE5"
else
    WORK_DIR="$(mktemp -d /data/data/com.termux/files/tmp/naab_r16_XXXXXX 2>/dev/null || mktemp -d)"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"

    # T-RCE5-1: C++ block with absolute-path #include must be rejected
    cat > "$WORK_DIR/test_cpp_include.naab" << 'EOF'
main {
    let result = <<cpp
#include "/etc/passwd"
int main() {
    std::cout << "bad";
    return 0;
}
>>
    print("bad")
}
EOF
    set +e
    OUT=$("$NAAB" "$WORK_DIR/test_cpp_include.naab" 2>&1)
    CODE=$?
    set -e
    if [ "$CODE" -ne 0 ] && ! echo "$OUT" | grep -q "^bad$"; then
        pass "T-RCE5-1: C++ absolute-path #include rejected by source scanner (exit=$CODE)"
    else
        fail "T-RCE5-1: C++ absolute-path #include was NOT rejected (exit=$CODE, out=$OUT)"
    fi

    # T-RCE5-2: Rust block with include_str! absolute path must be rejected
    cat > "$WORK_DIR/test_rust_include.naab" << 'EOF'
main {
    let result = <<rust
fn main() {
    let secret = include_str!("/etc/passwd");
    println!("{}", secret);
}
>>
    print("bad")
}
EOF
    set +e
    OUT=$("$NAAB" "$WORK_DIR/test_rust_include.naab" 2>&1)
    CODE=$?
    set -e
    if [ "$CODE" -ne 0 ] && ! echo "$OUT" | grep -q "^bad$"; then
        pass "T-RCE5-2: Rust include_str!(absolute) rejected by source scanner (exit=$CODE)"
    else
        fail "T-RCE5-2: Rust include_str!(absolute) was NOT rejected (exit=$CODE, out=$OUT)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-LSP6 — V-LSP-006: JSON depth guard in LSP server
# ============================================================================

echo ""
echo "=== T-LSP6: V-LSP-006 JSON depth guard in LSP server ==="

if grep -q 'checkJsonDepth' "$ROOT/tools/naab-lsp/lsp_server.cpp" 2>/dev/null; then
    pass "T-LSP6-1: lsp_server.cpp has checkJsonDepth guard"
else
    fail "T-LSP6-1: checkJsonDepth not found in lsp_server.cpp"
fi

# ============================================================================
# T-GOV11 — V-GOV-011: JSON depth guard in project context loader
# ============================================================================

echo ""
echo "=== T-GOV11: V-GOV-011 JSON depth guard in project context loader ==="

if grep -q 'checkJsonDepth' "$ROOT/src/runtime/project_context.cpp" 2>/dev/null; then
    pass "T-GOV11-1: project_context.cpp has checkJsonDepth guard"
else
    fail "T-GOV11-1: checkJsonDepth not found in project_context.cpp"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=== R16 Security Test Summary ==="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
