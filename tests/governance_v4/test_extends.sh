#!/usr/bin/env bash
# test_extends.sh — Test govern.json "extends" policy distribution
#
# Phase 4: Enterprise readiness — parent/child config inheritance,
# merge strategies, circular detection, depth limits.

PASS=0
FAIL=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$LANG_DIR/build/naab-lang"
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
WORKDIR="$TMPDIR/naab_extends_$$"
SIGNING_KEY="${HOME}/.naab/keys/signing.pem"

if [ ! -x "$NAAB" ]; then
    echo "SKIP: naab-lang not built"
    exit 0
fi

mkdir -p "$WORKDIR"

sign_gov() {
    local dir="$1"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
}

check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected '$expected', got '$actual')"
        FAIL=$((FAIL + 1))
    fi
}

check_contains() {
    local desc="$1" pattern="$2" text="$3"
    if echo "$text" | grep -q "$pattern"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (pattern '$pattern' not found)"
        FAIL=$((FAIL + 1))
    fi
}

cleanup() {
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

echo "=== Governance extends (policy distribution) tests ==="

# ============================================================
# T1: Basic child_wins inheritance
# ============================================================
echo ""
echo "--- T1: child_wins inheritance ---"
T1BASE="$WORKDIR/t1/base"
T1CHILD="$WORKDIR/t1/child"
mkdir -p "$T1BASE" "$T1CHILD"

cat > "$T1BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": {
    "timeout": { "global": 30 },
    "execution": { "polyglot_blocks": 20 }
  },
  "capabilities": {
    "shell": { "enabled": false },
    "network": { "enabled": false }
  },
  "scoring": { "enabled": true, "yellow_threshold": 20, "red_threshold": 50 }
}
EOF
sign_gov "$T1BASE"

cat > "$T1CHILD/govern.json" << EOF
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "limits": {
    "timeout": { "global": 10 }
  }
}
EOF
sign_gov "$T1CHILD"

# Test script: print 1
cat > "$T1CHILD/test.naab" << 'EOF'
main { print(1) }
EOF

OUTPUT=$(cd "$T1CHILD" && "$NAAB" --governance-dashboard test.naab 2>&1)
EXIT_CODE=$?

check "T1a: child config loads successfully" "0" "$EXIT_CODE"
# The dashboard should show governance active
check_contains "T1b: governance active" "Governance" "$OUTPUT"


# ============================================================
# T2: parent_wins merge strategy
# ============================================================
echo ""
echo "--- T2: parent_wins merge strategy ---"
T2BASE="$WORKDIR/t2/base"
T2CHILD="$WORKDIR/t2/child"
mkdir -p "$T2BASE" "$T2CHILD"

cat > "$T2BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": {
    "timeout": { "global": 30 }
  }
}
EOF
sign_gov "$T2BASE"

cat > "$T2CHILD/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "meta": {
    "inheritance": {
      "merge_strategy": "parent_wins"
    }
  },
  "limits": {
    "timeout": { "global": 2 }
  }
}
EOF
sign_gov "$T2CHILD"

# Functional test: child timeout is 2s, parent is 30s.
# If parent_wins works, the 30s timeout applies and sleep(3) completes.
# If child's 2s wins, the polyglot block times out.
cat > "$T2CHILD/test.naab" << 'EOF'
main {
    let r = <<python
import time
time.sleep(3)
print("survived_3s")
>>
    print(r)
}
EOF

OUTPUT=$(cd "$T2CHILD" && timeout 15 "$NAAB" run --governance-dashboard test.naab 2>&1)
EXIT_CODE=$?

check "T2a: parent_wins config loads" "0" "$EXIT_CODE"
check_contains "T2b: parent timeout wins over child" "survived_3s" "$OUTPUT"


# ============================================================
# T3: Circular extends detection
# ============================================================
echo ""
echo "--- T3: Circular extends detection ---"
T3A="$WORKDIR/t3/a"
T3B="$WORKDIR/t3/b"
mkdir -p "$T3A" "$T3B"

cat > "$T3A/govern.json" << 'EOF'
{
  "extends": "../b/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$T3A"

cat > "$T3B/govern.json" << 'EOF'
{
  "extends": "../a/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$T3B"

cat > "$T3A/test.naab" << 'EOF'
main { print("circular") }
EOF

OUTPUT=$(cd "$T3A" && "$NAAB" test.naab 2>&1)
EXIT_CODE=$?

# Should fail with circular detection error
check "T3a: circular extends rejected (non-zero exit)" "1" "$([ $EXIT_CODE -ne 0 ] && echo 1 || echo 0)"
check_contains "T3b: circular error message" "Circular extends" "$OUTPUT"


# ============================================================
# T4: Max depth exceeded
# ============================================================
echo ""
echo "--- T4: Max depth exceeded ---"
# Create a chain of 7 levels (exceeds default max_depth=5)
for i in 1 2 3 4 5 6 7; do
    mkdir -p "$WORKDIR/t4/level$i"
done

# Level 7 (deepest base — no extends)
cat > "$WORKDIR/t4/level7/govern.json" << 'EOF'
{
  "mode": "enforce"
}
EOF
sign_gov "$WORKDIR/t4/level7"

# Levels 6 down to 2: each extends the level above
for i in 6 5 4 3 2; do
    next=$((i + 1))
    cat > "$WORKDIR/t4/level$i/govern.json" << EOF
{
  "extends": "../level$next/govern.json",
  "mode": "enforce"
}
EOF
    sign_gov "$WORKDIR/t4/level$i"
done

# Level 1: the child config (extends level2)
cat > "$WORKDIR/t4/level1/govern.json" << 'EOF'
{
  "extends": "../level2/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$WORKDIR/t4/level1"

cat > "$WORKDIR/t4/level1/test.naab" << 'EOF'
main { print("deep") }
EOF

OUTPUT=$(cd "$WORKDIR/t4/level1" && "$NAAB" test.naab 2>&1)
EXIT_CODE=$?

check "T4a: deep extends chain rejected" "1" "$([ $EXIT_CODE -ne 0 ] && echo 1 || echo 0)"
check_contains "T4b: max_depth error message" "max_depth" "$OUTPUT"


# ============================================================
# T5: Capability inheritance (shell disabled in base, not in child)
# ============================================================
echo ""
echo "--- T5: Capability inheritance ---"
T5BASE="$WORKDIR/t5/base"
T5CHILD="$WORKDIR/t5/child"
mkdir -p "$T5BASE" "$T5CHILD"

cat > "$T5BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "capabilities": {
    "shell": { "enabled": false }
  }
}
EOF
sign_gov "$T5BASE"

# Child extends base, doesn't mention shell
cat > "$T5CHILD/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "limits": {
    "timeout": { "global": 10 }
  }
}
EOF
sign_gov "$T5CHILD"

# Try to use shell — should be blocked (inherited from base)
cat > "$T5CHILD/test.naab" << 'EOF'
main {
    let r = <<sh
echo "shell_test"
>>
    print(r)
}
EOF

OUTPUT=$(cd "$T5CHILD" && "$NAAB" run test.naab 2>&1)
EXIT_CODE=$?

# Shell should be blocked because base disables it and child inherits
check "T5a: shell blocked by inherited config" "1" "$([ $EXIT_CODE -ne 0 ] && echo 1 || echo 0)"
check_contains "T5b: shell governance block message" "denied" "$OUTPUT"


# ============================================================
# T6: Custom rules inherited from base (polyglot blocks)
# ============================================================
echo ""
echo "--- T6: Custom rules inheritance ---"
T6BASE="$WORKDIR/t6/base"
T6CHILD="$WORKDIR/t6/child"
mkdir -p "$T6BASE" "$T6CHILD"

cat > "$T6BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "custom_rules": [
    {
      "name": "no-marker",
      "pattern": "FORBIDDEN_MARKER",
      "message": "Marker not allowed",
      "level": "hard"
    }
  ],
  "security": { "sandbox_level": "elevated" }
}
EOF
sign_gov "$T6BASE"

cat > "$T6CHILD/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$T6CHILD"

# Custom rules apply to polyglot block content
printf 'main {\n    let r = <<sh\necho "FORBIDDEN_MARKER"\n>>\n    print(r)\n}\n' > "$T6CHILD/test.naab"

OUTPUT=$(cd "$T6CHILD" && "$NAAB" test.naab 2>&1)
EXIT_CODE=$?

check "T6a: custom rule inherited from base blocks" "1" "$([ $EXIT_CODE -ne 0 ] && echo 1 || echo 0)"
check_contains "T6b: custom rule error message" "Marker not allowed" "$OUTPUT"


# ============================================================
# T7: Missing extends file
# ============================================================
echo ""
echo "--- T7: Missing extends file ---"
T7="$WORKDIR/t7"
mkdir -p "$T7"

cat > "$T7/govern.json" << 'EOF'
{
  "extends": "../nonexistent/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$T7"

cat > "$T7/test.naab" << 'EOF'
main { print("missing") }
EOF

OUTPUT=$(cd "$T7" && "$NAAB" test.naab 2>&1)
EXIT_CODE=$?

check "T7a: missing extends file rejected" "1" "$([ $EXIT_CODE -ne 0 ] && echo 1 || echo 0)"
check_contains "T7b: cannot open error" "Cannot open extends base" "$OUTPUT"


# ============================================================
# T8: No extends — backward compatibility
# ============================================================
echo ""
echo "--- T8: No extends (backward compat) ---"
T8="$WORKDIR/t8"
mkdir -p "$T8"

cat > "$T8/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": {
    "timeout": { "global": 30 }
  }
}
EOF
sign_gov "$T8"

cat > "$T8/test.naab" << 'EOF'
main { print("no_extends") }
EOF

OUTPUT=$(cd "$T8" && "$NAAB" test.naab 2>&1)
EXIT_CODE=$?

check "T8a: no extends works normally" "0" "$EXIT_CODE"
check_contains "T8b: output correct" "no_extends" "$OUTPUT"


# ============================================================
# T9: Multi-level extends chain (grandparent → parent → child)
# ============================================================
echo ""
echo "--- T9: Multi-level extends chain ---"
T9GP="$WORKDIR/t9/grandparent"
T9P="$WORKDIR/t9/parent"
T9C="$WORKDIR/t9/child"
mkdir -p "$T9GP" "$T9P" "$T9C"

cat > "$T9GP/govern.json" << 'EOF'
{
  "mode": "enforce",
  "capabilities": { "shell": { "enabled": false } },
  "limits": { "timeout": { "global": 60 } }
}
EOF
sign_gov "$T9GP"

cat > "$T9P/govern.json" << 'EOF'
{
  "extends": "../grandparent/govern.json",
  "limits": { "timeout": { "global": 30 } }
}
EOF
sign_gov "$T9P"

cat > "$T9C/govern.json" << 'EOF'
{
  "extends": "../parent/govern.json"
}
EOF
sign_gov "$T9C"

cat > "$T9C/test.naab" << 'EOF'
main { print("chain_ok") }
EOF

OUTPUT=$(cd "$T9C" && "$NAAB" run --governance-dashboard test.naab 2>&1)
EXIT_CODE=$?

check "T9a: 3-level chain loads" "0" "$EXIT_CODE"
check_contains "T9b: output correct" "chain_ok" "$OUTPUT"

# Verify grandparent capability propagates through parent to child
cat > "$T9C/test_shell.naab" << 'EOF'
main {
    let r = <<sh
echo "should_fail"
>>
    print(r)
}
EOF

OUTPUT2=$(cd "$T9C" && "$NAAB" run test_shell.naab 2>&1)
EXIT2=$?

check "T9c: grandparent shell block inherited" "1" "$([ $EXIT2 -ne 0 ] && echo 1 || echo 0)"


# ============================================================
# T10: Tree-walk extends
# ============================================================
echo ""
echo "--- T10: Tree-walk extends ---"
T10BASE="$WORKDIR/t10/base"
T10CHILD="$WORKDIR/t10/child"
mkdir -p "$T10BASE" "$T10CHILD"

cat > "$T10BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": { "timeout": { "global": 30 } }
}
EOF
sign_gov "$T10BASE"

cat > "$T10CHILD/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "limits": { "timeout": { "global": 10 } }
}
EOF
sign_gov "$T10CHILD"

cat > "$T10CHILD/test.naab" << 'EOF'
main { print("tree_walk_extends") }
EOF

OUTPUT=$(cd "$T10CHILD" && "$NAAB" run --tree-walk --governance-dashboard test.naab 2>&1)
EXIT_CODE=$?

check "T10a: tree-walk extends loads" "0" "$EXIT_CODE"
check_contains "T10b: tree-walk output correct" "tree_walk_extends" "$OUTPUT"


# ============================================================
# T11: M4 field inheritance (restrictions inherited from parent)
# ============================================================
echo ""
echo "--- T11: M4 field inheritance ---"
T11BASE="$WORKDIR/t11/base"
T11CHILD="$WORKDIR/t11/child"
mkdir -p "$T11BASE" "$T11CHILD"

cat > "$T11BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "restrictions": {
    "dangerous_calls": { "enabled": true, "level": "hard" }
  }
}
EOF
sign_gov "$T11BASE"

cat > "$T11CHILD/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$T11CHILD"

cat > "$T11CHILD/test_safe.naab" << 'EOF'
main { print("m4_ok") }
EOF

OUTPUT=$(cd "$T11CHILD" && "$NAAB" run test_safe.naab 2>&1)
EXIT_CODE=$?
check "T11a: M4 inheritance loads" "0" "$EXIT_CODE"
check_contains "T11b: safe code passes" "m4_ok" "$OUTPUT"

# Dangerous code should be blocked by inherited restriction
cat > "$T11CHILD/test_danger.naab" << 'EOF'
main {
    let r = <<python
import os
os.system("echo pwned")
>>
    print(r)
}
EOF

OUTPUT2=$(cd "$T11CHILD" && "$NAAB" run test_danger.naab 2>&1)
EXIT2=$?

check "T11c: inherited restriction blocks" "3" "$EXIT2"
check_contains "T11d: dangerous pattern detected" "Dangerous pattern" "$OUTPUT2"


echo ""
echo "Results: $PASS passed, $FAIL failed out of $((PASS + FAIL))"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
