#!/usr/bin/env bash
# Scanner/Governance False-Positive Fix Verification Tests
# Fix 1: assigned_never_read — polyglot bindings preserved
# Fix 2: information_disclosure — bare env skip for NAAb
# Fix 3: vcs_secret_extraction — require has_vcs prerequisite
# Fix 4: no_pii — skip all-same-digit credit card matches
# Fix 5: hardcoded_return_value — function-name exemption
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
WORK_DIR="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_fp.XXXXXX")"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

# ── Source Verification Tests ─────────────────────────────────────────────────

echo "=== Source Verification ==="

SRC_SCANNER="$SCRIPT_DIR/../../src/scanner/scanner.cpp"
SRC_GOVCHECK="$SCRIPT_DIR/../../src/runtime/governance_checks.cpp"
SRC_LANGNAAB="$SCRIPT_DIR/../../src/scanner/checks_lang_naab.cpp"

# T1: stripPolyglotBlocks preserves binding list content
if grep -q 'binding_pat' "$SRC_SCANNER"; then
    pass "T1: stripPolyglotBlocks has binding extraction regex"
else
    fail "T1: stripPolyglotBlocks missing binding extraction"
fi

# T2: checkInfoDisclosure skips bare \benv\b for naab language
if grep -q 'language != "naab"' "$SRC_GOVCHECK"; then
    pass "T2: checkInfoDisclosure skips bare env for naab"
else
    fail "T2: checkInfoDisclosure missing naab env skip"
fi

# T3: checkVcsSecretExtraction requires has_vcs before checking signals
if grep -q '!has_vcs' "$SRC_GOVCHECK"; then
    pass "T3: checkVcsSecretExtraction has has_vcs prerequisite"
else
    fail "T3: checkVcsSecretExtraction missing has_vcs guard"
fi

# T4: checkPii skips all-same-digit credit card matches
if grep -q 'all_same.*continue' "$SRC_GOVCHECK"; then
    pass "T4: checkPii skips all-same-digit sequences"
else
    fail "T4: checkPii missing all-same-digit skip"
fi

# T5: hardcoded_return_value skips functions named *default*
if grep -q 'func_name.find("default")' "$SRC_LANGNAAB"; then
    pass "T5: hardcoded_return_value has function-name exemption"
else
    fail "T5: hardcoded_return_value missing function-name exemption"
fi

# ── Behavioral Tests ──────────────────────────────────────────────────────────

echo "=== Behavioral Tests ==="

# Minimal govern.json for scanner tests
cat > "$WORK_DIR/govern.json" << 'JSON'
{
    "version": "4.0",
    "mode": "enforce",
    "scanner": { "version": "1.0", "mode": "enforce",
        "code_quality": { "assigned_never_read": { "enabled": true, "level": "advisory" } },
        "lang_naab": { "hardcoded_return_value": { "enabled": true, "level": "advisory" } }
    },
    "restrictions": {
        "information_disclosure": { "enabled": true, "level": "advisory", "block_env_dump": true },
        "vcs_secret_extraction": { "enabled": true, "level": "advisory" }
    },
    "code_quality": {
        "no_pii": { "enabled": true, "level": "advisory", "detect_credit_card": true }
    }
}
JSON

# T6: Polyglot binding — variable used only in <<python[x]>> should NOT be flagged
cat > "$WORK_DIR/t6_binding.naab" << 'NAAB'
use math

fn compute(data) {
    let x = math.abs(data)
    let result = <<python[x]
x * 2
>>
    return result
}

main {
    print(compute(5))
}
NAAB

OUTPUT=$(cd "$WORK_DIR" && "$NAAB" --scan t6_binding.naab naab 2>&1)
if echo "$OUTPUT" | grep -q 'assigned_never_read.*\bx\b'; then
    fail "T6: polyglot binding var 'x' falsely flagged as assigned_never_read" "$OUTPUT"
else
    pass "T6: polyglot binding var 'x' not flagged"
fi

# T7: use env + env.get_args() should NOT trigger information_disclosure
cat > "$WORK_DIR/t7_env.naab" << 'NAAB'
use env

fn get_mode() {
    let args = env.get_args()
    if len(args) > 0 {
        return args[0]
    }
    return "default"
}

main {
    print(get_mode())
}
NAAB

OUTPUT=$(cd "$WORK_DIR" && "$NAAB" t7_env.naab 2>&1)
if echo "$OUTPUT" | grep -qi 'information.disclosure'; then
    fail "T7: 'use env' falsely triggered information_disclosure" "$OUTPUT"
else
    pass "T7: 'use env' not flagged as information_disclosure"
fi

# T8: file.read + key processing but NO git should NOT trigger vcs_secret_extraction
cat > "$WORK_DIR/t8_fileread.naab" << 'NAAB'
use file
use crypto

fn process_config() {
    let content = file.read("data.txt")
    let key = crypto.sha256(content)
    let result = string.split(content, "\n")
    return key
}

main {
    print("config processor")
}
NAAB

# Create dummy data file
echo "test data" > "$WORK_DIR/data.txt"

OUTPUT=$(cd "$WORK_DIR" && "$NAAB" --scan t8_fileread.naab naab 2>&1)
if echo "$OUTPUT" | grep -qi 'vcs_secret_extraction'; then
    fail "T8: file.read + key falsely triggered vcs_secret_extraction" "$OUTPUT"
else
    pass "T8: file.read + key without git not flagged"
fi

# T9: "0000000000000000" should NOT trigger no_pii credit card
cat > "$WORK_DIR/t9_zeros.naab" << 'NAAB'
fn get_genesis() {
    let hash = "0000000000000000"
    return hash
}

main {
    print(get_genesis())
}
NAAB

OUTPUT=$(cd "$WORK_DIR" && "$NAAB" t9_zeros.naab 2>&1)
if echo "$OUTPUT" | grep -qi 'Credit Card\|no_pii'; then
    fail "T9: all-zero string falsely triggered credit card PII" "$OUTPUT"
else
    pass "T9: all-zero string not flagged as credit card"
fi

# T10: fn get_defaults() returning hardcoded dict should NOT be flagged
cat > "$WORK_DIR/t10_defaults.naab" << 'NAAB'
fn get_defaults() {
    return { "enabled": true, "retries": 0 }
}

main {
    let cfg = get_defaults()
    print(cfg)
}
NAAB

OUTPUT=$(cd "$WORK_DIR" && "$NAAB" --scan t10_defaults.naab naab 2>&1)
if echo "$OUTPUT" | grep -q 'hardcoded_return_value'; then
    fail "T10: get_defaults() falsely flagged for hardcoded_return_value" "$OUTPUT"
else
    pass "T10: get_defaults() not flagged"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
