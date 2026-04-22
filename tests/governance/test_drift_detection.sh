#!/bin/bash
# Test: Code drift detection gate
# Verifies that drift detection catches structural regression in .naab files.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="$(cd "$(dirname "$SCRIPT_DIR/../../build/naab-lang")" && pwd)/naab-lang"
PASSED=0
FAILED=0
TOTAL=0

pass() { PASSED=$((PASSED + 1)); TOTAL=$((TOTAL + 1)); echo "  PASS: $1"; }
fail() { FAILED=$((FAILED + 1)); TOTAL=$((TOTAL + 1)); echo "  FAIL: $1"; }

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_XXXXXX")
trap "rm -rf $WORK_DIR" EXIT

echo "=== Test: Drift Detection Gate ==="

# --- Setup: full-featured test file ---
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
function baz() { return 3 }
function helper() { return 4 }
export function api_call() { return foo() + bar() + baz() + helper() }
main { print(api_call()) }
NAAB_EOF

# --- T1: Save baseline ---
cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "max_function_loss": 0.5,
      "max_loc_loss": 0.6,
      "max_export_loss": 0.0,
      "max_struct_loss": 0.5
    }
  }
}
EOF

OUTPUT=$("$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ -f "$WORK_DIR/.naab/drift-baseline.json" ] && [ $RC -eq 0 ]; then
  pass "T1: --drift-baseline-save creates baseline file"
else
  fail "T1: baseline file not created (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -3)"
fi

# --- T2: Preserved file passes ---
OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T2: unchanged file passes drift check"
else
  fail "T2: unchanged file should pass (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# --- T3: Gutted file (100% function loss) → HARD block ---
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
main { print("hello") }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "functions dropped"; then
  pass "T3: gutted file blocked (exit 3, function loss detected)"
else
  fail "T3: expected exit 3 + function loss message (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# --- T4: Check specific deleted function names reported ---
if echo "$OUTPUT" | grep -q "function 'foo' was deleted" && \
   echo "$OUTPUT" | grep -q "function 'helper' was deleted"; then
  pass "T4: specific deleted function names reported"
else
  fail "T4: expected specific function deletion messages"
  echo "    Output: $(echo "$OUTPUT" | grep -i 'deleted')"
fi

# --- T5: Export deletion detected ---
if echo "$OUTPUT" | grep -q "exported function 'api_call' was deleted"; then
  pass "T5: export deletion reported as API regression"
else
  fail "T5: expected export deletion message"
  echo "    Output: $(echo "$OUTPUT" | grep -i 'export')"
fi

# --- T6: LOC loss detected ---
if echo "$OUTPUT" | grep -q "loc dropped"; then
  pass "T6: LOC loss detected"
else
  fail "T6: expected LOC loss message"
fi

# --- T7: Execution blocked (hello NOT printed) ---
if ! echo "$OUTPUT" | grep -q "^hello$"; then
  pass "T7: execution blocked — gutted code did not run"
else
  fail "T7: gutted code executed despite HARD block"
fi

# --- T8: New file (no baseline) passes ---
cat > "$WORK_DIR/new_file.naab" << 'NAAB_EOF'
main { print("new") }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/new_file.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ] && echo "$OUTPUT" | grep -q "new"; then
  pass "T8: new file (no baseline entry) passes"
else
  fail "T8: new file should pass (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -3)"
fi

# --- T9: Tree-walker path also blocks ---
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
main { print("hello") }
NAAB_EOF

OUTPUT=$("$NAAB" --tree-walk "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "functions dropped"; then
  pass "T9: tree-walker path also blocks gutted file"
else
  fail "T9: tree-walker should block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# --- T10: Partial loss within threshold passes ---
# Restore file with 3 functions (lost 2 of 5 = 40% < 50% threshold)
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
function baz() { return 3 }
export function api_call() { return foo() + bar() + baz() }
main { print(api_call()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T10: partial loss within threshold passes (40% < 50%)"
else
  fail "T10: partial loss within threshold should pass (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# --- T11: Partial loss exceeding threshold blocks ---
# Only 2 of 5 functions = 60% loss > 50% threshold
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
export function api_call() { return foo() }
main { print(api_call()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "functions dropped"; then
  pass "T11: 60% function loss exceeds 50% threshold → blocked"
else
  fail "T11: expected block for 60% loss (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# ===================================================================
# Phase 2: Extended drift gates
# ===================================================================

# Fresh baseline for extended gate tests
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
use math
use json

struct Player {
    name: string
    hp: int
    x: int
    y: int
    inventory: string
}

function test_math() {
    let passed = 0
    let total = 3
    if math.abs(-5) == 5 { passed = passed + 1 }
    if math.floor(3.7) == 3 { passed = passed + 1 }
    if math.ceil(2.1) == 3 { passed = passed + 1 }
    return [passed, total]
}

function test_json() {
    let passed = 0
    let total = 2
    let data = json.parse("{\"a\":1}")
    if data.get("a") == 1 { passed = passed + 1 }
    let s = json.stringify(data)
    if string.contains(s, "a") { passed = passed + 1 }
    return [passed, total]
}

function analyze_data(input, weights, threshold, mode) {
    let result = 0
    for i in 0..len(input) {
        let val = input[i]
        if val > threshold {
            result = result + val * weights[i]
        } else {
            result = result + val
        }
    }
    if mode == "normalized" {
        result = result / len(input)
    }
    return result
}

export function api_call(data, config) {
    let threshold = config.get("threshold") ?? 0
    let mode = config.get("mode") ?? "raw"
    let weights = config.get("weights") ?? [1, 1, 1]
    return analyze_data(data, weights, threshold, mode)
}

main {
    let data = [10, 20, 30]
    let config = {"threshold": 15, "mode": "normalized", "weights": [1, 2, 3]}
    print(api_call(data, config))
}
NAAB_EOF

# Save the rich baseline
OUTPUT=$("$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -ne 0 ]; then
  fail "T12-setup: failed to save extended baseline (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -3)"
fi

# --- T12: Signature stability — gut params ---
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
use math
use json

struct Player {
    name: string
    hp: int
    x: int
    y: int
    inventory: string
}

function test_math() {
    let passed = 0
    let total = 3
    if math.abs(-5) == 5 { passed = passed + 1 }
    if math.floor(3.7) == 3 { passed = passed + 1 }
    if math.ceil(2.1) == 3 { passed = passed + 1 }
    return [passed, total]
}

function test_json() {
    let passed = 0
    let total = 2
    let data = json.parse("{\"a\":1}")
    if data.get("a") == 1 { passed = passed + 1 }
    let s = json.stringify(data)
    if string.contains(s, "a") { passed = passed + 1 }
    return [passed, total]
}

function analyze_data(input) {
    return input
}

export function api_call(data) {
    return analyze_data(data)
}

main {
    let data = [10, 20, 30]
    print(api_call(data))
}
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "params dropped"; then
  pass "T12: signature stability — param gutting detected"
else
  fail "T12: expected param loss detection (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T13: Import regression — remove imports (isolated gate) ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
use math
use json
function helper() { return 1 }
main { print(helper()) }
NAAB_EOF

cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "max_function_loss": 1.0,
      "max_loc_loss": 1.0,
      "max_export_loss": 1.0,
      "max_struct_loss": 1.0,
      "check_imports": true,
      "max_import_loss": 0.0,
      "check_signatures": false,
      "check_complexity": false,
      "check_comment_ratio": false,
      "check_hollow_exports": false,
      "check_polyglot": false,
      "check_struct_fields": false,
      "check_test_functions": false
    }
  }
}
EOF

"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# Remove all imports
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function helper() { return 1 }
main { print(helper()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "imports dropped"; then
  pass "T13: import regression — removed imports detected"
else
  fail "T13: expected import loss detection (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T14: Import regression — report specific imports ---
if echo "$OUTPUT" | grep -q "import 'math' was removed"; then
  pass "T14: specific removed import 'math' reported"
else
  fail "T14: expected specific import removal message"
  echo "    Output: $(echo "$OUTPUT" | grep -i 'import')"
fi

# --- T15: Struct field stability — gut fields ---
# Reset baseline
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
struct Player {
    name: string
    hp: int
    x: int
    y: int
    inventory: string
}

function use_player() {
    return "ok"
}

main { print(use_player()) }
NAAB_EOF

cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "max_function_loss": 1.0,
      "max_loc_loss": 1.0,
      "max_export_loss": 1.0,
      "max_struct_loss": 1.0,
      "check_struct_fields": true,
      "max_field_loss": 0.5,
      "check_signatures": false,
      "check_imports": false,
      "check_complexity": false,
      "check_comment_ratio": false,
      "check_hollow_exports": false,
      "check_polyglot": false,
      "check_test_functions": false
    }
  }
}
EOF

"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# Now gut the struct
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
struct Player {
    name: string
    hp: int
}

function use_player() {
    return "ok"
}

main { print(use_player()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "struct.*fields dropped"; then
  pass "T15: struct field stability — field loss detected"
else
  fail "T15: expected struct field loss detection (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T16: Struct field — specific deleted fields reported ---
if echo "$OUTPUT" | grep -q "field 'inventory' was deleted"; then
  pass "T16: specific deleted struct field reported"
else
  fail "T16: expected specific field deletion message"
  echo "    Output: $(echo "$OUTPUT" | grep -i 'field')"
fi

# --- T17: Test function regression ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function test_alpha() { return [1, 1] }
function test_beta() { return [1, 1] }
function test_gamma() { return [1, 1] }
main {
    print("tests")
}
NAAB_EOF

cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "max_function_loss": 1.0,
      "max_loc_loss": 1.0,
      "max_struct_loss": 1.0,
      "max_export_loss": 1.0,
      "check_test_functions": true,
      "max_test_loss": 0.0,
      "check_signatures": false,
      "check_imports": false,
      "check_complexity": false,
      "check_comment_ratio": false,
      "check_hollow_exports": false,
      "check_polyglot": false,
      "check_struct_fields": false
    }
  }
}
EOF

"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# Remove 2 of 3 test functions
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function test_alpha() { return [1, 1] }
main {
    print("tests")
}
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "test functions dropped"; then
  pass "T17: test function regression — deleted tests detected"
else
  fail "T17: expected test function loss (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T18: Specific deleted test function reported ---
if echo "$OUTPUT" | grep -q "test function 'test_beta' was deleted"; then
  pass "T18: specific deleted test function reported"
else
  fail "T18: expected specific test deletion message"
  echo "    Output: $(echo "$OUTPUT" | grep -i 'test_beta')"
fi

# --- T19: All gates pass when file unchanged ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
use math
function test_a() { return [1, 1] }
function helper(x, y) {
    if x > y { return x }
    return y
}
export function api(data, config) { return helper(data, config) }
main { print(api(1, 2)) }
NAAB_EOF

cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "max_function_loss": 0.5,
      "max_loc_loss": 0.6,
      "max_export_loss": 0.0,
      "max_struct_loss": 0.5
    }
  }
}
EOF

"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1
OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T19: all gates pass when file unchanged"
else
  fail "T19: unchanged file should pass all gates (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T20: Function name stability — rename-and-gut detected ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function alpha(x) { return x + 1 }
function beta(x) { return x + 2 }
function gamma(x) { return x + 3 }
main { print(alpha(1) + beta(2) + gamma(3)) }
NAAB_EOF

cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_function_names": true,
      "max_function_name_loss": 0.5
    }
  }
}
EOF

"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# Now rename 2 of 3 functions (keep count the same, but names change)
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function alpha(x) { return x + 1 }
function probe_one(x) { return x }
function probe_two(x) { return x }
main { print(alpha(1)) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "function names lost"; then
  pass "T20: function name stability — rename-and-gut detected"
else
  fail "T20: expected block for function name loss (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T21: Specific renamed function reported ---
if echo "$OUTPUT" | grep -qi "beta.*renamed\|beta.*deleted"; then
  pass "T21: specific renamed function 'beta' reported"
else
  fail "T21: expected specific function rename message"
  echo "    Output: $(echo "$OUTPUT" | grep -i 'drift')"
fi

# --- T22: Baseline tamper protection — missing baseline blocked when require_baseline=true ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
main { print(foo()) }
NAAB_EOF

cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "require_baseline": true
    }
  }
}
EOF

# Do NOT save baseline — it should fail-closed
OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "baseline.*missing\|baseline.*deleted"; then
  pass "T22: missing baseline blocked when require_baseline=true"
else
  fail "T22: expected block for missing baseline (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T23: require_baseline=false still allows missing baseline (backward compat) ---
cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "require_baseline": false
    }
  }
}
EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T23: missing baseline allowed when require_baseline=false"
else
  fail "T23: expected pass with require_baseline=false (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T24: --drift-baseline-save blocked when in blocked_flags ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
main { print(foo()) }
NAAB_EOF

cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": { "enabled": true, "level": "hard" }
  },
  "integrity": {
    "blocked_flags": ["--drift-baseline-save"]
  }
}
EOF

OUTPUT=$("$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "blocked.*drift-baseline-save\|prohibited\|drift-baseline-save.*locked"; then
  pass "T24: --drift-baseline-save blocked by integrity.blocked_flags"
else
  fail "T24: expected block for blocked flag (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T25: Signed baseline passes verification ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function alpha(x) { return x + 1 }
function beta(x) { return x + 2 }
main { print(alpha(1) + beta(2)) }
NAAB_EOF

cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": { "enabled": true, "level": "hard" }
  }
}
EOF

# Save baseline with key (auto-signs)
export NAAB_GOVERN_KEY="test-secret-key-12345"
"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# Verify .sig was created
if [ -f "$WORK_DIR/.naab/drift-baseline.json.sig" ]; then
  # Run with key — should pass
  OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
  RC=$?
  if [ $RC -eq 0 ]; then
    pass "T25: signed baseline passes verification"
  else
    fail "T25: signed baseline should pass (rc=$RC)"
    echo "    Output: $(echo "$OUTPUT" | head -8)"
  fi
else
  fail "T25: .sig file not created"
fi

# --- T26: Tampered baseline (modified after signing) blocked ---
# Tamper with the baseline file
echo '{"tampered": true}' >> "$WORK_DIR/.naab/drift-baseline.json"
OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "signature.*mismatch\|tampered\|modified since.*signed"; then
  pass "T26: tampered baseline blocked (signature mismatch)"
else
  fail "T26: expected block for tampered baseline (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi
unset NAAB_GOVERN_KEY

# --- T27: .sig exists but no key → runs but locked (trust signed config) ---
# .sig still exists from T25, but key is now unset
OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ] && echo "$OUTPUT" | grep -qi "locked\|NOTICE"; then
  pass "T27: missing key with .sig → runs normally, mutation locked"
else
  fail "T27: expected successful run with lock notice (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T28: Unsigned mode (no key, no sig) still works ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": { "enabled": true, "level": "hard" }
  }
}
EOF

# Save baseline without key (no signing)
"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# No .sig file should exist
if [ ! -f "$WORK_DIR/.naab/drift-baseline.json.sig" ]; then
  OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
  RC=$?
  if [ $RC -eq 0 ]; then
    pass "T28: unsigned mode works (backward compat)"
  else
    fail "T28: unsigned mode should pass (rc=$RC)"
    echo "    Output: $(echo "$OUTPUT" | head -8)"
  fi
else
  fail "T28: .sig should not exist without key"
fi

echo ""
echo "=== Results: $PASSED/$TOTAL passed, $FAILED failed ==="
exit $FAILED
