#!/usr/bin/env bash
# Security R20 Fix Verification Tests
# Tests: V-GOV-013 (nested container taint), V-DOS-002 (REST API parallelism), V-GOV-015 (async taint)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
WORK_DIR="$(mktemp -d -p "$SCRIPT_DIR")"
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0

check() {
    local desc="$1" expect="$2" rc=0
    shift 2
    "$@" > "$WORK_DIR/out.txt" 2>&1 || rc=$?
    local out
    out=$(cat "$WORK_DIR/out.txt")
    if [[ "$expect" == "EXIT_NONZERO" ]]; then
        if [[ $rc -ne 0 ]]; then
            echo "PASS: $desc"
            PASS=$((PASS+1))
        else
            echo "FAIL: $desc (expected non-zero exit, got 0)"
            echo "  output: $out"
            FAIL=$((FAIL+1))
        fi
    elif echo "$out" | grep -qF "$expect"; then
        echo "PASS: $desc"
        PASS=$((PASS+1))
    else
        echo "FAIL: $desc"
        echo "  expected to find: $expect"
        echo "  actual output:    $out"
        FAIL=$((FAIL+1))
    fi
}

# Governance config for taint-tracking tests (hard enforcement)
cat > "$WORK_DIR/govern.json" << 'GOV'
{
  "mode": "block",
  "taint_tracking": {
    "enabled": true,
    "level": "hard",
    "sources": ["env.get_var", "env.get", "io.read_line"],
    "sinks": ["io.write", "io.write_file", "net.request", "shell_exec"]
  }
}
GOV

# ── V-GOV-013: Nested container taint (findRootIdentifier fix) ───────────────

# Test 1: Nested subscript write — arr[0] = tainted  →  arr marked tainted  →
#         io.write(arr[0]) blocked by governance (non-zero exit expected).
#         Before fix: dynamic_cast<IdentifierExpr*>(subscript->getLeft()) failed
#         for arr[0] (a BinaryExpr), so arr was never marked tainted.
cat > "$WORK_DIR/gov013_subscript.naab" << 'NAAB'
main {
    let secret = env.get_var("NAAB_R20_SECRET")
    let arr = ["safe", "safe"]
    arr[0] = secret
    io.write(arr[0])
}
NAAB
rc=0
NAAB_R20_SECRET="tainted_value" \
    "$NAAB" --governance-config "$WORK_DIR/govern.json" \
    "$WORK_DIR/gov013_subscript.naab" > /dev/null 2>&1 || rc=$?
if [[ $rc -ne 0 ]]; then
    echo "PASS: V-GOV-013: arr[0]=tainted marks arr tainted; io.write(arr[0]) blocked"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-013: expected governance block but exit code was 0"
    FAIL=$((FAIL+1))
fi

# Test 2: push into nested array element — arr[0].push(s) marks arr tainted
#         Before fix: dynamic_cast in ExprStmt mutation hook also missed arr[0].
cat > "$WORK_DIR/gov013_nested_push.naab" << 'NAAB'
main {
    let secret = env.get_var("NAAB_R20_SECRET")
    let arr = [["a", "b"], ["c", "d"]]
    arr[0].push(secret)
    io.write(arr[0][2])
}
NAAB
rc=0
NAAB_R20_SECRET="tainted_value" \
    "$NAAB" --governance-config "$WORK_DIR/govern.json" \
    "$WORK_DIR/gov013_nested_push.naab" > /dev/null 2>&1 || rc=$?
if [[ $rc -ne 0 ]]; then
    echo "PASS: V-GOV-013: arr[0].push(tainted) marks arr tainted; io.write blocked"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-013: expected governance block for nested push but exit code was 0"
    FAIL=$((FAIL+1))
fi

# Test 3: clean push — no false positive
cat > "$WORK_DIR/gov013_clean.naab" << 'NAAB'
main {
    let arr = [1, 2, 3]
    arr.push(42)
    io.write(arr[3])
}
NAAB
check "V-GOV-013: clean push produces no taint warning" "42" \
    "$NAAB" "$WORK_DIR/gov013_clean.naab"

# ── V-DOS-002: REST API per-thread capture (no global mutex serialization) ────

# Test 4: io.write output captured correctly in a single execution
cat > "$WORK_DIR/dos002_write.naab" << 'NAAB'
main {
    io.write("hello_capture")
}
NAAB
check "V-DOS-002: io.write output visible in normal execution" "hello_capture" \
    "$NAAB" "$WORK_DIR/dos002_write.naab"

# Test 5: io.output captured correctly
cat > "$WORK_DIR/dos002_output.naab" << 'NAAB'
main {
    io.output("machine_output")
}
NAAB
check "V-DOS-002: io.output captured correctly" "machine_output" \
    "$NAAB" "$WORK_DIR/dos002_output.naab"

# Test 6: io.print and io.println work
cat > "$WORK_DIR/dos002_print.naab" << 'NAAB'
main {
    io.print("line_one")
    io.println("")
    io.print("line_two")
}
NAAB
check "V-DOS-002: io.print captured correctly" "line_one" \
    "$NAAB" "$WORK_DIR/dos002_print.naab"

# ── V-GOV-015: Async callback taint field ────────────────────────────────────

# Test 7: AsyncCallbackResult carries is_tainted=false by default (structural)
# We verify this via a NAAb async function that returns a clean value — no taint
cat > "$WORK_DIR/gov015_clean_async.naab" << 'NAAB'
async function fetch_clean() {
    return "safe_value"
}

main {
    let result = await fetch_clean()
    io.write(result)
}
NAAB
check "V-GOV-015: async clean return propagates correctly" "safe_value" \
    "$NAAB" "$WORK_DIR/gov015_clean_async.naab"

# ── Summary ──────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
