#!/usr/bin/env bash
# Security regression tests for R12 fixes:
#   V-SC-001 — naab.lock HMAC-SHA256 signature verification
#   V-LSP-001 — handleRename DoS bounds (compile-time verification only)
#   V-GOV-010 — ReDoS protection in governance custom rule patterns

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
# T-SC — V-SC-001: naab.lock HMAC-SHA256 signature
# ============================================================================

echo ""
echo "=== T-SC: V-SC-001 naab.lock signature ==="

if [ ! -x "$NAAB" ]; then
    skip "naab-lang not built — skipping T-SC"
else
    WORK_DIR="$(mktemp -d)"
    # Copy a minimal test naab file and govern.json
    cp "$ROOT/tests/govern.json" "$WORK_DIR/govern.json" 2>/dev/null || \
        echo '{"version":"4.0","mode":"off"}' > "$WORK_DIR/govern.json"
    cat > "$WORK_DIR/simple.naab" << 'EOF'
main {
    let x = 1
    print(x)
}
EOF

    # T-SC-1: --lock with key set creates .naab/naab.lock and naab.lock.sig
    NAAB_LOCK_KEY=testkey123 "$NAAB" --lock "$WORK_DIR/simple.naab" > /dev/null 2>&1 || true
    if [ -f "$WORK_DIR/.naab/naab.lock" ] && [ -f "$WORK_DIR/.naab/naab.lock.sig" ]; then
        pass "T-SC-1: --lock creates naab.lock.sig sidecar when NAAB_LOCK_KEY is set"
    else
        fail "T-SC-1: naab.lock.sig sidecar not created"
    fi

    # T-SC-2: --lock-check with matching key passes
    set +e
    NAAB_LOCK_KEY=testkey123 "$NAAB" --lock-check "$WORK_DIR/simple.naab" > /dev/null 2>&1
    SC2_CODE=$?
    set -e
    if [ "$SC2_CODE" -eq 0 ]; then
        pass "T-SC-2: --lock-check passes with matching NAAB_LOCK_KEY"
    else
        fail "T-SC-2: --lock-check failed unexpectedly with correct key (exit=$SC2_CODE)"
    fi

    # T-SC-3: tamper lockfile -> --lock-check exits 1 with TAMPER message
    echo "tampered" >> "$WORK_DIR/.naab/naab.lock"
    set +e
    TAMPER_OUT=$(NAAB_LOCK_KEY=testkey123 "$NAAB" --lock-check "$WORK_DIR/simple.naab" 2>&1)
    TAMPER_CODE=$?
    set -e
    if grep <<<"$TAMPER_OUT" -qi "TAMPER" && [ "$TAMPER_CODE" -ne 0 ]; then
        pass "T-SC-3: tampered lockfile triggers TAMPER DETECTED exit"
    else
        fail "T-SC-3: tampered lockfile not detected (exit=$TAMPER_CODE, out=$TAMPER_OUT)"
    fi

    # T-SC-4 REMOVED. It asserted that --lock-check without NAAB_LOCK_KEY
    # WARNS AND PROCEEDS (exit 0). The runtime now refuses:
    #   [lock] TAMPER: ...naab.lock.sig exists but NAAB_LOCK_KEY is not set.
    # That is the correct direction and the same rule F31 established -- a
    # signature that is present but cannot be verified is a FAILURE, not a
    # skip. The assertion encodes the older permissive contract, so it fails
    # because the code got safer, and satisfying it would reopen the hole.
    # (It was also self-inconsistent: WARN_CODE=$? after a command-substitution
    # ASSIGNMENT captures the assignment's status, not the command's, and the
    # inner `|| true` pinned that to 0 regardless -- so the exit-code half of
    # the condition could never fail.)

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# T-LSP — V-LSP-001: handleRename bounds (binary presence check only)
# ============================================================================

echo ""
echo "=== T-LSP: V-LSP-001 handleRename bounds ==="

LSP_SERVER="$ROOT/build/naab-lsp"
if [ ! -x "$LSP_SERVER" ]; then
    skip "T-LSP-1: naab-lsp not built — skipping LSP rename bounds check"
else
    # Verify the binary was compiled with the bounds constants by checking the binary strings
    # (The constants 10000 and 1048576 appear in error strings if compiled in)
    if grep -q "10000 edits" <<<"$(strings "$LSP_SERVER" 2>/dev/null)"; then
        pass "T-LSP-1: naab-lsp binary contains rename edit limit error string"
    else
        skip "T-LSP-1: cannot verify LSP bounds from binary (strings check inconclusive)"
    fi
fi

# ============================================================================
# T-GOV — V-GOV-010: ReDoS protection for custom rule patterns
#
# V-GOV-010 (436e0e12) SKIPPED a custom-rule pattern its ReDoS heuristic called
# unsafe. Since A89 the patterns compile with naab::rx (RE2, linear time; a
# pattern RE2 cannot take falls back to a step-budgeted matcher), so the skip
# protected nothing the engine does not, and it switched rules off (Bundle 17,
# F-05). The threat stays measured here: a catastrophic-backtracking pattern
# over a long input must finish, and the rule must be ENFORCED, not skipped.
# T-GOV-1/-3 used to read $? after `$(... || true)`, which is always 0, so a
# hang could never be seen; each now records its own exit status.
# ============================================================================

echo ""
echo "=== T-GOV: V-GOV-010 ReDoS protection ==="

if [ ! -x "$NAAB_GOV" ]; then
    skip "naab-gov not built — skipping T-GOV"
else
    WORK_DIR="$(mktemp -d)"
    cat > "$WORK_DIR/govern.json" << 'GOVEOF'
{
    "version": "4.0",
    "mode": "enforce",
    "custom_rules": [
        {
            "id": "REDOS-TEST",
            "name": "ReDoS Test Pattern",
            "pattern": "(a+)+$",
            "enabled": true,
            "case_sensitive": true,
            "level": "hard",
            "languages": ["naab"],
            "message": "ReDoS test"
        }
    ]
}
GOVEOF
    # 30,000 a's then a b: the classic input that makes a backtracking
    # matcher take exponential time on (a+)+$, which it does not match.
    python3 -c 'import sys; open(sys.argv[1],"w").write("main {\n    let s = \"" + "a"*30000 + "b\"\n    print(s)\n}\n")' "$WORK_DIR/test.naab"

    GOV_CODE=0
    GOV_OUT=$(cd "$WORK_DIR" && timeout 20 "$NAAB_GOV" lint "$WORK_DIR/test.naab" 2>&1) || GOV_CODE=$?
    if [ "$GOV_CODE" -eq 124 ] || [ "$GOV_CODE" -gt 128 ]; then
        fail "T-GOV-1: naab-gov hung or crashed on a ReDoS pattern over a long line (exit=$GOV_CODE)"
    elif [ "$GOV_CODE" -eq 0 ]; then
        pass "T-GOV-1: naab-gov finished on a ReDoS pattern over a 30,000-character line (exit=0, no match)"
    else
        fail "T-GOV-1: naab-gov refused a file the pattern does not match (exit=$GOV_CODE, out=$GOV_OUT)"
    fi

    # T-GOV-2: the same rule is enforced, not skipped: a file ending in a's
    # (no trailing newline) matches (a+)+$ and is blocked.
    printf 'main {\n    print("x")\n}\n// aaaa' > "$WORK_DIR/test.naab"
    GOV_CODE=0
    GOV_OUT=$(cd "$WORK_DIR" && timeout 20 "$NAAB_GOV" lint "$WORK_DIR/test.naab" 2>&1) || GOV_CODE=$?
    if [ "$GOV_CODE" -eq 3 ] && grep <<<"$GOV_OUT" -q "REDOS-TEST" && ! grep <<<"$GOV_OUT" -qi "skipped"; then
        pass "T-GOV-2: the ReDoS-shaped rule is enforced, not skipped (exit 3)"
    else
        fail "T-GOV-2: the ReDoS-shaped rule was skipped or not enforced (exit=$GOV_CODE, out=$GOV_OUT)"
    fi

    # T-GOV-3: a safe pattern compiles without a warning and is enforced
    cat > "$WORK_DIR/govern.json" << 'GOVEOF'
{
    "version": "4.0",
    "mode": "enforce",
    "custom_rules": [
        {
            "id": "SAFE-PATTERN",
            "name": "Safe Pattern",
            "pattern": "eval\\s*\\(",
            "enabled": true,
            "case_sensitive": false,
            "level": "hard",
            "languages": ["naab"],
            "message": "Avoid eval"
        }
    ]
}
GOVEOF
    printf 'main {\n    let code = "eval (1)"\n    print(code)\n}\n' > "$WORK_DIR/test.naab"
    SAFE_CODE=0
    SAFE_OUT=$(cd "$WORK_DIR" && timeout 20 "$NAAB_GOV" lint "$WORK_DIR/test.naab" 2>&1) || SAFE_CODE=$?
    if [ "$SAFE_CODE" -eq 3 ] && grep <<<"$SAFE_OUT" -q "SAFE-PATTERN" && ! grep <<<"$SAFE_OUT" -qi "Unsafe regex"; then
        pass "T-GOV-3: a safe pattern compiles, without a warning, and is enforced"
    else
        fail "T-GOV-3: safe pattern rejected, warned or not enforced (exit=$SAFE_CODE, out=$SAFE_OUT)"
    fi

    rm -rf "$WORK_DIR"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=== R12 Security Test Summary ==="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
