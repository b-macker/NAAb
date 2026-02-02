# NAAb Debugging Guide

Comprehensive guide to debugging NAAb programs with enhanced error messages and interactive debugging.

## Table of Contents

1. [Quick Start](#quick-start)
2. [Interactive Debugger](#interactive-debugger)
3. [Enhanced Error Messages](#enhanced-error-messages)
4. [Common Errors](#common-errors)
5. [Debugging Techniques](#debugging-techniques)
6. [Tips for LLMs](#tips-for-llms)

## Quick Start

Run with debugger:
```bash
naab-lang run --debug script.naab
```

Enable verbose error messages:
```bash
naab-lang run --verbose script.naab
```

## Interactive Debugger

### Starting the Debugger

```bash
naab-lang run --debug myprogram.naab
```

### Debugger Commands

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

### Setting Breakpoints

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

### Watch Expressions

Monitor variables automatically:
```
(debug) w sum
(debug) w data.length
(debug) c

[Step 10] sum=42, data.length=5
[Step 11] sum=47, data.length=5
```

### Inspecting Variables

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

### Example Debug Session

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

## Enhanced Error Messages

NAAb provides context-aware error messages with fix suggestions.

### Parser Errors

#### Wrong Entry Point

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

#### Unquoted Dictionary Keys

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

#### Reserved Keyword as Variable

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

### Runtime Errors

#### Undefined Variable with Suggestions

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

#### Type Mismatch

**Error:**
```
Runtime Error: Type mismatch at line 15

Expected: int
Actual: string

Hint: Cannot add string to int. Consider:
    - Convert string to int: parseInt(value)
    - Convert int to string: toString(count)

Example:
    let total = count + parseInt(value)  // ✅
```

**Fix:** Add type conversion

### Common Patterns

#### Using JavaScript Syntax

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

#### Dot Notation on Dictionary

**Error:**
```
Error: Cannot access member 'name' on type 'dict<string, any>'

Hint: Dictionaries use bracket notation, not dot notation.

Did you mean:
    let name = person["name"]  // ✅ Bracket notation for dicts

Instead of:
    let name = person.name  // ❌ Dot notation only for structs

Note: If you need dot notation, use a struct instead:
    struct Person { name: string, age: int }
    let person = Person { name: "Alice", age: 30 }
    let name = person.name  // ✅ Dot notation works
```

## Common Errors

### Compilation Errors

| Error | Cause | Solution |
|-------|-------|----------|
| `Unexpected token` | Syntax error | Check NAAb syntax rules |
| `Expected ';' or newline` | Missing semicolon on multi-statement line | Add semicolon or newline |
| `Unmatched '{'` | Missing closing brace | Count braces |
| `Invalid type annotation` | Wrong type syntax | Check type syntax |

### Runtime Errors

| Error | Cause | Solution |
|-------|-------|----------|
| `Undefined variable` | Typo or undeclared | Check spelling, declare variable |
| `Null reference` | Accessing null value | Add null check |
| `Index out of bounds` | Array access beyond length | Check array bounds |
| `Division by zero` | Dividing by zero | Add check before division |
| `Type mismatch` | Wrong type in operation | Convert types or fix logic |

## Debugging Techniques

### 1. Print Debugging

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

### 2. Conditional Breakpoints

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

### 3. Assertion Checks

```naab
fn divide(a: int, b: int) -> int {
    if b == 0 {
        throw "Division by zero"
    }
    return a / b
}
```

### 4. Try-Catch Debugging

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

### 5. Tracing Execution

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

## Tips for LLMs

When generating NAAb code, avoid these common mistakes:

### ❌ Don't Use: `fn main()`
✅ Use: `main {}`

### ❌ Don't: Unquoted dict keys
✅ Do: Quote all dictionary keys
```naab
let data = {"name": "Alice"}  // ✅
```

### ❌ Don't: JavaScript imports
✅ Do: NAAb `use` statements
```naab
use io  // ✅
```

### ❌ Don't: Over-annotate types
✅ Do: Let type inference work
```naab
let x = 42  // ✅ Type inferred
```

### ❌ Don't: Dot notation on dicts
✅ Do: Bracket notation for dicts, dot for structs

### ❌ Don't: Use `async` without polyglot
✅ Do: Use polyglot blocks for async
```naab
let data = <<python
import asyncio
# async code here
>>
```

## See Also

- [Error Messages Reference](ERROR_MESSAGES.md) - Complete error catalog
- [LLM Best Practices](LLM_BEST_PRACTICES.md) - Guide for code generation
- [Language Guide](../README.md) - NAAb language reference
