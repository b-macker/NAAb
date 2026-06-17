#!/usr/bin/env bash
# test_decision_oracle.sh — Governance Decision Oracle
#
# Tests that the governance engine makes correct BLOCK/ALLOW/ADVISORY decisions
# for known (config, code, expected-outcome) triples. This is the logic correctness
# layer: it verifies the engine's actual decisions, not just code structure.
#
# 65+ oracle triples across 8 categories:
#   CAP:   Capability gates (10)
#   ENF:   Enforcement levels (8)
#   TAINT: Taint tracking (12)
#   SEC:   Security checks (10)
#   RULE:  Custom rules (5)
#   RATCH: Ratchet enforcement (5)
#   HARD:  GovernanceHardError uncatchability (5)
#   EXIT:  Exit code correctness (10)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../../build/naab-lang}"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TMPBASE="$_SYSTMP/test_oracle_$$"
mkdir -p "$TMPBASE"

source "$SCRIPT_DIR/../../helpers/trust_setup.sh"
setup_isolated_trust
"$NAAB" --keygen "$TMPBASE/test-key.pem" 2>/dev/null
"$NAAB" --trust-key "$TMPBASE/test-key.pem.pub" 2>/dev/null
SIGNING_KEY="$TMPBASE/test-key.pem"

PASS=0; FAIL=0; SKIP=0

sign_dir() {
    local dir="$1"
    (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
}

cleanup() { teardown_isolated_trust; rm -rf "$TMPBASE"; }
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

check_contains() {
    local id="$1" desc="$2" output="$3" pattern="$4"
    if echo "$output" | grep -qi "$pattern"; then
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [$id] $desc (pattern '$pattern' not found)"
        FAIL=$((FAIL + 1))
    fi
}

check_not_contains() {
    local id="$1" desc="$2" output="$3" pattern="$4"
    if echo "$output" | grep -qi "$pattern"; then
        echo "  FAIL [$id] $desc (found '$pattern' unexpectedly)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    fi
}

# run_oracle DIR_NAME GOVERN_JSON NAAB_CODE [ENV_EXPORTS]
# Sets up a test directory with govern.json + test.naab, signs, runs.
# Captures exit code in ORACLE_RC, combined output in ORACLE_OUT.
run_oracle() {
    local name="$1" config="$2" code="$3" env_setup="${4:-}"
    local dir="$TMPBASE/$name"
    mkdir -p "$dir"
    echo "$config" > "$dir/govern.json"
    (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    echo "$code" > "$dir/test.naab"
    if [ -n "$env_setup" ]; then
        eval "$env_setup"
    fi
    ORACLE_OUT=$(cd "$dir" && "$NAAB" test.naab 2>&1) && ORACLE_RC=$? || ORACLE_RC=$?
}

echo "=== Governance Decision Oracle ==="
echo "  Binary: $NAAB"
echo ""

# =====================================================================
# CATEGORY 1: Capability Gates (10 tests)
# =====================================================================
echo "--- Category 1: Capability Gates ---"

# O-CAP-01: shell disabled → HARD block
run_oracle "cap01" '{
    "mode": "enforce",
    "capabilities": { "shell": { "enabled": false } },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
echo "should not run"
>>
    print(r)
}'
check "O-CAP-01" "shell disabled → exit 3" "3" "$ORACLE_RC"

# O-CAP-02: shell enabled → success
run_oracle "cap02" '{
    "mode": "enforce",
    "languages": { "allowed": ["shell"] },
    "capabilities": { "shell": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
echo "hello"
>>
    print(r)
}'
check "O-CAP-02" "shell enabled → exit 0" "0" "$ORACLE_RC"

# O-CAP-03: network disabled → HARD block
run_oracle "cap03" '{
    "mode": "enforce",
    "capabilities": { "network": { "enabled": false } },
    "security": { "sandbox_level": "elevated" }
}' 'use http
main {
    let r = http.get("http://localhost:99999")
    print(r)
}'
check "O-CAP-03" "network disabled → exit 3" "3" "$ORACLE_RC"

# O-CAP-04: filesystem disabled → HARD block
run_oracle "cap04" '{
    "mode": "enforce",
    "capabilities": { "filesystem": { "mode": "none" } },
    "security": { "sandbox_level": "elevated" }
}' "use file
main {
    let r = file.read(\"$TMPBASE/cap04/govern.json\")
    print(r)
}"
check "O-CAP-04" "filesystem none → exit 3" "3" "$ORACLE_RC"

# O-CAP-05: polyglot language blocked → HARD block
run_oracle "cap05" '{
    "mode": "enforce",
    "languages": { "blocked": ["python"] },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<python
print("blocked")
>>
    print(r)
}'
check "O-CAP-05" "blocked language → exit 3" "3" "$ORACLE_RC"

# O-CAP-06: env_vars blocked_read → HARD block
run_oracle "cap06" '{
    "mode": "enforce",
    "capabilities": {
        "env_vars": { "blocked_read": ["ORACLE_SECRET_VAR"] }
    },
    "security": { "sandbox_level": "elevated" }
}' 'use env
main {
    let v = env.get("ORACLE_SECRET_VAR")
    print(v)
}' 'export ORACLE_SECRET_VAR=hidden'
check "O-CAP-06" "env blocked_read → exit 3" "3" "$ORACLE_RC"
unset ORACLE_SECRET_VAR 2>/dev/null || true

# O-CAP-07: env_vars allowed_read (reading unlisted var) → SOFT (advisory, exit 0)
run_oracle "cap07" '{
    "mode": "enforce",
    "capabilities": {
        "env_vars": { "allowed_read": ["HOME"] }
    },
    "security": { "sandbox_level": "elevated" }
}' 'use env
main {
    let v = env.get("PATH")
    print("got path")
}'
check "O-CAP-07" "env allowed_read unlisted var → exit 3" "3" "$ORACLE_RC"

# O-CAP-08: env_vars blocked_write → HARD block
# Note: PATH has a hardcoded security check (exit 1), use a custom var name
run_oracle "cap08" '{
    "mode": "enforce",
    "capabilities": {
        "env_vars": { "blocked_write": ["ORACLE_BLOCKED_WRITE_VAR"] }
    },
    "security": { "sandbox_level": "elevated" }
}' 'use env
main {
    env.set("ORACLE_BLOCKED_WRITE_VAR", "test")
    print("should not reach")
}'
check "O-CAP-08" "env blocked_write → exit 3" "3" "$ORACLE_RC"

# O-CAP-09: codegen disabled → block
run_oracle "cap09" '{
    "mode": "enforce",
    "codegen": { "enabled": false },
    "security": { "sandbox_level": "elevated" }
}' 'use codegen
main {
    let r = codegen.run("python", "print(1)")
    print(r)
}'
check "O-CAP-09" "codegen disabled → exit 1" "1" "$ORACLE_RC"

# O-CAP-10: all capabilities enabled, clean code → exit 0
run_oracle "cap10" '{
    "mode": "enforce",
    "languages": { "allowed": ["shell"] },
    "capabilities": {
        "shell": { "enabled": true },
        "filesystem": { "mode": "read_write" },
        "env_vars": { "read": true }
    },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
echo "ok"
>>
    print(r)
}'
check "O-CAP-10" "all enabled, clean code → exit 0" "0" "$ORACLE_RC"

echo ""

# =====================================================================
# CATEGORY 2: Enforcement Levels (8 tests)
# =====================================================================
echo "--- Category 2: Enforcement Levels ---"

# O-ENF-01: taint_tracking level hard → exit 3
run_oracle "enf01" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/enf01/\"] },
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
    file.write(\"$TMPBASE/enf01/out.txt\", string(s))
}"
check "O-ENF-01" "taint hard → exit 3" "3" "$ORACLE_RC"

# O-ENF-02: taint_tracking level soft (no override) → exit 3 (SOFT is a block level)
run_oracle "enf02" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/enf02/\"] },
        \"env_vars\": { \"read\": true }
    },
    \"taint_tracking\": {
        \"enabled\": true, \"level\": \"soft\",
        \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"],
        \"sanitizers\": [\"sanitize_\"]
    },
    \"security\": { \"sandbox_level\": \"elevated\" }
}" "use env
use file
main {
    let s = env.get(\"HOME\")
    file.write(\"$TMPBASE/enf02/out.txt\", string(s))
    print(\"done\")
}"
check "O-ENF-02" "taint soft (no override) → exit 3" "3" "$ORACLE_RC"

# O-ENF-03: taint_tracking level advisory → exit 0 + advisory text
run_oracle "enf03" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/enf03/\"] },
        \"env_vars\": { \"read\": true }
    },
    \"taint_tracking\": {
        \"enabled\": true, \"level\": \"advisory\",
        \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"],
        \"sanitizers\": [\"sanitize_\"]
    },
    \"security\": { \"sandbox_level\": \"elevated\" }
}" "use env
use file
main {
    let s = env.get(\"HOME\")
    file.write(\"$TMPBASE/enf03/out.txt\", string(s))
    print(\"done\")
}"
check "O-ENF-03" "taint advisory → exit 0" "0" "$ORACLE_RC"

# O-ENF-04: taint_tracking disabled → exit 0, no taint checks
run_oracle "enf04" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/enf04/\"] },
        \"env_vars\": { \"read\": true }
    },
    \"taint_tracking\": {
        \"enabled\": false,
        \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"],
        \"sanitizers\": [\"sanitize_\"]
    },
    \"security\": { \"sandbox_level\": \"elevated\" }
}" "use env
use file
main {
    let s = env.get(\"HOME\")
    file.write(\"$TMPBASE/enf04/out.txt\", string(s))
    print(\"done\")
}"
check "O-ENF-04" "taint disabled → exit 0" "0" "$ORACLE_RC"

# O-ENF-05: taint_tracking level detect (no try/catch) → exit 1 (catchable throw)
run_oracle "enf05" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/enf05/\"] },
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
    file.write(\"$TMPBASE/enf05/out.txt\", string(s))
    print(\"should not reach\")
}"
check "O-ENF-05" "taint detect (no catch) → exit 1" "1" "$ORACLE_RC"

# O-ENF-06: taint_tracking level detect (with try/catch) → exit 0 (caught)
run_oracle "enf06" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/enf06/\"] },
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
        file.write(\"$TMPBASE/enf06/out.txt\", string(s))
    } catch (e) {
        print(\"DETECT_CAUGHT\")
    }
}"
check "O-ENF-06" "taint detect (with catch) → exit 0" "0" "$ORACLE_RC"
check_contains "O-ENF-06b" "taint detect caught by try/catch" "$ORACLE_OUT" "DETECT_CAUGHT"

# O-ENF-07: custom rule level hard → exit 3
run_oracle "enf07" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "ORACLE-ENF07", "name": "hard_rule",
        "pattern": "HARD_TRIGGER", "level": "hard",
        "enabled": true, "message": "Hard rule triggered"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// HARD_TRIGGER
1 + 1
>>
    print(r)
}'
check "O-ENF-07" "custom rule hard → exit 3" "3" "$ORACLE_RC"

# O-ENF-08: custom rule level advisory → exit 0 + advisory message
run_oracle "enf08" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "ORACLE-ENF08", "name": "advisory_rule",
        "pattern": "ADVISORY_TRIGGER", "level": "advisory",
        "enabled": true, "message": "Advisory rule triggered"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// ADVISORY_TRIGGER
1 + 1
>>
    print(r)
}'
check "O-ENF-08" "custom rule advisory → exit 0" "0" "$ORACLE_RC"

echo ""

# =====================================================================
# CATEGORY 3: Taint Tracking (12 tests)
# =====================================================================
echo "--- Category 3: Taint Tracking ---"

# Helper: taint config template with hard enforcement
taint_config() {
    local paths="$1"
    cat <<EOF
{
    "mode": "enforce",
    "capabilities": {
        "filesystem": { "mode": "read_write", "allowed_paths": ["$paths"] },
        "shell": { "enabled": true },
        "env_vars": { "read": true }
    },
    "taint_tracking": {
        "enabled": true, "level": "hard",
        "sources": ["env.get", "io.read_line", "file.read", "polyglot_output", "http.get"],
        "sinks": ["shell", "file.write"],
        "sanitizers": ["sanitize_", "validate.command"]
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
}

# O-TAINT-01: env.get → file.write (unsanitized) → BLOCK
run_oracle "taint01" "$(taint_config "$TMPBASE/taint01/")" "use env
use file
main {
    let cmd = env.get(\"HOME\")
    file.write(\"$TMPBASE/taint01/out.txt\", string(cmd))
    print(\"should not reach\")
}"
check "O-TAINT-01" "env.get → file.write (unsanitized) → blocked" "3" "$ORACLE_RC"

# O-TAINT-02: env.get → sanitize → file.write → ALLOW (sanitized taint cleared)
run_oracle "taint02" "$(taint_config "$TMPBASE/taint02/")" "use env
use file
fn sanitize_cmd(input) {
    if input == null { return \"\" }
    return string(input)
}
main {
    let cmd = env.get(\"HOME\")
    let clean = sanitize_cmd(cmd)
    file.write(\"$TMPBASE/taint02/out.txt\", clean)
    print(\"sanitized write OK\")
}"
check "O-TAINT-02" "env.get → sanitize → file.write → allowed" "0" "$ORACLE_RC"

# O-TAINT-03: io.read_line → file.write (unsanitized) → BLOCK
# We can't actually read stdin in test, so use env.get as taint source
run_oracle "taint03" "$(taint_config "$TMPBASE/taint03/")" "use env
use file
main {
    let data = env.get(\"USER\")
    file.write(\"$TMPBASE/taint03/out.txt\", string(data))
}"
check "O-TAINT-03" "tainted data → file.write → blocked" "3" "$ORACLE_RC"

# O-TAINT-04: env.get → sanitize → file.write → ALLOW
run_oracle "taint04" "$(taint_config "$TMPBASE/taint04/")" "use env
use file
fn sanitize_data(input) {
    if input == null { return \"\" }
    return string(input)
}
main {
    let data = env.get(\"USER\")
    let clean = sanitize_data(data)
    file.write(\"$TMPBASE/taint04/out.txt\", clean)
    print(\"TAINT_CLEARED\")
}"
check "O-TAINT-04" "tainted → sanitize → file.write → allowed" "0" "$ORACLE_RC"

# O-TAINT-05: file.read → file.write (unsanitized) → BLOCK
run_oracle "taint05" "$(taint_config "$TMPBASE/taint05/")" "use file
main {
    let data = file.read(\"$TMPBASE/taint05/govern.json\")
    file.write(\"$TMPBASE/taint05/out.txt\", string(data))
}"
check "O-TAINT-05" "file.read → file.write (unsanitized) → blocked" "3" "$ORACLE_RC"

# O-TAINT-06: polyglot output → file.write (unsanitized) → BLOCK
run_oracle "taint06" "$(taint_config "$TMPBASE/taint06/")" "use file
main {
    let data = <<shell
echo \"tainted_output\"
>>
    file.write(\"$TMPBASE/taint06/out.txt\", string(data))
}"
check "O-TAINT-06" "polyglot output → file.write → blocked" "3" "$ORACLE_RC"

# O-TAINT-07: env.get → variable → file.write (indirect taint) → BLOCK
run_oracle "taint07" "$(taint_config "$TMPBASE/taint07/")" "use env
use file
main {
    let a = env.get(\"HOME\")
    let b = a
    let c = b
    file.write(\"$TMPBASE/taint07/out.txt\", string(c))
}"
check "O-TAINT-07" "taint through variable chain → blocked" "3" "$ORACLE_RC"

# O-TAINT-08: taint through function parameter → BLOCK
run_oracle "taint08" "$(taint_config "$TMPBASE/taint08/")" "use env
use file
fn write_it(data) {
    file.write(\"$TMPBASE/taint08/out.txt\", string(data))
}
main {
    let s = env.get(\"HOME\")
    write_it(s)
}"
check "O-TAINT-08" "taint through function param → blocked" "3" "$ORACLE_RC"

# O-TAINT-09: taint through array element → BLOCK
run_oracle "taint09" "$(taint_config "$TMPBASE/taint09/")" "use env
use file
main {
    let arr = [env.get(\"HOME\")]
    file.write(\"$TMPBASE/taint09/out.txt\", string(arr[0]))
}"
check "O-TAINT-09" "taint through array element → blocked" "3" "$ORACLE_RC"

# O-TAINT-10: taint through string concat → BLOCK
run_oracle "taint10" "$(taint_config "$TMPBASE/taint10/")" "use env
use file
main {
    let s = env.get(\"HOME\")
    let combined = \"prefix_\" + string(s) + \"_suffix\"
    file.write(\"$TMPBASE/taint10/out.txt\", combined)
}"
check "O-TAINT-10" "taint through string concat → blocked" "3" "$ORACLE_RC"

# O-TAINT-11: taint through struct field → BLOCK
run_oracle "taint11" "$(taint_config "$TMPBASE/taint11/")" "use env
use file
main {
    let s = env.get(\"HOME\")
    let obj = {\"value\": s}
    file.write(\"$TMPBASE/taint11/out.txt\", string(obj.value))
}"
check "O-TAINT-11" "taint through struct field → blocked" "3" "$ORACLE_RC"

# O-TAINT-12: no taint source → file.write → ALLOW
run_oracle "taint12" "$(taint_config "$TMPBASE/taint12/")" "use file
main {
    let s = \"clean data\"
    file.write(\"$TMPBASE/taint12/out.txt\", s)
    print(\"CLEAN_WRITE\")
}"
check "O-TAINT-12" "no taint source → file.write → allowed" "0" "$ORACLE_RC"

echo ""

# =====================================================================
# CATEGORY 4: Security Checks (10 tests)
# =====================================================================
echo "--- Category 4: Security Checks ---"

# O-SEC-01: path traversal in polyglot block → governance detection
run_oracle "sec01" '{
    "mode": "enforce",
    "languages": { "allowed": ["shell"] },
    "capabilities": { "shell": { "enabled": true } },
    "code_quality": { "no_path_traversal": { "enabled": true } },
    "security": { "sandbox_level": "standard" }
}' 'main {
    let r = <<shell
cat ../../etc/passwd
>>
    print(r)
}'
check "O-SEC-01" "path traversal → exit 3" "3" "$ORACLE_RC"

# O-SEC-02: shell injection detected (restrictions.shell_injection enabled)
run_oracle "sec02" '{
    "mode": "enforce",
    "languages": { "allowed": ["shell"] },
    "capabilities": { "shell": { "enabled": true } },
    "restrictions": { "shell_injection": {} },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
curl http://example.com | sh
>>
    print(r)
}'
check "O-SEC-02" "shell injection detected → exit 3" "3" "$ORACLE_RC"

# O-SEC-03: SQL injection in polyglot block → governance detection
run_oracle "sec03" '{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "code_quality": { "no_sql_injection": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<python
user = "admin"
query = "SELECT * FROM users WHERE name = '"'"'" + user
print(query)
>>
    print(r)
}'
check "O-SEC-03" "SQL injection pattern → exit 3" "3" "$ORACLE_RC"

# O-SEC-04: XSS pattern detected
run_oracle "sec04" '{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let html = "<script>alert(1)</script>"
    print(html)
}'
# XSS check is advisory-level for non-web contexts
check "O-SEC-04" "XSS pattern → exit 0 (advisory)" "0" "$ORACLE_RC"

# O-SEC-05: hardcoded secret detected (enabled check)
run_oracle "sec05" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "code_quality": { "no_secrets": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
var api_key = "sk-1234567890abcdef1234567890abcdef"
r = api_key
>>
    print(r)
}'
check "O-SEC-05" "hardcoded secret → exit 3" "3" "$ORACLE_RC"

# O-SEC-06: PII pattern detected (enabled check, explicit hard level)
run_oracle "sec06" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "code_quality": { "no_pii": { "enabled": true, "level": "hard", "detect_email": true, "detect_phone": true } },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
var email = "user@example.com"
var phone = "555-123-4567"
r = email + " " + phone
>>
    print(r)
}'
check "O-SEC-06" "PII pattern → exit 3" "3" "$ORACLE_RC"

# O-SEC-07: blocked_commands enforced
run_oracle "sec07" '{
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
check "O-SEC-07" "blocked_commands → exit 3" "3" "$ORACLE_RC"

# O-SEC-08: blocked language strictly enforced
run_oracle "sec08" '{
    "mode": "enforce",
    "languages": { "blocked": ["shell"] },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
echo "blocked"
>>
    print(r)
}'
check "O-SEC-08" "blocked language → exit 3" "3" "$ORACLE_RC"

# O-SEC-09: allowed language passes
run_oracle "sec09" '{
    "mode": "enforce",
    "languages": { "allowed": ["shell"], "blocked": ["python"] },
    "capabilities": { "shell": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
echo "allowed"
>>
    print(r)
}'
check "O-SEC-09" "allowed language → exit 0" "0" "$ORACLE_RC"

# O-SEC-10: loop_iterations enforcement (correct config key)
run_oracle "sec10" '{
    "mode": "enforce",
    "limits": { "execution": { "loop_iterations": 5 } },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let i = 0
    while i < 1000 {
        i = i + 1
    }
    print("done: " + string(i))
}'
check "O-SEC-10" "loop_iterations exceeded → exit 3" "3" "$ORACLE_RC"

echo ""

# =====================================================================
# CATEGORY 5: Custom Rules (5 tests)
# =====================================================================
echo "--- Category 5: Custom Rules ---"

# O-RULE-01: custom rule hard → exit 3
run_oracle "rule01" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "RULE-01", "name": "forbidden_call",
        "pattern": "FORBIDDEN_CALL", "level": "hard",
        "enabled": true, "message": "Forbidden call found"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// FORBIDDEN_CALL
1
>>
    print(r)
}'
check "O-RULE-01" "custom rule hard → exit 3" "3" "$ORACLE_RC"

# O-RULE-02: custom rule advisory → exit 0
run_oracle "rule02" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "RULE-02", "name": "advisory_call",
        "pattern": "ADVISORY_MARKER", "level": "advisory",
        "enabled": true, "message": "Advisory marker found"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// ADVISORY_MARKER
1
>>
    print(r)
}'
check "O-RULE-02" "custom rule advisory → exit 0" "0" "$ORACLE_RC"

# O-RULE-03: custom rule disabled → exit 0, no warning
run_oracle "rule03" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "RULE-03", "name": "disabled_rule",
        "pattern": "DISABLED_MARKER", "level": "hard",
        "enabled": false, "message": "Should not fire"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// DISABLED_MARKER
1
>>
    print(r)
}'
check "O-RULE-03" "custom rule disabled → exit 0" "0" "$ORACLE_RC"

# O-RULE-04: regex pattern match
run_oracle "rule04" '{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "custom_rules": [{
        "id": "RULE-04", "name": "eval_ban",
        "pattern": "eval\\s*\\(", "level": "hard",
        "enabled": true, "message": "eval() is banned"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<python
x = eval("1+1")
print(x)
>>
    print(r)
}'
check "O-RULE-04" "regex custom rule → exit 3" "3" "$ORACLE_RC"

# O-RULE-05: multiple rules, first-match semantics — hard listed first wins
run_oracle "rule05" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [
        {
            "id": "RULE-05A", "name": "hard_one",
            "pattern": "MIXED_MARKER", "level": "hard",
            "enabled": true, "message": "Hard level"
        },
        {
            "id": "RULE-05B", "name": "advisory_one",
            "pattern": "MIXED_MARKER", "level": "advisory",
            "enabled": true, "message": "Advisory level"
        }
    ],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// MIXED_MARKER
1
>>
    print(r)
}'
check "O-RULE-05" "custom rules first-match: hard listed first → exit 3" "3" "$ORACLE_RC"

echo ""

# =====================================================================
# CATEGORY 6: Ratchet Enforcement (5 tests)
# =====================================================================
echo "--- Category 6: Ratchet Enforcement ---"

# O-RATCH-01: Tightening capability mid-run → new restriction enforced
# reloadIfChanged() fires before every polyglot block (polyglot.cpp:158)
# Ratchet checks capabilities.shell.enabled (governance_config.cpp:2921)
RDIR="$TMPBASE/ratch01"
mkdir -p "$RDIR/tight"
cat > "$RDIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["shell", "python"] },
    "capabilities": { "shell": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR"
cat > "$RDIR/tight/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["shell", "python"] },
    "capabilities": { "shell": { "enabled": false } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR/tight"
cat > "$RDIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$RDIR/tight/govern.json", "$RDIR/govern.json")
shutil.copy("$RDIR/tight/govern.json.sig", "$RDIR/govern.json.sig")
print("config tightened")
>>
    print(r1)
    let r2 = <<python
print("reload triggered")
>>
    print(r2)
    let r3 = <<shell
echo "should be blocked"
>>
    print(r3)
}
NAABEOF
ORACLE_OUT=$(cd "$RDIR" && timeout 30s "$NAAB" test.naab 2>&1) && ORACLE_RC=$? || ORACLE_RC=$?
check "O-RATCH-01" "tighten shell capability mid-run → blocked (exit 3)" "3" "$ORACLE_RC"

# O-RATCH-02: Loosening capability rejected → ratchet violation in stderr
# capabilities.shell.enabled: false→true is a ratchet violation
RDIR="$TMPBASE/ratch02"
mkdir -p "$RDIR/loose"
cat > "$RDIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "capabilities": { "shell": { "enabled": false } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR"
cat > "$RDIR/loose/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "capabilities": { "shell": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR/loose"
cat > "$RDIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$RDIR/loose/govern.json", "$RDIR/govern.json")
shutil.copy("$RDIR/loose/govern.json.sig", "$RDIR/govern.json.sig")
print("config loosened")
>>
    print(r1)
    let r2 = <<python
print("still strict")
>>
    print(r2)
}
NAABEOF
ORACLE_OUT=$(cd "$RDIR" && timeout 30s "$NAAB" test.naab 2>&1) && ORACLE_RC=$? || ORACLE_RC=$?
check_contains "O-RATCH-02" "ratchet rejected capability loosening" "$ORACLE_OUT" "ratchet"

# O-RATCH-03: Numeric limit tightening → new limit enforced
RDIR="$TMPBASE/ratch03"
mkdir -p "$RDIR/tight"
cat > "$RDIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "limits": { "execution": { "loop_iterations": 100 } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR"
cat > "$RDIR/tight/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "limits": { "execution": { "loop_iterations": 5 } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR/tight"
cat > "$RDIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$RDIR/tight/govern.json", "$RDIR/govern.json")
shutil.copy("$RDIR/tight/govern.json.sig", "$RDIR/govern.json.sig")
print("limits tightened")
>>
    print(r1)
    let r2 = <<python
print("reload triggered")
>>
    print(r2)
    let i = 0
    while i < 50 {
        i = i + 1
    }
    print("loop done: " + string(i))
}
NAABEOF
ORACLE_OUT=$(cd "$RDIR" && timeout 30s "$NAAB" test.naab 2>&1) && ORACLE_RC=$? || ORACLE_RC=$?
check "O-RATCH-03" "numeric limit tightened → loop blocked (exit 3)" "3" "$ORACLE_RC"

# O-RATCH-04: Independent ratchet fields — tighten shell, network unrelated
RDIR="$TMPBASE/ratch04"
mkdir -p "$RDIR/tight"
cat > "$RDIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "capabilities": { "shell": { "enabled": true }, "network": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR"
cat > "$RDIR/tight/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "capabilities": { "shell": { "enabled": false }, "network": { "enabled": true } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR/tight"
cat > "$RDIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$RDIR/tight/govern.json", "$RDIR/govern.json")
shutil.copy("$RDIR/tight/govern.json.sig", "$RDIR/govern.json.sig")
print("shell tightened")
>>
    print(r1)
    let r2 = <<python
print("python still works after tightening")
>>
    print(r2)
}
NAABEOF
ORACLE_OUT=$(cd "$RDIR" && timeout 30s "$NAAB" test.naab 2>&1) && ORACLE_RC=$? || ORACLE_RC=$?
check "O-RATCH-04" "tighten shell, python unaffected" "0" "$ORACLE_RC"
check_contains "O-RATCH-04b" "python ran after capability tightening" "$ORACLE_OUT" "python still works after tightening"

# O-RATCH-05: Loosening numeric limit rejected
# loop_iterations: 5 → 0 (unlimited) is a ratchet violation
RDIR="$TMPBASE/ratch05"
mkdir -p "$RDIR/loose"
cat > "$RDIR/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "limits": { "execution": { "loop_iterations": 5 } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR"
cat > "$RDIR/loose/govern.json" << 'EOF'
{
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "limits": { "execution": { "loop_iterations": 0 } },
    "security": { "sandbox_level": "elevated" }
}
EOF
sign_dir "$RDIR/loose"
cat > "$RDIR/test.naab" << NAABEOF
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$RDIR/loose/govern.json", "$RDIR/govern.json")
shutil.copy("$RDIR/loose/govern.json.sig", "$RDIR/govern.json.sig")
print("tried to loosen limits")
>>
    print(r1)
    let r2 = <<python
print("still strict")
>>
    print(r2)
}
NAABEOF
ORACLE_OUT=$(cd "$RDIR" && timeout 30s "$NAAB" test.naab 2>&1) && ORACLE_RC=$? || ORACLE_RC=$?
check_contains "O-RATCH-05" "numeric limit loosening rejected" "$ORACLE_OUT" "ratchet"

echo ""

# =====================================================================
# CATEGORY 7: GovernanceHardError Uncatchability (5 tests)
# =====================================================================
echo "--- Category 7: GovernanceHardError Uncatchability ---"

hard_error_config() {
    cat <<'EOF'
{
    "mode": "enforce",
    "capabilities": {
        "env_vars": { "blocked_read": ["ORACLE_BLOCKED_VAR"] }
    },
    "security": { "sandbox_level": "elevated" }
}
EOF
}

# O-HARD-01: HARD block in try/catch → exit 3
run_oracle "hard01" "$(hard_error_config)" 'main {
    try {
        let v = env.get("ORACLE_BLOCKED_VAR")
        print("BYPASS: got value")
    } catch (e) {
        print("BYPASS: caught governance")
    }
    print("BYPASS: reached end")
}' 'export ORACLE_BLOCKED_VAR=secret'
check "O-HARD-01" "HARD block in try/catch → exit 3" "3" "$ORACLE_RC"
check_not_contains "O-HARD-01b" "catch block did not execute" "$ORACLE_OUT" "BYPASS"

# O-HARD-02: HARD block in nested try/catch → exit 3
run_oracle "hard02" "$(hard_error_config)" 'main {
    try {
        try {
            let v = env.get("ORACLE_BLOCKED_VAR")
            print("BYPASS: inner")
        } catch (inner_e) {
            print("BYPASS: inner catch")
        }
        print("BYPASS: between")
    } catch (outer_e) {
        print("BYPASS: outer catch")
    }
    print("BYPASS: end")
}' 'export ORACLE_BLOCKED_VAR=secret'
check "O-HARD-02" "nested try/catch → exit 3" "3" "$ORACLE_RC"
check_not_contains "O-HARD-02b" "no catch blocks executed" "$ORACLE_OUT" "BYPASS"

# O-HARD-03: DETECT in try/catch → exit 0 (caught)
run_oracle "hard03" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "taint_tracking": {
        "enabled": true, "level": "detect",
        "sources": ["env.get"], "sinks": ["file.write"],
        "sanitizers": ["sanitize_"]
    },
    "capabilities": { "env_vars": { "read": true } },
    "security": { "sandbox_level": "elevated" }
}' "use env
use file
main {
    try {
        let s = env.get(\"HOME\")
        file.write(\"$TMPBASE/hard03/out.txt\", string(s))
        print(\"BYPASS: wrote file\")
    } catch (e) {
        print(\"DETECT_CAUGHT\")
    }
}"
check "O-HARD-03" "DETECT in try/catch → exit 0" "0" "$ORACLE_RC"
check_contains "O-HARD-03b" "DETECT was caught" "$ORACLE_OUT" "DETECT_CAUGHT"

# O-HARD-04: HARD block in function called from try → exit 3
run_oracle "hard04" "$(hard_error_config)" 'fn read_secret() {
    let v = env.get("ORACLE_BLOCKED_VAR")
    return v
}
main {
    try {
        let val = read_secret()
        print("BYPASS: got " + string(val))
    } catch (e) {
        print("BYPASS: caught in outer")
    }
}' 'export ORACLE_BLOCKED_VAR=secret'
check "O-HARD-04" "HARD in called function → exit 3" "3" "$ORACLE_RC"
check_not_contains "O-HARD-04b" "function HARD not caught" "$ORACLE_OUT" "BYPASS"

# O-HARD-05: HARD block from custom rule in try → exit 3
run_oracle "hard05" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "HARD-05", "name": "hard_in_try",
        "pattern": "UNCATCHABLE_TRIGGER", "level": "hard",
        "enabled": true, "message": "Uncatchable trigger"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    try {
        let r = <<javascript
// UNCATCHABLE_TRIGGER
1
>>
        print("BYPASS: custom rule caught")
    } catch (e) {
        print("BYPASS: caught custom rule")
    }
}'
check "O-HARD-05" "custom rule HARD in try → exit 3" "3" "$ORACLE_RC"
check_not_contains "O-HARD-05b" "custom HARD not caught" "$ORACLE_OUT" "BYPASS"

unset ORACLE_BLOCKED_VAR 2>/dev/null || true

echo ""

# =====================================================================
# CATEGORY 8: Exit Code Correctness (10 tests)
# =====================================================================
echo "--- Category 8: Exit Code Correctness ---"

# O-EXIT-01: clean program → exit 0
run_oracle "exit01" '{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" }
}' 'main { print("clean") }'
check "O-EXIT-01" "clean program → exit 0" "0" "$ORACLE_RC"

# O-EXIT-02: runtime error → exit 1
run_oracle "exit02" '{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" }
}' 'main { throw "runtime error" }'
check "O-EXIT-02" "runtime throw → exit 1" "1" "$ORACLE_RC"

# O-EXIT-03: quality gate failure → exit 2
run_oracle "exit03" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "EXIT-03", "name": "qgate_marker",
        "pattern": "QGATE_MARKER", "level": "advisory",
        "enabled": true, "message": "Quality gate marker"
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
// QGATE_MARKER
1
>>
    print(r)
}'
check "O-EXIT-03" "quality gate failure → exit 2" "2" "$ORACLE_RC"

# O-EXIT-04: HARD governance block → exit 3
run_oracle "exit04" '{
    "mode": "enforce",
    "languages": { "blocked": ["shell"] },
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<shell
echo "blocked"
>>
    print(r)
}'
check "O-EXIT-04" "HARD governance block → exit 3" "3" "$ORACLE_RC"

# O-EXIT-05: bad config → exit 4
E5DIR="$TMPBASE/exit05"
mkdir -p "$E5DIR"
echo 'NOT_VALID_JSON{{{' > "$E5DIR/govern.json"
echo 'main { print("ok") }' > "$E5DIR/test.naab"
ORACLE_OUT=$(cd "$E5DIR" && "$NAAB" test.naab 2>&1) && ORACLE_RC=$? || ORACLE_RC=$?
check "O-EXIT-05" "bad config → exit 4" "4" "$ORACLE_RC"

# O-EXIT-06: SOFT block (no --governance-override) → exit 3 (SOFT is a block level)
run_oracle "exit06" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "EXIT-06", "name": "soft_rule",
        "pattern": "SOFT_MARKER", "level": "soft",
        "enabled": true, "message": "Soft block"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// SOFT_MARKER
1
>>
    print(r)
}'
check "O-EXIT-06" "SOFT (no override) → exit 3" "3" "$ORACLE_RC"

# O-EXIT-07: advisory only → exit 0
run_oracle "exit07" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [{
        "id": "EXIT-07", "name": "advisory_only",
        "pattern": "ADV_ONLY_MARKER", "level": "advisory",
        "enabled": true, "message": "Advisory only"
    }],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// ADV_ONLY_MARKER
1
>>
    print(r)
}'
check "O-EXIT-07" "advisory only → exit 0" "0" "$ORACLE_RC"

# O-EXIT-08: multiple violations, hard rule first → exit 3
run_oracle "exit08" '{
    "mode": "enforce",
    "languages": { "allowed": ["javascript"] },
    "custom_rules": [
        { "id": "EXIT-08A", "name": "hard", "pattern": "MULTI_MARKER", "level": "hard", "enabled": true, "message": "Hard" },
        { "id": "EXIT-08B", "name": "adv", "pattern": "MULTI_MARKER", "level": "advisory", "enabled": true, "message": "Adv" }
    ],
    "security": { "sandbox_level": "elevated" }
}' 'main {
    let r = <<javascript
// MULTI_MARKER
1
>>
    print(r)
}'
check "O-EXIT-08" "multiple rules, hard first → exit 3" "3" "$ORACLE_RC"

# O-EXIT-09: DETECT uncaught → exit 1
run_oracle "exit09" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/exit09/\"] },
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
    file.write(\"$TMPBASE/exit09/out.txt\", string(s))
    print(\"should not reach\")
}"
check "O-EXIT-09" "DETECT uncaught → exit 1" "1" "$ORACLE_RC"

# O-EXIT-10: DETECT caught → exit 0
run_oracle "exit10" "{
    \"mode\": \"enforce\",
    \"capabilities\": {
        \"filesystem\": { \"mode\": \"read_write\", \"allowed_paths\": [\"$TMPBASE/exit10/\"] },
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
        file.write(\"$TMPBASE/exit10/out.txt\", string(s))
    } catch (e) {
        print(\"DETECT_CAUGHT\")
    }
}"
check "O-EXIT-10" "DETECT caught → exit 0" "0" "$ORACLE_RC"

echo ""

# =====================================================================
# SUMMARY
# =====================================================================
TOTAL=$((PASS + FAIL + SKIP))
echo "=== Decision Oracle Results: $PASS passed, $FAIL failed, $SKIP skipped (of $TOTAL tests) ==="

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "FAILED: Governance decision logic errors detected."
    exit 1
fi

echo "  All governance decisions verified correct."
exit 0
