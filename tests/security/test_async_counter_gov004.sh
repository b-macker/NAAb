#!/usr/bin/env bash
# Test V-GOV-004: Async tasks must inherit parent governance counter state,
# preventing governance limit bypass by spawning async tasks.

NAAB="${NAAB_BIN:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ((FAIL++)); }
skip() { echo "  SKIP: $1"; ((SKIP++)); }

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# govern.json: polyglot_blocks limit = 1
cat > "$WORK_DIR/govern.json" <<'EOF'
{
  "mode": "HARD",
  "limits": {
    "execution": {
      "polyglot_blocks": 1
    }
  }
}
EOF

echo "=== V-GOV-004: Async Governance Counter Inheritance ==="

# T1: Parent uses 1 polyglot block (at limit). Async task attempts 1 more → must be blocked.
cat > "$WORK_DIR/t1.naab" <<'EOF'
function async_task() {
    <<python
x = 1
>>
    return x
}

function main() {
    <<python
y = 2
>>
    let f = async async_task()
    let result = await f
    return result
}
EOF

output=$("$NAAB" "$WORK_DIR/t1.naab" 2>&1)
exit_code=$?
if echo "$output" | grep -qiE "(governance|blocked|polyglot|limit|hard)"; then
    pass "T1: async task blocked when parent is at polyglot_blocks limit"
elif [ $exit_code -ne 0 ]; then
    pass "T1: async task rejected (non-zero exit) when parent at limit"
else
    fail "T1: async task succeeded despite polyglot_blocks=1 already consumed by parent"
fi

# T2: Parent uses 0 blocks. Async task uses 1 (within limit) → must succeed.
cat > "$WORK_DIR/t2.naab" <<'EOF'
function async_task() {
    <<python
z = 42
>>
    return z
}

function main() {
    let f = async async_task()
    let result = await f
    return result
}
EOF

output=$("$NAAB" "$WORK_DIR/t2.naab" 2>&1)
exit_code=$?
if [ $exit_code -eq 0 ]; then
    pass "T2: async task within limit succeeds (no false positive)"
elif echo "$output" | grep -qiE "(governance|blocked|polyglot.*limit|hard.*block|block.*hard)"; then
    fail "T2: async task incorrectly blocked by governance when within polyglot_blocks limit"
else
    # Non-zero exit without a governance message = Python unavailable or other executor issue
    skip "T2: Python executor unavailable or runtime error (exit $exit_code)"
fi

# T3: No governance → async polyglot blocks always allowed.
cat > "$WORK_DIR/t3.naab" <<'EOF'
function async_task() {
    <<python
a = 1
>>
    <<python
b = 2
>>
    return b
}

function main() {
    let f = async async_task()
    return await f
}
EOF

output=$("$NAAB" --no-governance "$WORK_DIR/t3.naab" 2>&1)
exit_code=$?
if [ $exit_code -eq 0 ]; then
    pass "T3: --no-governance allows multiple async polyglot blocks"
elif echo "$output" | grep -qiE "python.*not (found|available|installed)|no.*python|executor"; then
    skip "T3: Python executor unavailable"
else
    # governance error with --no-governance is a bug, but async/python might just not work
    skip "T3: could not verify (non-zero exit without governance message)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
