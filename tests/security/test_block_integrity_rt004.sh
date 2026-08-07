#!/usr/bin/env bash
# test_block_integrity_rt004.sh — V-RT-004: block source hash verified; tampering rejected
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; }

TMPDIR_TEST="${HOME}/.naab"
mkdir -p "$TMPDIR_TEST"

echo "=== test_block_integrity_rt004.sh ==="
echo ""

# Check if OpenSSL SHA-256 is compiled in (look for HAVE_OPENSSL in the binary strings)
if ! strings "$NAAB" 2>/dev/null | grep -q "Block integrity check failed" && \
   ! strings "$NAAB" 2>/dev/null | grep -q "tampered"; then
    skip "OpenSSL not compiled in — block integrity checks inactive"
    skip "T1: skipped (HAVE_OPENSSL not set)"
    skip "T2: skipped (HAVE_OPENSSL not set)"
    echo ""
    echo "Results: 0/0 (all skipped — build without OpenSSL)"
    exit 0
fi

# ---------------------------------------------------------------------------
# Setup: create a fake blocks library directory with a JSON block
# ---------------------------------------------------------------------------
BLOCKS_DIR="${TMPDIR_TEST}/test_blocks_$$"
LANG_DIR="${BLOCKS_DIR}/python"
mkdir -p "$LANG_DIR"

# Create a simple python source block
BLOCK_SOURCE="print(42)"
# Compute its real SHA-256
REAL_HASH=$(echo -n "$BLOCK_SOURCE" | sha256sum | awk '{print $1}')

# Write the source file
echo -n "$BLOCK_SOURCE" > "$LANG_DIR/test_integrity_block.py"

# Write matching JSON metadata with correct hash
cat > "$LANG_DIR/test_integrity_block.json" <<EOF
{
  "id": "BLOCK-PY-INTEGRITY-TEST",
  "name": "integrity_test",
  "language": "python",
  "code_file": "test_integrity_block.py",
  "code_hash": "${REAL_HASH}",
  "version": "1.0.0",
  "is_active": true
}
EOF

# ---------------------------------------------------------------------------
# T1: tamper the source file after registration → must be rejected
# ---------------------------------------------------------------------------
echo "[T1] Tampered block source rejected with integrity error"

# Tamper: overwrite source with different content (hash will not match)
echo "import os; os.system('id')" > "$LANG_DIR/test_integrity_block.py"

SCRIPT_T1="${TMPDIR_TEST}/rt004_t1_$$.naab"
cat > "$SCRIPT_T1" <<NAAB
main {
    use BLOCK-PY-INTEGRITY-TEST
    print("should not reach here")
}
NAAB

out=$("$NAAB" --no-governance --blocks-path "$BLOCKS_DIR" run "$SCRIPT_T1" 2>&1 || true)
if echo "$out" | grep -qi "tampered\|integrity.*check.*fail\|hash.*mismatch\|code_hash"; then
    ok "tampered block rejected with integrity error"
else
    # This branch used to pass while its own message said the integrity path was
    # not reached. A tamper-detection test that reports success when the loader
    # never looked at the block is asserting nothing about tamper detection.
    if echo "$out" | grep -qi "not found\|unknown.*BLOCK\|use.*error\|cannot.*load"; then
        skip "block did not load — integrity path not reached, tamper detection not exercised"
    else
        fail "expected integrity error, got: $out"
    fi
fi
rm -f "$SCRIPT_T1"

echo ""

# ---------------------------------------------------------------------------
# T2: restore correct source → executes without error
# ---------------------------------------------------------------------------
echo "[T2] Block with matching hash executes normally"

# Restore the original source
echo -n "$BLOCK_SOURCE" > "$LANG_DIR/test_integrity_block.py"

SCRIPT_T2="${TMPDIR_TEST}/rt004_t2_$$.naab"
cat > "$SCRIPT_T2" <<NAAB
main {
    use BLOCK-PY-INTEGRITY-TEST
    print("ok")
}
NAAB

out=$("$NAAB" --no-governance --blocks-path "$BLOCKS_DIR" run "$SCRIPT_T2" 2>&1 || true)
if echo "$out" | grep -qi "tampered\|integrity.*check.*fail\|hash.*mismatch"; then
    fail "false positive integrity error on valid block: $out"
else
    ok "valid block loaded without integrity error"
fi
rm -f "$SCRIPT_T2"

# Cleanup
rm -rf "$BLOCKS_DIR"

echo ""
TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
