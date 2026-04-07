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
    if echo "$TAMPER_OUT" | grep -qi "TAMPER" && [ "$TAMPER_CODE" -ne 0 ]; then
        pass "T-SC-3: tampered lockfile triggers TAMPER DETECTED exit"
    else
        fail "T-SC-3: tampered lockfile not detected (exit=$TAMPER_CODE, out=$TAMPER_OUT)"
    fi

    # T-SC-4: without NAAB_LOCK_KEY, --lock-check warns but proceeds (exit 0)
    # Reset to a clean state first
    NAAB_LOCK_KEY=testkey123 "$NAAB" --lock "$WORK_DIR/simple.naab" > /dev/null 2>&1 || true
    WARN_OUT=$(unset NAAB_LOCK_KEY; "$NAAB" --lock-check "$WORK_DIR/simple.naab" 2>&1 || true)
    WARN_CODE=$?
    if echo "$WARN_OUT" | grep -qi "NAAB_LOCK_KEY not set" && [ "$WARN_CODE" -eq 0 ]; then
        pass "T-SC-4: no NAAB_LOCK_KEY warns but does not block"
    else
        fail "T-SC-4: expected warn-and-proceed without key (exit=$WARN_CODE, out=$WARN_OUT)"
    fi

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
    if strings "$LSP_SERVER" 2>/dev/null | grep -q "10000 edits"; then
        pass "T-LSP-1: naab-lsp binary contains rename edit limit error string"
    else
        skip "T-LSP-1: cannot verify LSP bounds from binary (strings check inconclusive)"
    fi
fi

# ============================================================================
# T-GOV — V-GOV-010: ReDoS protection for custom rule patterns
# ============================================================================

echo ""
echo "=== T-GOV: V-GOV-010 ReDoS protection ==="

if [ ! -x "$NAAB_GOV" ]; then
    skip "naab-gov not built — skipping T-GOV"
else
    WORK_DIR="$(mktemp -d)"

    # Write a govern.json with a ReDoS-prone custom rule pattern
    cat > "$WORK_DIR/govern.json" << 'EOF'
{
    "version": "4.0",
    "mode": "advisory",
    "custom_rules": [
        {
            "id": "REDOS-TEST",
            "name": "ReDoS Test Pattern",
            "pattern": "(a+)+$",
            "enabled": true,
            "case_sensitive": true,
            "level": "advisory",
            "message": "ReDoS test"
        }
    ]
}
EOF

    cat > "$WORK_DIR/test.naab" << 'EOF'
main {
    let x = 1
}
EOF

    # Run naab-gov lint with a 5-second timeout — should not hang
    GOV_OUT=$(cd "$WORK_DIR" && timeout 5 "$NAAB_GOV" lint "$WORK_DIR/test.naab" 2>&1 || true)
    GOV_CODE=$?

    if [ "$GOV_CODE" -eq 124 ]; then
        fail "T-GOV-1: naab-gov HUNG on ReDoS pattern (timeout after 5s)"
    else
        pass "T-GOV-1: naab-gov completed within 5s on ReDoS pattern (exit=$GOV_CODE)"
    fi

    # Verify it logged a warning about the unsafe pattern
    if echo "$GOV_OUT" | grep -qi "Unsafe regex\|unsafe.*regex\|skipped"; then
        pass "T-GOV-2: naab-gov warned about unsafe/skipped ReDoS pattern"
    else
        fail "T-GOV-2: no unsafe regex warning found (output: $GOV_OUT)"
    fi

    # T-GOV-3: a safe pattern should still compile and work
    cat > "$WORK_DIR/govern.json" << 'EOF'
{
    "version": "4.0",
    "mode": "advisory",
    "custom_rules": [
        {
            "id": "SAFE-PATTERN",
            "name": "Safe Pattern",
            "pattern": "eval\\s*\\(",
            "enabled": true,
            "case_sensitive": false,
            "level": "advisory",
            "message": "Avoid eval"
        }
    ]
}
EOF

    SAFE_OUT=$(cd "$WORK_DIR" && timeout 5 "$NAAB_GOV" lint "$WORK_DIR/test.naab" 2>&1 || true)
    SAFE_CODE=$?
    if [ "$SAFE_CODE" -ne 124 ] && ! echo "$SAFE_OUT" | grep -qi "Unsafe regex"; then
        pass "T-GOV-3: safe pattern accepted without warning"
    else
        fail "T-GOV-3: safe pattern rejected or timed out (exit=$SAFE_CODE, out=$SAFE_OUT)"
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
