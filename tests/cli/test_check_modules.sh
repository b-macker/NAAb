#!/usr/bin/env bash
# test_check_modules.sh -- `naab-lang check` agrees with the run (A188).
#
# The type checker behind `naab-lang check` erred both ways (observed on
# c1312132 and d32f8603):
#   - it REJECTED programs that run: every `use`/`import` alias read as an
#     undefined variable (the checker visited no import statement), `codegen`
#     and the other modules missing from its hand-typed list read the same,
#     and `"text" + 3` / `"a" * 2` were type errors though both engines compute
#     them;
#   - it PASSED programs that fail: `regex`, `path`, `uuid`, `base64`, `yaml`
#     were in its list, so `regex.matches(...)` without `use regex` passed
#     `check` and fails on both engines ("Undefined variable 'regex'").
# The checker now binds exactly what the VM binds: the VM's prelude
# (naab/stdlib_prelude.h, the one list the VM reads too), `string`, and each
# import's alias; and types `+` with a string on either side, and string * int,
# as the engines compute them.
#
# Every arm runs the SAME program through `check` and through `run` (the VM,
# the default engine): the run is the oracle, never a value typed here.
#   CK-00     CONTROL: a plain program checks clean and runs.
#   CK-01..09 programs that run must check clean (exit 0).
#   CK-10, CK-11, CK-13, CK-14 programs that fail must fail `check` (exit 1):
#     CK-10 CONTROL an undefined variable; CK-11 a module the program never
#     imported (`regex` without `use`, which passed before); CK-13/CK-14
#     CONTROL `"a" - 1` and `"a" * 2.5` (the type rule widened for `+` and
#     string * int only).
# The checker binds only the names an import writes. On the VM an import
# also makes every function of the module callable by its bare name, exported
# or not (A216, observed; the tree-walker refuses) -- a defect, not semantics
# for the checker to copy, so no arm pins it here.
#
# Kill table (mutants of src/semantic/type_checker.cpp, run by hand):
#   T1 visit(Program) visits no import again          -> CK-01..CK-07
#   T2 ModuleUseStmt binds nothing                    -> CK-01, CK-02, CK-06, CK-07
#   T3 ModuleUseStmt binds the full dotted path       -> CK-09
#   T4 ImportStmt binds nothing                       -> CK-03, CK-04, CK-05
#   T5 ImportStmt binds the item's name, not alias    -> CK-04
#   T6 the old hand-typed module list                 -> CK-11
#   T7 + accepts string + string only (old rule)      -> CK-08
#   T8 string * int not typed                         -> CK-08b
#   T9 string * any (float too)                       -> CK-14

set -uo pipefail
PASS=0; FAIL=0; UNMEAS=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_checkmods.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -6 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

printf 'export fn f() {\n    return 42\n}\n' > "$WORK/mymod.naab"
mkdir -p "$WORK/pkg"; printf 'export fn g() {\n    return 7\n}\n' > "$WORK/pkg/inner.naab"

# prog ID BODY: ID.naab is BODY. check_arm ID DESC WANT(accept|refuse) MARKER
check_arm() {
    local id="$1" desc="$2" want="$3" marker="${4:-}" crc rrc cout rout
    cout=$(cd "$WORK" && "$NAAB" check "$id.naab" 2>&1); crc=$?
    rout=$(cd "$WORK" && "$NAAB" run "$id.naab" --no-governance 2>&1); rrc=$?
    if [ "$want" = accept ]; then
        if [ "$rrc" -ne 0 ] || { [ -n "$marker" ] && ! grep -qF -- "$marker" <<<"$rout"; }; then
            bad "$id" "the program did not run as the arm assumes (rc=$rrc): UNMEASURABLE" "$rout"; UNMEAS=$((UNMEAS+1)); return
        fi
        if [ "$crc" -eq 0 ]; then ok "$id" "$desc"; else bad "$id" "$desc: check refused a program that runs (rc=$crc)" "$cout"; fi
    else
        if [ "$rrc" -eq 0 ]; then
            bad "$id" "the program ran, so it is not a refusal control (rc=0): UNMEASURABLE" "$rout"; UNMEAS=$((UNMEAS+1)); return
        fi
        if [ "$crc" -ne 0 ]; then ok "$id" "$desc"; else bad "$id" "$desc: check passed a program that fails (rc=0)" "$cout"; fi
    fi
}
prog() { printf '%s\n' "$2" > "$WORK/$1.naab"; }

echo "== programs that run check clean"
prog CK-00 'main {
    let x = 1 + 2
    print("SUM=" + string(x))
}'
check_arm CK-00 "CONTROL: a plain program" accept "SUM=3"
prog CK-01 'use mymod
main {
    print("M=" + string(mymod.f()))
}'
check_arm CK-01 "use mymod" accept "M=42"
prog CK-02 'use mymod as q
main {
    print("Q=" + string(q.f()))
}'
check_arm CK-02 "use mymod as q" accept "Q=42"
prog CK-03 'import "./mymod.naab" as mine
main {
    print("I=" + string(mine.f()))
}'
check_arm CK-03 "import FILE as alias" accept "I=42"
prog CK-04 'import {f as h} from "./mymod.naab"
main {
    print("H=" + string(h()))
}'
check_arm CK-04 "import {f as h} from FILE" accept "H=42"
prog CK-05 'import * as mm from "./mymod.naab"
main {
    print("W=" + string(mm.f()))
}'
check_arm CK-05 "import * as mm from FILE" accept "W=42"
prog CK-06 'use codegen
main {
    let langs = codegen.supported_languages()
    print("CG=" + string(len(langs) > 0))
}'
check_arm CK-06 "use codegen (a module the old list lacked)" accept "CG=true"
prog CK-07 'use regex
main {
    print("RX=" + string(regex.matches("ab", "a")))
}'
check_arm CK-07 "use regex" accept "RX="
prog CK-08 'main {
    let a = "text" + 3
    let b = 3 + "a"
    let c = [1] + "a"
    let d = "a" + 1.5
    print("CAT=" + a + b + c + d)
}'
check_arm CK-08 "+ with a string on either side" accept "CAT=text33a[1]aa1.5"
prog CK-08b 'main {
    let r = "ab" * 2
    let s = 2 * "c"
    print("REP=" + r + s)
}'
check_arm CK-08b "string * int and int * string" accept "REP=ababcc"
prog CK-09 'use pkg.inner
main {
    print("P=" + string(inner.g()))
}'
check_arm CK-09 "use a.b binds b" accept "P=7"

echo "== programs that fail still fail check"
prog CK-10 'main {
    print(nosuchvar + 1)
}'
check_arm CK-10 "CONTROL: an undefined variable" refuse
prog CK-11 'main {
    print(regex.matches("ab", "a"))
}'
check_arm CK-11 "a module the program never imported (regex without use)" refuse
prog CK-13 'main {
    let r = "a" - 1
    print(r)
}'
check_arm CK-13 "CONTROL: string - int" refuse
prog CK-14 'main {
    let r = "a" * 2.5
    print(r)
}'
# "a" * 2.5 fails on the tree-walker and, since A215, on the VM; the run
# oracle reads the VM, so on a build before A215 this arm is UNMEASURABLE.
check_arm CK-14 "CONTROL: string * float" refuse

echo
echo "check vs run: $PASS passed, $FAIL failed, $UNMEAS unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNMEAS" -eq 0 ]
