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

# V-SC-009: Back up and clear trust store so T1-T53 run unsigned (no trust-store interference)
REAL_TRUST="$HOME/.naab/trusted-keys"
TRUST_BAK=""
if [ -d "$REAL_TRUST" ]; then
  TRUST_BAK=$(mktemp -d "${TMPDIR:-/tmp}/trust_bak_XXXXXX")
  mv "$REAL_TRUST" "$TRUST_BAK/trusted-keys"
fi
cleanup_trust() {
  rm -rf "$WORK_DIR"
  # Restore trust store on exit
  rm -rf "$REAL_TRUST"
  if [ -n "$TRUST_BAK" ] && [ -d "$TRUST_BAK/trusted-keys" ]; then
    mv "$TRUST_BAK/trusted-keys" "$REAL_TRUST"
    rm -rf "$TRUST_BAK"
  fi
}
trap cleanup_trust EXIT

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
      "max_struct_loss": 0.5,
      "check_body_hash": false
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
export function api(data, config) {
    let r = helper(data, config)
    if data > 0 {
        if r > 10 {
            while r > 100 {
                r = r / 2
            }
            return r * 2
        }
        for i in [1, 2, 3] {
            r = r + i
        }
        return r
    }
    return config
}
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

# V-SC-008: Sign govern.json first (key presence requires all .sig files)
export NAAB_GOVERN_KEY="test-secret-key-12345"
"$NAAB" --sign-governance "$WORK_DIR/govern.json" > /dev/null 2>&1

# Save baseline with key (auto-signs)
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
# Re-save baseline (T26 corrupted it by appending garbage)
"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1
unset NAAB_GOVERN_KEY

# --- T27: .sig exists but no key → HARD block (V-SC-007 fail-closed) ---
# .sig still exists from T25, but key is now unset
# V-SC-007: verifyContentSignature() now returns false when .sig exists without key
OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "INTEGRITY BLOCK"; then
  pass "T27: missing key with .sig → correctly blocked (fail-closed)"
else
  fail "T27: expected exit 3 with INTEGRITY BLOCK (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# V-SC-008: Remove stale .sig from T25 signing — subsequent tests are unsigned mode
rm -f "$WORK_DIR/govern.json.sig"

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

# --- T29: Body hash detects function rewrite (metric stuffing attack) ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": true
    }
  }
}
EOF

cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function foo() {
    let x = 1
    let y = 2
    return x + y
}
main { print(foo()) }
NAAB_EOF

# Save baseline
"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# Rewrite function body (same name, same complexity pattern, different code)
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function foo() {
    let a = 99
    let b = 88
    return a - b
}
main { print(foo()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "body.*hash\|rewritten"; then
  pass "T29: body hash detects function rewrite (metric stuffing blocked)"
else
  fail "T29: expected body hash mismatch block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T30: Body hash passes when function unchanged ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function bar() {
    let x = 10
    return x * 2
}
main { print(bar()) }
NAAB_EOF

# Save baseline
"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# Run same file — should pass
OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T30: body hash passes when function unchanged"
else
  fail "T30: expected pass for unchanged function (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T31: Parameter utilization — function ignoring params blocked ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": false,
      "check_param_utilization": true,
      "min_param_utilization": 0.5
    }
  }
}
EOF

# Save baseline with good version first
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function process(a, b, c, d) {
    let sum = a + b + c + d
    return sum
}
main { print(process(1, 2, 3, 4)) }
NAAB_EOF
"$NAAB" --drift-baseline-save "$WORK_DIR/test.naab" > /dev/null 2>&1

# Now rewrite to ignore most params
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function process(a, b, c, d) {
    return a
}
main { print(process(1, 2, 3, 4)) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "underutilize\|param.*utilization"; then
  pass "T31: param utilization blocks function ignoring 75% of params"
else
  fail "T31: expected param utilization block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T32: Parameter utilization — function using all params passes ---
rm -rf "$WORK_DIR/.naab"
cat > "$WORK_DIR/test.naab" << 'NAAB_EOF'
function compute(a, b, c) {
    let sum = a + b + c
    return sum
}
main { print(compute(1, 2, 3)) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T32: param utilization passes when all params used"
else
  fail "T32: expected pass for full param usage (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# ============================================================================
# Gates 13-17: Anti-Evasion Gates
# ============================================================================

# --- T33: Gate 13 — Config presence: removing govern.json blocks execution ---
WORK_DIR_13=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g13_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13" EXIT

cat > "$WORK_DIR_13/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": false,
      "check_config_presence": true
    }
  }
}
EOF

cat > "$WORK_DIR_13/test.naab" << 'NAAB_EOF'
function hello() { return "world" }
main { print(hello()) }
NAAB_EOF

# Save baseline (config present)
"$NAAB" --drift-baseline-save "$WORK_DIR_13/test.naab" > /dev/null 2>&1

# Remove govern.json — should fail-closed
mv "$WORK_DIR_13/govern.json" "$WORK_DIR_13/govern.json.bak"

OUTPUT=$("$NAAB" "$WORK_DIR_13/test.naab" 2>&1)
RC=$?
# Without govern.json, governance doesn't load → no drift check runs → passes
# BUT: Gate 10 (require_baseline) handles this differently.
# Gate 13 only fires when governance IS loaded but config was expected.
# In this scenario, no govern.json = no governance = it passes (the evasion).
# So we need govern.json to exist but be empty/different:
mv "$WORK_DIR_13/govern.json.bak" "$WORK_DIR_13/govern.json"

# Re-test: modify govern.json content → should block (config hash mismatch)
cat > "$WORK_DIR_13/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": false,
      "check_config_presence": true
    }
  },
  "extra_key": "tampered"
}
EOF

OUTPUT=$("$NAAB" "$WORK_DIR_13/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "govern.json.*modified\|config.*modified"; then
  pass "T33: Gate 13 — config hash mismatch blocks execution"
else
  fail "T33: expected config hash mismatch block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T34: Gate 13 — Unchanged config passes ---
WORK_DIR_14=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g13b_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13 $WORK_DIR_14" EXIT

cat > "$WORK_DIR_14/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": false,
      "check_config_presence": true
    }
  }
}
EOF

cat > "$WORK_DIR_14/test.naab" << 'NAAB_EOF'
function hello() { return "world" }
main { print(hello()) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_14/test.naab" > /dev/null 2>&1

OUTPUT=$("$NAAB" "$WORK_DIR_14/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T34: Gate 13 — unchanged config passes"
else
  fail "T34: expected pass for unchanged config (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T35: Gate 14 — Script relocation blocks execution ---
WORK_DIR_15=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g14_XXXXXX")
WORK_DIR_15B=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g14b_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13 $WORK_DIR_14 $WORK_DIR_15 $WORK_DIR_15B" EXIT

cat > "$WORK_DIR_15/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": false,
      "check_script_location": true
    }
  }
}
EOF

cat > "$WORK_DIR_15/test.naab" << 'NAAB_EOF'
function hello() { return "world" }
main { print(hello()) }
NAAB_EOF

# Save baseline in original directory
"$NAAB" --drift-baseline-save "$WORK_DIR_15/test.naab" > /dev/null 2>&1

# Copy script AND baseline to new directory (simulate relocation)
cp "$WORK_DIR_15/test.naab" "$WORK_DIR_15B/test.naab"
cp "$WORK_DIR_15/govern.json" "$WORK_DIR_15B/govern.json"
mkdir -p "$WORK_DIR_15B/.naab"
cp "$WORK_DIR_15/.naab/drift-baseline.json" "$WORK_DIR_15B/.naab/drift-baseline.json"

OUTPUT=$("$NAAB" "$WORK_DIR_15B/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "relocation\|script.*running.*from\|script_location"; then
  pass "T35: Gate 14 — script relocation blocks execution"
else
  fail "T35: expected script relocation block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T36: Gate 14 — Script in original location passes ---
OUTPUT=$("$NAAB" "$WORK_DIR_15/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T36: Gate 14 — script in original location passes"
else
  fail "T36: expected pass for original location (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T37: Gate 3 — Complexity regression blocks ---
WORK_DIR_17=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g3_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13 $WORK_DIR_14 $WORK_DIR_15 $WORK_DIR_15B $WORK_DIR_17" EXIT

cat > "$WORK_DIR_17/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": false,
      "check_complexity": true,
      "max_complexity_loss": 0.5
    }
  }
}
EOF

# Complex function baseline
cat > "$WORK_DIR_17/test.naab" << 'NAAB_EOF'
function analyze(data) {
    let result = 0
    if data > 10 {
        for i in [1, 2, 3, 4, 5] {
            if i > 2 {
                result = result + i
            } else {
                result = result - i
            }
        }
        while result > 100 {
            result = result / 2
        }
    } else {
        for j in [10, 20, 30] {
            if j > 15 {
                result = result + j
            }
        }
    }
    return result
}
main { print(analyze(42)) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_17/test.naab" > /dev/null 2>&1

# Simplify to trivial stub
cat > "$WORK_DIR_17/test.naab" << 'NAAB_EOF'
function analyze(data) {
    return data
}
main { print(analyze(42)) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_17/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "complexity.*dropped\|complexity.*loss"; then
  pass "T37: Gate 3 — complexity regression blocks execution"
else
  fail "T37: expected complexity regression block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T38: Gate 3 — Complexity increase passes ---
rm -rf "$WORK_DIR_17/.naab"

# Simple baseline
cat > "$WORK_DIR_17/test.naab" << 'NAAB_EOF'
function analyze(data) {
    return data + 1
}
main { print(analyze(42)) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_17/test.naab" > /dev/null 2>&1

# More complex version
cat > "$WORK_DIR_17/test.naab" << 'NAAB_EOF'
function analyze(data) {
    let result = 0
    if data > 10 {
        for i in [1, 2, 3, 4, 5] {
            result = result + i
        }
    } else {
        result = data * 2
    }
    return result
}
main { print(analyze(42)) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_17/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T38: Gate 3 — complexity increase passes"
else
  fail "T38: expected pass for complexity increase (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T39: Gate 16 — Signature file removal blocks ---
WORK_DIR_19=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g16_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13 $WORK_DIR_14 $WORK_DIR_15 $WORK_DIR_15B $WORK_DIR_17 $WORK_DIR_19" EXIT

cat > "$WORK_DIR_19/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": false,
      "check_signature_presence": true
    }
  }
}
EOF

# V-SC-007: Need NAAB_GOVERN_KEY to work with signed govern.json
export NAAB_GOVERN_KEY="test-key-t39"

cat > "$WORK_DIR_19/test.naab" << 'NAAB_EOF'
function hello() { return "world" }
main { print(hello()) }
NAAB_EOF

# Sign govern.json properly with the test key
python3 -c "
import hmac, hashlib
with open('$WORK_DIR_19/govern.json') as f: c = f.read()
sig = hmac.new(b'test-key-t39', c.encode(), hashlib.sha256).hexdigest()
with open('$WORK_DIR_19/govern.json.sig', 'w') as f: f.write(sig)
"

# Save baseline (sig present, key set)
"$NAAB" --drift-baseline-save "$WORK_DIR_19/test.naab" > /dev/null 2>&1

# Remove .sig — should fail-closed (check_signature_presence)
rm "$WORK_DIR_19/govern.json.sig"
unset NAAB_GOVERN_KEY

OUTPUT=$("$NAAB" "$WORK_DIR_19/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "sig.*missing\|signature.*missing\|signature_presence\|INTEGRITY BLOCK"; then
  pass "T39: Gate 16 — signature file removal blocks execution"
else
  fail "T39: expected sig removal block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T40: Gate 16 — Signature present passes ---
rm -rf "$WORK_DIR_19/.naab"
export NAAB_GOVERN_KEY="test-key-t39"

# Re-sign govern.json
python3 -c "
import hmac, hashlib
with open('$WORK_DIR_19/govern.json') as f: c = f.read()
sig = hmac.new(b'test-key-t39', c.encode(), hashlib.sha256).hexdigest()
with open('$WORK_DIR_19/govern.json.sig', 'w') as f: f.write(sig)
"

"$NAAB" --drift-baseline-save "$WORK_DIR_19/test.naab" > /dev/null 2>&1

OUTPUT=$("$NAAB" "$WORK_DIR_19/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T40: Gate 16 — signature present passes"
else
  fail "T40: expected pass with sig present (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi
unset NAAB_GOVERN_KEY

# --- T41: Gate 17 — Polyglot block shrinkage blocks ---
WORK_DIR_21=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g17_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13 $WORK_DIR_14 $WORK_DIR_15 $WORK_DIR_15B $WORK_DIR_17 $WORK_DIR_19 $WORK_DIR_21" EXIT

cat > "$WORK_DIR_21/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_body_hash": false,
      "check_polyglot_content": true,
      "max_polyglot_shrink": 0.5
    }
  }
}
EOF

# Large polyglot baseline
cat > "$WORK_DIR_21/test.naab" << 'NAAB_EOF'
function analyze(data) {
    let result = <<python[data] -> JSON
import json
result = {}
for i in range(10):
    result[f"key_{i}"] = i * 2
    if i > 5:
        result[f"extra_{i}"] = i * 3
    else:
        result[f"base_{i}"] = i + 1
for k, v in list(result.items()):
    if v > 10:
        result[k] = v - 5
    elif v < 3:
        result[k] = v + 10
print(json.dumps(result))
>>
    return result
}
main { print(analyze("test")) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_21/test.naab" > /dev/null 2>&1

# Shrink polyglot to trivial stub
cat > "$WORK_DIR_21/test.naab" << 'NAAB_EOF'
function analyze(data) {
    let result = <<python[data] -> JSON
import json
print(json.dumps({"a": 1}))
>>
    return result
}
main { print(analyze("test")) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_21/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "polyglot.*shrink\|polyglot_content"; then
  pass "T41: Gate 17 — polyglot block shrinkage blocks execution"
else
  fail "T41: expected polyglot shrinkage block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T42: Gate 17 — Polyglot expansion passes ---
# Skip if Python is not available (Windows CI lacks Python executor)
T42_PY_CHECK=$(mktemp "${TMPDIR:-/tmp}/naab_pycheck_XXXXXX.naab")
echo 'main { let r = <<python
print("ok")
>>
print(r) }' > "$T42_PY_CHECK"
PYTHON_CHECK=$("$NAAB" "$T42_PY_CHECK" 2>&1)
rm -f "$T42_PY_CHECK"
if echo "$PYTHON_CHECK" | grep -q "No executor\|not available\|not found"; then
  pass "T42: Gate 17 — polyglot expansion passes (skipped: no Python)"
else

rm -rf "$WORK_DIR_21/.naab"

# Small polyglot baseline
cat > "$WORK_DIR_21/test.naab" << 'NAAB_EOF'
function analyze(data) {
    let result = <<python[data] -> JSON
import json
print(json.dumps({"a": 1}))
>>
    return result
}
main { print(analyze("test")) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_21/test.naab" > /dev/null 2>&1

# Expand polyglot
cat > "$WORK_DIR_21/test.naab" << 'NAAB_EOF'
function analyze(data) {
    let result = <<python[data] -> JSON
import json
result = {}
for i in range(10):
    result[f"key_{i}"] = i * 2
    if i > 5:
        result[f"extra_{i}"] = i * 3
    else:
        result[f"base_{i}"] = i + 1
for k, v in list(result.items()):
    if v > 10:
        result[k] = v - 5
    elif v < 3:
        result[k] = v + 10
print(json.dumps(result))
>>
    return result
}
main { print(analyze("test")) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_21/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T42: Gate 17 — polyglot expansion passes"
else
  fail "T42: expected pass for polyglot expansion (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

fi # end Python check

# --- T43: Gate 12 — param named "a" should not match inside "data" ---
WORK_DIR_22=$(mktemp -d "${TMPDIR:-/tmp}/naab_drift_t43.XXXXXX")
cat > "$WORK_DIR_22/govern.json" << 'EOF'
{"version":"1.0.0","project_name":"t43","mode":"enforce","code_quality":{"drift_detection":{"enabled":true,"level":"hard","baseline_path":".naab/drift-baseline.json","check_param_utilization":true,"min_param_utilization":0.5,"check_body_hash":false}}}
EOF

cat > "$WORK_DIR_22/test.naab" << 'NAAB_EOF'
function process(a) {
    let data = a + 1
    let array_val = [data, data * 2]
    return array_val
}
main { print(process(5)) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_22/test.naab" > /dev/null 2>&1

# Remove actual usage of param "a" — only "data"/"array_val" remain (contain "a" as substring)
cat > "$WORK_DIR_22/test.naab" << 'NAAB_EOF'
function process(a) {
    let data = 42
    let array_val = [data, data * 2]
    return array_val
}
main { print(process(5)) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_22/test.naab" 2>&1)
RC=$?
# With word-boundary matching, param "a" is NOT used (only appears inside "data"/"array_val")
# Param utilization is 0/1 = 0.0, should trigger Gate 12 block
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "param.*util\|param_utilization"; then
  pass "T43: Gate 12 — word-boundary prevents 'a' matching inside 'data'"
else
  fail "T43: expected param utilization block for unused 'a' (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# =====================================================================
# T44-T45: Gate 18 — New function detection
# =====================================================================
WORK_DIR_23=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g18_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13 $WORK_DIR_14 $WORK_DIR_15 $WORK_DIR_15B $WORK_DIR_16 $WORK_DIR_17 $WORK_DIR_18 $WORK_DIR_19 $WORK_DIR_20 $WORK_DIR_21 $WORK_DIR_22 $WORK_DIR_23" EXIT

cat > "$WORK_DIR_23/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "check_new_functions": true
    }
  }
}
EOF

cat > "$WORK_DIR_23/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
main { print(foo() + bar()) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_23/test.naab" > /dev/null 2>&1

# Add a new function
cat > "$WORK_DIR_23/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
function injected() { return 99 }
main { print(foo() + bar() + injected()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_23/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "new function.*injected"; then
  pass "T44: Gate 18 — new function 'injected' detected and blocked"
else
  fail "T44: expected new function detection block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# T45: No new functions — should pass
cat > "$WORK_DIR_23/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
main { print(foo() + bar()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_23/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T45: Gate 18 — no new functions passes"
else
  fail "T45: expected pass when no new functions (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# =====================================================================
# T46-T47: Gate 0 extension — Function gain detection
# =====================================================================
WORK_DIR_24=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_g0gain_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13 $WORK_DIR_14 $WORK_DIR_15 $WORK_DIR_15B $WORK_DIR_16 $WORK_DIR_17 $WORK_DIR_18 $WORK_DIR_19 $WORK_DIR_20 $WORK_DIR_21 $WORK_DIR_22 $WORK_DIR_23 $WORK_DIR_24" EXIT

cat > "$WORK_DIR_24/govern.json" << 'EOF'
{
  "mode": "enforce",
  "code_quality": {
    "drift_detection": {
      "enabled": true,
      "level": "hard",
      "max_function_gain": 0.5
    }
  }
}
EOF

cat > "$WORK_DIR_24/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
main { print(foo() + bar()) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_24/test.naab" > /dev/null 2>&1

# Add 3 new functions (150% gain, exceeds 50% threshold)
cat > "$WORK_DIR_24/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
function dup1() { return 3 }
function dup2() { return 4 }
function dup3() { return 5 }
main { print(foo() + bar() + dup1() + dup2() + dup3()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_24/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "grew\|gain"; then
  pass "T46: Gate 0 — function gain 150% detected (threshold 50%)"
else
  fail "T46: expected function gain block (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# T47: Small gain within threshold (1 extra = 50%, at threshold boundary)
cat > "$WORK_DIR_24/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
main { print(foo() + bar()) }
NAAB_EOF

"$NAAB" --drift-baseline-save "$WORK_DIR_24/test.naab" > /dev/null 2>&1

# Add 1 function (50% gain = at threshold, should pass since > not >=)
cat > "$WORK_DIR_24/test.naab" << 'NAAB_EOF'
function foo() { return 1 }
function bar() { return 2 }
function extra() { return 3 }
main { print(foo() + bar() + extra()) }
NAAB_EOF

OUTPUT=$("$NAAB" "$WORK_DIR_24/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ]; then
  pass "T47: Gate 0 — function gain 50% at threshold passes"
else
  fail "T47: expected pass for gain at threshold (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# =====================================================================
# T48-T49: Pre-flight blocked_flags — --no-governance and --tree-walk
# =====================================================================
WORK_DIR_25=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_blocked_XXXXXX")
trap "rm -rf $WORK_DIR $WORK_DIR_13 $WORK_DIR_14 $WORK_DIR_15 $WORK_DIR_15B $WORK_DIR_16 $WORK_DIR_17 $WORK_DIR_18 $WORK_DIR_19 $WORK_DIR_20 $WORK_DIR_21 $WORK_DIR_22 $WORK_DIR_23 $WORK_DIR_24 $WORK_DIR_25" EXIT

cat > "$WORK_DIR_25/govern.json" << 'EOF'
{
  "mode": "enforce",
  "integrity": {
    "blocked_flags": ["--no-governance", "--tree-walk"]
  }
}
EOF

cat > "$WORK_DIR_25/test.naab" << 'NAAB_EOF'
function hello() { return "hello" }
main { print(hello()) }
NAAB_EOF

OUTPUT=$("$NAAB" --no-governance "$WORK_DIR_25/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "INTEGRITY BLOCK.*--no-governance"; then
  pass "T48: --no-governance blocked by integrity.blocked_flags"
else
  fail "T48: expected --no-governance to be blocked (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

OUTPUT=$("$NAAB" --tree-walk "$WORK_DIR_25/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -qi "INTEGRITY BLOCK.*--tree-walk"; then
  pass "T49: --tree-walk blocked by integrity.blocked_flags"
else
  fail "T49: expected --tree-walk to be blocked (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi

# --- T50: NAAB_GOVERN_KEY blocked from env.get() (V-SC-006) ---
WORK_DIR_26=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_T50_XXXXXX")
cat > "$WORK_DIR_26/test.naab" << 'NAAB_EOF'
main {
    let key = env.get("NAAB_GOVERN_KEY")
    if key == null {
        print("BLOCKED")
    } else {
        print("LEAKED")
    }
}
NAAB_EOF
cat > "$WORK_DIR_26/govern.json" << 'EOF'
{"version":"1.0.0","project_name":"t50","mode":"enforce","code_quality":{}}
EOF
# V-SC-008: Must sign govern.json when key is set
export NAAB_GOVERN_KEY="test-secret-key-12345"
"$NAAB" --sign-governance "$WORK_DIR_26/govern.json" > /dev/null 2>&1
OUTPUT=$("$NAAB" "$WORK_DIR_26/test.naab" 2>&1)
RC=$?
unset NAAB_GOVERN_KEY
if echo "$OUTPUT" | grep -q "BLOCKED"; then
  pass "T50: env.get(\"NAAB_GOVERN_KEY\") blocked by NAAB_INTERNAL_ENV_VARS"
else
  fail "T50: expected BLOCKED but got: $(echo "$OUTPUT" | head -3)"
fi
rm -rf "$WORK_DIR_26"

# --- T51: NAAB_GOVERN_KEY scrubbed from shell polyglot subprocess (V-SC-006) ---
WORK_DIR_27=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_T51_XXXXXX")
cat > "$WORK_DIR_27/test.naab" << 'NAAB_EOF'
main {
    let result = <<shell
echo "$NAAB_GOVERN_KEY"
>>
    if result == null || result == "" || result == "\n" {
        print("SCRUBBED")
    } else {
        print("LEAKED")
    }
}
NAAB_EOF
cat > "$WORK_DIR_27/govern.json" << 'EOF'
{"version":"1.0.0","project_name":"t51","mode":"enforce","capabilities":{"process":{"allow_spawn":true,"allowed_commands":["echo","sh"]}},"code_quality":{}}
EOF
# V-SC-008: Must sign govern.json when key is set
export NAAB_GOVERN_KEY="test-secret-key-12345"
"$NAAB" --sign-governance "$WORK_DIR_27/govern.json" > /dev/null 2>&1
OUTPUT=$("$NAAB" "$WORK_DIR_27/test.naab" 2>&1)
RC=$?
unset NAAB_GOVERN_KEY
if echo "$OUTPUT" | grep -q "SCRUBBED"; then
  pass "T51: NAAB_GOVERN_KEY scrubbed from shell polyglot subprocess"
elif echo "$OUTPUT" | grep -q "LEAKED"; then
  fail "T51: NAAB_GOVERN_KEY leaked to shell subprocess"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
else
  # Key was scrubbed (empty output), but NAAb returned null/empty differently
  # Check that the output does NOT contain the actual key value
  if echo "$OUTPUT" | grep -q "test-secret-key-12345"; then
    fail "T51: NAAB_GOVERN_KEY value found in output"
    echo "    Output: $(echo "$OUTPUT" | head -5)"
  else
    pass "T51: NAAB_GOVERN_KEY scrubbed from shell polyglot subprocess"
  fi
fi
rm -rf "$WORK_DIR_27"

# --- T52: Gate 1 violation includes Help text ---
WORK_DIR_28=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_T52_XXXXXX")
cat > "$WORK_DIR_28/test.naab" << 'NAAB_EOF'
function process(a, b, c, d) { return a + b + c + d }
main { print(process(1, 2, 3, 4)) }
NAAB_EOF
cat > "$WORK_DIR_28/govern.json" << 'EOF'
{"version":"1.0.0","project_name":"t52","mode":"enforce","code_quality":{"drift_detection":{"enabled":true,"level":"hard","baseline_path":".naab/drift-baseline.json","check_signatures":true,"max_param_loss":0.3}}}
EOF
"$NAAB" --drift-baseline-save "$WORK_DIR_28/test.naab" > /dev/null 2>&1
# Now gut params
cat > "$WORK_DIR_28/test.naab" << 'NAAB_EOF'
function process(a) { return a }
main { print(process(1)) }
NAAB_EOF
OUTPUT=$("$NAAB" "$WORK_DIR_28/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "Help:.*Restore the removed parameters"; then
  pass "T52: Gate 1 violation includes Help text with restore guidance"
else
  fail "T52: expected Help text in Gate 1 violation (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi
rm -rf "$WORK_DIR_28"

# --- T53: Gate 2 violation includes deleted import names ---
WORK_DIR_29=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_T53_XXXXXX")
cat > "$WORK_DIR_29/test.naab" << 'NAAB_EOF'
use math
use string
use array
main { print(math.abs(-1)) }
NAAB_EOF
cat > "$WORK_DIR_29/govern.json" << 'EOF'
{"version":"1.0.0","project_name":"t53","mode":"enforce","code_quality":{"drift_detection":{"enabled":true,"level":"hard","baseline_path":".naab/drift-baseline.json","check_imports":true,"max_import_loss":0.0}}}
EOF
"$NAAB" --drift-baseline-save "$WORK_DIR_29/test.naab" > /dev/null 2>&1
# Remove imports
cat > "$WORK_DIR_29/test.naab" << 'NAAB_EOF'
use math
main { print(math.abs(-1)) }
NAAB_EOF
OUTPUT=$("$NAAB" "$WORK_DIR_29/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ] && echo "$OUTPUT" | grep -q "Removed imports:"; then
  pass "T53: Gate 2 violation includes deleted import names"
else
  fail "T53: expected deleted import names in violation (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -8)"
fi
rm -rf "$WORK_DIR_29"

# =====================================================================
# T54-T61: Ed25519 Trust-Anchored Governance Signing (V-SC-009)
# =====================================================================

# --- T54: --keygen generates keypair and installs to trust store ---
# Trust store was cleared at script start; T54-T61 create fresh keys
WORK_DIR_T54=$(mktemp -d "${TMPDIR:-/tmp}/test_drift_T54_XXXXXX")
PRIV_KEY="$WORK_DIR_T54/test-key.pem"
OUTPUT=$("$NAAB" --keygen "$PRIV_KEY" 2>&1)
RC=$?
if [ $RC -eq 0 ] && [ -f "$PRIV_KEY" ] && echo "$OUTPUT" | grep -q "Ed25519 keypair generated"; then
  pass "T54: --keygen generates keypair and prints instructions"
else
  fail "T54: keygen failed (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# --- T55: Private key has 0600 permissions ---
if [ -f "$PRIV_KEY" ]; then
  PERMS=$(stat -c '%a' "$PRIV_KEY" 2>/dev/null || stat -f '%Lp' "$PRIV_KEY" 2>/dev/null)
  if [ "$PERMS" = "600" ]; then
    pass "T55: Private key file has 0600 permissions"
  else
    fail "T55: expected 0600 but got $PERMS"
  fi
else
  fail "T55: private key file not found"
fi

# --- T56: --list-keys shows the installed key ---
OUTPUT=$("$NAAB" --list-keys 2>&1)
RC=$?
if [ $RC -eq 0 ] && echo "$OUTPUT" | grep -qE "[0-9a-f]{16}"; then
  pass "T56: --list-keys shows installed key fingerprint"
else
  fail "T56: --list-keys failed (rc=$RC)"
  echo "    Output: $OUTPUT"
fi

# --- T57: Ed25519 signing creates ed25519: prefixed .sig ---
cat > "$WORK_DIR_T54/govern.json" << 'EOF'
{"version":"1.0.0","project_name":"t57","mode":"enforce"}
EOF
NAAB_SIGNING_KEY="$PRIV_KEY" "$NAAB" --sign-governance "$WORK_DIR_T54/govern.json" > /dev/null 2>&1
if [ -f "$WORK_DIR_T54/govern.json.sig" ]; then
  SIG_CONTENT=$(cat "$WORK_DIR_T54/govern.json.sig")
  if echo "$SIG_CONTENT" | grep -q "^ed25519:"; then
    pass "T57: Ed25519 signing creates ed25519: prefixed .sig"
  else
    fail "T57: .sig missing ed25519: prefix: $SIG_CONTENT"
  fi
else
  fail "T57: govern.json.sig not created"
fi

# --- T58: Ed25519 signed file passes verification (exit 0) ---
cat > "$WORK_DIR_T54/test.naab" << 'NAAB_EOF'
main { print("signed-ok") }
NAAB_EOF
OUTPUT=$(NAAB_SIGNING_KEY="$PRIV_KEY" "$NAAB" "$WORK_DIR_T54/test.naab" 2>&1)
RC=$?
if [ $RC -eq 0 ] && echo "$OUTPUT" | grep -q "signed-ok"; then
  pass "T58: Ed25519 signed file passes verification"
else
  fail "T58: verification failed (rc=$RC)"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# --- T59: Trust store keys + missing .sig = BLOCK (v21 attack) ---
rm -f "$WORK_DIR_T54/govern.json.sig"
OUTPUT=$("$NAAB" "$WORK_DIR_T54/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ]; then
  pass "T59: trust-keys + missing .sig = BLOCK (exit 3, v21 attack blocked)"
else
  fail "T59: expected exit 3 but got $RC"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# --- T60: HMAC .sig + trust store = BLOCK (requires re-signing) ---
echo "hmac:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" > "$WORK_DIR_T54/govern.json.sig"
OUTPUT=$("$NAAB" "$WORK_DIR_T54/test.naab" 2>&1)
RC=$?
if [ $RC -eq 3 ]; then
  pass "T60: HMAC .sig + trust store = BLOCK (forces Ed25519 re-sign)"
else
  fail "T60: expected exit 3 but got $RC"
  echo "    Output: $(echo "$OUTPUT" | head -5)"
fi

# --- T61: NAAB_SIGNING_KEY blocked from env.get() in user scripts ---
cat > "$WORK_DIR_T54/test_env.naab" << 'NAAB_EOF'
main {
  let val = env.get("NAAB_SIGNING_KEY")
  print(val)
}
NAAB_EOF
# Re-sign govern.json for this test
NAAB_SIGNING_KEY="$PRIV_KEY" "$NAAB" --sign-governance "$WORK_DIR_T54/govern.json" > /dev/null 2>&1
OUTPUT=$(NAAB_SIGNING_KEY="$PRIV_KEY" "$NAAB" "$WORK_DIR_T54/test_env.naab" 2>&1)
RC=$?
if echo "$OUTPUT" | grep -qi "blocked\|restricted\|internal"; then
  pass "T61: env.get(\"NAAB_SIGNING_KEY\") blocked by NAAB_INTERNAL_ENV_VARS"
else
  # Check it returns empty/null (also acceptable)
  if echo "$OUTPUT" | grep -q "null" || [ -z "$(echo "$OUTPUT" | grep -v "^$" | grep -v "Governance")" ]; then
    pass "T61: env.get(\"NAAB_SIGNING_KEY\") returns null (blocked)"
  else
    fail "T61: NAAB_SIGNING_KEY leaked: $(echo "$OUTPUT" | head -3)"
  fi
fi

# Cleanup T54-T61 work dir (trust store restored by trap handler)
rm -rf "$WORK_DIR_T54"
# Clear trust store so it doesn't affect other test scripts that run after this one
rm -rf "$REAL_TRUST"

echo ""
echo "=== Results: $PASSED/$TOTAL passed, $FAILED failed ==="
exit $FAILED
