#!/usr/bin/env bash
# ============================================================
# test_closing_lines.sh -- every run ends with a summary that matches what
# happened (A174, A39(c))
#
# THE DEFECTS (observed on 58796deb, docs/open-investigations.md)
#
#   A174    a refused run printed no closing summary: it ended at the error.
#   A39(c)  the two closing lines disagreed. "-- Governance: PASS (N static,
#           M runtime, 0 violations) --" is the after-run audit's verdict:
#           "static" counted every check before it, runtime gates included,
#           and "violations" only the audit's findings. After two refusals
#           the operator overrode, a run printed "OVERRIDE" twice, then
#           "Summary ... 2 blocked" (nothing was blocked), then
#           "PASS ... 0 violations".
#
# THE FIX (the owner's decision, 2026-11-16: keep PASS, fix the counts):
#   Summary counts by disposition -- "P passed, W warned, [O overridden, ]
#     [A approved, ][U audit-only, ]R refused" -- and its rows say WARN /
#     OVERRIDDEN / AUDIT ONLY / REFUSED.
#   A finished run:  "-- Governance: PASS (2 warnings; after-run audit: 0 findings) --"
#   A refused run:   the summary, then "-- Governance: REFUSED (rule) --", on
#                    stderr after the error on stdout, once.
#
# ARMS
#   CLO-01  refused during the run: summary with "1 refused", then
#           "-- Governance: REFUSED (capabilities.filesystem.path) --"
#   CLO-02  refused before the run (a static HARD block): the same, naming
#           code_quality.no_placeholders
#   CLO-03  the REFUSED line is printed once, and is the last stderr line
#   CLO-04  a finished run with warnings: "1 warned" and
#           "-- Governance: PASS (1 warning; after-run audit: 0 findings) --"
#   CLO-05  overridden refusals: "2 overridden", no "blocked", PASS line
#           "(2 overridden; after-run audit: 0 findings)"
#   CLO-06  audit mode: "2 audit-only" and the PASS line says so
#   CLO-07  a clean run: no summary, "-- Governance: PASS (after-run audit: 0 findings) --"
#   CLO-08  control: exit codes are unchanged (3 refused, 0 finished)
#
# KILL TABLE (mutants, each turns the named arms red; run by hand)
#   M1 main.cpp's catch sites skip printRefusalClosing     CLO-01, CLO-03
#   M2 the static-block exit skips it                       CLO-02
#   M3 printRefusalClosing prints every call (no once)      CLO-03 (needs two exits; see note)
#   M4 summary counts by level again ("blocked")            CLO-05, CLO-06
#   M5 PASS line back to "(N static, M runtime, ...)"       CLO-04..CLO-07
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/naab-closing-lines-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "${TEST_TMP:?}"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB (UNMEASURABLE)"; exit 1; }

echo "=== test_closing_lines.sh (A174, A39(c)) ==="

# arm DIR MODE CONFIG_BODY PROGRAM [args...] -> out.txt err.txt, RC
arm() {
    local d="$1" mode="$2" cfg="$3" prog="$4"; shift 4
    mkdir -p "$d/vault"; echo s > "$d/vault/x.txt"
    cat > "$d/govern.json" <<EOF
{"version":"4.0","mode":"$mode","security":{"sandbox_level":"elevated"},
 $cfg}
EOF
    printf '%s' "$prog" > "$d/p.naab"
    (cd "$d" && VAR_ONE=1 VAR_TWO=2 timeout 60 "$NAAB" run p.naab "$@" > out.txt 2> err.txt)
    RC=$?
}
has()  { grep -qF -- "$2" "$1"; }
last_line() { grep -v '^[[:space:]]*$' "$1" | tail -1; }

BLOCKED_READ='"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}'

# ---------- CLO-01 / CLO-03: refused during the run ----------
W="$TEST_TMP/clo01"
arm "$W" enforce "$BLOCKED_READ" $'use file\nmain {\n    print("before")\n    let k = file.read("vault/x.txt")\n}\n'
if [ "$RC" -ne 3 ] || ! has "$W/out.txt" "File path blocked"; then
    fail "CLO-01" "fixture: expected a refusal (exit 3), got rc=$RC" "$(head -2 "$W/out.txt" | tr '\n' ' ')"
elif has "$W/err.txt" "[governance] Summary (mode: enforce): " && has "$W/err.txt" " 1 refused" &&
     [ "$(last_line "$W/err.txt")" = "-- Governance: REFUSED (capabilities.filesystem.path) --" ]; then
    pass "CLO-01" "refused during the run: summary, then REFUSED (capabilities.filesystem.path)"
else
    fail "CLO-01" "no closing summary for a refused run" "last stderr line: $(last_line "$W/err.txt")"
fi
N="$(grep -c -- '-- Governance: REFUSED' "$W/err.txt")"
if [ "$N" = "1" ] && [ "$(last_line "$W/err.txt")" = "-- Governance: REFUSED (capabilities.filesystem.path) --" ]; then
    pass "CLO-03" "the REFUSED line is printed once, last"
else
    fail "CLO-03" "REFUSED printed $N times, or not last" "$(last_line "$W/err.txt")"
fi

# ---------- CLO-02: refused before the run ----------
W="$TEST_TMP/clo02"
arm "$W" enforce '"code_quality":{"no_placeholders":{"enabled":true,"level":"hard"}}' \
$'fn f() {\n    // TODO: later\n    return 1\n}\nmain {\n    print(f())\n}\n'
if [ "$RC" -ne 3 ]; then
    fail "CLO-02" "fixture: expected a static refusal (exit 3), got rc=$RC"
elif [ "$(last_line "$W/err.txt")" = "-- Governance: REFUSED (code_quality.no_placeholders) --" ] &&
     has "$W/err.txt" " 1 refused"; then
    pass "CLO-02" "refused before the run: summary, then REFUSED (code_quality.no_placeholders)"
else
    fail "CLO-02" "no closing summary for a static refusal" "last stderr line: $(last_line "$W/err.txt")"
fi

# ---------- CLO-04: finished with a warning ----------
W="$TEST_TMP/clo04"
arm "$W" enforce '"code_quality":{"no_placeholders":{"enabled":true,"level":"advisory"}}' \
$'fn f() {\n    // TODO: later\n    return 1\n}\nmain {\n    print(f())\n}\n'
if [ "$RC" -eq 0 ] && has "$W/err.txt" " 1 warned, 0 refused" &&
   [ "$(last_line "$W/err.txt")" = "-- Governance: PASS (1 warning; after-run audit: 0 findings) --" ]; then
    pass "CLO-04" "a finished run with a warning: \"1 warned\" and PASS (1 warning; ...)"
else
    fail "CLO-04" "rc=$RC" "$(grep -E 'Summary|Governance:' "$W/err.txt" | tr '\n' '|')"
fi

# ---------- CLO-05: overridden ----------
W="$TEST_TMP/clo05"
arm "$W" enforce '"capabilities":{"env_vars":{"allowed_read":["HOME"]}}' \
$'use env\nmain {\n    let x = env.get("VAR_ONE")\n    let y = env.get("VAR_TWO")\n    print("RAN")\n}\n' --governance-override
if [ "$RC" -eq 0 ] && has "$W/out.txt" "RAN" && has "$W/err.txt" " 2 overridden, 0 refused" &&
   ! grep -q 'blocked' "$W/err.txt" &&
   [ "$(last_line "$W/err.txt")" = "-- Governance: PASS (2 overridden; after-run audit: 0 findings) --" ]; then
    pass "CLO-05" "overridden refusals: \"2 overridden\", nothing \"blocked\", PASS (2 overridden; ...)"
else
    fail "CLO-05" "rc=$RC" "$(grep -E 'Summary|Governance:|blocked' "$W/err.txt" | tr '\n' '|')"
fi

# ---------- CLO-06: audit mode ----------
W="$TEST_TMP/clo06"
arm "$W" audit '"capabilities":{"env_vars":{"allowed_read":["HOME"]}}' \
$'use env\nmain {\n    let x = env.get("VAR_ONE")\n    let y = env.get("VAR_TWO")\n    print("RAN")\n}\n'
if [ "$RC" -eq 0 ] && has "$W/err.txt" " 2 audit-only, 0 refused" &&
   [ "$(last_line "$W/err.txt")" = "-- Governance: PASS (2 audit-only; after-run audit: 0 findings) --" ]; then
    pass "CLO-06" "audit mode: \"2 audit-only\" in the summary and the PASS line"
else
    fail "CLO-06" "rc=$RC" "$(grep -E 'Summary|Governance:' "$W/err.txt" | tr '\n' '|')"
fi

# ---------- CLO-07: clean ----------
W="$TEST_TMP/clo07"
arm "$W" enforce '"capabilities":{"env_vars":{"allowed_read":["HOME"]}}' \
$'main {\n    print("RAN")\n}\n'
if [ "$RC" -eq 0 ] && ! grep -q 'Summary' "$W/err.txt" &&
   [ "$(last_line "$W/err.txt")" = "-- Governance: PASS (after-run audit: 0 findings) --" ]; then
    pass "CLO-07" "a clean run: no summary, PASS (after-run audit: 0 findings)"
else
    fail "CLO-07" "rc=$RC" "$(grep -E 'Summary|Governance:' "$W/err.txt" | tr '\n' '|')"
fi

# ---------- CLO-08: exit codes unchanged ----------
[ -f "$TEST_TMP/clo01/err.txt" ] && [ -f "$TEST_TMP/clo04/err.txt" ]
R1="$(cd "$TEST_TMP/clo01" && timeout 60 "$NAAB" run p.naab >/dev/null 2>&1; echo $?)"
R4="$(cd "$TEST_TMP/clo04" && timeout 60 "$NAAB" run p.naab >/dev/null 2>&1; echo $?)"
if [ "$R1" = "3" ] && [ "$R4" = "0" ]; then
    pass "CLO-08" "control: exit codes unchanged (refused 3, finished 0)"
else
    fail "CLO-08" "exit codes refused=$R1 finished=$R4, expected 3 and 0"
fi

echo
echo "=== $PASS_COUNT passed, $FAIL_COUNT failed ==="
[ "$FAIL_COUNT" -eq 0 ]
