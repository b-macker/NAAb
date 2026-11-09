#!/usr/bin/env bash
# test_custom_rules_run.sh -- an enabled custom rule is enforced on every route,
# or the config is refused; it is never dropped (F-34, F-05; Bundle 17)
#
# Two ways a configured custom_rules entry did nothing (observed on master
# 314bccfd, from Gemini's SentinelGrid report):
#   F-34  a rule for "naab" (or for every language) was applied by naab-gov
#         lint and to polyglot blocks, never to NAAb code at naab-lang run:
#         lint exit 3, the same program run exit 0, "Governance: PASS".
#         checkNaabSourceText(), which every NAAb file that runs passes
#         through (the entry file and each imported module), now applies them.
#   F-05  a pattern the ReDoS heuristic called unsafe was SKIPPED at load with
#         a warning (V-GOV-010, 436e0e12), and so was one that did not
#         compile: the rule `\brequire\s*\(...(child_process|fs|net|https?)...`
#         was skipped as "overlapping alternatives" and the JavaScript it
#         forbids ran, exit 0. Patterns now compile with naab::rx (RE2, linear
#         time; a pattern RE2 cannot take falls back to a step-budgeted
#         matcher that refuses rather than hangs, A89), so V-GOV-010's concern
#         is met by the engine; a pattern that does not compile refuses the
#         config (exit 4) instead of loading without the rule.
#
#   CR-00   CONTROL a JavaScript block with the marker is refused (the rule works)
#   CR-01   native NAAb code with the marker is refused at run (exit 3)
#   CR-01b  naab-gov lint agrees (exit 3)
#   CR-02   the marker in an imported module is refused at run
#   CR-03   a rule with no languages list applies to NAAb code too
#   CR-04   the finding names the line the match is on
#   CR-05c  CONTROL a rule for "python" only leaves NAAb code alone (exit 0)
#   CR-06c  CONTROL NAAb code without the marker runs (exit 0)
#   CR-07   Gemini's rule (skipped on master) is enforced on a JavaScript block
#   CR-08   V-GOV-010: (a+)+$ over a 30,000-character line finishes, no signal,
#           and its matching control is refused
#   CR-09   a rule that needs the fallback matcher (backreference) is enforced
#   CR-10   a pattern that does not compile refuses the config (exit 4), naming it
#   CR-10c  CONTROL the same rule with "enabled": false loads and runs
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-09):
#   checkNaabSourceText without checkCustomRules        CR-01, CR-02, CR-03, CR-04, CR-08
#   the matched line not computed                       CR-04
#   the V-GOV-010 skip restored                         CR-07, CR-08
#   an invalid pattern skipped with a warning again     CR-10
# Red first (observed, master 314bccfd): CR-01, CR-02, CR-03, CR-04, CR-07,
# CR-08 and CR-10 fail; CR-01b, CR-09 (master's heuristic let that pattern
# through) and the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_crrun_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

# rule ID PATTERN LANGS [ENABLED]  -> one custom_rules entry (JSON)
rule() {
    python3 -c 'import json,sys
r={"id":sys.argv[1],"pattern":sys.argv[2],"level":"hard","enabled":sys.argv[4]=="true","message":"custom rule hit"}
if sys.argv[3]!="-": r["languages"]=sys.argv[3].split(",")
print(json.dumps(r))' "$1" "$2" "$3" "${4:-true}"
}
# setup ID RULE_JSON -> a project dir with govern.json
setup() {
    D="$W/$1"; mkdir -p "$D"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "custom_rules": [%s]}\n' "$2" > "$D/govern.json"
}
run() { RC=0; OUT=$(cd "$D" && timeout 60s "$NAAB" "$@" 2>&1) || RC=$?; }
refused() { [ "$RC" -eq 3 ] && ! grep -q "RAN=" <<<"$OUT" && grep -q 'custom_rules' <<<"$OUT"; }

M='MARKER_[0-9]+'
NAAB_MARK='main {
    let tok = "MARKER_42"
    print("RAN=" + tok)
}'
NAAB_CLEAN='main {
    print("RAN=clean")
}'
JS_MARK='main {
    let r = <<javascript
"MARKER_42"
>>
    print("RAN=" + string(r))
}'

echo "=== custom rules: enforced on every route, or the config refused ==="

setup cr00 "$(rule CR-00 "$M" naab,javascript)"; printf '%s\n' "$JS_MARK" > "$D/p.naab"; run p.naab
if refused; then pass CR-00 "CONTROL: a JavaScript block with the marker is refused"
else fail CR-00 "the rule did not refuse a JavaScript block (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"; exit 1; fi

setup cr01 "$(rule CR-01 "$M" naab)"; printf '%s\n' "$NAAB_MARK" > "$D/p.naab"; run p.naab
if refused; then pass CR-01 "native NAAb code with the marker is refused at run"
else fail CR-01 "a custom rule for naab did not apply at run (rc=$RC)" "$OUT"; fi

if [ -x "$GOV" ] || [ -x "$GOV.exe" ]; then
    RC=0; OUT=$(cd "$D" && timeout 60s "$GOV" lint p.naab 2>&1) || RC=$?
    if [ "$RC" -eq 3 ] && grep -q 'custom_rules' <<<"$OUT"; then pass CR-01b "naab-gov lint agrees (exit 3)"
    else fail CR-01b "lint did not refuse the same file (rc=$RC)" "$OUT"; fi
else unm CR-01b "naab-gov not built"; fi

setup cr02 "$(rule CR-02 "$M" naab)"
printf 'export fn tag() {\n    return "MARKER_7"\n}\n' > "$D/lib.naab"
printf 'import "./lib.naab" as lib\nmain {\n    print("RAN=" + lib.tag())\n}\n' > "$D/p.naab"
run p.naab
if refused; then pass CR-02 "the marker in an imported module is refused at run"
else fail CR-02 "an imported module was not checked (rc=$RC)" "$OUT"; fi

setup cr03 "$(rule CR-03 "$M" -)"; printf '%s\n' "$NAAB_MARK" > "$D/p.naab"; run p.naab
if refused; then pass CR-03 "a rule with no languages list applies to NAAb code"
else fail CR-03 "an all-languages rule did not apply to NAAb code (rc=$RC)" "$OUT"; fi

setup cr04 "$(rule CR-04 "$M" naab)"; printf '%s\n' "$NAAB_MARK" > "$D/p.naab"; run p.naab
if refused && grep -qE 'line 2\b' <<<"$OUT"; then pass CR-04 "the finding names line 2, where the match is"
else fail CR-04 "the finding does not name the matched line (rc=$RC)" "$OUT"; fi

setup cr05 "$(rule CR-05 "$M" python)"; printf '%s\n' "$NAAB_MARK" > "$D/p.naab"; run p.naab
if [ "$RC" -eq 0 ] && grep -q "RAN=MARKER_42" <<<"$OUT"; then pass CR-05c "CONTROL: a python-only rule leaves NAAb code alone"
else fail CR-05c "a python-only rule refused NAAb code (rc=$RC)" "$OUT"; fi

setup cr06 "$(rule CR-06 "$M" naab)"; printf '%s\n' "$NAAB_CLEAN" > "$D/p.naab"; run p.naab
if [ "$RC" -eq 0 ] && grep -q "RAN=clean" <<<"$OUT"; then pass CR-06c "CONTROL: NAAb code without the marker runs"
else fail CR-06c "clean NAAb code did not run (rc=$RC)" "$OUT"; fi

GEMINI='\brequire\s*\(\s*['"'"'"](child_process|fs|net|https?)['"'"'"]\s*\)'
setup cr07 "$(rule CR-07 "$GEMINI" javascript)"
printf 'main {\n    let r = <<javascript\n"require('"'"'fs'"'"')"\n>>\n    print("RAN=" + string(r))\n}\n' > "$D/p.naab"
run p.naab
if refused; then pass CR-07 "the rule master skipped as 'overlapping alternatives' is enforced"
else fail CR-07 "the heuristic-flagged rule was not enforced (rc=$RC)" "$OUT"; fi

# CR-08: V-GOV-010's own threat, a catastrophic-backtracking pattern over a
# long line. The text is in the SOURCE (custom rules read source text).
setup cr08 "$(rule CR-08 '(a+)+$' naab)"
python3 -c 'import sys; open(sys.argv[1],"w").write("main {\n    let s = \"" + "a"*30000 + "b\"\n    print(\"RAN=long\")\n}\n")' "$D/p.naab"
run p.naab
if [ "$RC" -lt 124 ] && grep -q "RAN=long" <<<"$OUT"; then
    setup cr08m "$(rule CR-08m '(a+)+$' naab)"
    python3 -c 'import sys; open(sys.argv[1],"w").write("main {\n    print(\"RAN=x\")\n}\n// " + "a"*30000)' "$D/p.naab"
    run p.naab
    if refused; then pass CR-08 "(a+)+\$ over 30,000 characters finishes, and its matching control is refused"
    else fail CR-08 "the ReDoS-shaped rule did not refuse its matching control (rc=$RC)" "$OUT"; fi
else fail CR-08 "the ReDoS-shaped rule hung, crashed or refused a non-matching file (rc=$RC)" "$OUT"; fi

setup cr09 "$(rule CR-09 '(MARK)\1' javascript)"
printf 'main {\n    let r = <<javascript\n"MARKMARK"\n>>\n    print("RAN=" + string(r))\n}\n' > "$D/p.naab"
run p.naab
if refused; then pass CR-09 "a backreference rule (fallback matcher) is enforced"
else fail CR-09 "a backreference rule was not enforced (rc=$RC)" "$OUT"; fi

setup cr10 "$(rule CR-10 '([unclosed' naab)"; printf '%s\n' "$NAAB_CLEAN" > "$D/p.naab"; run p.naab
if [ "$RC" -eq 4 ] && grep -q 'CR-10' <<<"$OUT" && ! grep -q "RAN=" <<<"$OUT"; then
    pass CR-10 "a pattern that does not compile refuses the config (exit 4), naming the rule"
else fail CR-10 "an invalid pattern did not refuse the config (rc=$RC)" "$OUT"; fi

setup cr10c "$(rule CR-10c '([unclosed' naab false)"; printf '%s\n' "$NAAB_CLEAN" > "$D/p.naab"; run p.naab
if [ "$RC" -eq 0 ] && grep -q "RAN=clean" <<<"$OUT"; then pass CR-10c "CONTROL: the same rule disabled loads and runs"
else fail CR-10c "a disabled invalid rule refused the config (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
