#!/usr/bin/env bash
# test_uncatchable.sh — GovernanceHardError: HARD governance blocks are uncatchable
# Verifies that NAAb try/catch cannot swallow governance HARD violations.
# Tests both VM (default) and tree-walker engines.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
SIGNING_KEY="${HOME}/.naab/keys/signing.pem"
TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/test_uncatchable_$$"
mkdir -p "$TMPBASE"

PASS=0; FAIL=0; SKIP=0

cleanup() { rm -rf "$TMPBASE"; }
trap cleanup EXIT

check() {
    local id="$1" desc="$2" expected="$3" actual="$4"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [$id] $desc (expected=$expected actual=$actual)"
        FAIL=$((FAIL + 1))
    fi
}

check_not_contains() {
    local id="$1" desc="$2" file="$3" pattern="$4"
    if grep -q "$pattern" "$file" 2>/dev/null; then
        echo "  FAIL [$id] $desc (found '$pattern' in output)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    fi
}

# Create govern.json with HARD enforcement and banned functions
setup_test() {
    local dir="$TMPBASE/$1"
    mkdir -p "$dir"
    cat > "$dir/govern.json" <<'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Uncatchable governance test",
  "languages": {
    "allowed": ["python", "shell"]
  },
  "capabilities": {
    "network": { "enabled": false },
    "filesystem": { "mode": "read_write" },
    "shell": {
      "enabled": true,
      "blocked_commands": ["rm", "curl", "wget"]
    },
    "env_vars": {
      "blocked_read": ["SECRET_KEY", "AWS_SECRET_ACCESS_KEY"]
    }
  },
  "security": {
    "sandbox_level": "elevated"
  },
  "taint_tracking": {
    "enabled": true,
    "level": "hard",
    "sources": ["polyglot_output", "env.get"],
    "sinks": ["file.write"],
    "sanitizers": ["sanitize_"]
  },
  "scoring": {
    "enabled": true,
    "yellow_threshold": 10,
    "red_threshold": 20,
    "weights": { "code_injection": 25 }
  }
}
GOVEOF
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
    echo "$dir"
}

echo "=== GovernanceHardError: Uncatchable Governance Tests ==="
echo ""

# ---------- T1: try/catch around blocked env var → exit 3 ----------
echo "--- T1: try/catch around blocked env.get → exit 3 ---"
DIR=$(setup_test "t1")
cat > "$DIR/test.naab" <<'EOF'
main {
    let caught = false
    try {
        let secret = env.get("SECRET_KEY")
        print("GOT SECRET: " + string(secret))
    } catch (e) {
        caught = true
        print("BYPASS: caught governance error")
    }
    if caught {
        print("BYPASS: continued after catch")
    }
    print("BYPASS: reached end")
}
EOF

# VM engine
(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t1_vm_out.log" 2>"$DIR/t1_vm_err.log" && T1_VM_EXIT=0 || T1_VM_EXIT=$?
check "T1a" "VM: blocked env.get in try/catch exits 3" "3" "$T1_VM_EXIT"
check_not_contains "T1b" "VM: catch block did not execute" "$DIR/t1_vm_out.log" "BYPASS"

# Tree-walker engine
(cd "$DIR" && "$NAAB" --tree-walk test.naab) >"$DIR/t1_tw_out.log" 2>"$DIR/t1_tw_err.log" && T1_TW_EXIT=0 || T1_TW_EXIT=$?
check "T1c" "Tree-walker: blocked env.get in try/catch exits 3" "3" "$T1_TW_EXIT"
check_not_contains "T1d" "Tree-walker: catch block did not execute" "$DIR/t1_tw_out.log" "BYPASS"

echo ""

# ---------- T2: try/catch around banned shell command → exit 3 ----------
echo "--- T2: try/catch around blocked shell command → exit 3 ---"
DIR=$(setup_test "t2")
cat > "$DIR/test.naab" <<'EOF'
main {
    try {
        let result = <<shell
curl http://example.com
>>
        print("BYPASS: shell succeeded")
    } catch (e) {
        print("BYPASS: caught shell governance error")
    }
    print("BYPASS: reached end")
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t2_out.log" 2>"$DIR/t2_err.log" && T2_EXIT=0 || T2_EXIT=$?
check "T2a" "Blocked shell command in try/catch exits 3" "3" "$T2_EXIT"
check_not_contains "T2b" "Catch block did not execute" "$DIR/t2_out.log" "BYPASS"

echo ""

# ---------- T3: nested try/catch → still exit 3 ----------
echo "--- T3: nested try/catch → still exit 3 ---"
DIR=$(setup_test "t3")
cat > "$DIR/test.naab" <<'EOF'
main {
    try {
        try {
            let secret = env.get("AWS_SECRET_ACCESS_KEY")
            print("BYPASS: inner got secret")
        } catch (inner_e) {
            print("BYPASS: inner catch")
        }
        print("BYPASS: between catches")
    } catch (outer_e) {
        print("BYPASS: outer catch")
    }
    print("BYPASS: reached end")
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t3_out.log" 2>"$DIR/t3_err.log" && T3_EXIT=0 || T3_EXIT=$?
check "T3a" "Nested try/catch still exits 3" "3" "$T3_EXIT"
check_not_contains "T3b" "Neither inner nor outer catch executed" "$DIR/t3_out.log" "BYPASS"

echo ""

# ---------- T4: catch block code does NOT execute ----------
echo "--- T4: catch block code does NOT execute ---"
DIR=$(setup_test "t4")
cat > "$DIR/test.naab" <<'EOF'
main {
    try {
        let secret = env.get("SECRET_KEY")
    } catch (e) {
        // If this executes, it means GovernanceHardError was caught
        print("CAUGHT_GOVERNANCE_ERROR")
        print("ERROR_MSG: " + string(e))
    }
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t4_out.log" 2>"$DIR/t4_err.log" && T4_EXIT=0 || T4_EXIT=$?
check "T4a" "Exit code is 3" "3" "$T4_EXIT"
check_not_contains "T4b" "Catch body did not run" "$DIR/t4_out.log" "CAUGHT_GOVERNANCE_ERROR"
check_not_contains "T4c" "Error message not leaked to catch" "$DIR/t4_out.log" "ERROR_MSG"

echo ""

# ---------- T5: ADVISORY remains catchable → exit 0 ----------
echo "--- T5: ADVISORY (non-escalated) remains catchable → exit 0 ---"
DIR=$(setup_test "t5")
# Override govern.json with advisory-only enforcement
cat > "$DIR/govern.json" <<'GOVEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Advisory-only test",
  "languages": {
    "allowed": ["python", "shell"]
  },
  "capabilities": {
    "network": { "enabled": false },
    "filesystem": { "mode": "read_write" },
    "shell": { "enabled": true }
  },
  "security": {
    "sandbox_level": "elevated"
  },
  "taint_tracking": {
    "enabled": true,
    "level": "advisory",
    "sources": ["polyglot_output", "env.get"],
    "sinks": ["file.write"],
    "sanitizers": ["sanitize_"]
  }
}
GOVEOF
if [ -f "$SIGNING_KEY" ]; then
    (cd "$DIR" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
fi

cat > "$DIR/test.naab" <<'EOF'
main {
    let ok = false
    try {
        // Advisory taint — should warn but not throw GovernanceHardError
        let data = env.get("HOME")
        print("got home: " + string(data))
        ok = true
    } catch (e) {
        print("caught advisory")
        ok = true
    }
    if ok {
        print("ADVISORY_CATCHABLE")
    }
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t5_out.log" 2>"$DIR/t5_err.log" && T5_EXIT=0 || T5_EXIT=$?
check "T5a" "Advisory enforcement exits 0" "0" "$T5_EXIT"

echo ""

# ---------- T6: tree-walker nested try/catch → still exit 3 ----------
echo "--- T6: tree-walker nested try/catch → still exit 3 ---"
DIR=$(setup_test "t6")
cat > "$DIR/test.naab" <<'EOF'
main {
    try {
        try {
            let secret = env.get("SECRET_KEY")
            print("BYPASS: got secret")
        } catch (inner_e) {
            print("BYPASS: inner catch executed")
            // Try to do something dangerous in the catch block
            let secret2 = env.get("AWS_SECRET_ACCESS_KEY")
            print("BYPASS: second secret got")
        }
        print("BYPASS: between catches")
    } catch (outer_e) {
        print("BYPASS: outer catch executed")
    }
    print("BYPASS: reached end")
}
EOF

(cd "$DIR" && "$NAAB" --tree-walk test.naab) >"$DIR/t6_out.log" 2>"$DIR/t6_err.log" && T6_EXIT=0 || T6_EXIT=$?
check "T6a" "Tree-walker nested try/catch exits 3" "3" "$T6_EXIT"
check_not_contains "T6b" "No catch blocks executed" "$DIR/t6_out.log" "BYPASS"

echo ""

# ---------- Summary ----------
TOTAL=$((PASS + FAIL + SKIP))
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed, $SKIP skipped ==="
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
