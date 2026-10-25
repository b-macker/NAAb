#!/usr/bin/env bash
# ============================================================
# test_default_params.sh — a parameter with a default (`fn f(x, y = 10)`)
# must get its default when the call leaves it out, on the VM as on the
# tree-walker. (docs/open-investigations.md A71)
#
# The VM never applied defaults. Compiler::compileFunctionBody() counted the
# parameters without one into `arity` and made every parameter a local, but
# compiled no default_value; VM::callFunction() then set the frame's slots
# over the arguments that WERE pushed, so a missing parameter's slot was
# whatever the stack held next -- usually the callee's first local. Observed
# before the fix (USER_GUIDE rewrite, 2026-10-25): `fn a(name = "World")`
# returned null from a(); `fn c(x, y = 10)` gave c(1) = 2; `fn d(n = 5) { let
# t = 100; return n }` returned 100. The tree-walker got all three right. The
# counting is in the VM's first commit (1dd08e83) and nothing ever filled
# CompiledFunction::default_chunks.
#
# The tree-walker's rules are the spec (call_dispatch.cpp, "Bind default
# values for missing arguments"): a default is evaluated at call time, in
# parameter order, in the callee's scope (so it may use earlier parameters
# and the variables a closure captured), and copied unless the parameter is a
# `ref` type. A null passed explicitly is an argument, not a missing one.
#
# Every arm asserts a hand-written expectation on BOTH engines. DP-C arms are
# controls: calls that pass every argument, and calls that must still fail.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/defparams-XXXXXX")"
trap '[ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"' EXIT

PASS_COUNT=0; FAIL_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

# run_both DIR -> VM, VRC, TW, TRC: each engine's KEY=value lines and exit code
run_both() {
    local out
    out=$(cd "$1" && "$NAAB" --no-governance t.naab 2>&1); VRC=$?
    VM=$(grep -E '^[A-Z0-9_]+=' <<<"$out")
    out=$(cd "$1" && "$NAAB" --no-governance --tree-walk t.naab 2>&1); TRC=$?
    TW=$(grep -E '^[A-Z0-9_]+=' <<<"$out")
}

# $1=id $2=description $3=expected output (KEY=value lines) $4=source
check() {
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    printf '%s\n' "$4" > "$d/t.naab"
    run_both "$d"
    if [ "$VM" = "$3" ] && [ "$TW" = "$3" ] && [ "$VRC" = 0 ] && [ "$TRC" = 0 ]; then
        pass "$1" "$2"
    else
        fail "$1" "$2" "expected '$3' | VM (rc=$VRC): '${VM:-<nothing>}' | tree-walk (rc=$TRC): '${TW:-<nothing>}'"
    fi
}

# $1=id $2=description $3=source -- both engines must refuse the call: a
# non-zero exit and no RESULT= line.
check_refused() {
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    printf '%s\n' "$3" > "$d/t.naab"
    run_both "$d"
    if [ "$VRC" != 0 ] && [ "$TRC" != 0 ] && ! grep -q '^RESULT=' <<<"$VM$TW"; then
        pass "$1" "$2"
    else
        fail "$1" "$2" "both should fail | VM (rc=$VRC): '${VM:-<nothing>}' | tree-walk (rc=$TRC): '${TW:-<nothing>}'"
    fi
}

echo ""
echo "=== default parameter values (VM vs tree-walker) ==="

check DP-01 "a string default is used when the argument is left out" "A=World" '
fn a(name = "World") {
    return name
}
main {
    print("A=" + a())
}'

check DP-02 "a default after a required parameter" "C=11
C2=3" '
fn c(x, y = 10) {
    return x + y
}
main {
    print("C=" + string(c(1)))
    print("C2=" + string(c(1, 2)))
}'

check DP-03 "the default, not the callee's next local, fills the slot" "D=5" '
fn d(n = 5) {
    let t = 100
    return n
}
main {
    print("D=" + string(d()))
}'

check DP-04 "several defaults, filled from the left" "M=6
M2=14
M3=111" '
fn m(a, b = 2, c = 3) {
    return a + b + c
}
main {
    print("M=" + string(m(1)))
    print("M2=" + string(m(1, 10)))
    print("M3=" + string(m(1, 10, 100)))
}'

check DP-05 "a default may use an earlier parameter" "R=9
R2=12" '
fn area(w, h = w) {
    return w * h
}
main {
    print("R=" + string(area(3)))
    print("R2=" + string(area(3, 4)))
}'

check DP-06 "a default is evaluated on every call" "L1=[1]
L2=[2]" '
fn collect(x, acc = []) {
    acc.push(x)
    return acc
}
main {
    print("L1=" + string(collect(1)))
    print("L2=" + string(collect(2)))
}'

check DP-07 "a lambda default may use a captured variable" "F=11" '
main {
    let base = 10
    let f = fn(x, y = base) { return x + y }
    print("F=" + string(f(1)))
}'

check DP-08 "a default is copied: changing it leaves the captured list alone" "S=[1]
X=[1, 2]" '
main {
    let shared = [1]
    let f = fn(xs = shared) {
        xs.push(2)
        return xs
    }
    let x = f()
    print("S=" + string(shared))
    print("X=" + string(x))
}'

check DP-09 "an explicit null is an argument, not a missing one" "N=null" '
fn a(name = "World") {
    return name
}
main {
    print("N=" + string(a(null)))
}'

check DP-10 "an async function gets its default" "Y=7" '
async fn later(x, y = 5) {
    return x + y
}
main {
    let v = await later(2)
    print("Y=" + string(v))
}'

check DP-11 "a generator gets its default" "G=0,1,2" '
fn upto(limit = 3) {
    let i = 0
    while i < limit {
        yield i
        i = i + 1
    }
}
main {
    let seen = []
    for n in upto() {
        seen.push(string(n))
    }
    print("G=" + seen.join(","))
}'

check DP-12 "a function passed as a value, called from native code, gets its default" "P=[11, 12]" '
use array
fn plus(x, step = 10) {
    return x + step
}
main {
    print("P=" + string(array.map_fn([1, 2], plus)))
}'

check DP-C1 "control: every argument passed, no default used" "Z=3" '
fn c(x, y = 10) {
    return x + y
}
main {
    print("Z=" + string(c(1, 2)))
}'

# The body only formats b, so a b left unset would print RESULT=null rather
# than fail on its own: the refusal has to come from the arity check.
check_refused DP-C2 "control: a required parameter after a defaulted one is still required" '
fn f(a = 1, b) {
    return "b=" + string(b)
}
main {
    print("RESULT=" + string(f(5)))
}'

check_refused DP-C3 "control: too many arguments is still an error" '
fn c(x, y = 10) {
    return x + y
}
main {
    print("RESULT=" + string(c(1, 2, 3)))
}'

check_refused DP-C4 "control: too few arguments for a required parameter is still an error" '
fn c(x, y = 10) {
    return x + y
}
main {
    print("RESULT=" + string(c()))
}'

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
[ "$FAIL_COUNT" -eq 0 ]
