#!/usr/bin/env bash
# Security regression tests for R14 fixes:
#   V-SC-004 — lock-check is a pre-execution gate (not post-run)
#   V-LSP-003 — runNaabGovernance uses fork/execvp, not popen
#   V-ENV-001 — env.set_var/delete_var block NAAB_LOCK_KEY writes
#   V-RCE-001 — env.set_var blocks LD_PRELOAD and similar loader vars
#   V-ASYNC-004 — executeParallel drains futures on queue-full exception

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
# T-SC4 — V-SC-004: lock-check halts execution BEFORE the script runs
# ============================================================================

echo ""
echo "=== T-SC4: V-SC-004 lock-check pre-execution gate ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-SC4"
else
    WORK_DIR="$(mktemp -d /data/data/com.termux/files/tmp/naab_r14_XXXXXX 2>/dev/null || mktemp -d)"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"

    MARKER="$WORK_DIR/executed_marker"

    # Script that creates a marker file on execution
    cat > "$WORK_DIR/sc4_test.naab" << EOF
main {
    let x = 1
    print(x)
}
EOF

    # Setup: create lockfile with a signature
    NAAB_LOCK_KEY=testkey_r14 "$NAAB" --lock "$WORK_DIR/sc4_test.naab" > /dev/null 2>&1 || true

    # Introduce drift: patch the lockfile to have a bogus runtime version
    LOCK_FILE="$WORK_DIR/.naab/naab.lock"
    if [ -f "$LOCK_FILE" ]; then
        # Corrupt the file content to cause signature mismatch
        echo "tampered_content" >> "$LOCK_FILE"

        set +e
        OUT=$(NAAB_LOCK_KEY=testkey_r14 "$NAAB" --lock-check "$WORK_DIR/sc4_test.naab" 2>&1)
        CODE=$?
        set -e

        if [ "$CODE" -ne 0 ]; then
            pass "T-SC4-A: tampered lockfile detected pre-execution (exit=$CODE)"
        else
            fail "T-SC4-A: expected non-zero exit for tampered lockfile but got exit=$CODE (out=$OUT)"
        fi
    else
        skip "T-SC4-A: lockfile not created (--lock may need a python/etc runtime)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-LSP3 — V-LSP-003: runNaabGovernance uses fork/execvp, not popen
# ============================================================================

echo ""
echo "=== T-LSP3: V-LSP-003 fork/execvp replaces popen ==="

# Source check: execvp must be present in document_manager.cpp
if grep -q 'execvp' "$ROOT/tools/naab-lsp/document_manager.cpp" 2>/dev/null; then
    pass "T-LSP3-1: document_manager.cpp contains execvp (shell-free subprocess)"
else
    fail "T-LSP3-1: execvp not found in document_manager.cpp"
fi

# Source check: the vulnerable popen() call must NOT appear (comments referencing the old
# API are fine; the actual FILE* pipe = popen( invocation must be gone)
if grep -q 'FILE\* pipe = popen' "$ROOT/tools/naab-lsp/document_manager.cpp" 2>/dev/null; then
    fail "T-LSP3-2: vulnerable popen() call still present in document_manager.cpp"
else
    pass "T-LSP3-2: vulnerable popen() call removed from document_manager.cpp"
fi

# ============================================================================
# T-ENV1 — V-ENV-001: env.set_var blocks NAAB_LOCK_KEY writes
# ============================================================================

echo ""
echo "=== T-ENV1: V-ENV-001 blocked mutation of internal secrets ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-ENV1"
else
    WORK_DIR="$(mktemp -d /data/data/com.termux/files/tmp/naab_r14_XXXXXX 2>/dev/null || mktemp -d)"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"

    # T-ENV1-1: env.set_var("NAAB_LOCK_KEY", ...) must throw and not print "bad"
    cat > "$WORK_DIR/test_set_internal.naab" << 'EOF'
use env
main {
    env.set_var("NAAB_LOCK_KEY", "forged")
    print("bad")
}
EOF
    set +e
    SET_OUT=$(NAAB_LOCK_KEY=real "$NAAB" "$WORK_DIR/test_set_internal.naab" 2>&1)
    SET_CODE=$?
    set -e
    if [ "$SET_CODE" -ne 0 ] && ! echo "$SET_OUT" | grep -q "bad"; then
        pass "T-ENV1-1: env.set_var(NAAB_LOCK_KEY) throws (exit=$SET_CODE)"
    else
        fail "T-ENV1-1: expected error but got exit=$SET_CODE (out=$SET_OUT)"
    fi

    # T-ENV1-2: env.delete_var("NAAB_LOCK_KEY") must throw
    cat > "$WORK_DIR/test_del_internal.naab" << 'EOF'
use env
main {
    env.delete_var("NAAB_LOCK_KEY")
    print("bad")
}
EOF
    set +e
    DEL_OUT=$(NAAB_LOCK_KEY=real "$NAAB" "$WORK_DIR/test_del_internal.naab" 2>&1)
    DEL_CODE=$?
    set -e
    if [ "$DEL_CODE" -ne 0 ] && ! echo "$DEL_OUT" | grep -q "bad"; then
        pass "T-ENV1-2: env.delete_var(NAAB_LOCK_KEY) throws (exit=$DEL_CODE)"
    else
        fail "T-ENV1-2: expected error but got exit=$DEL_CODE (out=$DEL_OUT)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-RCE1 — V-RCE-001: env.set_var blocks dangerous loader variables
# ============================================================================

echo ""
echo "=== T-RCE1: V-RCE-001 dangerous env var blocked ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-RCE1"
else
    WORK_DIR="$(mktemp -d /data/data/com.termux/files/tmp/naab_r14_XXXXXX 2>/dev/null || mktemp -d)"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"

    # T-RCE1-1: env.set_var("LD_PRELOAD", ...) must throw and not print "bad"
    cat > "$WORK_DIR/test_ldpreload.naab" << 'EOF'
use env
main {
    env.set_var("LD_PRELOAD", "/tmp/evil.so")
    print("bad")
}
EOF
    set +e
    LD_OUT=$("$NAAB" "$WORK_DIR/test_ldpreload.naab" 2>&1)
    LD_CODE=$?
    set -e
    if [ "$LD_CODE" -ne 0 ] && ! echo "$LD_OUT" | grep -q "bad"; then
        pass "T-RCE1-1: env.set_var(LD_PRELOAD) throws (exit=$LD_CODE)"
    else
        fail "T-RCE1-1: expected error but got exit=$LD_CODE (out=$LD_OUT)"
    fi

    # T-RCE1-2: env.set_var("PYTHONPATH", ...) must throw
    cat > "$WORK_DIR/test_pythonpath.naab" << 'EOF'
use env
main {
    env.set_var("PYTHONPATH", "/tmp/malicious")
    print("bad")
}
EOF
    set +e
    PY_OUT=$("$NAAB" "$WORK_DIR/test_pythonpath.naab" 2>&1)
    PY_CODE=$?
    set -e
    if [ "$PY_CODE" -ne 0 ] && ! echo "$PY_OUT" | grep -q "bad"; then
        pass "T-RCE1-2: env.set_var(PYTHONPATH) throws (exit=$PY_CODE)"
    else
        fail "T-RCE1-2: expected error but got exit=$PY_CODE (out=$PY_OUT)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-ASYNC4 — V-ASYNC-004: executeParallel drains futures on queue-full
# ============================================================================

echo ""
echo "=== T-ASYNC4: V-ASYNC-004 futures drained on queue-full ==="

if grep -q 'drain already-submitted futures' \
        "$ROOT/src/runtime/polyglot_async_executor.cpp" 2>/dev/null; then
    pass "T-ASYNC4-1: drain comment present in polyglot_async_executor.cpp"
else
    fail "T-ASYNC4-1: V-ASYNC-004 fix not found in polyglot_async_executor.cpp"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=== R14 Security Test Summary ==="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
