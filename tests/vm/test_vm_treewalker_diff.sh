#!/usr/bin/env bash
# test_vm_treewalker_diff.sh — VM / Tree-Walker Governance Differential
#
# Verifies governance decisions are identical regardless of execution engine.
# Each test runs the same (config, code) pair through both VM and --tree-walk,
# asserting identical exit codes.
#
# 18 differential tests covering capability gates, taint, custom rules,
# GovernanceHardError, env vars, security checks, and clean baselines.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TMPBASE="$_SYSTMP/test_diff_$$"
mkdir -p "$TMPBASE"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
"$NAAB" --keygen "$TMPBASE/test-key.pem" 2>/dev/null
"$NAAB" --trust-key "$TMPBASE/test-key.pem.pub" 2>/dev/null
SIGNING_KEY="$TMPBASE/test-key.pem"

PASS=0; FAIL=0; KNOWN=0

cleanup() { teardown_isolated_trust; rm -rf "$TMPBASE"; }
trap cleanup EXIT

# Known divergences — real bugs documented by this test.
# Update this list as bugs are fixed.
# D02/D18: TW taint throws runtime_error (exit 1) not GovernanceHardError (exit 3)
# D16: TW bad config exits 1 not 4
# D17: TW DETECT taint doesn't throw at all (exits 0)
KNOWN_DIVERGENCES="D02 D16 D17 D18"

is_known() {
    local id="$1"
    echo "$KNOWN_DIVERGENCES" | grep -qw "$id"
}

# run_diff ID CONFIG CODE [ENV_SETUP]
# Runs code under both VM and tree-walker with the same govern.json.
# Asserts identical exit codes.
run_diff() {
    local id="$1" config="$2" code="$3" env_setup="${4:-}"
    local dir="$TMPBASE/$id"
    mkdir -p "$dir"
    echo "$config" > "$dir/govern.json"
    (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    echo "$code" > "$dir/test.naab"
    if [ -n "$env_setup" ]; then
        eval "$env_setup"
    fi

    local VM_RC TW_RC
    VM_OUT=$(cd "$dir" && "$NAAB" test.naab 2>&1) && VM_RC=$? || VM_RC=$?
    TW_OUT=$(cd "$dir" && "$NAAB" --tree-walk test.naab 2>&1) && TW_RC=$? || TW_RC=$?

    if [ "$VM_RC" = "$TW_RC" ]; then
        echo "  PASS [$id] VM=$VM_RC TW=$TW_RC"
        PASS=$((PASS + 1))
    elif is_known "$id"; then
        echo "  KNOWN [$id] divergence: VM=$VM_RC TW=$TW_RC (documented bug)"
        KNOWN=$((KNOWN + 1))
    else
        echo "  FAIL [$id] exit code mismatch: VM=$VM_RC TW=$TW_RC"
        FAIL=$((FAIL + 1))
        echo "    VM output: ${VM_OUT:0:200}"
        echo "    TW output: ${TW_OUT:0:200}"
    fi
}

echo "=== VM / Tree-Walker Governance Differential ==="
echo "  Binary: $NAAB"
echo ""

# D-01: Capability block (shell disabled)
echo "--- Capability & Language Gates ---"
run_diff "D01" '{
    "mode": "enforce",
    "capabilities": { "shell": { "enabled": false } },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
echo "blocked"
>>
    print(r)
}'

# D-02: Taint source → sink (unsanitized)
run_diff "D02" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/D02/\"] },
        \"env_vars\": { \"read\": true }
    },
    \"taint_tracking\": {
        \"enabled\": true, \"level\": \"hard\",
        \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"],
        \"sanitizers\": [\"sanitize_\"]
    },
    \"security\": { \"sandbox_level\": \"elevated\" }
}" "use env
use file
main {
    let s = env.get(\"HOME\")
    file.write(\"$TMPBASE/D02/out.txt\", string(s))
}"

# D-03: Taint with sanitizer (should allow)
run_diff "D03" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/D03/\"] },
        \"env_vars\": { \"read\": true }
    },
    \"taint_tracking\": {
        \"enabled\": true, \"level\": \"hard\",
        \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"],
        \"sanitizers\": [\"sanitize_\"]
    },
    \"security\": { \"sandbox_level\": \"elevated\" }
}" "use env
use file
fn sanitize_val(x) {
    if x == null { return \"\" }
    return string(x)
}
main {
    let s = env.get(\"HOME\")
    let clean = sanitize_val(s)
    file.write(\"$TMPBASE/D03/out.txt\", clean)
    print(\"CLEAN\")
}"

# D-04: Custom rule HARD
echo ""
echo "--- Custom Rules ---"
run_diff "D04" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "DIFF-04", "name": "hard_rule",
        "pattern": "DIFF_HARD_MARKER", "level": "hard",
        "enabled": true, "message": "Hard rule"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// DIFF_HARD_MARKER
1
>>
    print(r)
}'

# D-05: Custom rule ADVISORY
run_diff "D05" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "DIFF-05", "name": "adv_rule",
        "pattern": "DIFF_ADV_MARKER", "level": "advisory",
        "enabled": true, "message": "Advisory rule"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// DIFF_ADV_MARKER
1
>>
    print(r)
}'

# D-06: GovernanceHardError uncatchable
echo ""
echo "--- GovernanceHardError ---"
run_diff "D06" '{
    "mode": "enforce",
    "capabilities": {
        "env_vars": { "blocked_read": ["DIFF_BLOCKED_VAR"] }
    },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    try {
        let v = env.get("DIFF_BLOCKED_VAR")
        print("BYPASS")
    } catch (e) {
        print("BYPASS: caught")
    }
}' 'export DIFF_BLOCKED_VAR=secret'
unset DIFF_BLOCKED_VAR 2>/dev/null || true

# D-07: DETECT level caught
run_diff "D07" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/D07/\"] },
        \"env_vars\": { \"read\": true }
    },
    \"taint_tracking\": {
        \"enabled\": true, \"level\": \"detect\",
        \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"],
        \"sanitizers\": [\"sanitize_\"]
    },
    \"security\": { \"sandbox_level\": \"elevated\" }
}" "use env
use file
main {
    try {
        let s = env.get(\"HOME\")
        file.write(\"$TMPBASE/D07/out.txt\", string(s))
    } catch (e) {
        print(\"DETECT_CAUGHT\")
    }
}"

# D-08: Blocked language
echo ""
echo "--- Security Checks ---"
run_diff "D08" '{
    "mode": "enforce",
    "languages": { "blocked": ["python"] },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<python
print("blocked")
>>
    print(r)
}'

# D-09: Blocked command
run_diff "D09" '{
    "mode": "enforce",
    "languages": { "allowed": ["shell"] },
    "capabilities": {
        "shell": { "enabled": true, "blocked_commands": ["curl", "wget"] }
    },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
curl http://example.com
>>
    print(r)
}'

# D-10: env.get blocked var
echo ""
echo "--- Env Var Enforcement ---"
run_diff "D10" '{
    "mode": "enforce",
    "capabilities": {
        "env_vars": { "blocked_read": ["DIFF_SECRET"] }
    },
    "security": { "sandbox_level": "elevated" }
}' 'use env
main {
    let v = env.get("DIFF_SECRET")
    print(v)
}' 'export DIFF_SECRET=hidden'
unset DIFF_SECRET 2>/dev/null || true

# D-11: env.set blocked var
run_diff "D11" '{
    "mode": "enforce",
    "capabilities": {
        "env_vars": { "blocked_write": ["DIFF_WRITE_BLOCKED"] }
    },
    "security": { "sandbox_level": "elevated" }
}' 'use env
main {
    env.set("DIFF_WRITE_BLOCKED", "test")
    print("should not reach")
}'

# D-12: Clean program (no violations)
echo ""
echo "--- Baselines ---"
run_diff "D12" '{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" }
}' 'main { print("clean") }'

# D-13: Runtime error (not governance)
run_diff "D13" '{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" }
}' 'main { throw "runtime error" }'

# D-14: Multiple capability checks pass
run_diff "D14" '{
    "mode": "enforce",
    "languages": { "allowed": ["shell"] },
    "capabilities": {
        "shell": { "enabled": true },
        "env_vars": { "read": true }
    },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
echo "ok"
>>
    print(r)
}'

# D-15: Advisory with quality gate
run_diff "D15" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "DIFF-15", "name": "qgate",
        "pattern": "QGATE_DIFF", "level": "advisory",
        "enabled": true, "message": "QGate test"
    }],
    "quality_gate": {
        "enabled": true,
        "conditions": [{
            "metric": "advisory_violations",
            "operator": ">", "threshold": 0
        }]
    },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// QGATE_DIFF
1
>>
    print(r)
}'

# D-16: Bad config → exit 4 (VM) vs exit 1 (TW)
# KNOWN DIVERGENCE: tree-walker doesn't map config parse errors to exit 4
echo ""
echo "--- Error Conditions ---"
E16DIR="$TMPBASE/D16"
mkdir -p "$E16DIR"
echo 'INVALID_JSON{{{' > "$E16DIR/govern.json"
echo 'main { print("ok") }' > "$E16DIR/test.naab"
VM_OUT=$(cd "$E16DIR" && "$NAAB" test.naab 2>&1) && VM_RC=$? || VM_RC=$?
TW_OUT=$(cd "$E16DIR" && "$NAAB" --tree-walk test.naab 2>&1) && TW_RC=$? || TW_RC=$?
if [ "$VM_RC" = "$TW_RC" ]; then
    echo "  PASS [D16] VM=$VM_RC TW=$TW_RC"
    PASS=$((PASS + 1))
elif is_known "D16"; then
    echo "  KNOWN [D16] divergence: VM=$VM_RC TW=$TW_RC (config error exit code)"
    KNOWN=$((KNOWN + 1))
else
    echo "  FAIL [D16] exit code mismatch: VM=$VM_RC TW=$TW_RC"
    FAIL=$((FAIL + 1))
fi

# D-17: DETECT uncaught
run_diff "D17" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/D17/\"] },
        \"env_vars\": { \"read\": true }
    },
    \"taint_tracking\": {
        \"enabled\": true, \"level\": \"detect\",
        \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"],
        \"sanitizers\": [\"sanitize_\"]
    },
    \"security\": { \"sandbox_level\": \"elevated\" }
}" "use env
use file
main {
    let s = env.get(\"HOME\")
    file.write(\"$TMPBASE/D17/out.txt\", string(s))
}"

# D-18: Taint through variable chain
run_diff "D18" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/D18/\"] },
        \"env_vars\": { \"read\": true }
    },
    \"taint_tracking\": {
        \"enabled\": true, \"level\": \"hard\",
        \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"],
        \"sanitizers\": [\"sanitize_\"]
    },
    \"security\": { \"sandbox_level\": \"elevated\" }
}" "use env
use file
main {
    let a = env.get(\"HOME\")
    let b = a
    let c = b
    file.write(\"$TMPBASE/D18/out.txt\", string(c))
}"

echo ""
TOTAL=$((PASS + FAIL + KNOWN))
echo "=== Differential Results: $PASS passed, $FAIL failed, $KNOWN known divergences (of $TOTAL tests) ==="

if [ $KNOWN -gt 0 ]; then
    echo ""
    echo "  Known divergences (tree-walker bugs to fix):"
    echo "    D02/D18: taint GovernanceHardError vs runtime_error in tree-walker"
    echo "    D16: bad config exit code (VM=4 TW=1)"
    echo "    D17: DETECT taint not thrown in tree-walker (VM=1 TW=0)"
fi

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "FAILED: New VM/tree-walker governance divergence detected."
    exit 1
fi

echo "  All non-known governance decisions agree between VM and tree-walker."
exit 0
