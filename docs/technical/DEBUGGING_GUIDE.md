# NAAb Debugging and Development Tools

## Overview

This comprehensive guide provides both core debugging techniques and a suite of development helpers for NAAb programs. It covers interactive debugging, enhanced error messages, common issues, and specialized tools for various NAAb components like shell blocks, type conversions, and performance profiling.

---

## Core Debugging Tools

### Quick Start

Run with debugger:
```bash
naab-lang run --debug script.naab
```

Enable verbose messages:
```bash
naab-lang run script.naab --verbose
```

### Interactive Debugger

The NAAb interactive debugger allows you to step through your code, inspect variables, and set breakpoints.

#### Starting the Debugger

```bash
naab-lang run --debug myprogram.naab
```

#### Debugger Commands

| Command | Shortcut | Description |
|---------|----------|-------------|
| `continue` | `c` | Continue execution until next breakpoint |
| `step` | `s` | Step to next line (into functions) |
| `next` | `n` | Step over function calls |
| `vars` | `v` | Show all local variables |
| `print <expr>` | `p <expr>` | Evaluate and print expression |
| `watch <expr>` | `w <expr>` | Add watch expression |
| `breakpoint <file>:<line>` | `b <file>:<line>` | Set breakpoint |
| `help` | `h` | Show all commands |
| `quit` | `q` | Exit debugger |

#### Setting Breakpoints

**In code (comment-based):**
```naab
fn processData(data: list<int>) {
    let sum = 0
    for item in data {
        // breakpoint
        sum = sum + item    // Execution pauses here
    }
    return sum
}
```

**At runtime:**
```
(debug) b myfile.naab:15
Breakpoint set at myfile.naab:15
```

**List all breakpoints:**
```
(debug) b
Breakpoints:
  myfile.naab:15
  myfile.naab:32
```

#### Watch Expressions

Monitor variables automatically:
```
(debug) w sum
(debug) w data.length
(debug) c

[Step 10] sum=42, data.length=5
[Step 11] sum=47, data.length=5
```

#### Inspecting Variables

**Print single variable:**
```
(debug) p sum
42
```

**Print complex expression:**
```
(debug) p data[0] + data[1]
15
```

**Show all locals:**
```
(debug) v

--- Local Variables ---
sum = 42
data = [1, 2, 3, 4, 5]
i = 3
```

#### Example Debug Session

```naab
// debug_example.naab
use io

fn fibonacci(n: int) -> int {
    if n <= 1 {
        return n
    }

    let a = 0
    let b = 1
    let i = 2

    while i <= n {
        // breakpoint
        let temp = a + b
        a = b
        b = temp
        i = i + 1
    }

    return b
}

main {
    let result = fibonacci(10)
    io.write(result)
}
```

**Debug session:**
```bash
$ naab-lang run --debug debug_example.naab

Breakpoint hit at debug_example.naab:14

(debug) p a
0

(debug) p b
1

(debug) p temp
1

(debug) w temp
Added watch: temp

(debug) c
[Step 15] temp=1
[Step 16] temp=1
[Step 17] temp=2
[Step 18] temp=3
...
```

### Enhanced Error Messages

NAAb provides context-aware error messages with fix suggestions.

#### Parser Errors

##### Wrong Entry Point

**Error:**
```
Error: Unexpected token 'fn' at line 5, column 1

Hint: NAAb uses 'main {}' as the entry point, not 'fn main()'.

Did you mean:
    main {
        // your code
    }

Instead of:
    fn main() {  // ❌ This doesn't work
        // your code
    }
```

**Fix:** Change `fn main()` to `main {}`

##### Unquoted Dictionary Keys

**Error:**
```
Error: Expected '}' or ',' but got ':' at line 3, column 9

Hint: Dictionary keys must be quoted strings in NAAb.

Did you mean:
    let person = {
        "name": "Alice",  // ✅ Quoted keys
        "age": 30
    }

Instead of:
    let person = {
        name: "Alice",  // ❌ Unquoted keys
        age: 30
    }

Note: Use structs for fixed schemas, dictionaries for dynamic data.
```

**Fix:** Quote all dictionary keys

##### Reserved Keyword as Variable

**Error:**
```
Error: Cannot use reserved keyword 'config' as variable name

Hint: 'config' is a reserved keyword in NAAb.

Suggested alternatives:
    - cfg
    - configuration
    - settings
    - options

Example:
    let cfg = loadSettings()  // ✅
```

**Fix:** Use alternative name

#### Runtime Errors

##### Undefined Variable with Suggestions

**Error:**
```
Runtime Error: Undefined variable 'confg' at line 12, column 5

Stack trace:
    at processData (src/app.naab:12:5)
    at main (src/app.naab:20:3)

Hint: Variable 'confg' is not defined. Did you mean one of these?
    - config (defined at line 5)
    - cfg (imported from 'settings')
    - conf (defined at line 8)

Context:
    10 |     let data = loadData()
    11 |     let settings = getSettings()
 -> 12 |     let result = processWithConfig(confg)  // ❌ Typo here
    13 |     return result

Local variables at this point:
    data = [1, 2, 3, 4, 5]
    settings = {"debug": true, "timeout": 30}
    config = {"mode": "production"}  // ← This is probably what you meant
```

**Fix:** Correct the typo to `config`

##### Type Mismatch

**Error Example (for non-'+' arithmetic operators):**
```
Runtime Error: Type mismatch at line 15

Expected: int, float, or bool
Actual: string

Hint: Cannot subtract string from int. The '+' operator allows string concatenation,
but other arithmetic operators (-, *, /, %) require numeric types. Consider:
    - Convert string to int: string.to_int(value)
    - Convert int to string: json.stringify(count)

Example:
    // This example will cause a type mismatch for '-'
    let count = 10
    let value = "5"
    let total = count - value  // ❌ Runtime Error: Type mismatch for '-'

    // Corrected to show string concatenation for '+'
    let concatenated = count + value // ✅ Works: "105" (string)

    // Corrected for subtraction:
    let subtracted = count - string.to_int(value) // ✅ Works: 5 (int)
```
**Fix:** Add type conversion or ensure numeric operands for strict arithmetic operations.

**Fix:** Add type conversion

#### Common Patterns

##### Using JavaScript Syntax

**Error:**
```
Error: Incorrect import syntax

Hint: NAAb uses 'use' for imports, not 'import'.

Did you mean:
    use io  // ✅ For stdlib
    use my_module as mod  // ✅ For custom modules

Instead of:
    import io from "std"  // ❌ Not JavaScript!
```

##### Dot Notation on Dictionary

**Observed Behavior:** NAAb supports dot notation for dictionary access, printing `Alice`.
```naab
let person = {"name": "Alice"}
let name = person.name  // ✅ Dot notation is supported for dictionaries
print(name) // Output: Alice
```
**Clarification:** While bracket notation `person["name"]` is also supported and often used for dynamic keys, dot notation offers a concise alternative for static keys.


### Common Errors

#### Compilation Errors

| Error | Cause | Solution |
|-------|-------|----------|
| `Unexpected token` | Syntax error | Check NAAb syntax rules |
| `Expected ';' or newline` | Missing semicolon on multi-statement line | Add semicolon or newline |
| `Unmatched '{'` | Missing closing brace | Count braces |
| `Invalid type annotation` | Wrong type syntax | Check type syntax |

#### Runtime Errors

| Error | Cause | Solution |
|-------|-------|----------|
| `Undefined variable` | Typo or undeclared | Check spelling, declare variable |
| `Null reference` | Accessing null value | Add null check |
| `Index out of bounds` | Array access beyond length | Check array bounds |
| `Division by zero` | Dividing by zero | Add check before division |
| `Type mismatch` | Wrong type in operation | Convert types or fix logic |

### Debugging Techniques

#### 1. Print Debugging

```naab
use io

fn debug(label: string, value: any) {
    io.write(label + ": " + toString(value))
}

fn calculate(x: int, y: int) -> int {
    debug("x", x)  // Print values
    debug("y", y)
    let result = x * y
    debug("result", result)
    return result
}
```

#### 2. Conditional Breakpoints

```naab
fn processItems(items: list<int>) {
    for item in items {
        if item > 100 {
            // breakpoint  // Only when item > 100
        }
        process(item)
    }
}
```

#### 3. Assertion Checks

```naab
fn divide(a: int, b: int) -> float {
    if b == 0 {
        throw "Division by zero"
    }
    return a / b
}
```

#### 4. Try-Catch Debugging

```naab
fn safeOperation() {
    try {
        riskyCode()
    } catch (error) {
        io.write("Error: " + error)  // Log error
        throw error  // Re-throw
    }
}
```

#### 5. Tracing Execution

```naab
let DEBUG = true

fn trace(msg: string) {
    if DEBUG {
        io.write("[TRACE] " + msg)
    }
}

fn algorithm() {
    trace("Starting algorithm")
    trace("Step 1 complete")
    trace("Step 2 complete")
}
```

### Tips for LLMs

When generating NAAb code, avoid these common mistakes:

#### ❌ Don't Use: `fn main()`
✅ Use: `main {}`

#### ❌ Don't: Unquoted dict keys
✅ Do: Quote all dictionary keys
```naab
let data = {"name": "Alice"}  // ✅
```

#### ❌ Don't: JavaScript imports
✅ Do: NAAb `use` statements
```naab
use io  // ✅
```

#### ❌ Don't: Over-annotate types
✅ Do: Let type inference work
```naab
let x = 42  // ✅ Type inferred
```

#### ❌ Don't: Dot notation on dicts
✅ Do: Bracket notation for dicts, dot for structs

#### ❌ Don't: Use `async` without polyglot
✅ Do: Use polyglot blocks for async
```naab
let data = <<python
import asyncio
# async code here
>>
```

---

## Debug Helpers & Development Tools

### Shell Block Debugging

#### Helper: test_shell_debug.naab
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

### Type Conversion Debugging

#### Common Patterns

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

#### Debug Helper for Type Conversions

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

### Module Alias Checker

#### Helper: check_module_aliases.sh
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
            wrong_usage=$(grep -n "${module}\." "$file" | grep -v "^[[:space:]]*\/\/")

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

### Struct Serialization Testing

#### Helper: test_struct_json.naab
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

### Variable Binding Validator

#### Helper: test_variable_binding.naab
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

    let filename = "/tmp/test.txt" // Use /tmp for better write permissions
    let content = "Hello World"

    let result = <<sh[filename, content]
# Note: Ensure you have write permissions to the directory for the filename.
# Using /tmp is generally safer for temporary files.
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

### Error Detection Patterns

#### Common Errors & Debug Commands

```bash
# Find NAAB_VAR_ usage (old syntax)
grep -r "NAAB_VAR_" --include="*.naab" .

# Find string.to_string (non-existent function)
grep -r "string.to_string\|str.to_string" --include="*.naab" .

# Find 3-argument str.concat calls
grep -r "str.concat.*,.*,.*)" --include="*.naab" .

# Find module alias mismatches
for file in *.naab; do
    echo "=== $file ==="
    # Extract: use string as str
    grep "use string as" "$file"
    # Find: string. usage (should be str.)
    grep "string\." "$file" | grep -v "^[[:space:]]*use "
done
```

### Performance Profiling

#### Helper: profile_execution.naab
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

### Memory Leak Detection

#### Helper: test_memory_leaks.naab
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

### Integration Test Runner

#### Helper: run_all_tests.sh
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

### Debugging Checklist

When encountering issues, check:

#### Syntax Issues
- [ ] Are semicolons optional everywhere?
- [ ] Are struct literals using commas or newlines?
- [ ] Are type names lowercase?

#### Module Issues
- [ ] Does `use X as Y` match usage (`Y.function` not `X.function`)?
- [ ] Are all stdlib modules imported?
- [ ] Are custom modules in the same directory or correct path?

#### Type Conversion
- [ ] Using `json.stringify()` not `str.to_string()`?
- [ ] Using 2-argument `str.concat()` not 3+?
- [ ] Chaining concatenations if needed?

#### Inline Code Blocks
- [ ] Python blocks use direct variable names (not `NAAB_VAR_`)?
- [ ] Shell blocks use `<<sh[var1, var2]` binding syntax?
- [ ] Shell variables quoted: `"$varname"`?
- [ ] Python blocks return value assigned to underscore?

#### Shell Block Returns
- [ ] Accessing `result.exit_code`, `result.stdout`, `result.stderr`?
- [ ] Checking `exit_code == 0` for success?
- [ ] Handling stderr output appropriately?

#### Struct Serialization
- [ ] Using `json.stringify()` on structs?
- [ ] Struct fields match expected JSON keys?
- [ ] Nested structs serialize correctly?

### CI/CD Integration

#### GitHub Actions Example

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
        ! grep -r "str.to_string" --include="*.naab" examples/
```

### Quick Reference Card

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

## Usage

These helper files are designed to be copied to your project for targeted debugging or used as reference.

1.  Copy `.naab` and `.sh` helper files to your project directory as needed.
2.  For shell scripts, grant execute permissions: `chmod +x *.sh`.
3.  Execute `.naab` files with `naab-lang run <file.naab>`.
4.  Execute shell scripts directly: `./run_all_tests.sh`.
5.  Check output for failures or diagnostic information.

## Contributing

Add new debug helpers by creating `test_*.naab` files or `.sh` scripts following these patterns:
-   Clear test names and descriptions.
-   Detailed output for easy understanding.
-   Include both positive and negative test cases.
-   Provide clear error messages for failures.
-   Include a summary of results at the end.
