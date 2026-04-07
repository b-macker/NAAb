#!/usr/bin/env bash
# Security regression tests for R15 fixes:
#   V-LSP-005 — naab-lang --lint-only skips execution (no zero-click RCE via LSP)
#   V-LSP-004 — STDIN redirected to /dev/null; "--" arg separator blocks flag injection
#   V-RCE-002 — PATH, BASH_ENV, ENV, COMPILER_PATH, etc. added to dangerous var denylist
#   V-RCE-003 — env var checks are case-insensitive (ld_preload blocked on Windows too)
#   V-API-003 — constantTimeCompare no longer early-returns on length mismatch

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
# T-LSP5 — V-LSP-005: --lint-only skips script execution entirely
# ============================================================================

echo ""
echo "=== T-LSP5: V-LSP-005 --lint-only skips execution ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-LSP5"
else
    WORK_DIR="$(mktemp -d /data/data/com.termux/files/tmp/naab_r15_XXXXXX 2>/dev/null || mktemp -d)"
    MARKER="$WORK_DIR/executed_marker"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"

    # Script whose side-effect (touching a file) proves it executed
    cat > "$WORK_DIR/side_effect.naab" << EOF
use file
main {
    file.write("$MARKER", "ran")
    print("executed")
}
EOF

    set +e
    OUT=$("$NAAB" --lint-only -- "$WORK_DIR/side_effect.naab" 2>&1)
    CODE=$?
    set -e

    if [ "$CODE" -eq 0 ] && [ ! -f "$MARKER" ]; then
        pass "T-LSP5-1: --lint-only exited 0 without creating marker file (no execution)"
    elif [ "$CODE" -ne 0 ] && [ ! -f "$MARKER" ]; then
        pass "T-LSP5-1: --lint-only exited non-zero without executing (governance gate or parse error)"
    else
        fail "T-LSP5-1: marker file exists — script was executed under --lint-only (out=$OUT)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-LSP4 — V-LSP-004: STDIN redirect + "--" flag separator in document_manager
# ============================================================================

echo ""
echo "=== T-LSP4: V-LSP-004 STDIN redirect and flag separator ==="

# Source check: STDIN_FILENO redirect present
if grep -q 'STDIN_FILENO' "$ROOT/tools/naab-lsp/document_manager.cpp" 2>/dev/null; then
    pass "T-LSP4-1: document_manager.cpp redirects STDIN_FILENO to /dev/null"
else
    fail "T-LSP4-1: STDIN_FILENO redirect not found in document_manager.cpp"
fi

# Source check: "--" separator present in argv construction
if grep -q '"--"' "$ROOT/tools/naab-lsp/document_manager.cpp" 2>/dev/null; then
    pass "T-LSP4-2: document_manager.cpp uses \"--\" flag separator in argv"
else
    fail "T-LSP4-2: \"--\" separator not found in document_manager.cpp argv"
fi

# ============================================================================
# T-RCE2 — V-RCE-002: PATH and BASH_ENV are now blocked
# ============================================================================

echo ""
echo "=== T-RCE2: V-RCE-002 expanded dangerous var denylist ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-RCE2"
else
    WORK_DIR="$(mktemp -d /data/data/com.termux/files/tmp/naab_r15_XXXXXX 2>/dev/null || mktemp -d)"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"

    # T-RCE2-1: env.set_var("PATH", ...) must throw
    cat > "$WORK_DIR/test_path.naab" << 'EOF'
use env
main {
    env.set_var("PATH", "/tmp/evil")
    print("bad")
}
EOF
    set +e
    OUT=$("$NAAB" "$WORK_DIR/test_path.naab" 2>&1)
    CODE=$?
    set -e
    if [ "$CODE" -ne 0 ] && ! echo "$OUT" | grep -q "bad"; then
        pass "T-RCE2-1: env.set_var(PATH) throws (exit=$CODE)"
    else
        fail "T-RCE2-1: expected error but got exit=$CODE (out=$OUT)"
    fi

    # T-RCE2-2: env.set_var("BASH_ENV", ...) must throw
    cat > "$WORK_DIR/test_bash_env.naab" << 'EOF'
use env
main {
    env.set_var("BASH_ENV", "/tmp/evil.sh")
    print("bad")
}
EOF
    set +e
    OUT=$("$NAAB" "$WORK_DIR/test_bash_env.naab" 2>&1)
    CODE=$?
    set -e
    if [ "$CODE" -ne 0 ] && ! echo "$OUT" | grep -q "bad"; then
        pass "T-RCE2-2: env.set_var(BASH_ENV) throws (exit=$CODE)"
    else
        fail "T-RCE2-2: expected error but got exit=$CODE (out=$OUT)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-RCE3 — V-RCE-003: case-insensitive env var blocking
# ============================================================================

echo ""
echo "=== T-RCE3: V-RCE-003 case-insensitive denylist ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-RCE3"
else
    WORK_DIR="$(mktemp -d /data/data/com.termux/files/tmp/naab_r15_XXXXXX 2>/dev/null || mktemp -d)"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"

    # T-RCE3-1: lowercase ld_preload must also be blocked
    cat > "$WORK_DIR/test_lower.naab" << 'EOF'
use env
main {
    env.set_var("ld_preload", "/tmp/evil.so")
    print("bad")
}
EOF
    set +e
    OUT=$("$NAAB" "$WORK_DIR/test_lower.naab" 2>&1)
    CODE=$?
    set -e
    if [ "$CODE" -ne 0 ] && ! echo "$OUT" | grep -q "bad"; then
        pass "T-RCE3-1: env.set_var(ld_preload) throws — case-insensitive block works"
    else
        fail "T-RCE3-1: lowercase ld_preload bypassed denylist (exit=$CODE, out=$OUT)"
    fi

    # T-RCE3-2: mixed-case pythonPath must also be blocked
    cat > "$WORK_DIR/test_mixed.naab" << 'EOF'
use env
main {
    env.set_var("PythonPath", "/tmp/malicious")
    print("bad")
}
EOF
    set +e
    OUT=$("$NAAB" "$WORK_DIR/test_mixed.naab" 2>&1)
    CODE=$?
    set -e
    if [ "$CODE" -ne 0 ] && ! echo "$OUT" | grep -q "bad"; then
        pass "T-RCE3-2: env.set_var(PythonPath) throws — case-insensitive block works"
    else
        fail "T-RCE3-2: mixed-case PythonPath bypassed denylist (exit=$CODE, out=$OUT)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-API3 — V-API-003: constantTimeCompare no longer leaks key length
# ============================================================================

echo ""
echo "=== T-API3: V-API-003 constantTimeCompare length oracle removed ==="

# Source check: length oracle comment present
if grep -q 'length oracle' "$ROOT/src/runtime/crypto_utils.cpp" 2>/dev/null; then
    pass "T-API3-1: V-API-003 fix comment present in crypto_utils.cpp"
else
    fail "T-API3-1: V-API-003 fix comment not found in crypto_utils.cpp"
fi

# Source check: early-return on length mismatch must be removed
if ! grep -q 'if (a.length() != b.length())' \
        "$ROOT/src/runtime/crypto_utils.cpp" 2>/dev/null; then
    pass "T-API3-2: early-return on length mismatch removed from constantTimeCompare"
else
    fail "T-API3-2: early-return still present in constantTimeCompare"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=== R15 Security Test Summary ==="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
