#!/bin/bash
# Property-Based Test: Governance Transparency Invariant
# PROPERTY: Governance NEVER alters stdout of correct, pure-computation code.
# Method: Generate programs, run with/without governance, diff stdout.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB_BIN="${NAAB_BIN:-$PROJECT_DIR/build/naab-lang}"
# Resolve to absolute path so sign_gov works from temp dirs
NAAB_BIN="$(cd "$(dirname "$NAAB_BIN")" && pwd)/$(basename "$NAAB_BIN")"
TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/naab_prop_XXXXXX")
mkdir -p "$TMPDIR/gov" "$TMPDIR/nogov"

# Signing helper (trusted keys require govern.json signatures)
SIGNING_KEY="${HOME}/.naab/keys/signing.pem"
sign_gov() {
    local dir="$1"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB_BIN" --sign-governance 2>/dev/null) || true
    fi
}

# "With governance" dir: enforce mode, allow all pure NAAb (no polyglot needed)
echo '{"mode":"enforce","security":{"sandbox_level":"elevated"}}' > "$TMPDIR/gov/govern.json"
sign_gov "$TMPDIR/gov"

# "Without governance" dir: mode off (governance loaded but inactive)
# Cannot use --no-governance because signed trust store blocks it.
echo '{"mode":"off"}' > "$TMPDIR/nogov/govern.json"
sign_gov "$TMPDIR/nogov"

PASSED=0
FAILED=0
TOTAL=0

check_transparency() {
    local name="$1"
    local program="$2"
    TOTAL=$((TOTAL + 1))

    # Write program to both dirs (each has its own govern.json)
    local src_gov="$TMPDIR/gov/prop_${name}.naab"
    local src_nogov="$TMPDIR/nogov/prop_${name}.naab"
    echo "$program" > "$src_gov"
    echo "$program" > "$src_nogov"

    local out_gov="$TMPDIR/out_gov_${name}.txt"
    local out_nogov="$TMPDIR/out_nogov_${name}.txt"

    # Run WITH governance (enforce mode)
    if ! timeout 10s "$NAAB_BIN" "$src_gov" > "$out_gov" 2>/dev/null; then
        echo "  FAIL: $name — governance run crashed/blocked"
        FAILED=$((FAILED + 1))
        return
    fi

    # Run WITHOUT governance (mode:off — signed trust store blocks --no-governance)
    if ! timeout 10s "$NAAB_BIN" "$src_nogov" > "$out_nogov" 2>/dev/null; then
        echo "  FAIL: $name — mode:off run crashed"
        FAILED=$((FAILED + 1))
        return
    fi

    # Diff stdout — governance must never alter pure-computation output
    if diff -q "$out_gov" "$out_nogov" > /dev/null 2>&1; then
        echo "  PASS: $name"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $name — output differs with/without governance"
        diff "$out_gov" "$out_nogov" | head -5 | sed 's/^/       /'
        FAILED=$((FAILED + 1))
    fi
}

echo "--- Invariant 1: Governance Transparency ---"

# Pure arithmetic
check_transparency "arithmetic_basic" 'main {
    let x = 42
    let y = x * 3 + 7
    let z = y - x
    print(x)
    print(y)
    print(z)
}'

check_transparency "arithmetic_float" 'main {
    let pi = 3.14159
    let r = 5.0
    let area = pi * r * r
    print(area)
}'

check_transparency "arithmetic_negative" 'main {
    let a = -10
    let b = a * -3
    let c = b + a
    print(a)
    print(b)
    print(c)
}'

# String operations
check_transparency "string_ops" 'main {
    let s = "hello"
    let t = s + " " + "world"
    print(t)
    print(t.length())
    print(t.upper())
    print(t.contains("world"))
}'

check_transparency "string_interpolation" 'main {
    let name = "NAAb"
    let version = 42
    let msg = "Language: ${name}, v${version}"
    print(msg)
}'

# Array operations
check_transparency "array_ops" 'main {
    let arr = [1, 2, 3, 4, 5]
    arr.push(6)
    print(arr.length())
    let sum = 0
    for x in arr {
        sum = sum + x
    }
    print(sum)
    print(arr[0])
    print(arr[5])
}'

check_transparency "array_slice" 'main {
    let arr = [10, 20, 30, 40, 50]
    let sub = arr[1:4]
    for x in sub {
        print(x)
    }
}'

# Match expressions
check_transparency "match_expr" 'fn classify(n: int) -> string {
    return match n {
        0 => "zero",
        1 => "one",
        2 => "two",
        _ => "many"
    }
}
main {
    print(classify(0))
    print(classify(1))
    print(classify(2))
    print(classify(99))
}'

# Closures
check_transparency "closures" 'fn make_adder(n: int) {
    return fn(x) { return x + n }
}
main {
    let add5 = make_adder(5)
    let add10 = make_adder(10)
    print(add5(3))
    print(add10(3))
    print(add5(add10(1)))
}'

# Generators
check_transparency "generators" 'fn range(n: int) {
    let i = 0
    while i < n {
        yield i
        i = i + 1
    }
}
main {
    let sum = 0
    for x in range(10) {
        sum = sum + x
    }
    print(sum)
}'

# Struct operations
check_transparency "structs" 'struct Point {
    x: int
    y: int
}
fn distance_sq(p: Point) -> int {
    return p.x * p.x + p.y * p.y
}
main {
    let p = new Point { x: 3, y: 4 }
    print(p.x)
    print(p.y)
    print(distance_sq(p))
}'

# Recursion
check_transparency "recursion" 'fn fib(n: int) -> int {
    if n <= 1 { return n }
    return fib(n - 1) + fib(n - 2)
}
main {
    let i = 0
    while i < 10 {
        print(fib(i))
        i = i + 1
    }
}'

# Dict operations
check_transparency "dict_ops" 'main {
    let d = {"a": 1, "b": 2, "c": 3}
    print(d["a"])
    print(d["b"])
    print(d.length())
    d["d"] = 4
    print(d.length())
}'

# Nested control flow
check_transparency "nested_control" 'main {
    let result = 0
    let i = 0
    while i < 5 {
        let j = 0
        while j < 5 {
            if i == j {
                result = result + i * j
            }
            j = j + 1
        }
        i = i + 1
    }
    print(result)
}'

# If-expressions
check_transparency "if_expr" 'main {
    let x = 10
    let label = if x > 5 { "big" } else { "small" }
    print(label)
    let y = if x > 100 { "huge" } else { if x > 0 { "positive" } else { "zero" } }
    print(y)
}'

# Enum
check_transparency "enum_ops" 'enum Color {
    Red,
    Green,
    Blue
}
main {
    let c = Color.Red
    print(c)
    let c2 = Color.Blue
    print(c2)
}'

# Exception handling (non-taint)
check_transparency "try_catch" 'fn might_fail(x: int) -> int {
    if x < 0 {
        throw "negative: ${x}"
    }
    return x * 2
}
main {
    let results = []
    let i = -2
    while i <= 3 {
        try {
            results.push(might_fail(i))
        } catch (e) {
            results.push(-1)
        }
        i = i + 1
    }
    for r in results {
        print(r)
    }
}'

# Shadowing
check_transparency "shadowing" 'main {
    let x = 1
    print(x)
    if true {
        let x = 2
        print(x)
        if true {
            let x = 3
            print(x)
        }
        print(x)
    }
    print(x)
}'

# Pipeline
check_transparency "null_coalesce" 'main {
    let a = "hello"
    let b = a ?? "default"
    print(b)
}'

echo ""
echo "Governance Transparency: $PASSED/$TOTAL passed"
if [ $FAILED -eq 0 ]; then
    echo "PROPERTY HOLDS: Governance never alters output of correct code"
else
    echo "PROPERTY VIOLATED! $FAILED test(s) showed different output"
fi

# Cleanup
rm -rf "$TMPDIR"

[ $FAILED -eq 0 ]
