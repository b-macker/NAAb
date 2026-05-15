#!/bin/bash
# Tests for fail-closed execution boundary
# Verifies governance-sandbox synchronization and security gates

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0
TEST_DIR="${HOME}/.naab_fail_closed_test_$$"
ORIG_DIR="$(pwd)"
mkdir -p "$TEST_DIR"

cleanup() { cd "$ORIG_DIR"; rm -rf "$TEST_DIR"; }
trap cleanup EXIT

check() {
    local desc="$1"
    local condition="$2"
    if eval "$condition"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== NAAb Fail-Closed Execution Boundary Tests ==="
echo ""

# --- Test 1: Enforce mode defaults to standard sandbox ---
echo "--- Test 1: Enforce mode defaults to standard sandbox ---"

mkdir -p "$TEST_DIR/t1"
cat > "$TEST_DIR/t1/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "limits": { "execution": { "loop_iterations": 1000 } }
}
EOF

cat > "$TEST_DIR/t1/test.naab" <<'EOF'
use process
main {
    let r = process.run("echo hi")
    print(r.get("stdout"))
}
EOF

cd "$TEST_DIR/t1"
"$NAAB_BIN" test.naab > out.txt 2> err.txt
T1_EXIT=$?

check "Enforce mode blocks process.run without shell capability" \
    "[ '$T1_EXIT' -ne 0 ]"

# --- Test 2: Governance capabilities sync to sandbox ---
echo ""
echo "--- Test 2: Governance capabilities sync to sandbox ---"

mkdir -p "$TEST_DIR/t2"
cat > "$TEST_DIR/t2/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "capabilities": {
    "shell": { "enabled": false }
  },
  "limits": { "execution": { "loop_iterations": 1000 } }
}
EOF

cat > "$TEST_DIR/t2/test.naab" <<'EOF'
main {
    let r = <<shell
echo "should not run"
>>
    print(r)
}
EOF

cd "$TEST_DIR/t2"
"$NAAB_BIN" test.naab > out.txt 2> err.txt
T2_EXIT=$?

check "Shell disabled in govern.json blocks shell block" \
    "[ '$T2_EXIT' -ne 0 ]"

# --- Test 3: process.exit() sandbox gate ---
echo ""
echo "--- Test 3: process.exit() sandbox gate ---"

mkdir -p "$TEST_DIR/t3"
cat > "$TEST_DIR/t3/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "capabilities": {
    "shell": { "enabled": false }
  },
  "limits": { "execution": { "loop_iterations": 1000 } }
}
EOF

cat > "$TEST_DIR/t3/test.naab" <<'EOF'
use process
main {
    process.exit(0)
}
EOF

cd "$TEST_DIR/t3"
"$NAAB_BIN" test.naab > out.txt 2> err.txt
T3_EXIT=$?

check "process.exit() blocked when shell disabled" \
    "[ '$T3_EXIT' -ne 0 ]"

check "process.exit error suggests using return" \
    "grep -q 'return' out.txt err.txt 2>/dev/null"

# --- Test 4: log.set_output() path validation ---
echo ""
echo "--- Test 4: log.set_output() path validation ---"

mkdir -p "$TEST_DIR/t4"
cat > "$TEST_DIR/t4/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "sandbox_level": "restricted",
  "limits": { "execution": { "loop_iterations": 1000 } }
}
EOF

cat > "$TEST_DIR/t4/test.naab" <<'EOF'
use log
main {
    log.set_output("/etc/evil_log")
}
EOF

cd "$TEST_DIR/t4"
"$NAAB_BIN" test.naab --sandbox-level restricted > out.txt 2> err.txt
T4_EXIT=$?

check "log.set_output to /etc blocked by sandbox" \
    "[ '$T4_EXIT' -ne 0 ]"

# --- Test 5: No govern.json = unrestricted (backward compat) ---
echo ""
echo "--- Test 5: Backward compatibility without govern.json ---"

mkdir -p "$TEST_DIR/t5"
cat > "$TEST_DIR/t5/test.naab" <<'EOF'
main {
    print("hello world")
}
EOF

cd "$TEST_DIR/t5"
"$NAAB_BIN" test.naab --no-governance > out.txt 2> err.txt
T5_EXIT=$?

check "Scripts without governance run successfully" \
    "[ '$T5_EXIT' -eq 0 ]"

check "Output is correct" \
    "grep -q 'hello world' out.txt"

# --- Test 6: Error messages don't leak bypass info ---
echo ""
echo "--- Test 6: Error message security ---"

# Collect all error outputs from previous tests
cat "$TEST_DIR"/t*/out.txt "$TEST_DIR"/t*/err.txt 2>/dev/null > "$TEST_DIR/all_output.txt"

check "No --no-governance in error messages" \
    "! grep -q -- '--no-governance' '$TEST_DIR/all_output.txt'"

check "No --sandbox-level in error messages" \
    "! grep -q -- '--sandbox-level' '$TEST_DIR/all_output.txt'"

check "No --governance-override in error messages" \
    "! grep -q -- '--governance-override' '$TEST_DIR/all_output.txt'"

# --- Results ---
echo ""
echo "=== Results: $PASS/$((PASS + FAIL)) passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
