#!/usr/bin/env bash
# test_hardcoded_status_score.sh -- no_hardcoded_results' status and score
# sub-checks (U-014, the owner's decision 14).
#
# `code_quality.no_hardcoded_results.check_dict_status_fields` and
# `check_perfect_scores` were declared (governance.h), written by both
# templates and `naab-lang init`, and parsed and read by nothing: a block whose
# function returns {"status": "success", "score": 100} whatever its input ran
# under `no_hardcoded_results: hard` (observed on d32f8603, HS-01). Both are now
# parsed and read by checkHardcodedResults(), on the text WITH its string
# literals (the check's pattern table reads it without, and a status is a
# string):
#
#   check_dict_status_fields  a returned dict/object literal whose status-like
#                             key (status/state/result/outcome) holds a
#                             success-like string and whose every value is a
#                             literal.
#   check_perfect_scores      a score-like name (score, accuracy, confidence,
#                             precision, recall, rating, any prefix) set or
#                             mapped to 100, 100.0 or 1.0 that ends the
#                             expression; for an assignment, a name the code
#                             writes nowhere else.
#
# Measured before shipping on the repository's 1,698 polyglot blocks
# (tools/langconform/corpus_diff.py, master vs this build): the first score
# pattern flagged 7, 5 of them computed (`1.0 - err`, a starting value later
# decremented); with the two exclusions, 2 (constant scores in test fixtures)
# and nothing lost. The status pattern matched no block in the corpus.
#
#   HS-00   CONTROL: the check's existing table fires (`return True  # for
#           now`), so the check is on and the harness reads its rule.
#   HS-01   a NAAb program whose python block returns a constant success dict
#           with a perfect score is refused (exit 3) and the block's value is
#           never printed; HS-01c CONTROL: the same program computing both runs.
#   HS-02   a constant success dict is flagged: python, javascript (unquoted
#           key), ruby (=>); HS-02c CONTROL: a computed value in the dict, or an
#           error status, is not.
#   HS-03   a constant perfect score is flagged: assignment, dict key, JS key;
#           HS-03c CONTROL: computed (`1.0 - x`), a starting value written
#           again, and a comparison are not.
#   HS-04   each flag switches its own sub-check off: check_dict_status_fields
#           false leaves a status-only block unflagged and still flags a score;
#           check_perfect_scores false the reverse.
#   HS-05   at advisory the finding is reported and the program runs.
#
# Kill table (mutants of src/runtime/governance_engine.cpp and
# governance_config.cpp, run by hand):
#   H1 the status sub-check never matches          -> HS-02, HS-04
#   H2 the score sub-check never matches           -> HS-03, HS-04
#   H3 the two flags are not parsed                -> HS-04
#   H4 the status sub-check ignores its flag       -> HS-04
#   H5 any dict with a success status matches      -> HS-02c
#   H6 the score literal need not end the expression -> HS-03c
#   H7 a starting value written again still matches -> HS-03c
#   H8 the sub-checks read the stripped text       -> HS-01, HS-02, HS-03

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
GOV="${GOV:-$(dirname "$NAAB")/naab-gov}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_hcss.XXXXXX")"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] && [ -x "$GOV" ] || { echo "UNMEASURABLE: naab-lang or naab-gov not built"; exit 1; }

hr() { printf '{"version":"3.0","mode":"audit","code_quality":{"no_hardcoded_results":{"enabled":true,"level":"hard"%s}}}\n' "${1:-}" > "$2"; }
hr '' "$W/on.json"
hr ', "check_dict_status_fields": false' "$W/nostatus.json"
hr ', "check_perfect_scores": false' "$W/noscore.json"
for c in on nostatus noscore; do
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$W/$c.json" || { echo "UNMEASURABLE: fixture $c.json is not JSON"; exit 1; }
done

# flagged LANG CFG CODE -> 0 when no_hardcoded_results is among the findings.
# A checker that gives no JSON is UNMEASURABLE (exit 2), never "not flagged".
flagged() {
    local out
    out=$(printf '%s\n' "$3" | "$GOV" check --language "$1" --config "$W/$2.json" 2>/dev/null)
    printf '%s' "$out" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    sys.exit(2)
sys.exit(0 if any(v.get("rule") == "code_quality.no_hardcoded_results" for v in d.get("violations", [])) else 1)'
}
# expect ID WANT(yes|no) LANG CFG CODE DESCRIPTION
expect() {
    flagged "$3" "$4" "$5"; local r=$?
    if [ "$r" -eq 2 ]; then bad "$1" "UNMEASURABLE: naab-gov check gave no JSON"; return; fi
    if { [ "$2" = yes ] && [ "$r" -eq 0 ]; } || { [ "$2" = no ] && [ "$r" -eq 1 ]; }; then ok "$1" "$6"
    else bad "$1" "$6 (wanted flagged=$2)" "$5"; fi
}

expect HS-00 yes python on $'def validate(d):\n    return True  # for now' "CONTROL: the existing table fires"

# HS-01: the U-014 reproduction, through a real run.
mkdir -p "$W/run"
printf '{"version":"3.0","mode":"enforce","code_quality":{"no_hardcoded_results":{"enabled":true,"level":"hard"}}}\n' > "$W/run/govern.json"
cat > "$W/run/const.naab" <<'EOF'
main {
    let r = <<python
def check(x):
    return {"status": "success", "score": 100}
str(check(41))
>>
    print("RAN=" + r)
}
EOF
cat > "$W/run/comp.naab" <<'EOF'
main {
    let r = <<python
def check(x):
    errors = [v for v in x if v < 0]
    status = "success" if not errors else "failed"
    return {"status": status, "score": 100 * (1 - len(errors) / max(len(x), 1))}
str(check([1, -2]))
>>
    print("RAN=" + r)
}
EOF
OUT=$(cd "$W/run" && "$NAAB" run const.naab 2>&1); RC=$?
if [ "$RC" -eq 3 ] && ! grep -q '^RAN=' <<<"$OUT" && grep -q 'code_quality.no_hardcoded_results' <<<"$OUT"; then
    ok HS-01 "a block returning a constant success dict with a perfect score is refused (exit 3)"
else bad HS-01 "the constant result ran (rc=$RC)" "$OUT"; fi
OUT=$(cd "$W/run" && "$NAAB" run comp.naab 2>&1); RC=$?
if [ "$RC" -eq 0 ] && grep -q "^RAN=.*'status': 'failed'" <<<"$OUT"; then
    ok HS-01c "CONTROL: the same program computing status and score runs"
elif grep -qi 'python.*not available\|No executor found' <<<"$OUT"; then
    bad HS-01c "UNMEASURABLE: no python executor here" "$OUT"
else bad HS-01c "the computed result did not run (rc=$RC)" "$OUT"; fi

expect HS-02a yes python on $'def check(x):\n    return {"status": "success"}' "a constant success dict (python) is flagged"
expect HS-02b yes javascript on $'function check(x) {\n  return {status: "ok"};\n}' "a constant success object (javascript, unquoted key) is flagged"
expect HS-02c1 no python on $'def check(x):\n    n = len(x)\n    return {"status": "ok", "count": n}' "CONTROL: a success dict with a computed value is not flagged"
expect HS-02c2 no python on $'def f(x):\n    return {"status": "error", "code": 1}' "CONTROL: an error status is not flagged"
expect HS-02r yes ruby on $'def check(x)\n  return {"status" => "passed"}\nend' "a constant success hash (ruby, =>) is flagged"

expect HS-03a yes python on $'def grade(x):\n    accuracy = 1.0\n    return accuracy' "a constant perfect score assignment is flagged"
expect HS-03b yes python on $'def grade(x):\n    return {"confidence": 1.0, "label": x}' "a constant perfect score as a dict value is flagged"
expect HS-03j yes javascript on $'function g(x) {\n  return {score: 100};\n}' "a constant perfect score as an object key is flagged"
expect HS-03c1 no python on $'def grade(x):\n    score = 1.0 - len(x) * 0.1\n    return score' "CONTROL: a computed score (1.0 - x) is not flagged"
expect HS-03c2 no cpp on $'int main() {\n  double score = 100.0;\n  if (bad) score -= 20.0;\n  return 0;\n}' "CONTROL: a starting value the code writes again is not flagged"
expect HS-03c3 no python on $'def f(x):\n    if x.score == 100:\n        return 1\n    return 0' "CONTROL: a comparison is not flagged"

expect HS-04a no python nostatus $'def check(x):\n    return {"status": "success"}' "check_dict_status_fields: false switches the status sub-check off"
expect HS-04b yes python nostatus $'def grade(x):\n    accuracy = 1.0\n    return accuracy' "... and the score sub-check stays on"
expect HS-04c no python noscore $'def grade(x):\n    accuracy = 1.0\n    return accuracy' "check_perfect_scores: false switches the score sub-check off"
expect HS-04d yes python noscore $'def check(x):\n    return {"status": "success"}' "... and the status sub-check stays on"

mkdir -p "$W/adv"
printf '{"version":"3.0","mode":"enforce","code_quality":{"no_hardcoded_results":{"enabled":true,"level":"advisory"}}}\n' > "$W/adv/govern.json"
cp "$W/run/const.naab" "$W/adv/"
OUT=$(cd "$W/adv" && "$NAAB" run const.naab 2>&1); RC=$?
if [ "$RC" -eq 0 ] && grep -q '^RAN=' <<<"$OUT" && grep -q 'Hardcoded result: a constant success status' <<<"$OUT"; then
    ok HS-05 "at advisory the finding is reported and the program runs"
else bad HS-05 "advisory (rc=$RC)" "$OUT"; fi

echo
echo "hardcoded status/score: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
