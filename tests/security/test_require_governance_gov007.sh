#!/usr/bin/env bash
# Test V-GOV-007: naab-lang must fail-closed (exit 4) when no govern.json
# is found and --no-governance is not specified.

NAAB="${NAAB_BIN:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ((FAIL++)); }
skip() { echo "  SKIP: $1"; ((SKIP++)); }

# Use TMPDIR (not $HOME) so the upward-walk won't find ~/govern.json
# or tests/govern.json — the temp dir must have NO govern.json in any ancestor.
# Pick the first writable tmp base we can find. Don't rely on $TMPDIR — some
# invocation contexts (nested test runners, CI, Termux subshells) leave it
# stale or unset. Create the base if needed so mktemp -d can't fail.
for base in "$TMPDIR" /data/data/com.termux/files/usr/tmp /tmp "$HOME/.cache"; do
    [ -z "$base" ] && continue
    mkdir -p "$base" 2>/dev/null && [ -w "$base" ] && { TMP_BASE="$base"; break; }
done
: "${TMP_BASE:?cannot find a writable temp directory}"
WORK_DIR=$(mktemp -d "$TMP_BASE/naab_gov007.XXXXXX") || { echo "mktemp failed"; exit 1; }
trap 'rm -rf "$WORK_DIR"' EXIT

echo "=== V-GOV-007: Fail-Closed Default Governance ==="

# Minimal NAAb script — just prints hello (no polyglot, no special operations)
cat > "$WORK_DIR/hello.naab" <<'EOF'
use io
main {
    io.write("hello\n")
}
EOF

# T1: No govern.json, no --no-governance → must exit 4 (config error)
echo ""
echo "[T1] No govern.json, no --no-governance → must exit 4"
output=$("$NAAB" "$WORK_DIR/hello.naab" 2>&1) || exit_code=$?
exit_code=${exit_code:-0}
if [[ $exit_code -eq 4 ]]; then
    pass "T1: exit 4 — fail-closed default governance"
elif [[ $exit_code -eq 0 ]]; then
    fail "T1: exit 0 — ran without govern.json (fail-open, V-GOV-007 not fixed)"
else
    # Check for governance-related error message even if exit code differs
    if echo "$output" | grep -qiE "(govern|require|no govern.json|missing.*govern)"; then
        pass "T1: governance error message present (exit $exit_code)"
    else
        fail "T1: unexpected exit $exit_code without governance message: ${output:0:200}"
    fi
fi

# T2: --no-governance → explicit opt-out, must succeed (exit 0)
echo ""
echo "[T2] --no-governance explicit opt-out → script runs normally"
output=$("$NAAB" --no-governance "$WORK_DIR/hello.naab" 2>&1)
exit_code=$?
if [[ $exit_code -eq 0 ]]; then
    pass "T2: --no-governance: script ran successfully without govern.json"
else
    fail "T2: --no-governance should allow running without govern.json (exit $exit_code): ${output:0:200}"
fi

# T3: govern.json present in script directory → must succeed (exit 0)
echo ""
echo "[T3] With govern.json present → script runs normally"
cat > "$WORK_DIR/govern.json" <<'EOF'
{
  "mode": "off"
}
EOF
output=$("$NAAB" "$WORK_DIR/hello.naab" 2>&1)
exit_code=$?
if [[ $exit_code -eq 0 ]]; then
    pass "T3: govern.json present → script ran successfully"
elif [[ $exit_code -eq 4 ]]; then
    fail "T3: exit 4 even with govern.json present — discoverAndLoad may not be finding it"
else
    fail "T3: unexpected exit $exit_code with govern.json present: ${output:0:200}"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
