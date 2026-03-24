#!/bin/bash
# Differential Test: Polyglot Serialization/Deserialization
# PROPERTY: Running the same Python snippet through NAAb's executor and raw
# Python produces identical stdout. Any difference = serialization bug.
#
# Tests the full roundtrip:
#   NAAb value → serializeValueForLanguage() → Python code → execute → pyObjectToValue() → NAAb value
#
# Edge cases: floats, bools, nulls, special-char strings, nested collections

set -e

NAAB_BIN="${NAAB_BIN:-./build/naab-lang}"
TMPDIR="${TMPDIR:-$HOME/.naab_diff_tmp}"
mkdir -p "$TMPDIR"

PASSED=0
FAILED=0
TOTAL=0

# Check prerequisites
if ! command -v python3 &>/dev/null; then
    echo "--- Invariant 4: Polyglot Differential (SKIPPED — python3 not found) ---"
    exit 0
fi

# check_differential NAME NAAB_PROGRAM PYTHON_PROGRAM
check_differential() {
    local name="$1"
    local naab_program="$2"
    local python_program="$3"
    TOTAL=$((TOTAL + 1))

    local naab_src="$TMPDIR/diff_${name}.naab"
    local py_src="$TMPDIR/diff_${name}.py"

    echo "$naab_program" > "$naab_src"
    echo "$python_program" > "$py_src"

    local naab_out py_out
    naab_out=$(timeout 10s "$NAAB_BIN" --no-governance run "$naab_src" 2>/dev/null) || true
    py_out=$(timeout 10s python3 "$py_src" 2>/dev/null) || true

    if [ "$naab_out" = "$py_out" ]; then
        echo "  PASS: $name"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $name"
        echo "       NAAb:   $(echo "$naab_out" | head -1)"
        echo "       Python: $(echo "$py_out" | head -1)"
        FAILED=$((FAILED + 1))
    fi
}

echo "--- Invariant 4: Polyglot Differential (NAAb vs Python) ---"
echo ""
echo "  Part A: Scalar serialization"

# --- Integers ---
check_differential "int_zero" \
'main {
    let x = 0
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = 0
print(x)'

check_differential "int_positive" \
'main {
    let x = 42
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = 42
print(x)'

check_differential "int_negative" \
'main {
    let x = -1
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = -1
print(x)'

check_differential "int_max" \
'main {
    let x = 2147483647
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = 2147483647
print(x)'

check_differential "int_min" \
'main {
    let x = -2147483648
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = -2147483648
print(x)'

# --- Floats ---
check_differential "float_pi" \
'main {
    let x = 3.14159265358979
    let r = <<python[x]
print(repr(x))
>>
    print(r)
}' \
'x = 3.14159265358979
print(repr(x))'

check_differential "float_small" \
'main {
    let x = 0.000000001
    let r = <<python[x]
print(repr(x))
>>
    print(r)
}' \
'x = 0.000000001
print(repr(x))'

check_differential "float_large" \
'main {
    let x = 9999999.99999
    let r = <<python[x]
print(repr(x))
>>
    print(r)
}' \
'x = 9999999.99999
print(repr(x))'

check_differential "float_negative" \
'main {
    let x = -273.15
    let r = <<python[x]
print(repr(x))
>>
    print(r)
}' \
'x = -273.15
print(repr(x))'

# --- Booleans ---
check_differential "bool_true" \
'main {
    let x = true
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = True
print(x)'

check_differential "bool_false" \
'main {
    let x = false
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = False
print(x)'

# --- Strings ---
check_differential "string_simple" \
'main {
    let x = "hello world"
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = "hello world"
print(x)'

check_differential "string_empty" \
'main {
    let x = ""
    let r = <<python[x]
print(repr(x))
>>
    print(r)
}' \
'x = ""
print(repr(x))'

check_differential "string_with_quotes" \
'main {
    let x = "he said \"hi\""
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = "he said \"hi\""
print(x)'

check_differential "string_with_backslash" \
'main {
    let x = "path\\to\\file"
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = "path\\to\\file"
print(x)'

check_differential "string_with_newline" \
'main {
    let x = "line1\nline2"
    let r = <<python[x]
print(repr(x))
>>
    print(r)
}' \
'x = "line1\nline2"
print(repr(x))'

check_differential "string_with_tab" \
'main {
    let x = "col1\tcol2"
    let r = <<python[x]
print(repr(x))
>>
    print(r)
}' \
'x = "col1\tcol2"
print(repr(x))'

echo ""
echo "  Part B: Collection serialization"

# --- Lists ---
check_differential "list_empty" \
'main {
    let x = []
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = []
print(x)'

check_differential "list_ints" \
'main {
    let x = [1, 2, 3, 4, 5]
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = [1, 2, 3, 4, 5]
print(x)'

check_differential "list_nested" \
'main {
    let x = [[1, 2], [3, 4]]
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = [[1, 2], [3, 4]]
print(x)'

check_differential "list_mixed" \
'main {
    let x = [1, "hello", true, 3.14]
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
'x = [1, "hello", True, 3.14]
print(x)'

check_differential "list_strings" \
'main {
    let x = ["alpha", "beta", "gamma"]
    let r = <<python[x]
print(x)
>>
    print(r)
}' \
"x = [\"alpha\", \"beta\", \"gamma\"]
print(x)"

# --- Dicts ---
check_differential "dict_simple" \
'main {
    let x = {"a": 1, "b": 2}
    let r = <<python[x]
print(sorted(x.items()))
>>
    print(r)
}' \
'x = {"a": 1, "b": 2}
print(sorted(x.items()))'

check_differential "dict_nested" \
'main {
    let x = {"outer": {"inner": 42}}
    let r = <<python[x]
print(x["outer"]["inner"])
>>
    print(r)
}' \
'x = {"outer": {"inner": 42}}
print(x["outer"]["inner"])'

check_differential "dict_with_list" \
'main {
    let x = {"nums": [1, 2, 3]}
    let r = <<python[x]
print(x["nums"])
>>
    print(r)
}' \
'x = {"nums": [1, 2, 3]}
print(x["nums"])'

check_differential "list_of_dicts" \
'main {
    let x = [{"x": 1}, {"y": 2}]
    let r = <<python[x]
print(x[0]["x"], x[1]["y"])
>>
    print(r)
}' \
'x = [{"x": 1}, {"y": 2}]
print(x[0]["x"], x[1]["y"])'

echo ""
echo "  Part C: Roundtrip computation"

# --- Computation roundtrips ---
check_differential "compute_int" \
'main {
    let x = 42
    let r = <<python[x]
print(x * 2 + 1)
>>
    print(r)
}' \
'x = 42
print(x * 2 + 1)'

check_differential "compute_float" \
'main {
    let x = 0.1
    let r = <<python[x]
print(repr(x + 0.2))
>>
    print(r)
}' \
'x = 0.1
print(repr(x + 0.2))'

check_differential "compute_string" \
'main {
    let x = "hello world"
    let r = <<python[x]
print(x.upper())
>>
    print(r)
}' \
'x = "hello world"
print(x.upper())'

check_differential "compute_list_len" \
'main {
    let x = [10, 20, 30, 40, 50]
    let r = <<python[x]
print(len(x))
>>
    print(r)
}' \
'x = [10, 20, 30, 40, 50]
print(len(x))'

check_differential "compute_list_sum" \
'main {
    let x = [1, 2, 3, 4, 5]
    let r = <<python[x]
print(sum(x))
>>
    print(r)
}' \
'x = [1, 2, 3, 4, 5]
print(sum(x))'

check_differential "compute_dict_access" \
'main {
    let x = {"name": "NAAb", "version": 4}
    let r = <<python[x]
print(x["name"] + " v" + str(x["version"]))
>>
    print(r)
}' \
'x = {"name": "NAAb", "version": 4}
print(x["name"] + " v" + str(x["version"]))'

check_differential "compute_bool_logic" \
'main {
    let x = true
    let y = false
    let r = <<python[x, y]
print(x and not y)
print(x or y)
>>
    print(r)
}' \
'x = True
y = False
print(x and not y)
print(x or y)'

echo ""
echo "Polyglot Differential: $PASSED/$TOTAL passed"
if [ $FAILED -eq 0 ]; then
    echo "PROPERTY HOLDS: NAAb executor matches raw Python output"
else
    echo "PROPERTY VIOLATED! $FAILED test(s) show serialization divergence"
fi

# Cleanup
rm -rf "$TMPDIR"

[ $FAILED -eq 0 ]
