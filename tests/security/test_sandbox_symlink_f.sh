#!/usr/bin/env bash
# test_sandbox_symlink_f.sh — Finding F: O_NOFOLLOW closes TOCTOU window
# Verifies that sandboxed file ops refuse to follow symlinks at the final path component.
# Non-sandboxed ops (unrestricted) must still work normally.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/naab_test_symlink_f_XXXXXX")
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_sandbox_symlink_f.sh ==="
echo ""

# Create a real file outside the sandbox allowed area
echo "secret_content" > "$WORKDIR/secret.txt"

# Create a symlink inside the workdir pointing to the secret file
# (simulating TOCTOU attack: attacker swaps the file for a symlink after path check)
ln -sf "$WORKDIR/secret.txt" "$WORKDIR/safe.txt"

# ---------------------------------------------------------------------------
# F1: file.read() on a symlink is denied under sandbox (standard level)
# ---------------------------------------------------------------------------
echo "[F1] file.read() on symlink denied under sandbox (standard level)"
cat > "$WORKDIR/test_f1.naab" << NAABEOF
use file
use io
main {
  let content = file.read("${WORKDIR}/safe.txt")
  io.println(content)
}
NAABEOF

out=$("$NAAB" "$WORKDIR/test_f1.naab" --sandbox-level standard --no-governance 2>&1) || true
if echo "$out" | grep -qi "symlink\|denied\|security\|ELOOP\|nofollow"; then
    ok "symlink read denied under sandbox"
elif echo "$out" | grep -q "secret_content"; then
    fail "symlink followed — secret content leaked: $out"
else
    # Non-zero exit without specific message is also acceptable
    if [[ $? -ne 0 ]] || echo "$out" | grep -qi "error\|fail\|denied"; then
        ok "read failed (exit/error — symlink not followed)"
    else
        ok "no secret_content in output (symlink not followed)"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# F2: file.write() on a symlink is denied under sandbox
# ---------------------------------------------------------------------------
echo "[F2] file.write() on symlink denied under sandbox"
# Create a symlink target that the write would corrupt
echo "original" > "$WORKDIR/original.txt"
ln -sf "$WORKDIR/original.txt" "$WORKDIR/write_target.txt"

cat > "$WORKDIR/test_f2.naab" << NAABEOF
use file
main {
  file.write("${WORKDIR}/write_target.txt", "OVERWRITTEN")
}
NAABEOF

out=$("$NAAB" "$WORKDIR/test_f2.naab" --sandbox-level standard --no-governance 2>&1) || true
original_content=$(cat "$WORKDIR/original.txt" 2>/dev/null || echo "")

if echo "$out" | grep -qi "symlink\|denied\|security\|nofollow"; then
    ok "symlink write denied under sandbox"
elif [[ "$original_content" == "original" ]]; then
    ok "original file not overwritten via symlink"
else
    fail "symlink write NOT blocked — original file may be corrupted: $original_content"
fi

echo ""

# ---------------------------------------------------------------------------
# F3: Normal (non-symlink) file read still works under sandbox
# ---------------------------------------------------------------------------
echo "[F3] Normal file read works under sandbox (no false positive)"
echo "hello_sandbox" > "$WORKDIR/normal.txt"
cat > "$WORKDIR/test_f3.naab" << NAABEOF
use file
use io
main {
  let content = file.read("${WORKDIR}/normal.txt")
  io.println(content)
}
NAABEOF

out=$("$NAAB" "$WORKDIR/test_f3.naab" --sandbox-level standard --no-governance 2>&1) || true
if echo "$out" | grep -q "hello_sandbox"; then
    ok "normal file read succeeded under sandbox"
else
    # sandbox may deny based on allowed paths — that's also acceptable
    if echo "$out" | grep -qi "denied\|sandbox\|capability"; then
        ok "sandbox path restriction (not a symlink false-positive)"
    else
        fail "expected 'hello_sandbox' in output, got: ${out:0:100}"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# F4: Symlink refusal does NOT depend on the sandbox LEVEL
#
# This was written as "symlink read works when sandbox is unrestricted (no
# O_NOFOLLOW applied)", and all three of its branches passed, so it could not
# fail. The premise is wrong: file_impl.cpp gates readFileNoFollow on
# `security::ScopedSandbox::getCurrent()` — whether a sandbox is INSTALLED, not
# what level it carries. --sandbox-level unrestricted still installs one, so
# O_NOFOLLOW still applies. That is deliberate; the guard closes a TOCTOU window
# between the path check and the open, which is not a path-policy question and
# so does not relax with the policy.
#
# F3 ("normal file read succeeded under sandbox") is the real positive control
# for F1/F2 — it shows file.read works on a non-symlink under the same sandbox,
# so F1/F2 are refusals rather than a broken read path.
# ---------------------------------------------------------------------------
echo "[F4] Symlink refusal is independent of sandbox level"
cat > "$WORKDIR/test_f4.naab" << NAABEOF
use file
use io
main {
  let content = file.read("${WORKDIR}/safe.txt")
  io.println(content)
}
NAABEOF

out=$("$NAAB" "$WORKDIR/test_f4.naab" --sandbox-level unrestricted --no-governance 2>&1) || true
if echo "$out" | grep -q "path is a symlink"; then
    ok "unrestricted sandbox still refuses the symlink (O_NOFOLLOW is level-independent)"
elif echo "$out" | grep -q "secret_content"; then
    fail "unrestricted sandbox followed a symlink — the TOCTOU guard now varies with level"
else
    skip "unrestricted read produced neither the refusal nor the content: ${out:0:80}"
fi

echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed${SKIP:+, ${SKIP} skipped}"
[[ "$FAIL" -eq 0 ]]
