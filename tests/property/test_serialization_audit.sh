#!/bin/bash
# Serialization Boundary Audit
# Systematically tests every data type and edge case that crosses:
#   NAAb value → serializeValueForLanguage() → Python subprocess → pyObjectToValue() → NAAb value
#
# Categories:
#   A. Supported types (should roundtrip correctly)
#   B. String edge cases (unicode, null bytes, binary, long strings)
#   C. Collection depth limits (nesting 10+ levels)
#   D. Struct serialization
#   E. Unsupported/degenerate types (functions, generators, futures)
#   F. Silent breakage detection

set -e

NAAB_BIN="${NAAB_BIN:-./build/naab-lang}"
TMPDIR="${TMPDIR:-$HOME/.naab_audit_tmp}"
mkdir -p "$TMPDIR"

PASSED=0
FAILED=0
KNOWN_LIMITS=0
TOTAL=0

if ! command -v python3 &>/dev/null; then
    echo "--- Serialization Audit (SKIPPED — python3 not found) ---"
    exit 0
fi

# check_roundtrip: NAAb creates value, sends to Python, Python echoes back, NAAb prints
# Verifies the full serialize→deserialize roundtrip
check_roundtrip() {
    local name="$1"
    local naab_program="$2"
    local expected="$3"
    TOTAL=$((TOTAL + 1))

    local src="$TMPDIR/audit_${name}.naab"
    echo "$naab_program" > "$src"

    local actual
    actual=$(timeout 10s "$NAAB_BIN" --no-governance run "$src" 2>/dev/null) || true

    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $name"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $name"
        echo "       Expected: $(echo "$expected" | head -1 | cut -c1-80)"
        echo "       Got:      $(echo "$actual" | head -1 | cut -c1-80)"
        FAILED=$((FAILED + 1))
    fi
}

# check_no_crash: NAAb program should not crash (exit 0), output doesn't matter
check_no_crash() {
    local name="$1"
    local naab_program="$2"
    TOTAL=$((TOTAL + 1))

    local src="$TMPDIR/audit_${name}.naab"
    echo "$naab_program" > "$src"

    if timeout 10s "$NAAB_BIN" --no-governance run "$src" >/dev/null 2>&1; then
        echo "  PASS: $name (no crash)"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $name (crashed or timed out)"
        FAILED=$((FAILED + 1))
    fi
}

# check_known_limit: Expected to fail — documents a known limitation
check_known_limit() {
    local name="$1"
    local naab_program="$2"
    local description="$3"
    TOTAL=$((TOTAL + 1))

    local src="$TMPDIR/audit_${name}.naab"
    echo "$naab_program" > "$src"

    local actual
    actual=$(timeout 10s "$NAAB_BIN" --no-governance run "$src" 2>&1) || true

    echo "  KNOWN: $name — $description"
    KNOWN_LIMITS=$((KNOWN_LIMITS + 1))
}

echo "--- Serialization Boundary Audit ---"
echo ""
echo "  Part A: Core type roundtrips (NAAb → Python → NAAb)"

# Integer roundtrips
check_roundtrip "int_zero_rt" \
'main {
    let x = 0
    let r = <<python[x]
x
>>
    print(r)
}' "0"

check_roundtrip "int_max_rt" \
'main {
    let x = 2147483647
    let r = <<python[x]
x
>>
    print(r)
}' "2147483647"

check_roundtrip "int_min_rt" \
'main {
    let x = -2147483648
    let r = <<python[x]
x
>>
    print(r)
}' "-2147483648"

# Float roundtrips
check_roundtrip "float_pi_rt" \
'main {
    let x = 3.14159265358979
    let r = <<python[x]
x
>>
    print(r)
}' "3.14159265358979"

check_roundtrip "float_zero_rt" \
'main {
    let x = 0.0
    let r = <<python[x]
x
>>
    print(r)
}' "0"

# Bool roundtrips
check_roundtrip "bool_true_rt" \
'main {
    let x = true
    let r = <<python[x]
x
>>
    print(r)
}' "true"

check_roundtrip "bool_false_rt" \
'main {
    let x = false
    let r = <<python[x]
x
>>
    print(r)
}' "false"

# String roundtrips
check_roundtrip "string_simple_rt" \
'main {
    let x = "hello"
    let r = <<python[x]
x
>>
    print(r)
}' "hello"

# Null roundtrip
check_roundtrip "null_rt" \
'main {
    let r = <<python
None
>>
    print(r)
}' "null"

# List roundtrip
check_roundtrip "list_rt" \
'main {
    let x = [1, 2, 3]
    let r = <<python[x]
x
>>
    print(r)
}' "[1, 2, 3]"

# Dict roundtrip — use sorted keys for determinism
check_roundtrip "dict_rt" \
'main {
    let x = {"a": 1}
    let r = <<python[x]
x
>>
    print(r["a"])
}' "1"

echo ""
echo "  Part B: String edge cases"

# Unicode
check_roundtrip "unicode_emoji" \
'main {
    let x = "hello 🌍"
    let r = <<python[x]
x
>>
    print(r)
}' "hello 🌍"

check_roundtrip "unicode_cjk" \
'main {
    let x = "日本語テスト"
    let r = <<python[x]
x
>>
    print(r)
}' "日本語テスト"

check_roundtrip "unicode_arabic" \
'main {
    let x = "مرحبا"
    let r = <<python[x]
x
>>
    print(r)
}' "مرحبا"

# Special characters in strings
check_roundtrip "string_quotes" \
'main {
    let x = "he said \"hi\""
    let r = <<python[x]
x
>>
    print(r)
}' 'he said "hi"'

check_roundtrip "string_backslash" \
'main {
    let x = "path\\to\\file"
    let r = <<python[x]
x
>>
    print(r)
}' 'path\to\file'

check_roundtrip "string_newline" \
'main {
    let x = "line1\nline2"
    let r = <<python[x]
len(x)
>>
    print(r)
}' "11"

check_roundtrip "string_tab" \
'main {
    let x = "a\tb"
    let r = <<python[x]
len(x)
>>
    print(r)
}' "3"

# Null byte in string — this is a critical edge case
# C strings terminate at \0, but NAAb std::string can hold it
check_known_limit "string_null_byte" \
'main {
    let x = "hello"
    let r = <<python[x]
x + chr(0) + "world"
>>
    print(r)
}' "Null bytes in strings may be truncated at C/Python boundary"

# Very long string
check_no_crash "string_long_1mb" \
'main {
    let x = "a"
    let i = 0
    while i < 20 {
        x = x + x
        i = i + 1
    }
    let r = <<python[x]
len(x)
>>
    print(r)
}'

# Empty string
check_roundtrip "string_empty_rt" \
'main {
    let x = ""
    let r = <<python[x]
repr(x)
>>
    print(r)
}' "''"

# String with only whitespace
check_roundtrip "string_whitespace" \
'main {
    let x = "   "
    let r = <<python[x]
len(x)
>>
    print(r)
}' "3"

echo ""
echo "  Part C: Collection depth and size limits"

# Nested list 5 levels
check_roundtrip "nested_list_5" \
'main {
    let x = [[[[[42]]]]]
    let r = <<python[x]
x[0][0][0][0][0]
>>
    print(r)
}' "42"

# Nested dict 5 levels
check_roundtrip "nested_dict_5" \
'main {
    let d1 = {"v": 42}
    let d2 = {"c": d1}
    let d3 = {"b": d2}
    let d4 = {"a": d3}
    let r = <<python[d4]
d4["a"]["b"]["c"]["v"]
>>
    print(r)
}' "42"

# 10-level nesting via list construction
check_no_crash "nested_10_levels" \
'main {
    let x = [99]
    let i = 0
    while i < 10 {
        x = [x]
        i = i + 1
    }
    let r = <<python[x]
type(x).__name__
>>
    print(r)
}'

# Large list (1000 elements)
check_no_crash "large_list_1000" \
'main {
    let x = []
    let i = 0
    while i < 1000 {
        x.push(i)
        i = i + 1
    }
    let r = <<python[x]
len(x)
>>
    print(r)
}'

# Large dict (500 entries)
check_no_crash "large_dict_500" \
'main {
    let x = {}
    let i = 0
    while i < 500 {
        x["key_" + string(i)] = i
        i = i + 1
    }
    let r = <<python[x]
len(x)
>>
    print(r)
}'

# Mixed nesting: dict with lists with dicts
check_roundtrip "mixed_nesting" \
'main {
    let x = {"users": [{"name": "alice", "scores": [90, 85]}, {"name": "bob", "scores": [75, 88]}]}
    let r = <<python[x]
x["users"][0]["scores"][1]
>>
    print(r)
}' "85"

echo ""
echo "  Part D: Struct serialization"

# Struct → Python dict
check_roundtrip "struct_to_python" \
'struct Point {
    x: int
    y: int
}
main {
    let p = new Point { x: 3, y: 4 }
    let r = <<python[p]
p["x"] ** 2 + p["y"] ** 2
>>
    print(r)
}' "25"

# Nested struct
check_roundtrip "nested_struct" \
'struct Inner { val: int }
struct Outer { name: string, inner: Inner }
main {
    let i = new Inner { val: 42 }
    let o = new Outer { name: "test", inner: i }
    let r = <<python[o]
o["inner"]["val"]
>>
    print(r)
}' "42"

echo ""
echo "  Part E: Unsupported type behavior"

# Function value — serializeValueForLanguage returns "null" for unsupported types
check_known_limit "function_binding" \
'fn add(a, b) { return a + b }
main {
    let f = add
    let r = <<python[f]
print(type(f).__name__)
>>
    print(r)
}' "Functions serialize as null (no callable marshaling)"

# Generator value
check_known_limit "generator_binding" \
'fn gen() {
    yield 1
    yield 2
}
main {
    let g = gen()
    let r = <<python[g]
print(type(g).__name__)
>>
    print(r)
}' "Generators serialize as null (no iterator marshaling)"

echo ""
echo "  Part F: Dict key edge cases"

# Dict with numeric-like keys
check_roundtrip "dict_numeric_key" \
'main {
    let x = {"123": "numeric"}
    let r = <<python[x]
x["123"]
>>
    print(r)
}' "numeric"

# Dict with empty key
check_roundtrip "dict_empty_key" \
'main {
    let x = {"": "empty"}
    let r = <<python[x]
x[""]
>>
    print(r)
}' "empty"

# Dict with key containing quotes
check_roundtrip "dict_quote_key" \
'main {
    let x = {}
    x["key\"with\"quotes"] = 42
    let r = <<python[x]
x["key\"with\"quotes"]
>>
    print(r)
}' "42"

# Dict with key containing backslash
check_roundtrip "dict_backslash_key" \
'main {
    let x = {}
    x["path\\to"] = "file"
    let r = <<python[x]
x["path\\to"]
>>
    print(r)
}' "file"

# Dict with key containing newline
check_roundtrip "dict_newline_key" \
'main {
    let x = {}
    x["line1\nline2"] = "nl"
    let r = <<python[x]
list(x.keys())[0].count(chr(10))
>>
    print(r)
}' "1"

echo ""
echo "  Part G: Python→NAAb type coercion"

# Python int too large for int32 → double
check_roundtrip "python_large_int" \
'main {
    let r = <<python
2 ** 40
>>
    print(r)
}' "1099511627776"

# Python tuple → NAAb list
check_roundtrip "python_tuple" \
'main {
    let r = <<python
(1, 2, 3)
>>
    print(r)
}' "[1, 2, 3]"

# Python nested tuple
check_roundtrip "python_nested_tuple" \
'main {
    let r = <<python
((1, 2), (3, 4))
>>
    print(r)
}' "[[1, 2], [3, 4]]"

# Python set — not directly supported (dict keys must be strings)
check_known_limit "python_set" \
'main {
    let r = <<python
{1, 2, 3}
>>
    print(r)
}' "Python sets have no NAAb equivalent — converted to string repr"

# Python bytes
check_known_limit "python_bytes" \
'main {
    let r = <<python
b"hello"
>>
    print(r)
}' "Python bytes have no NAAb equivalent — converted to string repr"

# Python complex number
check_known_limit "python_complex" \
'main {
    let r = <<python
complex(3, 4)
>>
    print(r)
}' "Python complex has no NAAb equivalent — wrapped in PythonObjectValue"

echo ""
echo "  Part H: Boundary stress tests"

# Multiple bindings at once
check_roundtrip "multi_bind" \
'main {
    let a = 1
    let b = "two"
    let c = [3, 4]
    let d = {"five": 5}
    let r = <<python[a, b, c, d]
str(a) + b + str(len(c)) + str(d["five"])
>>
    print(r)
}' "1two25"

# Binding same variable name that shadows Python builtin
check_roundtrip "shadow_builtin" \
'main {
    let len = 42
    let r = <<python[len]
len
>>
    print(r)
}' "42"

# Value modified in Python doesn't affect NAAb (copy semantics)
check_roundtrip "copy_semantics" \
'main {
    let x = [1, 2, 3]
    let r = <<python[x]
x.append(4)
x
>>
    print(x)
}' "[1, 2, 3]"

echo ""

# Summary
ACCOUNTED=$((PASSED + KNOWN_LIMITS))
echo "Serialization Audit: $PASSED passed, $KNOWN_LIMITS known limits, $FAILED failures (of $TOTAL total)"
if [ $FAILED -eq 0 ]; then
    echo "AUDIT COMPLETE: All supported types roundtrip correctly"
else
    echo "AUDIT FOUND ISSUES: $FAILED unexpected failure(s)"
fi

# Cleanup
rm -rf "$TMPDIR"

[ $FAILED -eq 0 ]
