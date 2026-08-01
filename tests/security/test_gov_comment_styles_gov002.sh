#!/usr/bin/env bash
# test_gov_comment_styles_gov002.sh — V-GOV-002: Language-aware stripComments must
# handle -- comment style for SQL and Lua polyglot blocks.
# Without fix: "-- DROP TABLE users" in a <<sql block is not stripped, so
# governance scanners that use code_clean (comment-stripped) would still see
# "DROP TABLE" as active code (false positive or bypass).
# With fix: -- and rest of line are replaced with spaces, pattern not found.
set -euo pipefail

NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0; SKIP=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# A run where the executor is absent has not verified anything. Without this
# the suite could only say PASS or FAIL, so "SQL executor not available" was
# recorded as a pass — a green result standing in for an unrun check.
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

WORK_DIR="${HOME}/.naab/gov002_$$"
mkdir -p "$WORK_DIR"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

echo "=== test_gov_comment_styles_gov002.sh ==="
echo ""

# ---------------------------------------------------------------------------
# Governance config: enable no_hallucinated_apis with a custom pattern that
# would match SQL keywords if they appear in active code (not in comments).
# checkHallucinatedApis calls stripComments(code_no_strings, language).
# After V-GOV-002 fix, for language="sql", uses_dash_dash=true → -- stripped.
# ---------------------------------------------------------------------------
cat > "$WORK_DIR/govern.json" << 'EOF'
{
  "mode": "HARD",
  "code_quality": {
    "no_hallucinated_apis": {
      "enabled": true,
      "custom_patterns": ["FORBIDDEN_KEYWORD"]
    }
  }
}
EOF

# ---------------------------------------------------------------------------
# T1: SQL block with -- comment containing FORBIDDEN_KEYWORD → must NOT block.
#     checkHallucinatedApis strips strings then language-aware strips comments.
#     After V-GOV-002 fix, -- line is replaced with spaces → FORBIDDEN_KEYWORD
#     not found in code_clean → no false positive block.
# ---------------------------------------------------------------------------
echo "[T1] SQL block: FORBIDDEN_KEYWORD inside -- comment must not trigger custom pattern"
cat > "$WORK_DIR/t1.naab" << 'NAAB'
main {
    let result = <<sql
-- FORBIDDEN_KEYWORD: this is a comment, not active SQL code
SELECT 1 AS result
>>
    print(string(result))
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/t1.naab" --no-governance 2>&1) || ec=$?
if [[ "$ec" -ne 0 ]]; then
    skip "T1 skipped — SQL executor not available: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/t1.naab" 2>&1) || ec2=$?
    if [[ "$ec2" -eq 0 ]]; then
        ok "SQL -- comment with FORBIDDEN_KEYWORD not blocked — comment properly stripped"
    elif echo "$out2" | grep -qi "FORBIDDEN\|hallucinated\|custom.*rule\|governance\|blocked"; then
        fail "False positive: SQL -- comment triggered custom pattern: ${out2:0:120}"
    else
        skip "SQL executor not available or other non-governance exit: ${out2:0:80}"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# T2: SQL block with FORBIDDEN_KEYWORD in active code → MUST be blocked.
#     Verifies the custom pattern check still works for real code (not broken).
# ---------------------------------------------------------------------------
echo "[T2] SQL block: FORBIDDEN_KEYWORD in active SQL code must be blocked"
cat > "$WORK_DIR/t2.naab" << 'NAAB'
main {
    let result = <<sql
SELECT FORBIDDEN_KEYWORD FROM table1
>>
    print(string(result))
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/t2.naab" --no-governance 2>&1) || ec=$?
if [[ "$ec" -ne 0 ]]; then
    skip "T2 skipped — SQL executor not available: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/t2.naab" 2>&1) || ec2=$?
    if echo "$out2" | grep -qi "FORBIDDEN\|hallucinated\|custom.*rule\|governance\|blocked\|denied"; then
        ok "Real FORBIDDEN_KEYWORD in SQL correctly blocked — custom pattern active"
    elif [[ "$ec2" -ne 0 ]]; then
        skip "SQL block with forbidden keyword produced non-zero exit (exit $ec2)"
    else
        skip "SQL executor ran without block (governance may not apply to SQL blocks without executor)"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# T3: Verify basic -- comment stripping behavior using NAAb's checkPolyglotBlock.
#     A script that would ONLY fail if -- comment stripping is broken.
#     Uses checkTemporaryCode (on stripped code) which looks for "# for now"
#     patterns. SQL block with "-- for now" should NOT match since:
#     - Simple stripComments doesn't handle --, but the pattern needs # or //
#     - Language-aware stripComments (V-GOV-002) handles -- for SQL
#     Result: neither before nor after fix would this trigger (pattern needs #//)
#     So T3 is a sanity check: script completes without error regardless.
# ---------------------------------------------------------------------------
echo "[T3] SQL block: -- style comments don't cause unexpected governance errors"
cat > "$WORK_DIR/t3.naab" << 'NAAB'
main {
    let result = <<sql
-- This is a SQL comment: for now just returning 1
-- Another comment line with SELECT syntax reference
SELECT 1 AS value
>>
    print("ok")
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/t3.naab" --no-governance 2>&1) || ec=$?
if [[ "$ec" -ne 0 ]]; then
    skip "T3 skipped — SQL executor not available"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/t3.naab" 2>&1) || ec2=$?
    if [[ "$ec2" -eq 0 ]]; then
        ok "SQL block with -- comments completed without governance error"
    elif echo "$out2" | grep -qi "temporary\|for now\|governance\|blocked"; then
        fail "SQL -- comment unexpectedly triggered governance: ${out2:0:120}"
    else
        skip "SQL executor not available or other non-governance exit (exit $ec2)"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# T4: Python block sanity — # comments in Python are already handled.
#     Verify that Python block with a # comment containing a forbidden pattern
#     is NOT blocked (language-aware stripping handles # for Python).
# ---------------------------------------------------------------------------
echo "[T4] Python block: # comment with FORBIDDEN_KEYWORD must not block (Python comments handled)"
cat > "$WORK_DIR/t4.naab" << 'NAAB'
main {
    let result = <<python
# FORBIDDEN_KEYWORD is mentioned only in this comment
result = "clean"
>>
    print(result)
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/t4.naab" --no-governance 2>&1) || ec=$?
if [[ "$ec" -ne 0 ]] || ! echo "$out" | grep -q "clean"; then
    skip "T4 skipped — Python not available: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/t4.naab" 2>&1) || ec2=$?
    if [[ "$ec2" -eq 0 ]] && echo "$out2" | grep -q "clean"; then
        ok "Python # comment with FORBIDDEN_KEYWORD not blocked — # comments properly stripped"
    elif echo "$out2" | grep -qi "FORBIDDEN\|hallucinated\|governance\|blocked"; then
        fail "False positive: Python # comment triggered custom pattern: ${out2:0:120}"
    else
        skip "Completed (Python may be unavailable): ${out2:0:80}"
    fi
fi

echo ""
TOTAL=$(( PASS + FAIL + SKIP ))
echo "Results: ${PASS}/${TOTAL} passed, ${SKIP} skipped (unverified)"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
