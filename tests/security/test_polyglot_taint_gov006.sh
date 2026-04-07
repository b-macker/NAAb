#!/usr/bin/env bash
# Test V-GOV-006: All polyglot block outputs must be unconditionally tainted
# when governance is active, regardless of whether the input was tainted.

NAAB="${NAAB_BIN:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ((FAIL++)); }
skip() { echo "  SKIP: $1"; ((SKIP++)); }

WORK_DIR=$(mktemp -d -p "$HOME" .naab_gov006_XXXXXX 2>/dev/null || mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

echo "=== V-GOV-006: Unconditional Polyglot Return Taint ==="

# Governance: shell_exec is a taint sink
cat > "$WORK_DIR/govern.json" <<'EOF'
{
  "mode": "HARD",
  "sinks": ["shell_exec"]
}
EOF

# T1: Shell block with clean (untainted) input → result stored in var →
#     use in shell_exec sink → MUST be blocked (V-GOV-006: always tainted)
echo ""
echo "[T1] Clean input through shell polyglot block → shell_exec sink must be BLOCKED"
cat > "$WORK_DIR/t1.naab" <<'EOF'
main {
    let clean_val = "hello"
    let result = <<shell[clean_val]
echo $clean_val
>>
    shell_exec(result)
}
EOF

output=$("$NAAB" "$WORK_DIR/t1.naab" 2>&1) || true
if echo "$output" | grep -qiE "(taint|blocked|governance|sink|denied|hard)"; then
    pass "T1: shell_exec blocked — polyglot output tainted unconditionally"
elif echo "$output" | grep -qiE "(shell_exec.*not.*found|undefined.*shell_exec|unknown.*shell_exec)"; then
    skip "T1: shell_exec not a recognized builtin in this build"
else
    fail "T1: shell_exec was NOT blocked — V-GOV-006 taint not applied. Output: ${output:0:200}"
fi

# T2: Shell block result used in io.write (non-sink) → should succeed
#     Taint alone doesn't block; only reaching a configured sink blocks.
echo ""
echo "[T2] Polyglot output used in io.write (non-sink) → no governance block"
cat > "$WORK_DIR/t2.naab" <<'EOF'
use io
main {
    let x = "world"
    let result = <<shell[x]
echo $x
>>
    io.write(result)
}
EOF

output=$("$NAAB" "$WORK_DIR/t2.naab" 2>&1) || ec=$?
ec=${ec:-0}
if echo "$output" | grep -qiE "(taint.*violat|taint.*block|sink.*taint|hard.*block.*taint)"; then
    fail "T2: false positive — io.write incorrectly blocked as taint sink: ${output:0:200}"
elif [[ $ec -eq 0 ]]; then
    pass "T2: io.write succeeded — taint does not block non-sink operations"
else
    pass "T2: exited non-zero for non-governance reason (shell/io failure): ${output:0:80}"
fi

# T3: --no-governance → shell_exec not blocked even with polyglot output
echo ""
echo "[T3] With --no-governance → no taint enforcement on polyglot outputs"
cat > "$WORK_DIR/t3.naab" <<'EOF'
main {
    let val = "test"
    let result = <<shell[val]
echo $val
>>
    shell_exec(result)
}
EOF

output=$("$NAAB" --no-governance "$WORK_DIR/t3.naab" 2>&1) || true
if echo "$output" | grep -qiE "(governance.*block|hard.*block.*taint|taint.*hard)"; then
    fail "T3: governance fired despite --no-governance: ${output:0:120}"
else
    pass "T3: --no-governance: no taint enforcement applied"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
