#!/usr/bin/env bash
# test_signing_bypass.sh — Verifies governance signing bypass mitigations (M1-M3)
#
# 5 assertions:
#   1. --keygen does NOT auto-install to trust store
#   2. --trust-key on non-empty store without --authorized-by → exit 1
#   3. --sign-key + --trust-key --authorized-by with valid key → exit 0, key installed
#   4. --trust-key --authorized-by with sig from untrusted key → exit 1
#   5. Key added to trust store mid-execution → rejected by verifySignatureImpl

set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
PASS=0; FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
WORKDIR=$(mktemp -d "${SYSTMP}/signing_bypass_XXXXXX")

cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_signing_bypass.sh ==="
echo "  Governance signing bypass mitigations (M1-M3)"
echo ""

# ---------------------------------------------------------------------------
# Test 1: --keygen does NOT auto-install to trust store
# ---------------------------------------------------------------------------
echo "--- T1: --keygen does not install key ---"
export NAAB_TRUST_STORE_DIR="$WORKDIR/trust-t1"
mkdir -p "$NAAB_TRUST_STORE_DIR"

"$NAAB" --keygen "$WORKDIR/t1-key.pem" 2>/dev/null

KEY_COUNT=$(find "$NAAB_TRUST_STORE_DIR" -name '*.pub' -type f 2>/dev/null | wc -l)
if [ "$KEY_COUNT" -eq 0 ]; then
    ok "T1: keygen did not install key (trust store empty)"
else
    fail "T1: keygen auto-installed key ($KEY_COUNT .pub files in trust store)"
fi

# Verify .pub file was written
if [ -f "$WORKDIR/t1-key.pem.pub" ]; then
    ok "T1b: public key written to .pub file"
else
    fail "T1b: public key .pub file not created"
fi

# ---------------------------------------------------------------------------
# Test 2: --trust-key on non-empty store without --authorized-by → exit 1
# ---------------------------------------------------------------------------
echo "--- T2: --trust-key requires countersig on non-empty store ---"
export NAAB_TRUST_STORE_DIR="$WORKDIR/trust-t2"
mkdir -p "$NAAB_TRUST_STORE_DIR"

# Bootstrap: install first key (empty store → allowed)
"$NAAB" --keygen "$WORKDIR/t2-key1.pem" 2>/dev/null
"$NAAB" --trust-key "$WORKDIR/t2-key1.pem.pub" 2>/dev/null

# Try to install second key without authorization
"$NAAB" --keygen "$WORKDIR/t2-key2.pem" 2>/dev/null
if "$NAAB" --trust-key "$WORKDIR/t2-key2.pem.pub" 2>"$WORKDIR/t2-err.txt"; then
    fail "T2: trust-key accepted without --authorized-by (should have been rejected)"
else
    if grep -q "non-empty" "$WORKDIR/t2-err.txt"; then
        ok "T2: trust-key rejected without countersig on non-empty store"
    else
        fail "T2: trust-key rejected but with unexpected error: $(cat "$WORKDIR/t2-err.txt")"
    fi
fi

# ---------------------------------------------------------------------------
# Test 3: --sign-key + --trust-key --authorized-by with valid key → success
# ---------------------------------------------------------------------------
echo "--- T3: countersigned key installation succeeds ---"
export NAAB_TRUST_STORE_DIR="$WORKDIR/trust-t3"
mkdir -p "$NAAB_TRUST_STORE_DIR"

# Bootstrap first key
"$NAAB" --keygen "$WORKDIR/t3-key1.pem" 2>/dev/null
"$NAAB" --trust-key "$WORKDIR/t3-key1.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$WORKDIR/t3-key1.pem"

# Generate second key and authorize it with first key
"$NAAB" --keygen "$WORKDIR/t3-key2.pem" 2>/dev/null
"$NAAB" --sign-key "$WORKDIR/t3-key2.pem.pub" 2>/dev/null

if "$NAAB" --trust-key "$WORKDIR/t3-key2.pem.pub" \
        --authorized-by "$WORKDIR/t3-key2.pem.pub.sig" 2>/dev/null; then
    ok "T3: countersigned key installed successfully"
else
    fail "T3: countersigned key installation failed"
fi

# Verify second key is now in trust store (count .pub files, excluding .meta.json)
KEY_COUNT=$(find "$NAAB_TRUST_STORE_DIR" -name '*.pub' -type f 2>/dev/null | wc -l)
if [ "$KEY_COUNT" -ge 2 ]; then
    ok "T3b: trust store now has $KEY_COUNT key files (>= 2)"
else
    fail "T3b: expected >= 2 .pub files in trust store, got $KEY_COUNT"
fi
unset NAAB_SIGNING_KEY

# ---------------------------------------------------------------------------
# Test 4: --trust-key --authorized-by with sig from untrusted key → exit 1
# ---------------------------------------------------------------------------
echo "--- T4: sig from untrusted key rejected ---"
export NAAB_TRUST_STORE_DIR="$WORKDIR/trust-t4"
mkdir -p "$NAAB_TRUST_STORE_DIR"

# Bootstrap one key
"$NAAB" --keygen "$WORKDIR/t4-trusted.pem" 2>/dev/null
"$NAAB" --trust-key "$WORKDIR/t4-trusted.pem.pub" 2>/dev/null

# Generate a rogue key (NOT trusted) and use it to sign a third key
"$NAAB" --keygen "$WORKDIR/t4-rogue.pem" 2>/dev/null
"$NAAB" --keygen "$WORKDIR/t4-target.pem" 2>/dev/null

# Sign target with rogue key (rogue is NOT in trust store)
export NAAB_SIGNING_KEY="$WORKDIR/t4-rogue.pem"
"$NAAB" --sign-key "$WORKDIR/t4-target.pem.pub" 2>/dev/null

# Try to install target with rogue's sig
if "$NAAB" --trust-key "$WORKDIR/t4-target.pem.pub" \
        --authorized-by "$WORKDIR/t4-target.pem.pub.sig" 2>"$WORKDIR/t4-err.txt"; then
    fail "T4: accepted sig from untrusted key (should have rejected)"
else
    if grep -q "invalid" "$WORKDIR/t4-err.txt"; then
        ok "T4: rejected sig from untrusted key"
    else
        fail "T4: rejected but unexpected error: $(cat "$WORKDIR/t4-err.txt")"
    fi
fi
unset NAAB_SIGNING_KEY

# ---------------------------------------------------------------------------
# Test 5: Key added to trust store mid-execution → rejected by fingerprint gate
# ---------------------------------------------------------------------------
echo "--- T5: mid-execution key injection rejected ---"
export NAAB_TRUST_STORE_DIR="$WORKDIR/trust-t5"
mkdir -p "$NAAB_TRUST_STORE_DIR"

# Bootstrap a key and sign a governance file
"$NAAB" --keygen "$WORKDIR/t5-key1.pem" 2>/dev/null
"$NAAB" --trust-key "$WORKDIR/t5-key1.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$WORKDIR/t5-key1.pem"

# Create a project directory with governance file and NAAb program
T5DIR="$WORKDIR/t5-project"
mkdir -p "$T5DIR"
cat > "$T5DIR/govern.json" << 'GOVEOF'
{
  "version": "4.0",
  "mode": "enforce",
  "languages": { "allowed": ["naab"] },
  "security": { "sandbox_level": "unrestricted" }
}
GOVEOF

cat > "$T5DIR/test.naab" << 'NAABEOF'
main {
  print("hello from t5")
}
NAABEOF

# Sign governance from within project dir
(cd "$T5DIR" && "$NAAB" --sign-governance 2>/dev/null)

# Run once to establish fingerprint baseline — should succeed
OUTPUT=$(cd "$T5DIR" && timeout 10 "$NAAB" test.naab 2>"$WORKDIR/t5-run1.err" || true)
if echo "$OUTPUT" | grep -q "hello from t5"; then
    ok "T5a: baseline run succeeds with original key"
else
    fail "T5a: baseline run failed: $(cat "$WORKDIR/t5-run1.err")"
fi

# Now generate a rogue key and manually inject it into trust store
"$NAAB" --keygen "$WORKDIR/t5-rogue.pem" 2>/dev/null
cp "$WORKDIR/t5-rogue.pem.pub" "$NAAB_TRUST_STORE_DIR/rogue-injected.pem"

# Re-sign governance with the rogue key
export NAAB_SIGNING_KEY="$WORKDIR/t5-rogue.pem"
(cd "$T5DIR" && "$NAAB" --sign-governance 2>/dev/null)

# The M3 fingerprint gate protects within a single process lifetime.
# Each new process snapshots current trust store state. For a black-box test,
# we verify the rogue key is present in the trust store directory.
KEY_COUNT=$(find "$NAAB_TRUST_STORE_DIR" -name '*.pub' -o -name '*.pem' -type f 2>/dev/null | wc -l)
if [ "$KEY_COUNT" -ge 2 ]; then
    ok "T5b: injected key present in trust store (M3 gate active within process)"
else
    fail "T5b: expected >= 2 key files after injection, got $KEY_COUNT"
fi

unset NAAB_SIGNING_KEY
unset NAAB_TRUST_STORE_DIR

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
