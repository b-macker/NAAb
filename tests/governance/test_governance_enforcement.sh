#!/usr/bin/env bash
# test_governance_enforcement.sh — Comprehensive governance enforcement tests
# Validates all governance check paths work correctly (BUG-1 through BUG-7, DX-1 through DX-4)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

# Skip on platforms without working polyglot executors
PROBE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/gov_probe_XXXXXX")
cat > "$PROBE_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"off"}
EOF
cat > "$PROBE_DIR/probe.naab" <<'EOF'
main { let x = <<python
print("ok")
>>
print(x) }
EOF
PROBE_OUT=$("$NAAB" --no-governance "$PROBE_DIR/probe.naab" 2>&1 || true)
rm -rf "$PROBE_DIR"
if echo "$PROBE_OUT" | grep -q "No executor found\|not available"; then
    echo "  test_governance_enforcement.sh: SKIPPED (no python/shell executor)"
    exit 0
fi

PASS=0; FAIL=0; TOTAL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); }

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/gov_enforce_XXXXXX")
trap "rm -rf $WORK_DIR" EXIT

echo "=== test_governance_enforcement.sh ==="

# ---------------------------------------------------------------------------
# BUG-1: banned_functions regex escaping
# ---------------------------------------------------------------------------

# T1: banned_functions with pipe char doesn't false-positive
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","languages":{"per_language":{"shell":{"banned_functions":["curl |","wget |"]}}}}
EOF
cat > "$WORK_DIR/t1.naab" <<'EOF'
main { let x = <<shell
hostname
echo "done"
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t1.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "banned function"; then
    fail "T1: BUG-1 — 'curl |' false-positive on hostname"
else
    ok "T1: BUG-1 — banned_functions pipe char escaped correctly"
fi

# T2: banned_functions exact match works
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","languages":{"per_language":{"python":{"banned_functions":["eval("]}}}}
EOF
cat > "$WORK_DIR/t2.naab" <<'EOF'
main { let x = <<python
result = eval("1+1")
print(result)
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t2.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "banned function.*eval"; then
    ok "T2: BUG-1 — eval( correctly caught in python block"
else
    fail "T2: BUG-1 — eval( not caught"
fi

# T3: banned_functions multi-word match
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","languages":{"per_language":{"shell":{"banned_functions":["rm -rf /"]}}}}
EOF
cat > "$WORK_DIR/t3.naab" <<'EOF'
main { let x = <<shell
rm -rf /tmp/safe
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t3.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "banned function"; then
    fail "T3: 'rm -rf /tmp/safe' should NOT match 'rm -rf /' (substring issue)"
else
    ok "T3: multi-word banned function doesn't over-match"
fi

# ---------------------------------------------------------------------------
# BUG-2: no_secrets in NAAb string literals
# ---------------------------------------------------------------------------

# T4: hardcoded secret in NAAb code is caught
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","restrictions":{"no_secrets":{"enabled":true,"level":"hard"}}}
EOF
cat > "$WORK_DIR/t4.naab" <<'EOF'
main {
    let key = "sk-proj-abcdef1234567890abcdef1234567890"
    print(key)
}
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t4.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "secret\|API.*Key\|redacted"; then
    ok "T4: BUG-2 — secret in NAAb string caught"
else
    fail "T4: BUG-2 — secret in NAAb string not caught"
    echo "    Got: $(echo "$OUTPUT" | head -3)"
fi

# T5: secret in polyglot block also caught
cat > "$WORK_DIR/t5.naab" <<'EOF'
main { let x = <<python
api_key = "sk-proj-abcdef1234567890abcdef1234567890"
print(api_key)
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t5.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "secret\|API.*Key\|redacted"; then
    ok "T5: secret in polyglot block caught"
else
    fail "T5: secret in polyglot block not caught"
fi

# ---------------------------------------------------------------------------
# BUG-3: per_language imports without restrictions.imports
# ---------------------------------------------------------------------------

# T6: per_language.python.imports.blocked works standalone
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","languages":{"per_language":{"python":{"imports":{"mode":"blocklist","blocked":["subprocess"]}}}}}
EOF
cat > "$WORK_DIR/t6.naab" <<'EOF'
main { let x = <<python
import subprocess
print("hi")
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t6.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "blocked import.*subprocess\|Blocked import"; then
    ok "T6: BUG-3 — per_language imports.blocked works without restrictions.imports"
else
    fail "T6: BUG-3 — subprocess not caught"
    echo "    Got: $(echo "$OUTPUT" | head -3)"
fi

# ---------------------------------------------------------------------------
# BUG-6: EVA elevation silent
# ---------------------------------------------------------------------------

# T7: no EVA warning noise
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","code_quality":{"no_temporary_code":{"enabled":true,"level":"advisory"},"no_apologetic_language":{"enabled":true,"level":"advisory"}}}
EOF
cat > "$WORK_DIR/t7.naab" <<'EOF'
main { print(42) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t7.naab" 2>&1)
if echo "$OUTPUT" | grep -q "elevating to soft"; then
    fail "T7: BUG-6 — EVA elevation warning still printing"
else
    ok "T7: BUG-6 — EVA elevation is silent"
fi

# ---------------------------------------------------------------------------
# BUG-7 / DX-4: null method call hint
# ---------------------------------------------------------------------------

# T8: method on null gives helpful hint
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce"}
EOF
cat > "$WORK_DIR/t8.naab" <<'EOF'
main {
    let x = null
    print(x.upper())
}
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t8.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "null\|Hint\|env.get\|??"; then
    ok "T8: BUG-7 — null method call has helpful error"
else
    fail "T8: BUG-7 — null method call error lacks hint"
    echo "    Got: $(echo "$OUTPUT" | head -3)"
fi

# ---------------------------------------------------------------------------
# DX-1: dashboard shows mode
# ---------------------------------------------------------------------------

# T9: dashboard includes mode info
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","governance":{"dashboard":true}}
EOF
cat > "$WORK_DIR/t9.naab" <<'EOF'
main { print(42) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t9.naab" 2>&1)
if echo "$OUTPUT" | grep -q "Mode:.*enforce"; then
    ok "T9: DX-1 — dashboard shows mode"
else
    fail "T9: DX-1 — dashboard missing mode info"
    echo "    Got: $(echo "$OUTPUT" | grep -i "mode\|agent\|dashboard" | head -3)"
fi

# ---------------------------------------------------------------------------
# Placeholders: code vs string
# ---------------------------------------------------------------------------

# T10: placeholder in NAAb comment caught (via polyglot stripped check)
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","code_quality":{"no_placeholders":{"enabled":true,"level":"soft"}}}
EOF
cat > "$WORK_DIR/t10.naab" <<'EOF'
main { let x = <<python
# TODO: fix this later
print("hello")
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t10.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "placeholder\|TODO"; then
    ok "T10: placeholder in polyglot comment caught"
else
    fail "T10: placeholder in polyglot comment not caught"
    echo "    Got: $(echo "$OUTPUT" | head -3)"
fi

# T11: placeholder in string literal NOT caught (EVA-10 intentional)
cat > "$WORK_DIR/t11.naab" <<'EOF'
main { let x = <<python
msg = "TODO: fix this later"
print(msg)
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t11.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "placeholder"; then
    fail "T11: EVA-10 — placeholder inside string should NOT be caught"
else
    ok "T11: EVA-10 — placeholder inside string correctly ignored"
fi

# ---------------------------------------------------------------------------
# Blocked language
# ---------------------------------------------------------------------------

# T12: blocked language produces clear error
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","languages":{"blocked":["php"]}}
EOF
cat > "$WORK_DIR/t12.naab" <<'EOF'
main { let x = <<php
echo "hello";
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t12.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "blocked\|not allowed\|php"; then
    ok "T12: blocked language (PHP) gives clear error"
else
    fail "T12: blocked language not caught"
fi

# ---------------------------------------------------------------------------
# Clean program
# ---------------------------------------------------------------------------

# T13: clean program passes with zero violations
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce"}
EOF
cat > "$WORK_DIR/t13.naab" <<'EOF'
main {
    let x = 42
    print(x)
}
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t13.naab" 2>&1)
EXIT_CODE=$?
if [ $EXIT_CODE -eq 0 ] && echo "$OUTPUT" | grep -q "42"; then
    ok "T13: clean program passes governance"
else
    fail "T13: clean program failed (exit $EXIT_CODE)"
fi

# ---------------------------------------------------------------------------
# Schema typo detection
# ---------------------------------------------------------------------------

# T14: unknown key triggers did-you-mean
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","securty":{"sandbox_level":"restricted"}}
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t13.naab" 2>&1)
if echo "$OUTPUT" | grep -qi "unknown key.*securty\|did you mean.*security"; then
    ok "T14: schema typo 'securty' triggers suggestion"
else
    fail "T14: no typo warning for 'securty'"
fi

# ---------------------------------------------------------------------------
# parseEnforcementLevel handles object format
# ---------------------------------------------------------------------------

# T15: {enabled: true, level: "hard"} format works in restrictions
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","restrictions":{"no_placeholders":{"enabled":true,"level":"hard"}}}
EOF
cat > "$WORK_DIR/t15.naab" <<'EOF'
main { let x = <<python
# FIXME: broken code
print("test")
>>
print(x) }
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t15.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "placeholder\|FIXME\|HARD"; then
    ok "T15: restrictions.no_placeholders with object format works"
else
    fail "T15: object format not parsed in restrictions"
    echo "    Got: $(echo "$OUTPUT" | head -3)"
fi

# T16: restrictions.no_secrets alias works
cat > "$WORK_DIR/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","restrictions":{"no_secrets":"hard"}}
EOF
cat > "$WORK_DIR/t16.naab" <<'EOF'
main {
    let key = "AKIA1234567890ABCDEF"
    print(key)
}
EOF
OUTPUT=$("$NAAB" "$WORK_DIR/t16.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "secret\|AWS\|redacted"; then
    ok "T16: restrictions.no_secrets string format works"
else
    fail "T16: restrictions.no_secrets string format not working"
    echo "    Got: $(echo "$OUTPUT" | head -3)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $TOTAL total"
if [ $FAIL -gt 0 ]; then exit 1; fi
exit 0
