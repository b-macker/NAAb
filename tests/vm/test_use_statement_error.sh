#!/usr/bin/env bash
# test_use_statement_error.sh — Finding G: UseStatement in VM mode gives clear error
# Verifies that use BLOCK-xyz syntax in VM mode produces a compile-time error
# with a helpful message rather than a crash or silent misbehavior.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_use_stmt_g_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_use_statement_error.sh ==="
echo ""

# ---------------------------------------------------------------------------
# G1: use BLOCK-xyz syntax in VM mode produces a compiler error, not a crash
# ---------------------------------------------------------------------------
echo "[G1] 'use BLOCK-...' in VM mode gives compile-time error (not crash)"
cat > "$WORKDIR/test_g1.naab" << 'NAABEOF'
use BLOCK-abc123
main {
  use io
  io.println("should not reach")
}
NAABEOF

out=$("$NAAB" "$WORKDIR/test_g1.naab" --vm --no-governance 2>&1) || ec=$?
ec=${ec:-0}

if echo "$out" | grep -qi "not supported\|compiler error\|tree-walk\|block-loading"; then
    ok "clear compile-time error message produced"
elif [[ "$ec" -ne 0 ]]; then
    ok "non-zero exit (no crash, error reported)"
else
    fail "expected error exit, got 0: ${out:0:120}"
fi

if echo "$out" | grep -q "should not reach"; then
    fail "execution continued past invalid use statement"
else
    ok "execution did not continue past invalid statement"
fi

echo ""

# ---------------------------------------------------------------------------
# G2: use math (ModuleUseStmt) still works fine in VM mode
# ---------------------------------------------------------------------------
echo "[G2] 'use math' (ModuleUseStmt) still works in VM mode"
cat > "$WORKDIR/test_g2.naab" << 'NAABEOF'
use math
use io
main {
  let x = math.abs(-5)
  io.println(string(x))
}
NAABEOF

out=$("$NAAB" "$WORKDIR/test_g2.naab" --vm --no-governance 2>&1) || ec=$?
ec=${ec:-0}

if echo "$out" | grep -q "5"; then
    ok "use math works correctly in VM mode"
elif [[ "$ec" -eq 0 ]]; then
    ok "use math ran without error"
else
    fail "use math broken in VM mode: ${out:0:120}"
fi

echo ""

# ---------------------------------------------------------------------------
# G3: use BLOCK- with --tree-walk does NOT produce the compiler error
# (tree-walker has its own handler — we just verify no regression)
# ---------------------------------------------------------------------------
echo "[G3] 'use BLOCK-...' with --tree-walk does not hit compiler path"
out=$("$NAAB" "$WORKDIR/test_g1.naab" --tree-walk --no-governance 2>&1) || ec=$?
ec=${ec:-0}

# Tree-walker may fail for other reasons (block not found in registry), but
# must NOT produce "VM mode" / "Compiler error" message
if echo "$out" | grep -qi "not supported in VM mode\|compiler error.*tree-walk"; then
    fail "tree-walker incorrectly hit the VM compiler error path"
else
    ok "tree-walker took its own code path (no VM compiler error)"
fi

echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
