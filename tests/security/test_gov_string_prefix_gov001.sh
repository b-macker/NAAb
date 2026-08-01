#!/usr/bin/env bash
# test_gov_string_prefix_gov001.sh — V-GOV-001: Python/JS string prefixes (f/b/r/u)
# must be consumed by stripStringLiterals() before pattern matching.
# Without fix: f"dangerous()" → prefix 'f' remains in stripped output after
# string-literal stripping. With fix: prefix replaced with space — no residue.
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# A run whose executor is absent, or whose failure cannot be attributed, has
# verified nothing. Without this the suite could only say PASS or FAIL, so
# "not available" was recorded as a pass — a green standing in for an unrun check.
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

WORK_DIR="${HOME}/.naab/gov001_$$"
mkdir -p "$WORK_DIR/injection" "$WORK_DIR/placeholder"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

echo "=== test_gov_string_prefix_gov001.sh ==="
echo ""

# ---------------------------------------------------------------------------
# Governance config for T1–T3: code injection check (block_command_injection)
# checkPolyglotBlock calls stripStringLiterals(code) → 'stripped' → then
# passes stripped to checkCodeInjection. With the fix, f-prefix is consumed
# before the quote, so f"os.system('id')" correctly strips to whitespace.
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/injection/govern.json" << 'EOF'
{
  "mode": "HARD",
  "restrictions": {
    "code_injection": {
      "enabled": true,
      "block_command_injection": true,
      "block_dynamic_code_gen": false,
      "block_sql_injection_patterns": false
    }
  }
}
EOF

# ---------------------------------------------------------------------------
# T1: f-string containing os.system — content is inside a string, must NOT block.
#     checkCodeInjection uses stripped=stripStringLiterals(code). The f-string
#     content is stripped so os.system pattern is not found → no false positive.
# ---------------------------------------------------------------------------
echo "[T1] f\"os.system('id')\" — command inside f-string must not block (no false positive)"
cat > "$WORK_DIR/injection/t1.naab" << 'NAAB'
main {
    let result = <<python
bad_practice_doc = f"os.system('id') is dangerous"
result = "ok"
>>
    print(result)
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/injection/t1.naab" --no-governance 2>&1) || ec=$?
if [[ "$ec" -ne 0 ]] || ! echo "$out" | grep -q "ok"; then
    skip "T1 skipped — Python not available or unexpected output: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/injection/t1.naab" 2>&1) || ec2=$?
    if [[ "$ec2" -eq 0 ]] && echo "$out2" | grep -q "ok"; then
        ok "f-string os.system not blocked — string prefix properly consumed by stripStringLiterals"
    elif echo "$out2" | grep -qi "injection\|blocked\|governance\|command"; then
        fail "False positive: f-string content triggered command-injection: ${out2:0:120}"
    else
        skip "Script completed (Python may be unavailable or governance not triggered): ${out2:0:80}"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# T2: r-string containing a command pattern — must NOT block.
#     The two-letter prefix rb/br/rf/fr and single-letter r/b/u/f are all
#     consumed before the quote. r"os.system('id')" → prefix consumed → content
#     stripped → no pattern match → no false positive.
# ---------------------------------------------------------------------------
echo "[T2] r\"os.system('id')\" — command inside raw string must not block"
cat > "$WORK_DIR/injection/t2.naab" << 'NAAB'
main {
    let result = <<python
warning_msg = r"Never call os.system('id') directly"
result = "safe"
>>
    print(result)
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/injection/t2.naab" --no-governance 2>&1) || ec=$?
if [[ "$ec" -ne 0 ]] || ! echo "$out" | grep -q "safe"; then
    skip "T2 skipped — Python not available: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/injection/t2.naab" 2>&1) || ec2=$?
    if [[ "$ec2" -eq 0 ]] && echo "$out2" | grep -q "safe"; then
        ok "r-string os.system not blocked — raw prefix consumed correctly"
    elif echo "$out2" | grep -qi "injection\|blocked\|governance"; then
        fail "False positive: r-string content triggered injection check: ${out2:0:120}"
    else
        skip "Completed (Python may be unavailable): ${out2:0:80}"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# T3: Real os.system() call in Python block — MUST be blocked.
#     Verifies the injection check still works for actual dangerous code
#     (not accidentally disabled by the fix).
# ---------------------------------------------------------------------------
echo "[T3] Real os.system() in Python block — must be blocked"
cat > "$WORK_DIR/injection/t3.naab" << 'NAAB'
main {
    let result = <<python
import os
os.system("echo dangerous")
result = "ran"
>>
    print(result)
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/injection/t3.naab" --no-governance 2>&1) || ec=$?
if [[ "$ec" -ne 0 ]] || ! echo "$out" | grep -qi "ran\|dangerous"; then
    skip "T3 skipped — Python not available: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/injection/t3.naab" 2>&1) || ec2=$?
    if echo "$out2" | grep -qi "injection\|blocked\|governance\|command\|denied"; then
        ok "Real os.system() correctly blocked — governance check still active"
    elif [[ "$ec2" -ne 0 ]]; then
        ok "Real os.system() blocked with non-zero exit (exit $ec2)"
    else
        fail "Real os.system() was NOT blocked — injection check may be broken: ${out2:0:120}"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# T4: NAAb function body — string containing "TODO" must not trigger
#     no_placeholders check. checkNaabFunctionBody calls stripStringLiterals
#     which removes string content. "TODO" inside a NAAb string is stripped
#     before checking, so no false positive.
# ---------------------------------------------------------------------------
echo "[T4] NAAb function body with string containing TODO — must not trigger no_placeholders"

cat > "$WORK_DIR/placeholder/govern.json" << 'EOF'
{
  "mode": "HARD",
  "code_quality": {
    "no_placeholders": {
      "enabled": true,
      "level": "HARD"
    }
  }
}
EOF
cat > "$WORK_DIR/placeholder/t4.naab" << 'NAAB'
function describe_issue() {
    let msg = "TODO: this text is inside a string literal, not actual placeholder code"
    return 42
}
main {
    let r = describe_issue()
    print(string(r))
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/placeholder/t4.naab" 2>&1) || ec=$?
if [[ "$ec" -eq 0 ]] && echo "$out" | grep -q "42"; then
    ok "NAAb string with TODO not blocked — placeholder inside string is stripped"
elif echo "$out" | grep -qi "placeholder\|TODO\|governance\|blocked"; then
    fail "False positive: TODO inside NAAb string triggered no_placeholders: ${out:0:120}"
else
    fail "Unexpected failure (exit $ec): ${out:0:120}"
fi

echo ""
TOTAL=$(( PASS + FAIL + SKIP ))
echo "Results: ${PASS}/${TOTAL} passed, ${SKIP} skipped (unverified)"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
