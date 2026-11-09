#!/usr/bin/env bash
# test_exec_contract_scope.sh -- execution and derivation contracts judge the
# function the program has, by its code (F-32, F-41, F-42; Bundle 17)
#
# Three defects from Gemini's SentinelGrid report, each observed on master
# 314bccfd:
#   F-32  runExecutionContracts() ran every must_produce/must_vary/... in
#         govern.json after the program finished, and a contracted function
#         the program neither defines nor imports threw "not found" -> HARD:
#         every other program of a multi-program project exited 3 after it had
#         run, and `naab-gov contracts` on one module reported BLOCKED. By the
#         owner's decision (2026-11-09) such a function is skipped and named
#         ("Execution contracts not run: ..."); like every name-keyed contract
#         it cannot see a rename.
#   F-41  a missing argument was padded with an empty list whatever the
#         parameter's declared type, so with contracts.validate_inputs every
#         typed multi-parameter function failed must_differentiate/must_vary.
#         A position "params" types now gets an empty value of that type
#         (untyped keeps the list, f7dec793's choice).
#   F-42  must_derive_from was satisfied by ANY one listed parameter, found by
#         a regex over the raw body -- comments and strings included. Now
#         every listed parameter must appear in code (the owner's decision of
#         2026-11-09); comments and string literals do not count, and a
#         ${...} interpolation does (it is code).
#
#   EC-01   F-32: a program without the contracted function runs (exit 0) and
#           the skipped contract is named
#   EC-01c  CONTROL the same contract on a program whose function is wrong: exit 3
#   EC-02c  CONTROL a correct imported implementation passes (exit 0, not skipped)
#   EC-02w  CONTROL a wrong imported implementation is caught (exit 3)
#   EC-03   F-32: naab-gov contracts on a module is not blocked by another
#           module's must_produce (exit 0)
#   EC-04   F-41: must_differentiate on a typed two-parameter function with
#           validate_inputs passes (exit 0)
#   EC-04c  CONTROL a function that does not differentiate is still caught (exit 3)
#   EC-05   F-42: parameters named only in a comment do not satisfy it (exit 3)
#   EC-06   F-42: a parameter named only in a string does not (exit 3)
#   EC-07   F-42: one of three parameters used is not enough (exit 3)
#   EC-08c  CONTROL all three used: passes (exit 0)
#   EC-09c  CONTROL one used inside a ${...} interpolation counts (exit 0)
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-09):
#   absent functions not skipped (throw again)       EC-01, EC-03
#   padding always an empty list                     EC-04
#   the body read as raw text (no lexer)             EC-05, EC-06
#   any-one semantics restored                       EC-07
# Red first (observed, master 314bccfd): EC-01, EC-03, EC-04, EC-05, EC-06 and
# EC-07 fail; the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_execctr_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

# proj ID CONTRACTS_JSON [EXTRA]: a project dir with govern.json; D is set
proj() {
    D="$W/$1"; mkdir -p "$D"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "contracts": {"level": "hard"%s, "functions": %s}}\n' "${3:-}" "$2" > "$D/govern.json"
}
run() { RC=0; OUT=$(cd "$D" && timeout 60s "$NAAB" "$@" 2>&1) || RC=$?; }

DOUBLE='{"double_it": {"must_produce": [{"args": [2], "expect": 4}]}}'
echo "=== execution and derivation contracts judge the program's own code ==="

proj ec01 "$DOUBLE"
printf 'main {\n    print("RAN=other")\n}\n' > "$D/p.naab"; run p.naab
if [ "$RC" -eq 0 ] && grep -q "RAN=other" <<<"$OUT" && grep -q "Execution contracts not run: double_it" <<<"$OUT"; then
    pass EC-01 "a program without double_it runs, and the skipped contract is named"
else fail EC-01 "a program without the contracted function was blocked or not told (rc=$RC)" "$OUT"; fi

printf 'fn double_it(n) {\n    return n + 1\n}\nmain {\n    print("RAN=" + string(double_it(2)))\n}\n' > "$D/w.naab"; run w.naab
if [ "$RC" -eq 3 ] && grep -q "must_produce" <<<"$OUT"; then pass EC-01c "CONTROL: a wrong double_it is still caught (exit 3)"
else fail EC-01c "a wrong implementation was not caught (rc=$RC)" "$OUT"; fi

printf 'export fn double_it(n) {\n    return n * 2\n}\n' > "$D/lib.naab"
printf 'import "./lib.naab" as lib\nmain {\n    print("RAN=" + string(lib.double_it(2)))\n}\n' > "$D/i.naab"; run i.naab
if [ "$RC" -eq 0 ] && ! grep -q "not run" <<<"$OUT"; then pass EC-02c "CONTROL: a correct imported double_it is tested, not skipped"
else fail EC-02c "an imported implementation was skipped or refused (rc=$RC)" "$OUT"; fi

proj ec02w "$DOUBLE"
printf 'export fn double_it(n) {\n    return n + 1\n}\n' > "$D/lib.naab"
printf 'import "./lib.naab" as lib\nmain {\n    print("RAN=" + string(lib.double_it(2)))\n}\n' > "$D/i.naab"; run i.naab
if [ "$RC" -eq 3 ] && grep -q "must_produce" <<<"$OUT"; then pass EC-02w "CONTROL: a wrong imported double_it is caught (exit 3)"
else fail EC-02w "a wrong imported implementation was not caught (rc=$RC)" "$OUT"; fi

if [ -x "$GOV" ] || [ -x "$GOV.exe" ]; then
    proj ec03 '{"double_it": {"must_produce": [{"args": [2], "expect": 4}]}, "clamp": {"requires": ["n >= 0"], "ensures": ["result <= 10"]}}'
    printf 'fn clamp(n) {\n    if n > 10 { return 10 }\n    return n\n}\nmain {\n    print(string(clamp(3)))\n}\n' > "$D/m.naab"
    RC=0; OUT=$(cd "$D" && timeout 120s "$GOV" contracts m.naab 2>&1) || RC=$?
    if [ "$RC" -eq 0 ] && grep -q "no counterexample" <<<"$OUT" && ! grep -q "BLOCKED" <<<"$OUT"; then
        pass EC-03 "naab-gov contracts on one module is not blocked by another's must_produce"
    else fail EC-03 "the search was blocked by a contract on an absent function (rc=$RC)" "$OUT"; fi
else unm EC-03 "naab-gov not built"; fi

DIFF='{"score": {"params": ["x:int", "y:int"], "must_differentiate": [{"a": 1, "b": 2, "key": "r"}]}}'
proj ec04 "$DIFF" ', "validate_inputs": true'
printf 'fn score(x, y) {\n    return {"r": x * 2 + y}\n}\nmain {\n    print("RAN=" + string(score(1, 1).get("r")))\n}\n' > "$D/p.naab"; run p.naab
if [ "$RC" -eq 0 ] && grep -q "RAN=3" <<<"$OUT"; then pass EC-04 "a typed two-parameter function passes must_differentiate with validate_inputs"
else fail EC-04 "a typed multi-parameter function failed its execution contract (rc=$RC)" "$OUT"; fi

proj ec04c "$DIFF" ', "validate_inputs": true'
printf 'fn score(x, y) {\n    return {"r": 7 + y}\n}\nmain {\n    print("RAN=" + string(score(1, 1).get("r")))\n}\n' > "$D/p.naab"; run p.naab
if [ "$RC" -eq 3 ] && grep -q "must_differentiate" <<<"$OUT"; then pass EC-04c "CONTROL: a function that ignores x is caught (exit 3)"
else fail EC-04c "a non-differentiating function was not caught (rc=$RC)" "$OUT"; fi

DERIVE='{"decide": {"must_derive_from": {"action": ["roa", "penalty", "threshold"]}}}'
derive() {   # derive ID BODY_LINES
    proj "$1" "$DERIVE"
    printf 'fn decide(roa, penalty, threshold) {\n%s\n}\nmain {\n    print("RAN=" + string(decide("valid", 1, 2).get("action")))\n}\n' "$2" > "$D/p.naab"
    run p.naab
}
derive ec05 '    // roa penalty threshold are all weighed upstream
    return {"action": "accept"}'
if [ "$RC" -eq 3 ] && grep -q "Derivation contract" <<<"$OUT"; then pass EC-05 "names only in a comment do not satisfy must_derive_from"
else fail EC-05 "a comment satisfied must_derive_from (rc=$RC)" "$OUT"; fi

derive ec06 '    let s = "roa penalty threshold"
    return {"action": s}'
if [ "$RC" -eq 3 ] && grep -q "Derivation contract" <<<"$OUT"; then pass EC-06 "names only in a string do not satisfy must_derive_from"
else fail EC-06 "a string literal satisfied must_derive_from (rc=$RC)" "$OUT"; fi

derive ec07 '    if roa == "valid" { return {"action": "accept"} }
    return {"action": "reject"}'
if [ "$RC" -eq 3 ] && grep -q "penalty" <<<"$OUT"; then pass EC-07 "one of three parameters is not enough, and the missing one is named"
else fail EC-07 "one parameter of three satisfied must_derive_from (rc=$RC)" "$OUT"; fi

derive ec08 '    if roa == "valid" && penalty < threshold { return {"action": "accept"} }
    return {"action": "reject"}'
if [ "$RC" -eq 0 ] && grep -q "RAN=accept" <<<"$OUT"; then pass EC-08c "CONTROL: all three parameters used: passes"
else fail EC-08c "a function using every parameter was refused (rc=$RC)" "$OUT"; fi

derive ec09 '    let note = "p=${penalty}"
    if roa == "valid" && len(note) > threshold { return {"action": "accept"} }
    return {"action": "reject"}'
if [ "$RC" -eq 0 ] && grep -q "RAN=accept" <<<"$OUT"; then pass EC-09c "CONTROL: a parameter used inside \${...} counts"
else fail EC-09c "an interpolated parameter did not count (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
