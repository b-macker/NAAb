#!/usr/bin/env bash
# ============================================================
# test_value_equality.sh — `==` and `!=` compare lists, dictionaries and
# structs by value, on both engines (docs/open-investigations.md A72)
#
# THE FAILURE THIS CATCHES
#
# Both engines' equality handled null, bool, numbers and strings and called
# everything else unequal -- the VM's OP_EQ/OP_NE in vm.cpp, the tree-walker's
# BinaryOp::Eq/Ne in expressions.cpp ("Different types: not equal", reached by
# two lists as well). So `a == a` was false for any array, `[] == []` was
# false, and `x == []` could never be true. Observed before the fix (USER_GUIDE
# rewrite, 2026-10-25), both engines. Nothing in the history chose it, and the
# two NAAb programs in the tree that compare a container (`empty_keywords ==
# []`, `e_attrs == {}` in tests/gorilla) were written expecting it to work.
# NAAb copies arrays and dictionaries on assignment (value semantics), so
# value equality is the rule that fits.
#
# The rules, one helper for both engines and both operators
# (naab::interpreter::valuesEqual): scalars exactly as before (5 == 5.0, no
# coercion between types); lists element by element; dictionaries key by key;
# structs by type and fields; anything else unequal.
#
#   EQ-01  an array equals itself and an equal array; != is its inverse
#   EQ-02  [] == [] and {} == {}
#   EQ-03  element rules are the scalar rules: [1] == [1.0], [1] != ["1"]
#   EQ-04  order matters in a list, not in a dictionary
#   EQ-05  nested lists and dictionaries compare all the way down
#   EQ-06  structs: same type and fields equal; a changed field does not
#   EQ-07  a copy compares equal until one side changes
#   EQ-C1  CONTROL: scalars keep their old answers (5 == 5.0, "1" != 1,
#          null == null, true != 1)
#   EQ-C2  CONTROL: different lengths, keys and types are unequal
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/valeq-XXXXXX")"
trap '[ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"' EXIT

PASS_COUNT=0; FAIL_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

# $1=id $2=description $3=expected output (one R=... line) $4=source
check() {
    local d="$TEST_TMP/$1" out vm tw vrc trc
    mkdir -p "$d"
    printf '%s\n' "$4" > "$d/t.naab"
    out=$(cd "$d" && "$NAAB" --no-governance t.naab 2>&1); vrc=$?
    vm=$(grep -E '^R=' <<<"$out")
    out=$(cd "$d" && "$NAAB" --no-governance --tree-walk t.naab 2>&1); trc=$?
    tw=$(grep -E '^R=' <<<"$out")
    if [ "$vm" = "$3" ] && [ "$tw" = "$3" ] && [ "$vrc" = 0 ] && [ "$trc" = 0 ]; then
        pass "$1" "$2"
    else
        fail "$1" "$2" "expected '$3' | VM (rc=$vrc): '${vm:-<nothing>}' | tree-walk (rc=$trc): '${tw:-<nothing>}'"
    fi
}

echo ""
echo "=== == and != on lists, dictionaries and structs (VM vs tree-walker) ==="

check EQ-01 "an array equals itself and an equal array; != is its inverse" \
    "R=true,true,false,false" '
main {
    let a = [1, 2]
    print("R=" + string(a == a) + "," + string(a == [1, 2]) + "," + string(a != a) + "," + string(a != [1, 2]))
}'

check EQ-02 "empty list and empty dictionary" "R=true,true" '
main {
    print("R=" + string([] == []) + "," + string({} == {}))
}'

check EQ-03 "elements follow the scalar rules" "R=true,false,true" '
main {
    print("R=" + string([1] == [1.0]) + "," + string([1] == ["1"]) + "," + string([null] == [null]))
}'

check EQ-04 "order matters in a list, not in a dictionary" "R=false,true" '
main {
    print("R=" + string([1, 2] == [2, 1]) + "," + string({"a": 1, "b": 2} == {"b": 2, "a": 1}))
}'

check EQ-05 "nested values compare all the way down" "R=true,false" '
main {
    let x = {"k": [1, {"z": "deep"}]}
    let y = {"k": [1, {"z": "deep"}]}
    let z = {"k": [1, {"z": "other"}]}
    print("R=" + string(x == y) + "," + string(x == z))
}'

check EQ-06 "structs compare by type and fields" "R=true,false" '
struct Point {
    x: int
    y: int
}
main {
    let p = new Point { x: 1, y: 2 }
    let q = new Point { x: 1, y: 2 }
    let r = new Point { x: 1, y: 3 }
    print("R=" + string(p == q) + "," + string(p == r))
}'

check EQ-07 "a copy compares equal until one side changes" "R=true,false" '
main {
    let a = [1, 2]
    let b = a
    let before = a == b
    b.push(3)
    print("R=" + string(before) + "," + string(a == b))
}'

check EQ-C1 "control: scalars keep their old answers" "R=true,false,true,false,true" '
main {
    print("R=" + string(5 == 5.0) + "," + string("1" == 1) + "," + string(null == null) + "," + string(true == 1) + "," + string("ab" == "ab"))
}'

check EQ-C2 "control: different lengths, keys and types are unequal" "R=false,false,false,false" '
main {
    print("R=" + string([1] == [1, 2]) + "," + string({"a": 1} == {"b": 1}) + "," + string([1] == {"0": 1}) + "," + string([] == null))
}'

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
[ "$FAIL_COUNT" -eq 0 ]
