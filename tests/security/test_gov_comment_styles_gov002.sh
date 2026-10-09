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
# The custom pattern is ADVISORY at this config's level: it warns, it never
# changes the exit code. So "exit 0" cannot tell a stripped comment from a
# detected one, and "output mentions governance" matches the [governance]
# Loaded banner on every run. Each arm asserts on the detection message itself;
# T2 is the positive control that the message appears for ACTIVE code.
DETECTED="Hallucinated API pattern"

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
    if [[ "$out2" == *"$DETECTED"* ]]; then
        fail "False positive: SQL -- comment triggered custom pattern: ${out2:0:120}"
    elif [[ "$ec2" -eq 0 ]]; then
        ok "SQL -- comment with FORBIDDEN_KEYWORD not blocked — comment properly stripped"
    elif grep <<<"$out2" -qi "FORBIDDEN\|hallucinated\|custom.*rule\|governance\|blocked"; then
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
SELECT 1 AS FORBIDDEN_KEYWORD
>>
    print(string(result))
}
NAAB

# The query is valid SQL on its own (FORBIDDEN_KEYWORD is an active column
# alias, not a string or comment), so the probe below measures the executor and
# nothing else. It was SELECT ... FROM table1, a table that never exists, so
# the probe always failed. The probe runs from a directory with NO project
# config: since #244 --no-governance cannot switch off the govern.json beside
# t2.naab, whose whole point is to block this query.
T2_PROBE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/gov002_t2probe.XXXXXX")"
cp "$WORK_DIR/t2.naab" "$T2_PROBE_DIR/t2.naab"
ec=0
out=$("$NAAB" "$T2_PROBE_DIR/t2.naab" --no-governance 2>&1) || ec=$?
rm -rf "$T2_PROBE_DIR"
if [[ "$ec" -ne 0 ]]; then
    skip "T2 skipped — SQL executor not available: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/t2.naab" 2>&1) || ec2=$?
    if [[ "$out2" == *"$DETECTED in sql block: \"FORBIDDEN_KEYWORD\""* ]]; then
        ok "Real FORBIDDEN_KEYWORD in SQL correctly blocked — custom pattern active"
    elif [[ "$ec2" -ne 0 ]]; then
        skip "SQL block with forbidden keyword produced non-zero exit (exit $ec2)"
    else
        # The probe just proved the executor runs this query, so running it
        # unblocked under the governed config is the custom pattern failing.
        fail "SQL with active FORBIDDEN_KEYWORD ran unblocked -- the custom pattern did not apply: ${out2:0:120}"
    fi
fi

echo ""

# ---------------------------------------------------------------------------
# T3: a SQL block whose -- comments read like temporary-code markers runs
#     cleanly under the governed config, and returns its value.
#     It used to accept "exit 0" -- which an interpreter that ran nothing also
#     gives -- and its probe ran from the governed directory (since #244
#     --no-governance cannot switch that config off). It now requires the
#     block's VALUE in the output and no finding, with the probe in a
#     config-less directory, as T2's is.
#     Known gap, recorded rather than fixed here: the temporary-code check runs
#     on code with comments KEPT and its default patterns require a # or //
#     prefix, so a "-- for now" marker in SQL is never detected, stripped or
#     not. Comment syntax per language is the per-language descriptor work.
# ---------------------------------------------------------------------------
echo "[T3] SQL block: -- style comments don't cause unexpected governance errors"
cat > "$WORK_DIR/t3.naab" << 'NAAB'
main {
    let result = <<sql
-- This is a SQL comment: for now just returning 1
-- Another comment line with SELECT syntax reference
SELECT 1 AS value
>>
    print("T3_VALUE=" + string(result[0]["value"]))
}
NAAB

T3_PROBE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/gov002_t3probe.XXXXXX")"
cp "$WORK_DIR/t3.naab" "$T3_PROBE_DIR/t3.naab"
ec=0
out=$("$NAAB" "$T3_PROBE_DIR/t3.naab" --no-governance 2>&1) || ec=$?
rm -rf "$T3_PROBE_DIR"
if [[ "$out" != *"T3_VALUE=1"* ]]; then
    skip "T3 skipped — SQL executor not available: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/t3.naab" 2>&1) || ec2=$?
    if [[ "$out2" == *"T3_VALUE=1"* && "$out2" != *"$DETECTED"* && "$out2" != *"Temporary code marker"* ]]; then
        ok "SQL block with -- comments ran under governance and returned its value"
    else
        # The probe just proved this block runs and returns 1, so anything
        # else under the governed config is governance acting on comments.
        fail "SQL -- comments changed the governed run (exit $ec2): ${out2:0:160}"
    fi
fi

echo ""

# The block ends with a bare `result`: a block's value is its LAST EXPRESSION,
# and an assignment is not one, so a block ending in `result = "clean"`
# returned null and the probe skipped T4 as "Python not available".
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
result
>>
    print(result)
}
NAAB

ec=0
out=$("$NAAB" "$WORK_DIR/t4.naab" --no-governance 2>&1) || ec=$?
if [[ "$ec" -ne 0 ]] || ! grep <<<"$out" -q "clean"; then
    skip "T4 skipped — Python not available: ${out:0:80}"
else
    ec2=0
    out2=$("$NAAB" "$WORK_DIR/t4.naab" 2>&1) || ec2=$?
    if [[ "$out2" == *"$DETECTED"* ]]; then
        fail "False positive: Python # comment triggered custom pattern: ${out2:0:120}"
    elif [[ "$ec2" -eq 0 ]] && grep <<<"$out2" -q "clean"; then
        ok "Python # comment with FORBIDDEN_KEYWORD not blocked — # comments properly stripped"
    elif grep <<<"$out2" -qi "FORBIDDEN\|hallucinated\|governance\|blocked"; then
        fail "False positive: Python # comment triggered custom pattern: ${out2:0:120}"
    else
        skip "Completed (Python may be unavailable): ${out2:0:80}"
    fi
fi

echo ""
TOTAL=$(( PASS + FAIL + SKIP ))
echo "Results: ${PASS}/${TOTAL} passed, ${SKIP} skipped (unverified)"
if [[ "$FAIL" -gt 0 ]]; then exit 1; fi
