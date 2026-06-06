#!/bin/bash
# Test extends/merge_arrays on real naab-x1 project
# Validates inheritance of custom_rules, blocked_commands, env_vars arrays
set -e
PASS=0; FAIL=0; TOTAL=0
check() {
    TOTAL=$((TOTAL + 1))
    if eval "$2"; then
        PASS=$((PASS + 1))
        echo "  [PASS] $1"
    else
        FAIL=$((FAIL + 1))
        echo "  [FAIL] $1"
    fi
}

NAAB="${NAAB:-./build/naab-lang}"
PROJ="$HOME/naab-x1"
TMPD="$(mktemp -d)"
trap "rm -rf $TMPD" EXIT

echo "=== extends real-project integration ==="
echo ""

# Copy naab-x1's govern.json for all tests
cp "$PROJ/govern.json" "$TMPD/govern.json"
cp "$PROJ/govern.json.sig" "$TMPD/govern.json.sig"

# T1: naab-x1 loads with extends (basic smoke)
echo "--- T1: Smoke load ---"
cat > "$TMPD/smoke.naab" << 'EOF'
main {
    let r = <<shell
echo "smoke-ok"
>>
    print(r)
}
EOF
OUT=$($NAAB run --governance-dashboard "$TMPD/smoke.naab" 2>&1 || true)
check "T1.1 governance loaded" "echo '$OUT' | grep -q 'Loaded.*govern.json'"
check "T1.2 mode is enforce" "echo '$OUT' | grep -q 'mode: enforce'"
check "T1.3 smoke output" "echo '$OUT' | grep -q 'smoke-ok'"

# T2: Custom rules — org-base's no-hardcoded-keys fires on polyglot block
echo ""
echo "--- T2: Custom rules merge (polyglot) ---"
cat > "$TMPD/test_parent_rule.naab" << 'EOF'
main {
    let x = <<shell
echo "HARDCODED_SECRET_ABCDEF12345678"
>>
    print(x)
}
EOF
OUT2=$($NAAB run "$TMPD/test_parent_rule.naab" 2>&1 || true)
check "T2.1 org-base no-hardcoded-keys fires" "echo '$OUT2' | grep -qi 'hardcoded.*credential\|no-hardcoded\|custom_rule'"

# Test child's custom rule (SKIP_INTEGRITY_CHECK) in a polyglot block
cat > "$TMPD/test_child_rule.naab" << 'EOF'
main {
    let x = <<shell
echo "SKIP_INTEGRITY_CHECK enabled"
>>
    print(x)
}
EOF
OUT3=$($NAAB run "$TMPD/test_child_rule.naab" 2>&1 || true)
check "T2.2 child no-pinned-hash-bypass fires" "echo '$OUT3' | grep -qi 'integrity.*bypass\|no-pinned\|custom_rule'"

# T3: blocked_commands merge — org-base's "rm -rf" + child's "pip install"
echo ""
echo "--- T3: blocked_commands merge ---"
cat > "$TMPD/test_rmrf.naab" << 'EOF'
main {
    let r = <<shell
rm -rf /nonexistent 2>&1
>>
    print(r)
}
EOF
OUT4=$($NAAB run "$TMPD/test_rmrf.naab" 2>&1 || true)
check "T3.1 org-base rm-rf blocked" "echo '$OUT4' | grep -qi 'blocked\|denied\|not allowed'"

cat > "$TMPD/test_pip.naab" << 'EOF'
main {
    let r = <<shell
pip install malicious-pkg 2>&1
>>
    print(r)
}
EOF
OUT5=$($NAAB run "$TMPD/test_pip.naab" 2>&1 || true)
check "T3.2 child pip-install blocked" "echo '$OUT5' | grep -qi 'blocked\|denied\|not allowed'"

# Neither should block "echo" (allowed in both)
cat > "$TMPD/test_echo.naab" << 'EOF'
main {
    let r = <<shell
echo "hello-safe"
>>
    print(r)
}
EOF
OUT6=$($NAAB run "$TMPD/test_echo.naab" 2>&1 || true)
check "T3.3 echo not blocked" "echo '$OUT6' | grep -q 'hello-safe'"

# T4: env_vars.blocked_read enforcement — verify blocked vars are rejected
echo ""
echo "--- T4: env_vars blocked_read enforcement ---"

# T4.1: blocked_read from org-base (AWS_SECRET_ACCESS_KEY) should throw
export AWS_SECRET_ACCESS_KEY=test-secret-value-12345
cat > "$TMPD/test_blocked_read.naab" << 'EOF'
use env
main {
    let v = env.get("AWS_SECRET_ACCESS_KEY")
    print(v)
}
EOF
$NAAB run "$TMPD/test_blocked_read.naab" > "$TMPD/t4_out.txt" 2>&1 || T4_EXIT=$?
T4_EXIT=${T4_EXIT:-0}
check "T4.1 blocked_read rejects AWS_SECRET_ACCESS_KEY" "[ $T4_EXIT -ne 0 ]"
check "T4.2 value not leaked in output" "! grep -q 'test-secret-value-12345' $TMPD/t4_out.txt"
check "T4.3 error mentions blocked" "grep -qi 'blocked by policy' $TMPD/t4_out.txt"

# T4.4: allowed var (HOME) still works
cat > "$TMPD/test_allowed_read.naab" << 'EOF'
use env
main {
    let h = env.get("HOME")
    print("env-ok")
}
EOF
$NAAB run "$TMPD/test_allowed_read.naab" > "$TMPD/t4_ok.txt" 2>&1 || true
check "T4.4 HOME still readable" "grep -q 'env-ok' $TMPD/t4_ok.txt"

# T4.5: env.has() stealth-denies blocked var
cat > "$TMPD/test_has_blocked.naab" << 'EOF'
use env
main {
    let exists = env.has("AWS_SECRET_ACCESS_KEY")
    print(exists)
}
EOF
$NAAB run "$TMPD/test_has_blocked.naab" > "$TMPD/t4_has.txt" 2>&1 || true
check "T4.5 env.has() returns false for blocked var" "grep -q 'false' $TMPD/t4_has.txt"
unset AWS_SECRET_ACCESS_KEY

echo ""
echo "=== Results: $PASS/$TOTAL passed ==="
if [ $FAIL -gt 0 ]; then
    echo "FAILURES: $FAIL"
    exit 1
fi
