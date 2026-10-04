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
# Read the strings ONCE and match without a pipe. Under `set -o pipefail`,
# `strings | grep -q` reports failure exactly when the text IS found (grep -q
# exits at the match, strings takes SIGPIPE, pipefail returns that), so this
# probe declared OpenSSL absent on every build that has it and skipped every
# arm. "tampered" alone matched the govern.json signature message, unrelated.
NAAB_STRINGS="$(strings "$NAAB" 2>/dev/null || true)"
if [[ "$NAAB_STRINGS" != *"Block integrity check failed"* ]]; then
    skip "OpenSSL not compiled in — block integrity checks inactive"
    skip "T1: skipped (HAVE_OPENSSL not set)"
    skip "T2: skipped (HAVE_OPENSSL not set)"
    echo ""
    echo "Results: 0/0 (all skipped — build without OpenSSL)"
    exit 0
fi


# ---------------------------------------------------------------------------
# Setup. The registry reads blocks from $HOME/.naab/language/blocks/library/,
# and `use BLOCK-...` is a top-level statement the tree-walker supports (the VM
# refuses block loading). This suite used to pass `--blocks-path`, which does
# not exist ("Unknown command"), with `use` inside main -- no program ever ran,
# so tamper detection was never exercised. Each scenario gets its own HOME.
# ---------------------------------------------------------------------------
# Absolute, because every run below cds into its work dir first.
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/rt004.XXXXXX")"
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$(dirname "$0")/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$WORK"; teardown_isolated_trust' EXIT
echo '{ "version": "4.0", "mode": "off" }' > "$WORK/govern.json"
cat > "$WORK/prog.naab" <<'NAAB'
use BLOCK-PY-INTEGRITY-TEST as b
main {
    print("PROGRAM_RAN")
}
NAAB

# Each block leaves a MARKER FILE, and the arms assert on the file. A block's
# stdout is captured rather than forwarded on some builds (build-linux saw no
# "42" from print(42) while the program around it ran), so stdout cannot show
# that the block ran -- or, for the tampered source, that it did NOT run.
RAN_MARK="$WORK/block_ran"
TAMPER_MARK="$WORK/tampered_ran"
BLOCK_SOURCE="open('$RAN_MARK', 'w').write('ran')"
TAMPERED_SOURCE="import os; open('$TAMPER_MARK', 'w').write('x'); os.system('echo TAMPERED_RAN')"
REAL_HASH=$(printf '%s' "$BLOCK_SOURCE" | sha256sum | awk '{print $1}')

# new_home <name> -> fresh HOME holding the block with its correct hash; sets LIB
new_home() {
    H="$WORK/$1"
    LIB="$H/.naab/language/blocks/library"
    mkdir -p "$LIB/python"
    printf '%s' "$BLOCK_SOURCE" > "$LIB/python/test_integrity_block.py"
    cat > "$LIB/python/test_integrity_block.json" <<EOF
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
}
run_prog() { rm -f "$RAN_MARK" "$TAMPER_MARK"; (cd "$WORK" && HOME="$H" timeout 60 "$NAAB" --tree-walk prog.naab 2>&1 || true); }
tamper_ran() { [ -e "$TAMPER_MARK" ] || echo "$1" | grep -q "TAMPERED_RAN"; }
refused() { echo "$1" | grep -qi "tampered\|integrity.*check.*fail\|hash.*mismatch\|code_hash"; }

# T0 CONTROL: the untampered block loads and runs -- the harness reaches the
# block at all. Without it, every refusal below could be a load that never
# happened.
echo "[T0] CONTROL: the untampered block loads and runs"
new_home t0
out=$(run_prog)
if [ -f "$RAN_MARK" ] && echo "$out" | grep -q "PROGRAM_RAN"; then
    ok "untampered block loaded, ran (wrote its marker) and the program continued"
else
    fail "the untampered block did not run -- the arms below prove nothing: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T1: tamper the source file after registration → must be rejected
# ---------------------------------------------------------------------------
echo "[T1] Tampered block source rejected with integrity error"
new_home t1
echo "$TAMPERED_SOURCE" > "$LIB/python/test_integrity_block.py"
out=$(run_prog)
if refused "$out" && ! tamper_ran "$out"; then
    ok "tampered block rejected with integrity error"
else
    fail "expected integrity error, got: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T2: restore correct source → executes without error
# ---------------------------------------------------------------------------
echo "[T2] Block with matching hash executes normally"
printf '%s' "$BLOCK_SOURCE" > "$LIB/python/test_integrity_block.py"
out=$(run_prog)
if refused "$out"; then
    fail "false positive integrity error on valid block: ${out:0:200}"
elif echo "$out" | grep -q "PROGRAM_RAN"; then
    ok "valid block loaded without integrity error"
else
    fail "valid block did not run: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T3: tamper AFTER a clean run → still rejected. The registry caches block
# metadata in .block_cache.json, and the cache dropped code_hash: from the
# second run on, every block had an empty hash and the integrity check was
# skipped. Editing a file does not change its directory's mtime, so the stale
# cache was never invalidated -- the tampered source ran.
# ---------------------------------------------------------------------------
echo "[T3] Tampering after a clean run (cached metadata) is still rejected"
new_home t3
out=$(run_prog)                       # clean run: writes the metadata cache
if ! echo "$out" | grep -q "PROGRAM_RAN" || [ ! -f "$LIB/.block_cache.json" ]; then
    fail "the clean run did not run or wrote no cache -- T3 cannot be judged: ${out:0:200}"
else
    echo "$TAMPERED_SOURCE" > "$LIB/python/test_integrity_block.py"
    out=$(run_prog)
    if refused "$out" && ! tamper_ran "$out"; then
        ok "tampering after a cached clean run is rejected"
    else
        fail "the tampered block ran from cached metadata (integrity check skipped): ${out:0:200}"
    fi
fi
echo ""

# ---------------------------------------------------------------------------
# T4: a cache written in the old format (no code_hash) is not trusted.
# ---------------------------------------------------------------------------
echo "[T4] An old-format metadata cache without code_hash is not trusted"
new_home t4
printf '{"version":1,"blocks":{"BLOCK-PY-INTEGRITY-TEST":{"name":"integrity_test","language":"python","file_path":"%s"}}}' \
    "$LIB/python/test_integrity_block.json" > "$LIB/.block_cache.json"
touch -d '+1 minute' "$LIB/.block_cache.json" 2>/dev/null || true   # newer than the language dir
echo "$TAMPERED_SOURCE" > "$LIB/python/test_integrity_block.py"
out=$(run_prog)
if refused "$out" && ! tamper_ran "$out"; then
    ok "old-format cache discarded; tampered block rejected"
else
    fail "an old-format cache let the tampered block run: ${out:0:200}"
fi

echo ""
TOTAL=$(( PASS + FAIL ))
echo "Results: ${PASS}/${TOTAL} passed"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
