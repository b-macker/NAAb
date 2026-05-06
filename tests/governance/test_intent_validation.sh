#!/bin/bash
# test_intent_validation.sh — Regression tests for intent validation v1-v3 fixes
#
# Covers: word boundary (v1), synonym matching (v1+v3), let-string stripping (v1),
#         plural/singular matching (v3), compound identifier truncation (v3),
#         threshold enforcement, error message content.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0
FAIL=0
TOTAL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_intent_XXXXXX")"
trap "rm -rf $WORK" EXIT

# Helper: create govern.json with specific function_intents and level
make_gov() {
    local level="$1"
    local intents="$2"
    cat > "$WORK/govern.json" <<GOVEOF
{
    "version": "5.0",
    "mode": "enforce",
    "languages": { "allowed": ["python"] },
    "code_quality": {
        "intent_validation": {
            "enabled": true,
            "required": true,
            "level": "$level",
            "missing_level": "advisory",
            "mode": "hybrid",
            "min_function_lines": 3,
            "function_intents": {
                $intents
            }
        }
    }
}
GOVEOF
}

# ═══════════════════════════════════════════════════════════
# V1 FIX: Word boundary — underscore treated as boundary
# "load" should match in "load_policies", "evaluate" in "evaluate_action"
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"do_work": "Load data, evaluate results, print output"'
cat > "$WORK/t1.naab" <<'EOF'
fn do_work() {
    let data = load_policies("x")
    let result = evaluate_action(data)
    print("done: " + string(result))
    return result
}
fn load_policies(p) { return [1,2,3] }
fn evaluate_action(d) { return 42 }
main { do_work() }
EOF
OUTPUT=$("$NAAB" "$WORK/t1.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*do_work"; then
    fail "T1: Word boundary — snake_case identifiers should match intent keywords"
else
    ok "T1: Word boundary — load_policies matches 'load', evaluate_action matches 'evaluate'"
fi

# ═══════════════════════════════════════════════════════════
# V1 FIX: Synonym matching — "for" matches "iterate", "run" matches "execute"
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"process_items": "Iterate over items, execute each task, check results"'
cat > "$WORK/t2.naab" <<'EOF'
fn process_items(items) {
    let results = []
    for item in items {
        let r = run_task(item)
        let ok = verify_result(r)
        results.push(ok)
    }
    return results
}
fn run_task(x) { return x }
fn verify_result(x) { return true }
main { process_items([1,2,3]) }
EOF
OUTPUT=$("$NAAB" "$WORK/t2.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*process_items"; then
    fail "T2: Synonym matching — 'for' should match 'iterate', 'run' should match 'execute'"
else
    ok "T2: Synonym matching — for→iterate, run→execute, verify→check all work"
fi

# ═══════════════════════════════════════════════════════════
# V1 FIX: Let-string stripping — let x = "keyword" must NOT inflate overlap
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"sneaky_func": "Load data, evaluate results, detect patterns, log output, print summary"'
cat > "$WORK/t3.naab" <<'EOF'
fn sneaky_func() {
    let load = "load"
    let evaluate = "evaluate"
    let detect = "detect"
    let log_var = "log"
    let print_var = "print"
    let summary = "summary"
    let patterns = "patterns"
    return 42
}
main { sneaky_func() }
EOF
OUTPUT=$("$NAAB" "$WORK/t3.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*sneaky_func"; then
    ok "T3: Let-string stripping — keyword strings in let assignments don't inflate overlap"
else
    fail "T3: Let-string stripping — let x = \"keyword\" should be stripped but wasn't"
fi

# ═══════════════════════════════════════════════════════════
# V3 FIX: Synonym additions — verify, suite, correct
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"run_checks": "Verify results are correct, run test suite"'
cat > "$WORK/t4.naab" <<'EOF'
fn run_checks(data) {
    let valid = check_data(data)
    let expected = true
    if valid == expected {
        let pass_count = test_all(data)
        return pass_count
    }
    return 0
}
fn check_data(d) { return true }
fn test_all(d) { return 5 }
main { run_checks([1,2,3]) }
EOF
OUTPUT=$("$NAAB" "$WORK/t4.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*run_checks"; then
    fail "T4: V3 synonyms — 'verify' should match via 'check', 'correct' via 'expected', 'suite' via 'test'"
else
    ok "T4: V3 synonyms — verify→check, correct→expected, suite→test all work"
fi

# ═══════════════════════════════════════════════════════════
# V3 FIX: Plural/singular matching — "actions" matches "action"
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"handle_items": "Parse actions, evaluate verdicts, generate reports"'
cat > "$WORK/t5.naab" <<'EOF'
fn handle_items(data) {
    let action = parse_action(data)
    let verdict = evaluate_verdict(action)
    let report = generate_report(verdict)
    return report
}
fn parse_action(d) { return d }
fn evaluate_verdict(a) { return "ok" }
fn generate_report(v) { return "report: " + string(v) }
main { handle_items({}) }
EOF
OUTPUT=$("$NAAB" "$WORK/t5.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*handle_items"; then
    fail "T5: Plural matching — 'actions' should match 'action', 'verdicts'→'verdict', 'reports'→'report'"
else
    ok "T5: Plural matching — actions→action, verdicts→verdict, reports→report all work"
fi

# ═══════════════════════════════════════════════════════════
# V3 FIX: Compound identifier truncation — 5+ part names capped at 3
# ═══════════════════════════════════════════════════════════

# This function ONLY passes if the stuffed name inflates overlap.
# With truncation, the 6-part name contributes ≤3 keywords, which isn't enough.
# Compound truncation test: the ONLY source of keywords "gamma", "delta", "epsilon"
# is parts 4-6 of the stuffed identifier. If truncation works, those keywords
# will be missing and overlap drops below threshold.
make_gov "hard" '"trunc_test": "Alpha beta gamma delta epsilon zeta"'
cat > "$WORK/t6.naab" <<'EOF'
fn trunc_test() {
    let x = alpha_beta_and_gamma_delta_epsilon()
    if x > 0 {
        return x
    }
    return 0
}
fn alpha_beta_and_gamma_delta_epsilon() { return 42 }
main { trunc_test() }
EOF
OUTPUT=$("$NAAB" "$WORK/t6.naab" 2>&1 || true)
# With truncation: identifier becomes "alpha_beta_and" → matches alpha, beta only
# Without truncation: matches alpha, beta, gamma, delta, epsilon (5/6 = 83%)
# Threshold: max(0.3, 2/6) = 0.33. With truncation: 2/6 = 33% → borderline pass
# So we use a 7-keyword intent where only 2 match after truncation: 2/7 = 28% → FAIL
make_gov "hard" '"trunc_test": "Alpha beta gamma delta epsilon zeta eta"'
OUTPUT=$("$NAAB" "$WORK/t6.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*trunc_test"; then
    ok "T6: Compound truncation — parts 4+ of 6-part identifier are stripped, lowering overlap"
else
    fail "T6: Compound truncation — stuffed identifier parts 4+ should be truncated"
fi

# ═══════════════════════════════════════════════════════════
# Threshold enforcement — below 30% must fail
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"unrelated_func": "Load data, evaluate results, detect patterns, log output"'
cat > "$WORK/t7.naab" <<'EOF'
fn unrelated_func() {
    let x = 1 + 2
    let y = x * 3
    let z = y - 1
    return z
}
main { unrelated_func() }
EOF
OUTPUT=$("$NAAB" "$WORK/t7.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*unrelated_func"; then
    ok "T7: Threshold — code with zero keyword overlap is correctly blocked"
else
    fail "T7: Threshold — zero overlap should trigger intent mismatch"
fi

# ═══════════════════════════════════════════════════════════
# Error message content — must show matched/missing and NOT leak internals
# ═══════════════════════════════════════════════════════════

make_gov "soft" '"partial_func": "Load data, evaluate results, detect patterns, log output, print summary"'
cat > "$WORK/t8.naab" <<'EOF'
fn partial_func() {
    let x = load_data("file.txt")
    let y = x + 1
    let z = y + 2
    return z
}
fn load_data(f) { return 42 }
main { partial_func() }
EOF
OUTPUT=$("$NAAB" "$WORK/t8.naab" 2>&1 || true)

# Should show "Matched:" and "Missing:" in error output
if echo "$OUTPUT" | grep -q "Matched:"; then
    ok "T8a: Error message shows matched keywords"
else
    fail "T8a: Error message should show 'Matched:' line"
fi

if echo "$OUTPUT" | grep -q "Missing:"; then
    ok "T8b: Error message shows missing keywords"
else
    fail "T8b: Error message should show 'Missing:' line"
fi

# Must NOT leak bypass info (note: "synonyms" in user-facing hint is OK)
if echo "$OUTPUT" | grep -qi "no-governance\|governance-override\|truncat\|plural_variant\|KEYWORD_SYNONYM"; then
    fail "T8c: Error message leaks internal mechanism names"
else
    ok "T8c: Error message does not leak bypass/internal info"
fi

# ═══════════════════════════════════════════════════════════
# Comments and long strings stripped — must not inflate overlap
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"comment_func": "Load data, evaluate results, detect patterns, log output"'
cat > "$WORK/t9.naab" <<'EOF'
fn comment_func() {
    // load evaluate detect patterns log output
    let long_str = "load evaluate detect patterns log output data results"
    let x = 1 + 2
    return x
}
main { comment_func() }
EOF
OUTPUT=$("$NAAB" "$WORK/t9.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*comment_func"; then
    ok "T9: Comments and long strings stripped — don't inflate overlap"
else
    fail "T9: Comments and long strings should be stripped before matching"
fi

# ═══════════════════════════════════════════════════════════
# Natural snake_case identifiers — multiple keywords in one identifier
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"analyze": "Load data, compute risk score, generate report"'
cat > "$WORK/t10.naab" <<'EOF'
fn analyze(input) {
    let data = load_data(input)
    let score = compute_risk_score(data)
    let report = generate_report(score)
    return report
}
fn load_data(x) { return x }
fn compute_risk_score(d) { return 42 }
fn generate_report(s) { return "score: " + string(s) }
main { analyze("test") }
EOF
OUTPUT=$("$NAAB" "$WORK/t10.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*analyze"; then
    fail "T10: Natural snake_case — load_data, compute_risk_score, generate_report should match"
else
    ok "T10: Natural snake_case identifiers correctly match multiple intent keywords"
fi

# ═══════════════════════════════════════════════════════════
# Advisory level does not block execution
# ═══════════════════════════════════════════════════════════

make_gov "advisory" '"advisory_func": "Load data, evaluate results, detect patterns, log output"'
cat > "$WORK/t11.naab" <<'EOF'
fn advisory_func() {
    let x = 1 + 2
    return x
}
main {
    let r = advisory_func()
    print("result: " + string(r))
}
EOF
OUTPUT=$("$NAAB" "$WORK/t11.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -q "result: 3"; then
    ok "T11: Advisory level — execution proceeds despite zero overlap"
else
    fail "T11: Advisory level should warn but not block execution"
fi

# ═══════════════════════════════════════════════════════════
# 3-part identifiers NOT truncated (only 5+ parts are)
# ═══════════════════════════════════════════════════════════

make_gov "hard" '"three_part": "Compute risk score, load input data, format output text"'
cat > "$WORK/t12.naab" <<'EOF'
fn three_part() {
    let data = load_input_data("x")
    let score = compute_risk_score(data)
    let text = format_output_text(score)
    return text
}
fn load_input_data(x) { return x }
fn compute_risk_score(d) { return 50 }
fn format_output_text(s) { return "risk: " + string(s) }
main { three_part() }
EOF
OUTPUT=$("$NAAB" "$WORK/t12.naab" 2>&1 || true)
if echo "$OUTPUT" | grep -qi "Intent mismatch.*three_part"; then
    fail "T12: 3-part identifiers must NOT be truncated"
else
    ok "T12: 3-part identifiers (compute_risk_score) preserved — not truncated"
fi

# ═══════════════════════════════════════════════════════════
echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
if [ $FAIL -eq 0 ]; then
    echo "  test_intent_validation.sh: ALL PASSED"
else
    echo "  test_intent_validation.sh: FAILURES DETECTED"
    exit 1
fi
