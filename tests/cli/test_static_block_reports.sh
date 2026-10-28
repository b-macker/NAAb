#!/usr/bin/env bash
# test_static_block_reports.sh -- a static HARD block on the VM writes its reports (A43(e))
#
# --governance-sarif is the CI code-scanning channel. On the VM the per-function
# checks run inside compile() and the whole-source checks (secrets, PII,
# incomplete logic over the whole file), the intent preflight and agent review
# run in main.cpp just before it. Only compile() was wrapped in
# write-reports-then-rethrow (W-01, test_declaration_source.sh), so a block from
# a whole-source check left through the top-level catch with NEITHER report
# written: the SARIF omitted exactly the static blocks. Measured on master
# 6055ac3c: a PII literal in main{} exits 3 and writes no SARIF and no report;
# a TODO in a function body writes both.
#
#   SR-00c  CONTROL: a clean program writes both reports (exit 0)
#   SR-00n  CONTROL: the PII program under a config without no_pii runs (exit 0)
#           -- SR-02's exit 3 is the policy's
#   SR-01   a per-function block (a TODO) writes both reports naming the rule
#           (W-01's shape, pinned here for --governance-report too)
#   SR-02   a whole-source PII block writes both reports naming code_quality.no_pii
#   SR-03   a whole-source secret block writes both reports naming code_quality.no_secrets
#
# Kill table (mutant rebuilt, suite re-run; observed 2026-10-28):
#   the wrapper narrowed back to compile() alone   -> SR-02, SR-03 red
# Red first: on master SR-02 and SR-03 fail, every control and SR-01 pass.
# Secret and PII values are assembled from fragments: no literal sits in this
# file, only in the program it writes.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
# Unsigned configs: an isolated trust store, or a key installed elsewhere on
# the machine turns every run into an integrity block (exit 3).
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_sbr_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

POLICY='{ "mode": "enforce", "security": {"sandbox_level": "elevated"},
  "code_quality": {"no_placeholders": {"enabled": true, "level": "hard"},
                   "no_pii": {"enabled": true, "level": "hard"},
                   "no_secrets": {"enabled": true, "level": "hard"}} }'
NOPOLICY='{ "mode": "enforce", "security": {"sandbox_level": "elevated"} }'

# case ID CONFIG PROGRAM-TEXT -> sets RC and checks the two files; prints nothing
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/govern.json"
    printf '%s' "$3" > "$d/p.naab"
    RC=0
    (cd "$d" && "$NAAB" run p.naab --governance-sarif r.sarif --governance-report r.json >/dev/null 2>&1) || RC=$?
    D="$d"
}
both() { [ -s "$D/r.sarif" ] && [ -s "$D/r.json" ]; }
names() { grep -q "$1" "$D/r.sarif" && grep -q "$1" "$D/r.json"; }
state() { echo "rc=$RC sarif=$([ -s "$D/r.sarif" ] && echo yes || echo no) report=$([ -s "$D/r.json" ] && echo yes || echo no)"; }

PII="12""3-4""5-6789"
KEY="AK""IA""TESTTESTTESTTEST"

echo "=== Static blocks write their reports (A43(e)) ==="

run sr00c "$POLICY" $'main {\n    print("clean")\n}\n'
if [ "$RC" -eq 0 ] && both; then pass SR-00c "CONTROL: a clean program writes both reports"
else fail SR-00c "a clean program should write both reports" "$(state)"; fi

run sr00n "$NOPOLICY" "$(printf 'main {\n    let s = "%s"\n    print(s)\n}\n' "$PII")"
if [ "$RC" -eq 0 ]; then pass SR-00n "CONTROL: the PII program runs when no_pii is not configured"
else fail SR-00n "the PII program should run without no_pii" "$(state)"; fi

run sr01 "$POLICY" $'fn f() {\n    // TODO: finish\n    return 1\n}\nmain {\n    print(f())\n}\n'
if [ "$RC" -eq 3 ] && both && names 'code_quality.no_placeholders'; then
    pass SR-01 "a per-function block writes SARIF and the report, naming the rule"
else fail SR-01 "a per-function block should write both reports" "$(state)"; fi

run sr02 "$POLICY" "$(printf 'main {\n    let s = "%s"\n    print(s)\n}\n' "$PII")"
if [ "$RC" -eq 3 ] && both && names 'code_quality.no_pii'; then
    pass SR-02 "a whole-source PII block writes SARIF and the report, naming code_quality.no_pii"
else fail SR-02 "a whole-source PII block should write both reports" "$(state)"; fi

run sr03 "$POLICY" "$(printf 'main {\n    let k = "%s"\n    print(k)\n}\n' "$KEY")"
if [ "$RC" -eq 3 ] && both && names 'code_quality.no_secrets'; then
    pass SR-03 "a whole-source secret block writes SARIF and the report, naming code_quality.no_secrets"
else fail SR-03 "a whole-source secret block should write both reports" "$(state)"; fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
