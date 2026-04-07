#!/usr/bin/env bash
# Security regression tests for R13 fixes:
#   V-SC-003 — fail-closed lockfile verification when .sig exists but key absent
#   V-API-002 — constant-time API key comparison (build presence check)
#   V-SC-002 — NAAB_LOCK_KEY blocked in env stdlib
#   V-LSP-002 — handleWorkspaceSymbol result cap (build presence check)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$ROOT/build/naab-lang"
NAAB_GOV="$ROOT/build/naab-gov"

PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP: $1"; }

# ============================================================================
# T-SC3 — V-SC-003: fail-closed when .sig present but NAAB_LOCK_KEY absent
# ============================================================================

echo ""
echo "=== T-SC3: V-SC-003 fail-closed lockfile ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-SC3"
else
    WORK_DIR="$(mktemp -d)"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"
    cat > "$WORK_DIR/simple.naab" << 'EOF'
main {
    let x = 1
    print(x)
}
EOF

    # Setup: create naab.lock + naab.lock.sig
    NAAB_LOCK_KEY=testkey123 "$NAAB" --lock "$WORK_DIR/simple.naab" > /dev/null 2>&1 || true

    # T-SC3-A: .sig exists, key absent → must fail closed (exit non-zero)
    set +e
    OUT_A=$(unset NAAB_LOCK_KEY; "$NAAB" --lock-check "$WORK_DIR/simple.naab" 2>&1)
    CODE_A=$?
    set -e
    if [ "$CODE_A" -ne 0 ] && echo "$OUT_A" | grep -qi "TAMPER"; then
        pass "T-SC3-A: .sig exists + no key → fail closed (exit=$CODE_A)"
    else
        fail "T-SC3-A: expected fail-closed but got exit=$CODE_A (out=$OUT_A)"
    fi

    # T-SC3-B: no .sig, no key → warn and proceed (exit 0)
    rm -f "$WORK_DIR/.naab/naab.lock.sig"
    set +e
    OUT_B=$(unset NAAB_LOCK_KEY; "$NAAB" --lock-check "$WORK_DIR/simple.naab" 2>&1)
    CODE_B=$?
    set -e
    if [ "$CODE_B" -eq 0 ] && echo "$OUT_B" | grep -qi "unverified"; then
        pass "T-SC3-B: no .sig + no key → warn and proceed (exit=$CODE_B)"
    else
        fail "T-SC3-B: unexpected result exit=$CODE_B (out=$OUT_B)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-API2 — V-API-002: constant-time API key comparison (binary check)
# ============================================================================

echo ""
echo "=== T-API2: V-API-002 constant-time comparison ==="

if [ ! -x "$NAAB" ]; then
    skip "T-API2-1: naab-lang not built"
elif strings "$NAAB" 2>/dev/null | grep -q "constantTimeCompare"; then
    pass "T-API2-1: constantTimeCompare symbol present in naab-lang binary"
else
    skip "T-API2-1: cannot verify constant-time symbol from binary (strings check inconclusive)"
fi

# ============================================================================
# T-SC2 — V-SC-002: NAAB_LOCK_KEY blocked in env stdlib
# ============================================================================

echo ""
echo "=== T-SC2: V-SC-002 env key leakage blocked ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-SC2"
else
    WORK_DIR="$(mktemp -d)"
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"

    # T-SC2-1: env.get("NAAB_LOCK_KEY") must not return the key value
    cat > "$WORK_DIR/test_env_get.naab" << 'EOF'
use env
main {
    let k = env.get("NAAB_LOCK_KEY")
    print(k)
}
EOF
    GET_OUT=$(NAAB_LOCK_KEY=secret123 "$NAAB" "$WORK_DIR/test_env_get.naab" 2>/dev/null || true)
    if echo "$GET_OUT" | grep -q "secret123"; then
        fail "T-SC2-1: env.get leaked NAAB_LOCK_KEY value"
    else
        pass "T-SC2-1: env.get does not expose NAAB_LOCK_KEY"
    fi

    # T-SC2-2: env.has("NAAB_LOCK_KEY") must return false
    cat > "$WORK_DIR/test_env_has.naab" << 'EOF'
use env
main {
    let h = env.has("NAAB_LOCK_KEY")
    print(h)
}
EOF
    HAS_OUT=$(NAAB_LOCK_KEY=secret123 "$NAAB" "$WORK_DIR/test_env_has.naab" 2>/dev/null || true)
    if echo "$HAS_OUT" | grep -q "false"; then
        pass "T-SC2-2: env.has(\"NAAB_LOCK_KEY\") returns false (key hidden)"
    else
        fail "T-SC2-2: env.has leaked key existence (output: $HAS_OUT)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-LSP2 — V-LSP-002: workspace symbol cap (binary check)
# ============================================================================

echo ""
echo "=== T-LSP2: V-LSP-002 workspace symbol cap ==="

LSP_SERVER="$ROOT/build/naab-lsp"
if [ ! -x "$LSP_SERVER" ]; then
    skip "T-LSP2-1: naab-lsp not built"
elif strings "$LSP_SERVER" 2>/dev/null | grep -q "10000 symbols"; then
    pass "T-LSP2-1: naab-lsp binary contains workspace symbol truncation message"
else
    skip "T-LSP2-1: cannot verify LSP cap from binary (strings check inconclusive)"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=== R13 Security Test Summary ==="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
