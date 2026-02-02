# NAAb Debug Helpers & Development Tools

**Date:** 2026-01-26
**Purpose:** Comprehensive debugging utilities for NAAb development

---

## 1. Shell Block Debugging

### Helper: test_shell_debug.naab
Test shell block return values with detailed output:

```naab
use io

fn debug_shell_result(result: ShellResult, label: string) {
    io.write("\n=== ", label, " ===\n")
    io.write("  exit_code: ", result.exit_code)

    if result.exit_code == 0 {
        io.write(" ✓\n")
    } else {
        io.write(" ✗\n")
    }

    io.write("  stdout: '", result.stdout, "'\n")
    io.write("  stderr: '", result.stderr, "'\n")

    if result.exit_code != 0 && result.stderr != "" {
        io.write_error("  ERROR: ", result.stderr, "\n")
    }
}

main {
    // Test successful command
    let r1 = <<sh
echo "test output"
    >>
    debug_shell_result(r1, "Test 1: Simple echo")

    // Test command with stderr
    let r2 = <<sh
echo "error" >&2
    >>
    debug_shell_result(r2, "Test 2: Stderr output")

    // Test failing command
    let r3 = <<sh
exit 1
    >>
    debug_shell_result(r3, "Test 3: Non-zero exit")
}
```

---

## 2. Type Conversion Debugging

### Common Patterns

```naab
// ✅ CORRECT: Convert any type to string
let int_val = 42
let str_val = json.stringify(int_val)  // "42"

let bool_val = true
let bool_str = json.stringify(bool_val)  // "true"

let float_val = 3.14
let float_str = json.stringify(float_val)  // "3.14"

// ❌ WRONG: string.to_string doesn't exist
// let bad = string.to_string(42)  // ERROR!
```

### Debug Helper for Type Conversions

```naab
use io
use json

fn debug_type_conversion(value: any, type_name: string) {
    io.write("\nConverting ", type_name, ": ")
    let converted = json.stringify(value)
    io.write(type_name, " -> string: '", converted, "'\n")
}

main {
    debug_type_conversion(42, "int")
    debug_type_conversion(3.14, "float")
    debug_type_conversion(true, "bool")
    debug_type_conversion("hello", "string")
}
```

---

## 3. Module Alias Checker

### Helper: check_module_aliases.sh
Verify all module aliases are used consistently:

```bash
#!/bin/bash
# check_module_aliases.sh - Find inconsistent module alias usage

echo "=== Checking Module Alias Consistency ===\n"

# Check for 'use X as Y' followed by 'X.' usage
for file in *.naab; do
    if [ -f "$file" ]; then
        # Extract alias mappings
        aliases=$(grep "^use .* as " "$file" | sed 's/use \(.*\) as \(.*\)/\1:\2/')

        for mapping in $aliases; do
            module=$(echo "$mapping" | cut -d: -f1)
            alias=$(echo "$mapping" | cut -d: -f2)

            # Check if code uses full module name instead of alias
            wrong_usage=$(grep -n "${module}\." "$file" | grep -v "^[[:space:]]*\/\/" | head -5)

            if [ -n "$wrong_usage" ]; then
                echo "❌ $file: Uses '$module.' but should use '$alias.'"
                echo "$wrong_usage"
                echo ""
            fi
        done
    fi
done

echo "✅ Check complete"
```

---

## 4. Struct Serialization Testing

### Helper: test_struct_json.naab
Verify struct serialization works correctly:

```naab
use io
use json

struct TestPerson {
    name: string,
    age: int,
    active: bool
}

struct Nested {
    id: int,
    person: TestPerson
}

fn test_struct_serialization() {
    io.write("=== Struct Serialization Tests ===\n\n")

    // Test 1: Simple struct
    let p1 = new TestPerson {
        name: "Alice",
        age: 30,
        active: true
    }

    let json1 = json.stringify(p1)
    io.write("Simple struct: ", json1, "\n")

    // Test 2: List of structs
    let people = [p1]
    let json2 = json.stringify(people)
    io.write("List of structs: ", json2, "\n")

    // Test 3: Nested structs
    let nested = new Nested {
        id: 1,
        person: p1
    }
    let json3 = json.stringify(nested)
    io.write("Nested struct: ", json3, "\n")
}

main {
    test_struct_serialization()
}
```

---

## 5. Variable Binding Validator

### Helper: test_variable_binding.naab
Test variable binding in inline code blocks:

```naab
use io
use json

fn test_python_binding() {
    io.write("=== Python Variable Binding ===\n")

    let test_int = 42
    let test_str = "hello"
    let test_bool = true

    let result = <<python[test_int, test_str, test_bool]
import json

data = {
    "int_received": test_int,
    "str_received": test_str,
    "bool_received": test_bool,
    "types": {
        "int": str(type(test_int)),
        "str": str(type(test_str)),
        "bool": str(type(test_bool))
    }
}

_ = json.dumps(data)
    >>

    io.write("Result: ", result, "\n\n")
}

fn test_shell_binding() {
    io.write("=== Shell Variable Binding ===\n")

    let filename = "test.txt"
    let content = "Hello World"

    let result = <<sh[filename, content]
echo "$content" > "$filename" && cat "$filename"
    >>

    io.write("exit_code: ", result.exit_code, "\n")
    io.write("output: ", result.stdout, "\n\n")
}

main {
    test_python_binding()
    test_shell_binding()
}
```

---

## 6. Error Detection Patterns

### Common Errors & Debug Commands

```bash
# Find NAAB_VAR_ usage (old syntax)
grep -r "NAAB_VAR_" --include="*.naab" .

# Find string.to_string (non-existent function)
grep -r "string\.to_string\|str\.to_string" --include="*.naab" .

# Find 3-argument str.concat calls
grep -r "str\.concat.*,.*,.*)" --include="*.naab" .

# Find module alias mismatches
for file in *.naab; do
    echo "=== $file ==="
    # Extract: use string as str
    grep "use string as" "$file"
    # Find: string. usage (should be str.)
    grep "string\." "$file" | grep -v "^[[:space:]]*use "
done
```

---

## 7. Performance Profiling

### Helper: profile_execution.naab
Measure execution time of code sections:

```naab
use io
use time

fn profile_section(label: string, start_time: float) -> float {
    let end_time = time.now()
    let duration = end_time - start_time
    io.write("[PROFILE] ", label, ": ", duration, " seconds\n")
    return end_time
}

main {
    let t0 = time.now()

    // Section 1
    let t1 = profile_section("Initialization", t0)

    // Do work...
    let result = <<sh
sleep 0.1
    >>

    let t2 = profile_section("Shell execution", t1)

    // More work...
    let data = json.stringify({"test": "value"})

    let t3 = profile_section("JSON serialization", t2)

    profile_section("Total", t0)
}
```

---

## 8. Memory Leak Detection

### Helper: test_memory_leaks.naab
Test for memory leaks in struct creation:

```naab
use io

struct TestStruct {
    data: string,
    value: int
}

main {
    io.write("=== Memory Leak Test ===\n")
    io.write("Creating 10,000 structs in loop...\n")

    let count = 0
    while count < 10000 {
        let temp = new TestStruct {
            data: "test data",
            value: count
        }
        count = count + 1

        if count % 1000 == 0 {
            io.write("Progress: ", count, "\n")
        }
    }

    io.write("✓ Completed without crash\n")
    io.write("(Monitor memory usage externally with 'top' or 'ps')\n")
}
```

---

## 9. Integration Test Runner

### Helper: run_all_tests.sh
Run all test files and report results:

```bash
#!/bin/bash
# run_all_tests.sh - Run all *.naab test files

NAAB_BIN="./build/naab-lang"
PASS=0
FAIL=0

echo "=== NAAb Test Runner ===\n"

for test_file in test_*.naab; do
    if [ -f "$test_file" ]; then
        echo "Running: $test_file"

        if $NAAB_BIN run "$test_file" > /dev/null 2>&1; then
            echo "  ✓ PASS"
            PASS=$((PASS + 1))
        else
            echo "  ✗ FAIL"
            FAIL=$((FAIL + 1))
            $NAAB_BIN run "$test_file" 2>&1 | tail -10
        fi
        echo ""
    fi
done

echo "=== Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"

if [ $FAIL -eq 0 ]; then
    echo "✅ All tests passed!"
    exit 0
else
    echo "❌ Some tests failed"
    exit 1
fi
```

---

## 10. Debugging Checklist

When encountering issues, check:

### Syntax Issues
- [ ] Are semicolons optional everywhere?
- [ ] Are struct literals using commas or newlines?
- [ ] Are type names lowercase?

### Module Issues
- [ ] Does `use X as Y` match usage (`Y.function` not `X.function`)?
- [ ] Are all stdlib modules imported?
- [ ] Are custom modules in the same directory or correct path?

### Type Conversion
- [ ] Using `json.stringify()` not `str.to_string()`?
- [ ] Using 2-argument `str.concat()` not 3+?
- [ ] Chaining concatenations if needed?

### Inline Code Blocks
- [ ] Python blocks use direct variable names (not `NAAB_VAR_`)?
- [ ] Shell blocks use `<<sh[var1, var2]` binding syntax?
- [ ] Shell variables quoted: `"$varname"`?
- [ ] Python blocks return value assigned to underscore?

### Shell Block Returns
- [ ] Accessing `result.exit_code`, `result.stdout`, `result.stderr`?
- [ ] Checking `exit_code == 0` for success?
- [ ] Handling stderr output appropriately?

### Struct Serialization
- [ ] Using `json.stringify()` on structs?
- [ ] Struct fields match expected JSON keys?
- [ ] Nested structs serialize correctly?

---

## 11. CI/CD Integration

### GitHub Actions Example

```yaml
name: NAAb Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest

    steps:
    - uses: actions/checkout@v2

    - name: Build NAAb
      run: |
        mkdir build && cd build
        cmake ..
        make -j$(nproc)

    - name: Run Tests
      run: |
        cd tests
        ../build/naab-lang run test_struct_serialization.naab
        ../build/naab-lang run test_nested_generics.naab
        ../build/naab-lang run test_shell_return.naab

    - name: Check for Common Issues
      run: |
        # No NAAB_VAR_ usage
        ! grep -r "NAAB_VAR_" --include="*.naab" examples/
        # No str.to_string usage
        ! grep -r "str\.to_string" --include="*.naab" examples/
```

---

## 12. Quick Reference Card

```
┌─────────────────────────────────────────────────────┐
│ NAAb Quick Debug Reference                          │
├─────────────────────────────────────────────────────┤
│ Type Conversion:                                    │
│   ✓ json.stringify(value)                          │
│   ✗ str.to_string(value)  [DOESN'T EXIST]         │
│                                                     │
│ String Concatenation:                               │
│   ✓ str.concat(a, b)  [2 args only]                │
│   ✓ str.concat(str.concat(a, b), c)  [chain]       │
│   ✗ str.concat(a, b, c)  [TOO MANY ARGS]          │
│                                                     │
│ Module Aliases:                                     │
│   use string as str  → use str.concat()            │
│   use io as io  → use io.write()                   │
│                                                     │
│ Shell Blocks:                                       │
│   let r = <<sh[var1, var2]                         │
│   echo "$var1" > "$var2"                           │
│   >>                                                │
│   // Access: r.exit_code, r.stdout, r.stderr       │
│                                                     │
│ Python Blocks:                                      │
│   let r = <<python[x, y]                           │
│   result = x + y  # Direct names                   │
│   _ = str(result)  # Return via _                  │
│   >>                                                │
└─────────────────────────────────────────────────────┘
```

---

## Usage

1. Copy helpers to your project directory
2. Run `chmod +x *.sh` for shell scripts
3. Execute: `./run_all_tests.sh`
4. Check output for failures
5. Use individual helpers for targeted debugging

---

## Contributing

Add new debug helpers by creating test_*.naab files following these patterns:
- Clear test names
- Detailed output
- Both positive and negative test cases
- Error messages for failures
- Summary at end
