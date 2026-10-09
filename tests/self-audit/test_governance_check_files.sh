#!/usr/bin/env bash
# ============================================================
# test_governance_check_files.sh -- the Governance Check workflow's per-file
# decision (.github/scripts/governance_check_files.sh)
#
# The workflow runs every .naab file it finds. requirements.main_block now
# fires on both engines (it never fired on the VM), so a MODULE -- a file of
# exported functions with no main block -- exits 3 under a config that sets
# it, whatever its code says: tools/agent-governance's seven modules turned
# the PR report into "Failed: 7". Project owner's decision (2026-10-09): keep
# the engine strict and LINT module files instead of running them. naab-gov
# lint now compiles the file, so it applies the per-function checks a run
# applies (contracts, complexity floor, ...) as well as the source checks.
#
#   GW-01  a clean module under a main_block config is linted and passes
#          (run, it fails on the main block alone -- the defect)
#   GW-02  a module with a placeholder is linted and FAILS: source checks
#          reach modules
#   GW-03  a module whose function breaks a must_call contract FAILS: the
#          per-function checks reach modules. Fails with a lint that does not
#          compile the file (master's).
#   GW-03c CONTROL: the same contract breach in a program with a main block
#          fails on the run path, so GW-03's fixture breaks the contract at all
#   GW-04  CONTROL: a clean program with a main block is run and passes, so
#          the config does not refuse everything
#   GW-05  a module with no govern.json anywhere is run as before (nothing to
#          lint against) and passes
#   GW-06  a file that does not parse FAILS
#   GW-07  "main {" inside a comment does not make a module a program: the
#          decision is the parser's, not a grep's
#   GW-08  lint writes its JUnit report when it BLOCKS (it used to exit from
#          main() before writing any report, dropping the finding from the
#          uploaded reports exactly when there was one)
#   GW-09  the script's last two lines are the counts the workflow reads, and
#          its exit status is 1 when any file failed
#
# Against an interpreter that does nothing every file goes down the run path
# and "passes", so GW-01..03c, GW-06, GW-07 and GW-09 fail.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
CHECK="$REPO/.github/scripts/governance_check_files.sh"
NAAB="${NAAB:-$REPO/build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== Governance Check workflow: which files are run, which linted ==="

# Same rule as test_r22_fixes.sh: no naab-gov is UNMEASURABLE, never a pass.
if [ ! -x "$NAAB_GOV" ]; then
    echo "FAIL: naab-gov not built at $NAAB_GOV (UNMEASURABLE, not a pass)"
    echo "  build it with: cmake --build build --target naab-gov"
    exit 1
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-govcheck.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/gov" "$W/nogov"

cat > "$W/gov/govern.json" <<'EOF'
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "requirements": { "main_block": { "level": "hard" } },
  "code_quality": { "no_placeholders": { "enabled": true, "level": "hard" } },
  "contracts": { "level": "hard", "functions": { "compute": { "must_call": ["math.sqrt"] } } } }
EOF
printf 'use math\nexport fn compute(x) {\n    return math.sqrt(x)\n}\n' > "$W/gov/mod_clean.naab"
printf 'use math\nexport fn compute(x) {\n    // TODO: handle negative input\n    return math.sqrt(x)\n}\n' > "$W/gov/mod_todo.naab"
printf 'use math\nexport fn compute(x) {\n    return x * 2\n}\n' > "$W/gov/mod_contract.naab"
printf 'use math\nfn compute(x) {\n    return x * 2\n}\nmain {\n    print(compute(4))\n}\n' > "$W/gov/prog_contract.naab"
printf 'use math\nfn compute(x) {\n    return math.sqrt(x)\n}\nmain {\n    print(compute(4))\n}\n' > "$W/gov/prog_clean.naab"
printf 'use math\nexport fn compute(x {\n' > "$W/gov/broken.naab"
printf 'use math\n// a program would start with: main {\nexport fn compute(x) {\n    return math.sqrt(x)\n}\n' > "$W/gov/mod_comment_main.naab"
printf 'export fn twice(x) {\n    return x * 2\n}\n' > "$W/nogov/mod.naab"

# The order fixes each file's report index (sarif-N/junit-N).
FILES="./gov/mod_clean.naab ./gov/mod_todo.naab ./gov/mod_contract.naab ./gov/prog_contract.naab ./gov/prog_clean.naab ./nogov/mod.naab ./gov/broken.naab ./gov/mod_comment_main.naab"

OUT="$(cd "$W" && NAAB="$NAAB" NAAB_GOV="$NAAB_GOV" REPORT_DIR="$W/reports" bash "$CHECK" $FILES 2>&1)"
RC=$?

# mode <file>: run / lint / "" ; failed <file>: 0/1
mode_of() {
    case "$OUT" in
        *"=== Checking: $1 (lint) ==="*) echo lint ;;
        *"=== Checking: $1 (run) ==="*) echo run ;;
        *) echo "" ;;
    esac
}
failed() { case "$OUT" in *"--- FAILED: $1 "*) return 0 ;; *) return 1 ;; esac; }
show() { printf '%s' "$OUT" | grep -A6 -F "=== Checking: $1" | head -8 | tr '\n' '|' | cut -c1-300; }

# GW-01
if [ "$(mode_of ./gov/mod_clean.naab)" = lint ] && ! failed ./gov/mod_clean.naab; then
    ok "GW-01" "a clean module under requirements.main_block is linted and passes"
else
    bad "GW-01" "a clean module was not linted, or failed" "$(show ./gov/mod_clean.naab)"
fi
# GW-02
if [ "$(mode_of ./gov/mod_todo.naab)" = lint ] && failed ./gov/mod_todo.naab; then
    ok "GW-02" "a placeholder in a module fails the lint (source checks reach modules)"
else
    bad "GW-02" "a module's placeholder was not caught" "$(show ./gov/mod_todo.naab)"
fi
# GW-03
if [ "$(mode_of ./gov/mod_contract.naab)" = lint ] && failed ./gov/mod_contract.naab; then
    ok "GW-03" "a must_call breach in a module function fails the lint (per-function checks reach modules)"
else
    bad "GW-03" "a module function's contract breach was not caught by lint" "$(show ./gov/mod_contract.naab)"
fi
# GW-03c
if [ "$(mode_of ./gov/prog_contract.naab)" = run ] && failed ./gov/prog_contract.naab; then
    ok "GW-03c" "CONTROL: the same breach in a program is caught on the run path"
else
    bad "GW-03c" "CONTROL: the contract fixture is not caught even when run" "$(show ./gov/prog_contract.naab)"
fi
# GW-04
if [ "$(mode_of ./gov/prog_clean.naab)" = run ] && ! failed ./gov/prog_clean.naab; then
    ok "GW-04" "CONTROL: a clean program is run and passes"
else
    bad "GW-04" "CONTROL: a clean program did not pass" "$(show ./gov/prog_clean.naab)"
fi
# GW-05: only meaningful when no govern.json is discoverable above the work dir.
ABOVE=""
d="$W"
while [ "$d" != "/" ] && [ -n "$d" ]; do
    [ -f "$d/govern.json" ] && { ABOVE="$d/govern.json"; break; }
    d="$(dirname "$d")"
done
if [ -n "$ABOVE" ]; then
    skip "GW-05" "a govern.json above the work dir ($ABOVE) would govern it (UNMEASURABLE)"
elif [ "$(mode_of ./nogov/mod.naab)" = run ] && ! failed ./nogov/mod.naab; then
    ok "GW-05" "a module with no govern.json is run as before and passes"
else
    bad "GW-05" "a module with no govern.json was linted or failed" "$(show ./nogov/mod.naab)"
fi
# GW-06
if failed ./gov/broken.naab; then
    ok "GW-06" "a file that does not parse fails"
else
    bad "GW-06" "an unparsable file passed" "$(show ./gov/broken.naab)"
fi
# GW-07
if [ "$(mode_of ./gov/mod_comment_main.naab)" = lint ] && ! failed ./gov/mod_comment_main.naab; then
    ok "GW-07" "\"main {\" in a comment does not make a module a program"
else
    bad "GW-07" "a comment decided the file's mode" "$(show ./gov/mod_comment_main.naab)"
fi
# GW-08: mod_contract is file 3.
J="$W/reports/junit-3.xml"
if [ -s "$J" ] && grep -q '<failure' "$J"; then
    ok "GW-08" "lint writes its JUnit report when it blocks"
else
    bad "GW-08" "no JUnit report (or no failure in it) for a blocked module" "$(ls "$W/reports" 2>/dev/null | tr '\n' ' ')"
fi
# GW-09: four files fail (mod_todo, mod_contract, prog_contract, broken).
LAST2="$(printf '%s\n' "$OUT" | tail -n 2 | tr '\n' ' ')"
if [ "$RC" -eq 1 ] && [ "$LAST2" = "checked=8 failed=4 " ]; then
    ok "GW-09" "counts are the last two lines (checked=8 failed=4) and the exit status is 1"
else
    bad "GW-09" "counts or exit status wrong" "rc=$RC last two lines: $LAST2"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
