#!/usr/bin/env bash
# test_pipeline_module.sh -- the pipeline operator feeds a module function (A67).
#
# `x |> mod.fn(a)` means `mod.fn(x, a)`, as `x |> f(a)` means `f(x, a)` for a
# user function (the owner's decision 5). On the VM the compiler compiled the
# callee `mod.fn` on its own, and a module member without parentheses CALLS
# the function with no arguments, so the README's own example
#   "hello world" |> string.upper() |> string.split(" ")
# failed with "upper() takes exactly 1 argument" (observed, 2658be11 and
# d32f8603). A member target is now compiled as the method call obj.fn(x, a),
# which goes through OP_CALL_METHOD -- so a piped call meets every gate a
# direct call meets (PM-08..PM-10).
#
# The tree-walker still refuses a member target ("Pipeline requires callable
# function"; PM-TW pins that it never runs with a wrong value). Its
# standard-library checks read each argument by its position in the source
# (call_dispatch.cpp), so prepending the piped value there would shift every
# taint check onto the wrong argument; the fix that keeps them aligned is a
# parse-time rewrite, recorded in A67. The tree-walker is refused under
# enforce (Bundle 2).
#
#   PM-00      CONTROL: user-function pipelines run (they always did).
#   PM-01..07  module targets give the value the direct call gives (PM-05, a
#              function held in a dict, already worked: a CONTROL).
#   PM-08      a piped file.read of a blocked path is a HARD refusal (exit 3),
#              like the direct call (PM-08c CONTROL, same config).
#   PM-09      CONTROL: a piped file.read of an allowed path returns its text.
#   PM-10      tainted data piped into a sink is refused, like the direct call
#              (a later argument tainted); PM-10b the piped value itself (a
#              tainted path piped into file.write).
#   PM-TW      the tree-walker refuses a member target (no wrong value).
#
# Kill table (mutants of src/vm/compiler.cpp, run by hand):
#   C1 the member branch removed (old path)          -> PM-01..04, PM-06..09
#   C2 the piped value not passed (argc - 1)          -> PM-01, PM-03, PM-09
#   C3 the piped value not taint-marked               -> PM-10b
#   C4 the piped value passed LAST, not first         -> PM-03, PM-07

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_pipemod.XXXXXX")"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -6 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

# val ID EXPR WANT [PRELUDE]: print EXPR on the VM, no govern.json.
val() {
    printf '%s\nmain {\n    let d = {"f": fn(x, y) { return x * y }}\n    print("V=" + string(%s))\n}\n' "${4:-}" "$2" > "$W/$1.naab"
    OUT=$(cd "$W" && "$NAAB" run "$1.naab" --no-governance 2>&1); RC=$?
    if [ "$RC" -eq 0 ] && grep -qxF "V=$3" <<<"$OUT"; then ok "$1" "$2 = $3"
    else bad "$1" "$2 should be $3 (rc=$RC)" "$OUT"; fi
}
UF='fn twice(s) { return s + s }
fn shout(s, e) { return s + e }'
val PM-00 '"ab" |> twice' 'abab' "$UF"
val PM-00b '"ab" |> shout("!")' 'ab!' "$UF"
val PM-01 '"hello world" |> string.upper() |> string.split(" ")' '[HELLO, WORLD]'
val PM-02 '"ab" |> string.upper' 'AB'
val PM-03 '"hi" |> string.replace("i", "o")' 'ho'
val PM-04 '[3, 1, 2] |> array.sorted()' '[1, 2, 3]'
val PM-05 '3 |> d.f(4)' '12'
val PM-06 '"a,b" |> string.split(",") |> array.length()' '2'
val PM-07 '"abc" |> string.substring(1, 3)' 'bc'

# Governance: a piped call meets the gates a direct call meets.
mkdir -p "$W/g/vault" "$W/g/pub"
printf 'SECRET-TEXT\n' > "$W/g/vault/s.txt"; printf 'PUBLIC-TEXT\n' > "$W/g/pub/p.txt"
printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"filesystem":{"mode":"read","blocked_paths":["vault/"]}}}\n' > "$W/g/govern.json"
grun() { printf 'main {\n%s\n}\n' "$2" > "$W/g/$1.naab"; OUT=$(cd "$W/g" && "$NAAB" run "$1.naab" 2>&1); RC=$?; }
grun PM-08c '    let t = file.read("vault/s.txt")
    print("READ=" + t)'
if [ "$RC" -eq 3 ] && ! grep -q 'SECRET-TEXT' <<<"$OUT"; then ok PM-08c "CONTROL: the direct read of the blocked path is refused (exit 3)"
else bad PM-08c "the direct blocked read was not refused (rc=$RC) -- PM-08 UNMEASURABLE" "$OUT"; fi
grun PM-08 '    let t = "vault/s.txt" |> file.read()
    print("READ=" + t)'
if [ "$RC" -eq 3 ] && ! grep -q 'SECRET-TEXT' <<<"$OUT" && grep -qi 'blocked\|filesystem' <<<"$OUT"; then
    ok PM-08 "a piped read of the blocked path is refused by the filesystem gate (exit 3)"
else bad PM-08 "the piped blocked read was not refused by governance (rc=$RC)" "$OUT"; fi
grun PM-09 '    let t = "pub/p.txt" |> file.read()
    print("READ=" + t)'
if [ "$RC" -eq 0 ] && grep -q 'READ=PUBLIC-TEXT' <<<"$OUT"; then ok PM-09 "CONTROL: a piped read of an allowed path returns its text"
else bad PM-09 "the piped allowed read did not run (rc=$RC)" "$OUT"; fi

mkdir -p "$W/t"
printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"taint_tracking":{"enabled":true,"level":"hard","sources":["env.get"],"sinks":["file.write"]}}\n' > "$W/t/govern.json"
trun() { printf 'main {\n%s\n}\n' "$2" > "$W/t/$1.naab"; OUT=$(cd "$W/t" && PM_TAINT_SRC=hello "$NAAB" run "$1.naab" 2>&1); RC=$?; }
trun PM-10c '    let v = env.get("PM_TAINT_SRC")
    file.write("out.txt", v)'
direct_rc=$RC
trun PM-10 '    let v = env.get("PM_TAINT_SRC")
    "out2.txt" |> file.write(v)'
if [ "$direct_rc" -eq 3 ] && [ "$RC" -eq 3 ] && [ ! -f "$W/t/out2.txt" ]; then
    ok PM-10 "tainted data piped into a sink is refused, like the direct call"
else bad PM-10 "direct rc=$direct_rc, piped rc=$RC (both should be 3; out2.txt must not exist)" "$OUT"; fi

# PM-10b: the PIPED value is the tainted one (PM-10 taints a later argument).
mkdir -p "$W/t2"
printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"taint_tracking":{"enabled":true,"level":"hard","sources":["env.get"],"sinks":["file.write"]}}\n' > "$W/t2/govern.json"
t2run() { printf 'main {\n%s\n}\n' "$2" > "$W/t2/$1.naab"; OUT=$(cd "$W/t2" && PM_TAINT_SRC=tpath.txt "$NAAB" run "$1.naab" 2>&1); RC=$?; }
t2run PM-10bc '    let v = env.get("PM_TAINT_SRC")
    file.write(v, "content")
    print("U=wrote")'
direct_rc=$RC; direct_out="$OUT"
t2run PM-10b '    let v = env.get("PM_TAINT_SRC")
    v |> file.write("content")
    print("U=wrote")'
if [ "$direct_rc" -eq 3 ] && [ "$RC" -eq 3 ] && ! grep -q '^U=' <<<"$OUT" && [ ! -f "$W/t2/tpath.txt" ]; then
    ok PM-10b "a tainted value piped into a sink is refused, like the direct call"
else bad PM-10b "direct rc=$direct_rc, piped rc=$RC (both should be 3, no U= line)" "$direct_out
$OUT"; fi

printf 'main {\n    print("V=" + ("ab" |> string.upper()))\n}\n' > "$W/tw.naab"
OUT=$(cd "$W" && "$NAAB" run tw.naab --no-governance --tree-walk 2>&1); RC=$?
if [ "$RC" -ne 0 ] && ! grep -q '^V=' <<<"$OUT"; then ok PM-TW "the tree-walker refuses a member target, with no wrong value (A67, open there)"
else bad PM-TW "the tree-walker ran a member target (rc=$RC): update A67 and this arm" "$OUT"; fi

echo
echo "pipeline into a module: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
