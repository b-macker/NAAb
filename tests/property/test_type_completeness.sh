#!/bin/bash
# Property-Based Test: Type Error Completeness Invariant
# PROPERTY: --strict-types catches type mismatches regardless of call path.
# Method: Generate programs with deliberate type errors invoked through
# different paths. ALL must be caught by --strict-types.

set -e

NAAB_BIN="${NAAB_BIN:-./build/naab-lang}"
TMPDIR="${TMPDIR:-$HOME/.naab_prop_tmp}"
mkdir -p "$TMPDIR"

PASSED=0
FAILED=0
TOTAL=0

# Check that --strict-types rejects a program (non-zero exit)
check_type_error_caught() {
    local name="$1"
    local program="$2"
    TOTAL=$((TOTAL + 1))

    local src="$TMPDIR/type_${name}.naab"
    echo "$program" > "$src"

    if timeout 10s "$NAAB_BIN" --strict-types --no-governance run "$src" > /dev/null 2>&1; then
        echo "  FAIL: $name — type error NOT caught"
        FAILED=$((FAILED + 1))
    else
        echo "  PASS: $name"
        PASSED=$((PASSED + 1))
    fi
}

# Check that valid code passes --strict-types (zero exit)
check_type_valid() {
    local name="$1"
    local program="$2"
    TOTAL=$((TOTAL + 1))

    local src="$TMPDIR/type_${name}.naab"
    echo "$program" > "$src"

    if timeout 10s "$NAAB_BIN" --strict-types --no-governance run "$src" > /dev/null 2>&1; then
        echo "  PASS: $name (valid code accepted)"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $name — valid code rejected by --strict-types"
        FAILED=$((FAILED + 1))
    fi
}

echo "--- Invariant 3: Type Error Completeness ---"
echo ""
echo "  Part A: Type errors caught regardless of call path"

# Direct call
check_type_error_caught "direct_call" 'fn add(a: int, b: int) -> int {
    return a + b
}
main {
    add("hello", "world")
}'

# Return type mismatch
check_type_error_caught "return_mismatch" 'fn get_number() -> int {
    return "not a number"
}
main {
    let x = get_number()
}'

# Variable type mismatch
check_type_error_caught "var_mismatch" 'main {
    let x: int = "hello"
}'

# Nested call
check_type_error_caught "nested_call" 'fn add(a: int, b: int) -> int {
    return a + b
}
fn wrapper() -> int {
    return add("x", "y")
}
main {
    wrapper()
}'

# Multiple parameter errors
check_type_error_caught "multi_param" 'fn process(name: string, age: int, active: bool) {
    print(name)
}
main {
    process(42, "not_int", "not_bool")
}'

# Wrong arg count (structural)
check_type_error_caught "wrong_arity" 'fn greet(name: string) {
    print("Hello " + name)
}
main {
    greet("a", "b", "c")
}'

echo ""
echo "  Part B: Valid code accepted by --strict-types"

# Correct types
check_type_valid "correct_types" 'fn add(a: int, b: int) -> int {
    return a + b
}
main {
    let x = add(1, 2)
    print(x)
}'

# Correct string ops
check_type_valid "correct_strings" 'fn greet(name: string) -> string {
    return "Hello, " + name
}
main {
    print(greet("world"))
}'

# Mixed correct usage
check_type_valid "correct_mixed" 'fn compute(x: int, label: string) -> string {
    return label + ": " + string(x * 2)
}
main {
    print(compute(21, "result"))
}'

echo ""
echo "Type Completeness: $PASSED/$TOTAL passed"
if [ $FAILED -eq 0 ]; then
    echo "PROPERTY HOLDS: Type errors caught regardless of call path"
else
    echo "PROPERTY VIOLATED! $FAILED test(s) missed type errors"
fi

# Cleanup
rm -rf "$TMPDIR"

[ $FAILED -eq 0 ]
