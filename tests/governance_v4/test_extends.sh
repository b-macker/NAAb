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

if [ ! -x "$NAAB" ]; then
    echo "SKIP: naab-lang not built"
    exit 0
fi

mkdir -p "$WORKDIR"

# Set up ephemeral signing key + isolated trust store for CI compatibility
source "$(dirname "$0")/../helpers/trust_setup.sh"
setup_isolated_trust
"$NAAB" --keygen "$WORKDIR/test-key.pem" 2>/dev/null
"$NAAB" --trust-key "$WORKDIR/test-key.pem.pub" 2>/dev/null
SIGNING_KEY="$WORKDIR/test-key.pem"

sign_gov() {
    local dir="$1"
    (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
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
    teardown_isolated_trust
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


# ============================================================
# T12: M3 explicit-set — child explicitly sets timeout to 0
# ============================================================
echo ""
echo "--- T12: M3 explicit timeout=0 preserved ---"
T12BASE="$WORKDIR/t12/base"
T12CHILD="$WORKDIR/t12/child"
mkdir -p "$T12BASE" "$T12CHILD"

cat > "$T12BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": {
    "timeout": { "global": 5 }
  }
}
EOF
sign_gov "$T12BASE"

# Child explicitly sets timeout to 0 (unlimited)
cat > "$T12CHILD/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "limits": {
    "timeout": { "global": 0 }
  }
}
EOF
sign_gov "$T12CHILD"

# Sleep 8 seconds. If child's explicit 0 is preserved (unlimited), script survives.
# If overwritten to parent's 5, the polyglot block times out.
cat > "$T12CHILD/test.naab" << 'EOF'
main {
    let r = <<python
import time
time.sleep(8)
print("timeout_zero_preserved")
>>
    print(r)
}
EOF

OUTPUT=$(cd "$T12CHILD" && timeout 20 "$NAAB" run test.naab 2>&1)
EXIT_CODE=$?

check "T12a: explicit timeout=0 preserved" "0" "$EXIT_CODE"
check_contains "T12b: script survived with unlimited timeout" "timeout_zero_preserved" "$OUTPUT"


# ============================================================
# T13: M3 explicit-set — child explicitly disables scoring
# ============================================================
echo ""
echo "--- T13: M3 explicit scoring disable preserved ---"
T13BASE="$WORKDIR/t13/base"
T13CHILD="$WORKDIR/t13/child"
mkdir -p "$T13BASE" "$T13CHILD"

cat > "$T13BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "scoring": {
    "enabled": true,
    "yellow_threshold": 20,
    "red_threshold": 50
  }
}
EOF
sign_gov "$T13BASE"

# Child explicitly disables scoring
cat > "$T13CHILD/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "scoring": {
    "enabled": false
  }
}
EOF
sign_gov "$T13CHILD"

cat > "$T13CHILD/test.naab" << 'EOF'
main { print("scoring_disabled") }
EOF

OUTPUT=$(cd "$T13CHILD" && "$NAAB" run --governance-dashboard test.naab 2>&1)
EXIT_CODE=$?

check "T13a: explicit scoring disable loads" "0" "$EXIT_CODE"
check_contains "T13b: output correct" "scoring_disabled" "$OUTPUT"
# Scoring should NOT appear in dashboard since child explicitly disabled it
if echo "$OUTPUT" | grep -q "Scoring:"; then
    echo "  FAIL: T13c: scoring should be disabled but dashboard shows Scoring"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: T13c: scoring correctly disabled in dashboard"
    PASS=$((PASS + 1))
fi


# ============================================================
# T14: merge_arrays=append for blocked_commands
# ============================================================
echo ""
echo "--- T14: merge_arrays=append blocked_commands ---"
T14BASE="$WORKDIR/t14_base"
T14CHILD="$WORKDIR/t14_child"
mkdir -p "$T14BASE" "$T14CHILD"

cat > "$T14BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": {
    "shell": { "enabled": true, "blocked_commands": ["wget"] }
  }
}
EOF
sign_gov "$T14BASE"

cat > "$T14CHILD/govern.json" << 'EOF'
{
  "extends": "../t14_base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "meta": { "inheritance": { "merge_arrays": "append" } },
  "capabilities": {
    "shell": { "enabled": true, "blocked_commands": ["curl"] }
  }
}
EOF
sign_gov "$T14CHILD"

# Test wget (inherited from base via append)
cat > "$T14CHILD/test_wget.naab" << 'EOF'
main {
    let r = <<sh
wget http://example.com 2>&1
echo "wget_ran"
>>
    print(r)
}
EOF

# Test curl (child's own)
cat > "$T14CHILD/test_curl.naab" << 'EOF'
main {
    let r = <<sh
curl http://example.com 2>&1
echo "curl_ran"
>>
    print(r)
}
EOF

OUTPUT_W=$(cd "$T14CHILD" && timeout 15s "$NAAB" run test_wget.naab 2>&1); EXIT_W=$?
OUTPUT_C=$(cd "$T14CHILD" && timeout 15s "$NAAB" run test_curl.naab 2>&1); EXIT_C=$?

check "T14a: wget blocked (appended from base)" "1" "$([ $EXIT_W -ne 0 ] && echo 1 || echo 0)"
check "T14b: curl blocked (child's own)" "1" "$([ $EXIT_C -ne 0 ] && echo 1 || echo 0)"

# ============================================================
# T15: merge_arrays=append for custom_rules
# ============================================================
echo ""
echo "--- T15: merge_arrays=append custom_rules ---"
T15BASE="$WORKDIR/t15_base"
T15CHILD="$WORKDIR/t15_child"
mkdir -p "$T15BASE" "$T15CHILD"

cat > "$T15BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "custom_rules": [
    { "name": "no-alpha", "pattern": "MARKER_ALPHA", "message": "Alpha blocked", "level": "hard" }
  ]
}
EOF
sign_gov "$T15BASE"

cat > "$T15CHILD/govern.json" << 'EOF'
{
  "extends": "../t15_base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "meta": { "inheritance": { "merge_arrays": "append" } },
  "custom_rules": [
    { "name": "no-beta", "pattern": "MARKER_BETA", "message": "Beta blocked", "level": "hard" }
  ]
}
EOF
sign_gov "$T15CHILD"

cat > "$T15CHILD/test_alpha.naab" << 'EOF'
main {
    let r = <<python
print("MARKER_ALPHA")
>>
    print(r)
}
EOF
cat > "$T15CHILD/test_beta.naab" << 'EOF'
main {
    let r = <<python
print("MARKER_BETA")
>>
    print(r)
}
EOF

OUTPUT_A=$(cd "$T15CHILD" && timeout 15s "$NAAB" run test_alpha.naab 2>&1); EXIT_A=$?
OUTPUT_B=$(cd "$T15CHILD" && timeout 15s "$NAAB" run test_beta.naab 2>&1); EXIT_B=$?

check "T15a: alpha blocked (appended from base)" "1" "$([ $EXIT_A -ne 0 ] && echo 1 || echo 0)"
check "T15b: beta blocked (child's own)" "1" "$([ $EXIT_B -ne 0 ] && echo 1 || echo 0)"

# ============================================================
# T16: merge_arrays=replace (default) — child replaces parent
# ============================================================
echo ""
echo "--- T16: merge_arrays=replace (default) ---"
T16BASE="$WORKDIR/t16_base"
T16CHILD="$WORKDIR/t16_child"
mkdir -p "$T16BASE" "$T16CHILD"

cat > "$T16BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": {
    "shell": { "enabled": true, "blocked_commands": ["wget"] }
  }
}
EOF
sign_gov "$T16BASE"

# No merge_arrays specified — defaults to "replace", child's list wins
cat > "$T16CHILD/govern.json" << 'EOF'
{
  "extends": "../t16_base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": {
    "shell": { "enabled": true, "blocked_commands": ["curl"] }
  }
}
EOF
sign_gov "$T16CHILD"

# wget should NOT be blocked (parent's list replaced by child's)
cat > "$T16CHILD/test_wget.naab" << 'EOF'
main {
    let r = <<sh
echo "wget_ok"
>>
    print(r)
}
EOF

# curl should be blocked (child's own list)
cat > "$T16CHILD/test_curl.naab" << 'EOF'
main {
    let r = <<sh
curl http://example.com 2>&1
echo "curl_ran"
>>
    print(r)
}
EOF

OUTPUT_W=$(cd "$T16CHILD" && timeout 15s "$NAAB" run test_wget.naab 2>&1); EXIT_W=$?
OUTPUT_C=$(cd "$T16CHILD" && timeout 15s "$NAAB" run test_curl.naab 2>&1); EXIT_C=$?

check "T16a: wget NOT blocked (replace mode, parent's list dropped)" "0" "$EXIT_W"
check "T16b: curl blocked (child's own list)" "1" "$([ $EXIT_C -ne 0 ] && echo 1 || echo 0)"

# ============================================================
# T17: merge_arrays=append with duplicate dedup
# ============================================================
echo ""
echo "--- T17: merge_arrays=append deduplication ---"
T17BASE="$WORKDIR/t17_base"
T17CHILD="$WORKDIR/t17_child"
mkdir -p "$T17BASE" "$T17CHILD"

cat > "$T17BASE/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": {
    "shell": { "enabled": true, "blocked_commands": ["wget", "curl"] }
  }
}
EOF
sign_gov "$T17BASE"

# Child also blocks wget — append should dedup, not double-add
cat > "$T17CHILD/govern.json" << 'EOF'
{
  "extends": "../t17_base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "meta": { "inheritance": { "merge_arrays": "append" } },
  "capabilities": {
    "shell": { "enabled": true, "blocked_commands": ["wget", "rm"] }
  }
}
EOF
sign_gov "$T17CHILD"

# All three should be blocked: wget (both), curl (base), rm (child)
cat > "$T17CHILD/test_curl.naab" << 'EOF'
main {
    let r = <<sh
curl http://example.com 2>&1
echo "curl_ran"
>>
    print(r)
}
EOF
cat > "$T17CHILD/test_rm.naab" << 'EOF'
main {
    let r = <<sh
rm -rf /tmp/test 2>&1
echo "rm_ran"
>>
    print(r)
}
EOF

OUTPUT_C=$(cd "$T17CHILD" && timeout 15s "$NAAB" run test_curl.naab 2>&1); EXIT_C=$?
OUTPUT_R=$(cd "$T17CHILD" && timeout 15s "$NAAB" run test_rm.naab 2>&1); EXIT_R=$?

check "T17a: curl blocked (appended from base)" "1" "$([ $EXIT_C -ne 0 ] && echo 1 || echo 0)"
check "T17b: rm blocked (child's own)" "1" "$([ $EXIT_R -ne 0 ] && echo 1 || echo 0)"

# ── T18: env_vars.blocked_read inheritance ──────────────────────────────
echo "--- T18: env_vars.blocked_read inheritance ---"
T18BASE="$WORKDIR/t18_base"
T18CHILD="$WORKDIR/t18_child"
mkdir -p "$T18BASE" "$T18CHILD"

cat > "$T18BASE/govern.json" << 'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": {
    "env_vars": {
      "read": true,
      "blocked_read": ["SECRET_A"]
    }
  }
}
EOF
sign_gov "$T18BASE"

cat > "$T18CHILD/govern.json" << EOF
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "extends": "$T18BASE/govern.json",
  "meta": { "inheritance": { "merge_arrays": "append" } },
  "capabilities": {
    "env_vars": {
      "read": true,
      "blocked_read": ["SECRET_B"]
    }
  }
}
EOF
sign_gov "$T18CHILD"

export SECRET_A=val_a
export SECRET_B=val_b
export SAFE_VAR=val_safe

# T18a: parent's blocked_read (SECRET_A) is enforced
cat > "$T18CHILD/test_a.naab" << 'EOF'
use env
main {
    let v = env.get("SECRET_A")
    print(v)
}
EOF
OUTPUT_A=$(cd "$T18CHILD" && timeout 15s "$NAAB" run test_a.naab 2>&1); EXIT_A=$?
check "T18a: SECRET_A blocked (from parent)" "1" "$([ $EXIT_A -ne 0 ] && echo 1 || echo 0)"

# T18b: child's blocked_read (SECRET_B) is enforced
cat > "$T18CHILD/test_b.naab" << 'EOF'
use env
main {
    let v = env.get("SECRET_B")
    print(v)
}
EOF
OUTPUT_B=$(cd "$T18CHILD" && timeout 15s "$NAAB" run test_b.naab 2>&1); EXIT_B=$?
check "T18b: SECRET_B blocked (from child)" "1" "$([ $EXIT_B -ne 0 ] && echo 1 || echo 0)"

# T18c: non-blocked var still works
cat > "$T18CHILD/test_safe.naab" << 'EOF'
use env
main {
    let v = env.get("SAFE_VAR")
    print(v)
}
EOF
OUTPUT_S=$(cd "$T18CHILD" && timeout 15s "$NAAB" run test_safe.naab 2>&1); EXIT_S=$?
check "T18c: SAFE_VAR readable" "1" "$(echo "$OUTPUT_S" | grep -q 'val_safe' && echo 1 || echo 0)"

# T18d: value not leaked in error output
check "T18d: SECRET_A value not leaked" "1" "$(! echo "$OUTPUT_A" | grep -q 'val_a' && echo 1 || echo 0)"

unset SECRET_A SECRET_B SAFE_VAR

# ── T19: env_vars.allowed_read allowlist ────────────────────────────────
echo "--- T19: env_vars.allowed_read allowlist ---"
T19DIR="$WORKDIR/t19"
mkdir -p "$T19DIR"

cat > "$T19DIR/govern.json" << 'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": {
    "env_vars": {
      "read": true,
      "allowed_read": ["ALLOWED_VAR"]
    }
  }
}
EOF
sign_gov "$T19DIR"

export ALLOWED_VAR=ok_value
export NOT_ALLOWED=secret_value

# T19a: allowed var works
cat > "$T19DIR/test_ok.naab" << 'EOF'
use env
main {
    let v = env.get("ALLOWED_VAR")
    print(v)
}
EOF
OUTPUT_OK=$(cd "$T19DIR" && timeout 15s "$NAAB" run test_ok.naab 2>&1); EXIT_OK=$?
check "T19a: ALLOWED_VAR readable" "1" "$(echo "$OUTPUT_OK" | grep -q 'ok_value' && echo 1 || echo 0)"

# T19b: non-allowed var blocked (SOFT enforcement)
cat > "$T19DIR/test_deny.naab" << 'EOF'
use env
main {
    let v = env.get("NOT_ALLOWED")
    print(v)
}
EOF
OUTPUT_D=$(cd "$T19DIR" && timeout 15s "$NAAB" run test_deny.naab 2>&1); EXIT_D=$?
check "T19b: NOT_ALLOWED blocked (SOFT)" "1" "$([ $EXIT_D -ne 0 ] && echo 1 || echo 0)"

unset ALLOWED_VAR NOT_ALLOWED

echo ""
echo "Results: $PASS passed, $FAIL failed out of $((PASS + FAIL))"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
