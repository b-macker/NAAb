#!/usr/bin/env bash
# ============================================================================
# NAAb-37: Enterprise Readiness Chaos Test
#
# 28 tests (~56 assertions) covering:
#   Cat 1: Polyglot Reload Chaos (C37.1-C37.6)
#   Cat 2: Extends Adversarial (C37.7-C37.14)
#   Cat 3: M3 Explicit-Set Matrix (C37.15-C37.20)
#   Cat 4: Security Floor (C37.21-C37.24)
#   Cat 5: Enterprise Config Edge Cases (C37.25-C37.28)
#
# Usage: bash run-naab37.sh [--cat N]
# ============================================================================

LANG_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
NAAB="$LANG_DIR/build/naab-lang"
TMPBASE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
WORKDIR="$TMPBASE/naab37_$$"
SIGNING_KEY="${HOME}/.naab/keys/signing.pem"

PASS=0; FAIL=0; TOTAL=0
FAILURES=""

# Category selection
RUN_CAT=0
if [ "${1:-}" = "--cat" ] && [ -n "${2:-}" ]; then
    RUN_CAT="$2"
fi
should_run() { [ "$RUN_CAT" -eq 0 ] || [ "$RUN_CAT" -eq "$1" ]; }

check() {
    local desc="$1" expected="$2" actual="$3"
    TOTAL=$((TOTAL + 1))
    if [ "$expected" = "$actual" ]; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected '$expected', got '$actual')"
        FAIL=$((FAIL + 1))
        FAILURES="${FAILURES}\n  $desc"
    fi
}

check_contains() {
    local desc="$1" pattern="$2" text="$3"
    TOTAL=$((TOTAL + 1))
    if echo "$text" | grep -q "$pattern"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (pattern '$pattern' not found)"
        FAIL=$((FAIL + 1))
        FAILURES="${FAILURES}\n  $desc"
    fi
}

check_not_contains() {
    local desc="$1" pattern="$2" text="$3"
    TOTAL=$((TOTAL + 1))
    if echo "$text" | grep -q "$pattern"; then
        echo "  FAIL: $desc (pattern '$pattern' should NOT be present)"
        FAIL=$((FAIL + 1))
        FAILURES="${FAILURES}\n  $desc"
    else
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    fi
}

sign_gov() {
    local dir="$1"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
}

cleanup() { rm -rf "$WORKDIR" 2>/dev/null; }
trap cleanup EXIT

if [ ! -x "$NAAB" ]; then
    echo "SKIP: naab-lang not built"
    exit 0
fi

mkdir -p "$WORKDIR"

echo ""
echo "================================================================"
echo "  NAAb-37: Enterprise Readiness Chaos Test"
echo "================================================================"

# ====================================================================
# Cat 1: Polyglot Reload Chaos (C37.1-C37.6)
# ====================================================================
if should_run 1; then
echo ""
echo "--- Cat 1: Polyglot Reload Chaos ---"

# ---------------------------------------------------------------
# C37.1: Tighten timeout mid-run
# ---------------------------------------------------------------
echo ""
echo "  C37.1: Tighten timeout mid-run"
D="$WORKDIR/c37_1"
mkdir -p "$D" "$D/tight"

cat > "$D/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 30 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D"

cat > "$D/tight/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 2 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D/tight"

# Block 1 (python): sleep 2 for mtime gap, copy tight config+sig
# Block 2 (shell): sleep 5 — should time out under 2s after reload
cat > "$D/test.naab" << NAABEOF
main {
    let r1 = <<python
import shutil, time
time.sleep(2)
shutil.copy("$D/tight/govern.json", "$D/govern.json")
shutil.copy("$D/tight/govern.json.sig", "$D/govern.json.sig")
print("block1_ok")
>>
    print(r1)

    let r2 = <<sh
    sleep 5
    echo "survived_tighten"
    >>
    print(r2)
}
NAABEOF

OUTPUT=$(cd "$D" && timeout 20s "$NAAB" test.naab 2>&1); EXIT=$?
check_contains "C37.1a: block1 ran before tighten" "block1_ok" "$OUTPUT"
check_not_contains "C37.1b: block2 timed out after tighten" "survived_tighten" "$OUTPUT"

# ---------------------------------------------------------------
# C37.2: Ratchet rejects loosened timeout
# ---------------------------------------------------------------
echo ""
echo "  C37.2: Ratchet rejects loosened timeout"
D="$WORKDIR/c37_2"
mkdir -p "$D" "$D/loose"

cat > "$D/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 5 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D"

cat > "$D/loose/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 60 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D/loose"

# Block 1 (python): sleep 2 for mtime gap, copy loose config+sig
# Block 2 (shell): sleep 8 — should still time out under 5s (ratchet rejects loosening)
cat > "$D/test.naab" << NAABEOF
main {
    let r1 = <<python
import shutil, time
time.sleep(2)
shutil.copy("$D/loose/govern.json", "$D/govern.json")
shutil.copy("$D/loose/govern.json.sig", "$D/govern.json.sig")
print("before_loosen")
>>
    print(r1)

    let r2 = <<sh
    sleep 8
    echo "survived_loosen"
    >>
    print(r2)
}
NAABEOF

OUTPUT=$(cd "$D" && timeout 20s "$NAAB" test.naab 2>&1); EXIT=$?
check_not_contains "C37.2a: loosened timeout rejected" "survived_loosen" "$OUTPUT"
check_contains "C37.2b: ratchet violation reported" "ratchet\|Ratchet\|loosened\|Loosened" "$OUTPUT"

# ---------------------------------------------------------------
# C37.3: Corrupt signature mid-run
# ---------------------------------------------------------------
echo ""
echo "  C37.3: Corrupt signature mid-run"
D="$WORKDIR/c37_3"
mkdir -p "$D"

cat > "$D/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 30 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D"

cat > "$D/test.naab" << 'NAABEOF'
main {
    let r1 = <<sh
    echo "GARBAGE_SIG" > govern.json.sig
    echo "block1_corrupt"
    sleep 1
    >>
    print(r1)

    let r2 = <<sh
    echo "survived_corrupt"
    >>
    print(r2)
}
NAABEOF

OUTPUT=$(cd "$D" && timeout 15s "$NAAB" test.naab 2>&1); EXIT=$?
check_contains "C37.3a: survived corrupt sig (old config preserved)" "survived_corrupt" "$OUTPUT"
check "C37.3b: exit 0 despite corrupt sig" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.4: Malformed JSON mid-run
# ---------------------------------------------------------------
echo ""
echo "  C37.4: Malformed JSON mid-run"
D="$WORKDIR/c37_4"
mkdir -p "$D"

cat > "$D/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 30 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D"

cat > "$D/test.naab" << 'NAABEOF'
main {
    let r1 = <<sh
    echo "{INVALID" > govern.json
    echo "block1_malformed"
    sleep 1
    >>
    print(r1)

    let r2 = <<sh
    echo "survived_malformed"
    >>
    print(r2)
}
NAABEOF

OUTPUT=$(cd "$D" && timeout 15s "$NAAB" test.naab 2>&1); EXIT=$?
check_contains "C37.4a: survived malformed JSON (old config preserved)" "survived_malformed" "$OUTPUT"
check "C37.4b: exit 0 despite malformed JSON" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.5: Revoke shell mid-run
# ---------------------------------------------------------------
echo ""
echo "  C37.5: Revoke shell mid-run (python->shell transition)"
D="$WORKDIR/c37_5"
mkdir -p "$D" "$D/tight"

cat > "$D/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 30 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D"

cat > "$D/tight/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": false } },
  "limits": { "timeout": { "global": 30 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D/tight"

cat > "$D/test.naab" << NAABEOF
main {
    let r1 = <<python
import shutil, time
time.sleep(2)
shutil.copy("$D/tight/govern.json", "$D/govern.json")
shutil.copy("$D/tight/govern.json.sig", "$D/govern.json.sig")
print("python_revoked_shell")
>>
    print(r1)

    try {
        let r2 = <<sh
        echo "should_not_appear"
        >>
        print(r2)
    } catch (e) {
        print("BLOCKED: " + e)
    }
}
NAABEOF

OUTPUT=$(cd "$D" && timeout 20s "$NAAB" test.naab 2>&1); EXIT=$?
check_contains "C37.5a: python block ran" "python_revoked_shell" "$OUTPUT"
check_not_contains "C37.5b: shell blocked after revocation" "should_not_appear" "$OUTPUT"

# ---------------------------------------------------------------
# C37.6: Config touch, no content change
# ---------------------------------------------------------------
echo ""
echo "  C37.6: Config touch, no content change"
D="$WORKDIR/c37_6"
mkdir -p "$D"

cat > "$D/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 30 }, "execution": { "polyglot_blocks": 10 } }
}
EOF
sign_gov "$D"

cat > "$D/test.naab" << 'NAABEOF'
main {
    let r1 = <<sh
    touch govern.json
    echo "block1_touched"
    sleep 1
    >>
    print(r1)

    let r2 = <<sh
    echo "touch_noop_ok"
    >>
    print(r2)
}
NAABEOF

OUTPUT=$(cd "$D" && timeout 15s "$NAAB" test.naab 2>&1); EXIT=$?
check_contains "C37.6a: touch noop preserves behavior" "touch_noop_ok" "$OUTPUT"
check "C37.6b: exit 0" "0" "$EXIT"

fi  # Cat 1

# ====================================================================
# Cat 2: Extends Adversarial (C37.7-C37.14)
# ====================================================================
if should_run 2; then
echo ""
echo "--- Cat 2: Extends Adversarial ---"

# ---------------------------------------------------------------
# C37.7: Explicit-zero in 3-level chain
# ---------------------------------------------------------------
echo ""
echo "  C37.7: Explicit-zero in 3-level chain"
D="$WORKDIR/c37_7"
mkdir -p "$D/gp" "$D/parent" "$D/child"

cat > "$D/gp/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": { "timeout": { "global": 60 } }
}
EOF
sign_gov "$D/gp"

cat > "$D/parent/govern.json" << 'EOF'
{
  "extends": "../gp/govern.json",
  "mode": "enforce",
  "limits": { "timeout": { "global": 0 } }
}
EOF
sign_gov "$D/parent"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../parent/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r = <<python
import time
time.sleep(8)
print("zero_chain_ok")
>>
    print(r)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 20s "$NAAB" run test.naab 2>&1); EXIT=$?
check_contains "C37.7a: explicit-zero propagates through chain" "zero_chain_ok" "$OUTPUT"
check "C37.7b: exit 0" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.8: Conflicting merge strategies in chain
# ---------------------------------------------------------------
echo ""
echo "  C37.8: Conflicting merge strategies in chain"
D="$WORKDIR/c37_8"
mkdir -p "$D/gp" "$D/parent" "$D/child"

cat > "$D/gp/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 60 } }
}
EOF
sign_gov "$D/gp"

# Parent uses parent_wins: GP's 60 overwrites parent's 10
cat > "$D/parent/govern.json" << 'EOF'
{
  "extends": "../gp/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "meta": { "inheritance": { "merge_strategy": "parent_wins" } },
  "limits": { "timeout": { "global": 10 } }
}
EOF
sign_gov "$D/parent"

# Child uses default child_wins, explicit timeout=2
cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../parent/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 2 } }
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r = <<sh
sleep 5
echo "survived_conflict"
>>
    print(r)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 20s "$NAAB" run test.naab 2>&1); EXIT=$?
check_not_contains "C37.8a: child's 2s timeout enforced" "survived_conflict" "$OUTPUT"
check "C37.8b: non-zero exit (timeout)" "1" "$([ $EXIT -ne 0 ] && echo 1 || echo 0)"

# ---------------------------------------------------------------
# C37.9: Child re-enables shell (child_wins)
# ---------------------------------------------------------------
echo ""
echo "  C37.9: Child re-enables shell (child_wins)"
D="$WORKDIR/c37_9"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": false } },
  "limits": { "timeout": { "global": 30 } }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } }
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r = <<sh
echo "shell_reenabled"
>>
    print(r)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 15s "$NAAB" run test.naab 2>&1); EXIT=$?
check_contains "C37.9a: child_wins re-enables shell" "shell_reenabled" "$OUTPUT"
check "C37.9b: exit 0" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.10: Mode ratchet across extends
# ---------------------------------------------------------------
echo ""
echo "  C37.10: Mode ratchet across extends"
D="$WORKDIR/c37_10"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "restrictions": {
    "dangerous_calls": { "enabled": true, "level": "hard" }
  }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "audit"
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r = <<python
import os
os.system("echo pwned")
>>
    print(r)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 15s "$NAAB" run test.naab 2>&1); EXIT=$?
check "C37.10a: mode ratchet enforces (exit 3)" "3" "$EXIT"
check_contains "C37.10b: dangerous pattern detected" "Dangerous pattern\|dangerous" "$OUTPUT"

# ---------------------------------------------------------------
# C37.11: Multiple children share parent, no cross-contamination
# ---------------------------------------------------------------
echo ""
echo "  C37.11: Multiple children share parent"
D="$WORKDIR/c37_11"
mkdir -p "$D/parent" "$D/childA" "$D/childB"

cat > "$D/parent/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 30 } }
}
EOF
sign_gov "$D/parent"

cat > "$D/childA/govern.json" << 'EOF'
{
  "extends": "../parent/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 5 } }
}
EOF
sign_gov "$D/childA"

cat > "$D/childB/govern.json" << 'EOF'
{
  "extends": "../parent/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } }
}
EOF
sign_gov "$D/childB"

cat > "$D/childA/test.naab" << 'EOF'
main {
    let r = <<sh
sleep 8
echo "a_survived"
>>
    print(r)
}
EOF

cat > "$D/childB/test.naab" << 'EOF'
main {
    let r = <<sh
sleep 8
echo "b_survived"
>>
    print(r)
}
EOF

OUTPUT_A=$(cd "$D/childA" && timeout 20s "$NAAB" run test.naab 2>&1); EXIT_A=$?
OUTPUT_B=$(cd "$D/childB" && timeout 20s "$NAAB" run test.naab 2>&1); EXIT_B=$?

check_not_contains "C37.11a: childA timed out (5s)" "a_survived" "$OUTPUT_A"
check_contains "C37.11b: childB survived (inherited 30s)" "b_survived" "$OUTPUT_B"

# ---------------------------------------------------------------
# C37.12: Custom rules accumulate via append merge
# ---------------------------------------------------------------
echo ""
echo "  C37.12: Custom rules accumulate via append merge"
D="$WORKDIR/c37_12"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "custom_rules": [
    {
      "name": "no-alpha",
      "pattern": "MARKER_ALPHA",
      "message": "Alpha blocked",
      "level": "hard"
    }
  ]
}
EOF
sign_gov "$D/base"

# Child uses merge_arrays=append to accumulate custom rules from base
cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "meta": { "inheritance": { "merge_arrays": "append" } },
  "custom_rules": [
    {
      "name": "no-beta",
      "pattern": "MARKER_BETA",
      "message": "Beta blocked",
      "level": "hard"
    }
  ]
}
EOF
sign_gov "$D/child"

# Test alpha (inherited from base via append)
cat > "$D/child/test_alpha.naab" << 'EOF'
main {
    let r = <<python
print("MARKER_ALPHA")
>>
    print(r)
}
EOF

# Test beta (child's own rule)
cat > "$D/child/test_beta.naab" << 'EOF'
main {
    let r = <<python
print("MARKER_BETA")
>>
    print(r)
}
EOF

OUTPUT_A=$(cd "$D/child" && timeout 15s "$NAAB" run test_alpha.naab 2>&1); EXIT_A=$?
OUTPUT_B=$(cd "$D/child" && timeout 15s "$NAAB" run test_beta.naab 2>&1); EXIT_B=$?

check "C37.12a: inherited alpha rule blocks (append merge)" "1" "$([ $EXIT_A -ne 0 ] && echo 1 || echo 0)"
check "C37.12b: child beta rule blocks" "1" "$([ $EXIT_B -ne 0 ] && echo 1 || echo 0)"

# ---------------------------------------------------------------
# C37.13: Extends to absolute path
# ---------------------------------------------------------------
echo ""
echo "  C37.13: Extends to absolute path"
D="$WORKDIR/c37_13"
mkdir -p "$D/abs_parent" "$D/child"

cat > "$D/abs_parent/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": { "timeout": { "global": 30 } }
}
EOF
sign_gov "$D/abs_parent"

# Child extends using absolute path
cat > "$D/child/govern.json" << EOF
{
  "extends": "$D/abs_parent/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main { print("abs_path_ok") }
EOF

OUTPUT=$(cd "$D/child" && timeout 10s "$NAAB" run --governance-dashboard test.naab 2>&1); EXIT=$?
check_contains "C37.13a: absolute path extends works" "abs_path_ok" "$OUTPUT"
check "C37.13b: exit 0" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.14: Extends with all 4 enterprise configs inherited
# ---------------------------------------------------------------
echo ""
echo "  C37.14: All 4 enterprise configs inherited"
D="$WORKDIR/c37_14"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "scoring": { "enabled": true, "yellow_threshold": 20, "red_threshold": 50 },
  "telemetry": { "enabled": true, "output_file": "telem.jsonl" },
  "restrictions": { "dangerous_calls": { "enabled": true, "level": "hard" } },
  "circuit_breaker": { "enabled": true, "max_level": 5 }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main { print("enterprise_inherited") }
EOF

OUTPUT=$(cd "$D/child" && timeout 10s "$NAAB" run --governance-dashboard test.naab 2>&1); EXIT=$?
check_contains "C37.14a: governance active" "Governance\|governance" "$OUTPUT"
check "C37.14b: exit 0" "0" "$EXIT"

fi  # Cat 2

# ====================================================================
# Cat 3: M3 Explicit-Set Matrix (C37.15-C37.20)
# ====================================================================
if should_run 3; then
echo ""
echo "--- Cat 3: M3 Explicit-Set Matrix ---"

# ---------------------------------------------------------------
# C37.15: All 4 bug categories in one chain
# ---------------------------------------------------------------
echo ""
echo "  C37.15: All 4 bug categories in one chain"
D="$WORKDIR/c37_15"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": { "timeout": { "global": 60 } },
  "scoring": { "enabled": true, "yellow_threshold": 20, "red_threshold": 50 },
  "circuit_breaker": { "enabled": true, "max_level": 5 },
  "restrictions": { "dangerous_calls": { "enabled": true, "level": "hard" } }
}
EOF
sign_gov "$D/base"

# Child explicitly disables everything (M3: all 4 bug categories)
cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "limits": { "timeout": { "global": 0 } },
  "scoring": { "enabled": false },
  "circuit_breaker": { "enabled": false },
  "restrictions": { "dangerous_calls": false }
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r = <<python
import time
time.sleep(8)
print("all_categories_ok")
>>
    print(r)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 20s "$NAAB" run --governance-dashboard test.naab 2>&1); EXIT=$?
check_contains "C37.15a: timeout=0 honored (survived 8s)" "all_categories_ok" "$OUTPUT"
check_not_contains "C37.15b: scoring disabled in dashboard" "Scoring:" "$OUTPUT"

# ---------------------------------------------------------------
# C37.16: 5 enabled=true default structs inherit from parent
# ---------------------------------------------------------------
echo ""
echo "  C37.16: 5 enabled=true default structs inherit"
D="$WORKDIR/c37_16"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "contradiction_detection": { "enabled": true },
  "vcs_secret_extraction": { "enabled": true },
  "obfuscation": { "enabled": true },
  "duplicate_calls": { "enabled": true },
  "polyglot_try_catch": { "enabled": true }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main { print("structs_inherited") }
EOF

OUTPUT=$(cd "$D/child" && timeout 10s "$NAAB" run test.naab 2>&1); EXIT=$?
check_contains "C37.16a: config loads with 5 structs" "structs_inherited" "$OUTPUT"
check "C37.16b: exit 0" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.17: Boolean false vs unset (dangerous_calls)
# ---------------------------------------------------------------
echo ""
echo "  C37.17: Boolean false vs unset (dangerous_calls)"
D="$WORKDIR/c37_17"
mkdir -p "$D/base" "$D/childA" "$D/childB"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "restrictions": { "dangerous_calls": { "enabled": true, "level": "hard" } }
}
EOF
sign_gov "$D/base"

# Child A: explicitly disables dangerous_calls
cat > "$D/childA/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "restrictions": { "dangerous_calls": false }
}
EOF
sign_gov "$D/childA"

# Child B: doesn't mention restrictions (inherits parent's enabled)
cat > "$D/childB/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$D/childB"

# Both run os.system() — A should pass (disabled), B should block (inherited)
cat > "$D/childA/test.naab" << 'EOF'
main {
    let r = <<python
import os
os.system("echo childA_ok")
>>
    print(r)
}
EOF
cat > "$D/childB/test.naab" << 'EOF'
main {
    let r = <<python
import os
os.system("echo childB_ok")
>>
    print(r)
}
EOF

OUTPUT_A=$(cd "$D/childA" && timeout 10s "$NAAB" run test.naab 2>&1); EXIT_A=$?
OUTPUT_B=$(cd "$D/childB" && timeout 10s "$NAAB" run test.naab 2>&1); EXIT_B=$?

check "C37.17a: childA dangerous_calls explicitly disabled (exit 0)" "0" "$EXIT_A"
check "C37.17b: childB inherits dangerous_calls block (exit 3)" "3" "$EXIT_B"

# ---------------------------------------------------------------
# C37.18: Zero vs unset for polyglot_blocks
# ---------------------------------------------------------------
echo ""
echo "  C37.18: Zero vs unset for polyglot_blocks"
D="$WORKDIR/c37_18"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "execution": { "polyglot_blocks": 3 } }
}
EOF
sign_gov "$D/base"

# Child explicitly sets polyglot_blocks=0 (unlimited)
cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "execution": { "polyglot_blocks": 0 } }
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r1 = <<sh
echo "blk_1"
>>
    print(r1)
    let r2 = <<sh
echo "blk_2"
>>
    print(r2)
    let r3 = <<sh
echo "blk_3"
>>
    print(r3)
    let r4 = <<sh
echo "blk_4"
>>
    print(r4)
    let r5 = <<sh
echo "blk_5"
>>
    print(r5)
    let r6 = <<sh
echo "blk_6"
>>
    print(r6)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 20s "$NAAB" run test.naab 2>&1); EXIT=$?
# All 6 blocks should run (parent's limit of 3 overridden by child's explicit 0)
FOUND=0
for i in 1 2 3 4 5 6; do
    echo "$OUTPUT" | grep -q "blk_$i" && FOUND=$((FOUND + 1))
done
check "C37.18a: all 6 blocks ran (explicit 0=unlimited)" "6" "$FOUND"
check "C37.18b: exit 0" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.19: Agent dispatch hard_stop explicit-zero
# ---------------------------------------------------------------
echo ""
echo "  C37.19: Agent dispatch hard_stop explicit-zero"
D="$WORKDIR/c37_19"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "agent_dispatch": { "hard_stop": { "max_calls_per_run": 100 } }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "agent_dispatch": { "hard_stop": { "max_calls_per_run": 0 } }
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main { print("hard_stop_zero_ok") }
EOF

OUTPUT=$(cd "$D/child" && timeout 10s "$NAAB" run --governance-dashboard test.naab 2>&1); EXIT=$?
check_contains "C37.19a: config loads without crash" "hard_stop_zero_ok" "$OUTPUT"
check "C37.19b: exit 0" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.20: parent_wins ignores explicit-set tracker
# ---------------------------------------------------------------
echo ""
echo "  C37.20: parent_wins ignores explicit-set tracker"
D="$WORKDIR/c37_20"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "limits": { "timeout": { "global": 30 } }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "meta": { "inheritance": { "merge_strategy": "parent_wins" } },
  "limits": { "timeout": { "global": 5 } }
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r = <<python
import time
time.sleep(8)
print("parent_wins_30s")
>>
    print(r)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 20s "$NAAB" run test.naab 2>&1); EXIT=$?
check_contains "C37.20a: parent's 30s wins over child's 5s" "parent_wins_30s" "$OUTPUT"
check "C37.20b: exit 0" "0" "$EXIT"

fi  # Cat 3

# ====================================================================
# Cat 4: Security Floor (C37.21-C37.24)
# ====================================================================
if should_run 4; then
echo ""
echo "--- Cat 4: Security Floor ---"

# ---------------------------------------------------------------
# C37.21: parent_wins restriction ratchet
# ---------------------------------------------------------------
echo ""
echo "  C37.21: parent_wins restriction ratchet"
D="$WORKDIR/c37_21"
mkdir -p "$D/base" "$D/child"

# Parent enables dangerous_calls restriction
cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "restrictions": { "dangerous_calls": { "enabled": true, "level": "hard" } }
}
EOF
sign_gov "$D/base"

# Child tries to disable it under parent_wins — parent's restriction should win
cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "meta": { "inheritance": { "merge_strategy": "parent_wins" } },
  "restrictions": { "dangerous_calls": false }
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r = <<python
import os
os.system("echo should_be_blocked")
>>
    print(r)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 15s "$NAAB" run test.naab 2>&1); EXIT=$?
check "C37.21a: parent_wins keeps dangerous_calls enabled (exit 3)" "3" "$EXIT"
check_not_contains "C37.21b: blocked output absent" "should_be_blocked" "$OUTPUT"

# ---------------------------------------------------------------
# C37.22: Blocked commands accumulate via append merge
# ---------------------------------------------------------------
echo ""
echo "  C37.22: Blocked commands accumulate via append merge"
D="$WORKDIR/c37_22"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": {
    "shell": { "enabled": true, "blocked_commands": ["wget"] }
  }
}
EOF
sign_gov "$D/base"

# Child uses merge_arrays=append to accumulate blocked_commands from base
cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "meta": { "inheritance": { "merge_arrays": "append" } },
  "capabilities": {
    "shell": { "enabled": true, "blocked_commands": ["curl"] }
  }
}
EOF
sign_gov "$D/child"

# Test wget (inherited from parent via append)
cat > "$D/child/test_wget.naab" << 'EOF'
main {
    let r = <<sh
wget http://example.com 2>&1
echo "wget_ran"
>>
    print(r)
}
EOF

# Test curl (child's own block)
cat > "$D/child/test_curl.naab" << 'EOF'
main {
    let r = <<sh
curl http://example.com 2>&1
echo "curl_ran"
>>
    print(r)
}
EOF

OUTPUT_W=$(cd "$D/child" && timeout 15s "$NAAB" run test_wget.naab 2>&1); EXIT_W=$?
OUTPUT_C=$(cd "$D/child" && timeout 15s "$NAAB" run test_curl.naab 2>&1); EXIT_C=$?

check "C37.22a: wget blocked (inherited via append)" "1" "$([ $EXIT_W -ne 0 ] && echo 1 || echo 0)"
check "C37.22b: curl blocked (child's own)" "1" "$([ $EXIT_C -ne 0 ] && echo 1 || echo 0)"

# ---------------------------------------------------------------
# C37.23: parent_wins forces strict timeout
# ---------------------------------------------------------------
echo ""
echo "  C37.23: parent_wins forces strict timeout"
D="$WORKDIR/c37_23"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "limits": { "timeout": { "global": 3 } }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": true } },
  "meta": { "inheritance": { "merge_strategy": "parent_wins" } },
  "limits": { "timeout": { "global": 60 } }
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main {
    let r = <<sh
sleep 5
echo "survived_strict"
>>
    print(r)
}
EOF

OUTPUT=$(cd "$D/child" && timeout 15s "$NAAB" run test.naab 2>&1); EXIT=$?
check_not_contains "C37.23a: parent's 3s timeout enforced" "survived_strict" "$OUTPUT"
check "C37.23b: non-zero exit (timeout)" "1" "$([ $EXIT -ne 0 ] && echo 1 || echo 0)"

# ---------------------------------------------------------------
# C37.24: Mode floor across 3-level chain
# ---------------------------------------------------------------
echo ""
echo "  C37.24: Mode floor across 3-level chain"
D="$WORKDIR/c37_24"
mkdir -p "$D/gp" "$D/parent" "$D/child"

cat > "$D/gp/govern.json" << 'EOF'
{
  "mode": "enforce"
}
EOF
sign_gov "$D/gp"

cat > "$D/parent/govern.json" << 'EOF'
{
  "extends": "../gp/govern.json",
  "mode": "audit"
}
EOF
sign_gov "$D/parent"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../parent/govern.json",
  "mode": "off"
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main { print("mode_floor_ok") }
EOF

OUTPUT=$(cd "$D/child" && timeout 10s "$NAAB" run --governance-dashboard test.naab 2>&1); EXIT=$?
check_contains "C37.24a: mode ratcheted to enforce" "enforce" "$OUTPUT"
check "C37.24b: exit 0" "0" "$EXIT"

fi  # Cat 4

# ====================================================================
# Cat 5: Enterprise Config Edge Cases (C37.25-C37.28)
# ====================================================================
if should_run 5; then
echo ""
echo "--- Cat 5: Enterprise Config Edge Cases ---"

# ---------------------------------------------------------------
# C37.25: Telemetry inherited via extends
# ---------------------------------------------------------------
echo ""
echo "  C37.25: Telemetry inherited via extends"
D="$WORKDIR/c37_25"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "telemetry": { "enabled": true, "output_file": "telem.jsonl" }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main { print("telem_inherited") }
EOF

OUTPUT=$(cd "$D/child" && timeout 10s "$NAAB" run test.naab 2>&1); EXIT=$?
check "C37.25a: exit 0" "0" "$EXIT"
# Check if telemetry file was created in child workdir
if [ -f "$D/child/telem.jsonl" ]; then
    check "C37.25b: telemetry file created" "1" "1"
else
    check "C37.25b: telemetry file created" "1" "0"
fi

# ---------------------------------------------------------------
# C37.26: Edge-value telemetry config
# ---------------------------------------------------------------
echo ""
echo "  C37.26: Edge-value telemetry config (zeros clamped)"
D="$WORKDIR/c37_26"
mkdir -p "$D"

cat > "$D/govern.json" << 'EOF'
{
  "mode": "enforce",
  "telemetry": {
    "enabled": true,
    "output_file": "telem.jsonl",
    "forward_batch_size": 0,
    "forward_timeout_ms": 0,
    "forward_buffer_max": 0
  }
}
EOF
sign_gov "$D"

cat > "$D/test.naab" << 'EOF'
main { print("edge_telem_ok") }
EOF

OUTPUT=$(cd "$D" && timeout 10s "$NAAB" run test.naab 2>&1); EXIT=$?
check_contains "C37.26a: no crash with edge telemetry values" "edge_telem_ok" "$OUTPUT"
check "C37.26b: exit 0" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.27: All enterprise features enabled simultaneously
# ---------------------------------------------------------------
echo ""
echo "  C37.27: All enterprise features enabled simultaneously"
D="$WORKDIR/c37_27"
mkdir -p "$D"

cat > "$D/govern.json" << 'EOF'
{
  "mode": "enforce",
  "scoring": { "enabled": true, "yellow_threshold": 20, "red_threshold": 50 },
  "telemetry": { "enabled": true, "output_file": "telem.jsonl" },
  "restrictions": { "dangerous_calls": { "enabled": true, "level": "hard" } },
  "circuit_breaker": { "enabled": true, "max_level": 5 },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true },
  "contradiction_detection": { "enabled": true },
  "governance_health": { "enabled": true },
  "exposure_tracking": { "enabled": true },
  "taint_tracking": { "enabled": true },
  "code_quality": { "enabled": true }
}
EOF
sign_gov "$D"

cat > "$D/test.naab" << 'EOF'
main { print("all_features_ok") }
EOF

OUTPUT=$(cd "$D" && timeout 10s "$NAAB" run --governance-dashboard test.naab 2>&1); EXIT=$?
check_contains "C37.27a: all features load without crash" "all_features_ok" "$OUTPUT"
check "C37.27b: exit 0" "0" "$EXIT"

# ---------------------------------------------------------------
# C37.28: Full inheritance stack
# ---------------------------------------------------------------
echo ""
echo "  C37.28: Full inheritance stack"
D="$WORKDIR/c37_28"
mkdir -p "$D/base" "$D/child"

cat > "$D/base/govern.json" << 'EOF'
{
  "mode": "enforce",
  "scoring": { "enabled": true, "yellow_threshold": 20, "red_threshold": 50 },
  "telemetry": { "enabled": true, "output_file": "telem.jsonl" },
  "restrictions": { "dangerous_calls": { "enabled": true, "level": "hard" } },
  "limits": { "timeout": { "global": 30 } }
}
EOF
sign_gov "$D/base"

cat > "$D/child/govern.json" << 'EOF'
{
  "extends": "../base/govern.json",
  "mode": "enforce"
}
EOF
sign_gov "$D/child"

cat > "$D/child/test.naab" << 'EOF'
main { print("full_stack_ok") }
EOF

OUTPUT=$(cd "$D/child" && timeout 10s "$NAAB" run --governance-dashboard test.naab 2>&1); EXIT=$?
check_contains "C37.28a: governance active in full stack" "Governance\|governance" "$OUTPUT"
# Check telemetry file
if [ -f "$D/child/telem.jsonl" ]; then
    check "C37.28b: telemetry output created via inheritance" "1" "1"
else
    check "C37.28b: telemetry output created via inheritance" "1" "0"
fi

fi  # Cat 5

# ====================================================================
# Summary
# ====================================================================
echo ""
echo "================================================================"
echo "  Results: $PASS passed, $FAIL failed out of $TOTAL"
echo "================================================================"
if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "Failures:"
    echo -e "$FAILURES"
    echo ""
fi
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
