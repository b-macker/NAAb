# NAAb Language User Guide
**Version**: 0.2.0
**Last Updated**: December 30, 2024

## Table of Contents

1. [Introduction](#introduction)
2. [Getting Started](#getting-started)
3. [Language Basics](#language-basics)
4. [Working with Data](#working-with-data)
5. [Functions](#functions)
6. [Control Flow](#control-flow)
7. [Error Handling](#error-handling)
8. [Standard Library](#standard-library)
9. [Multi-File Applications](#multi-file-applications)
10. [Block Assembly](#block-assembly)
11. [Advanced Features](#advanced-features)
12. [Best Practices](#best-practices)

---

## Introduction

NAAb is a modern block assembly language designed for composing reusable code blocks across multiple programming languages. With 24,483 blocks in the registry, NAAb allows you to build applications by assembling pre-built, tested components.

### Key Features

- **Multi-language support**: Execute C++, JavaScript, and Python blocks
- **Type-safe composition**: Validate block compatibility before execution
- **Pipeline syntax**: Chain operations with `|>` operator
- **Rich stdlib**: 13 modules (string, array, math, io, json, http, collections, etc.)
- **Exception handling**: try/catch/finally with stack traces
- **Module system**: Import/export for code organization
- **REST API**: HTTP interface for executing NAAb programs

---

## Getting Started

### Installation

# Build
cmake -B build
cmake --build build -j4

# Install binaries to ~/
cp build/naab-lang ~/
```

### Your First Program

Create `hello.naab`:

```naab
print("Hello, NAAb!")
```

Run it:

```bash
~/naab-lang run hello.naab
```

Output:
```
Hello, NAAb!
```

### Interactive REPL

```bash
~/naab-repl
```

Try these commands:

```naab
> let x = 42
> print(x)
42
> let sum = 10 + 32
> print(sum)
42
```

---

## Language Basics

### Variables

```naab
let name = "Alice"
let age = 30
let score = 95.5
let active = true
```

Variables are dynamically typed but support type annotations:

```naab
let count: int = 42
let name: string = "Bob"
```

### Data Types

- **Int**: `42`, `-10`, `0`
- **Float**: `3.14`, `-0.5`, `2.0`
- **String**: `"hello"`, `'world'`
- **Bool**: `true`, `false`
- **Array**: `[1, 2, 3]`, `["a", "b", "c"]`
- **Dict**: `{"key": "value", "count": 42}`

### Operators

**Arithmetic**:
```naab
let a = 10 + 5    // 15
let b = 10 - 5    // 5
let c = 10 * 5    // 50
let d = 10 / 5    // 2
let e = 10 % 3    // 1
```

**Comparison**:
```naab
let eq = (5 == 5)   // true
let ne = (5 != 3)   // true
let lt = (3 < 5)    // true
let le = (5 <= 5)   // true
let gt = (5 > 3)    // true
let ge = (5 >= 5)   // true
```

**Logical**:
```naab
let and = true && false    // false
let or = true || false     // true
let not = !true            // false
```

**Short-circuit evaluation**:
```naab
// Second expression not evaluated if first is false
if (x != 0 && (100 / x) > 5) {
    print("Safe division")
}
```

---

## Working with Data

### Arrays

```naab
import "stdlib" as std

let numbers = [1, 2, 3, 4, 5]

// Access
print(numbers[0])  // 1

// Length
print(std.length(numbers))  // 5

// Add element
numbers = std.push(numbers, 6)  // [1, 2, 3, 4, 5, 6]

// Remove last
let last = std.pop(numbers)  // 6

// Reverse
let reversed = std.reverse(numbers)  // [5, 4, 3, 2, 1]

// Check contains
let has = std.contains(numbers, 3)  // true

// Join to string
let joined = std.join(numbers, ",")  // "1,2,3,4,5"
```

### Higher-Order Array Functions

```naab
// Map - transform array
fn double(x) {
    return x * 2
}
let doubled = std.map_fn(numbers, double)  // [2, 4, 6, 8, 10]

// Filter - select elements
fn is_even(x) {
    return x % 2 == 0
}
let evens = std.filter_fn(numbers, is_even)  // [2, 4]

// Reduce - aggregate
fn add(acc, x) {
    return acc + x
}
let sum = std.reduce_fn(numbers, add, 0)  // 15

// Find - first match
fn greater_than_3(x) {
    return x > 3
}
let found = std.find(numbers, greater_than_3)  // 4
```

### Strings

```naab
import "stdlib" as std

let text = "Hello, World!"

// Length
print(std.length(text))  // 13

// Case conversion
let upper = std.upper(text)  // "HELLO, WORLD!"
let lower = std.lower(text)  // "hello, world!"

// Trim whitespace
let trimmed = std.trim("  hello  ")  // "hello"

// Split
let words = std.split(text, ", ")  // ["Hello", "World!"]

// Replace
let replaced = std.replace(text, "World", "NAAb")  // "Hello, NAAb!"

// Check contains
let has = std.contains(text, "World")  // true

// Check prefix/suffix
let starts = std.starts_with(text, "Hello")  // true
let ends = std.ends_with(text, "!")  // true

// Substring
let sub = std.substring(text, 0, 5)  // "Hello"

// Find index
let index = std.index_of(text, "World")  // 7

// Repeat
let repeated = std.repeat("ha", 3)  // "hahaha"
```

### Dictionaries

```naab
let person = {
    "name": "Alice",
    "age": 30,
    "city": "Boston"
}

// Access
print(person["name"])  // "Alice"

// Modify
person["age"] = 31

// Check key exists
// (use try/catch for safe access)
```

---

## Functions

### Function Declaration

```naab
fn greet(name) {
    return "Hello, " + name + "!"
}

let message = greet("Alice")
print(message)  // "Hello, Alice!"
```

### Default Parameters

```naab
fn greet(name = "World") {
    return "Hello, " + name + "!"
}

print(greet())        // "Hello, World!"
print(greet("Bob"))   // "Hello, Bob!"
```

### Recursive Functions

```naab
fn factorial(n) {
    if (n <= 1) {
        return 1
    }
    return n * factorial(n - 1)
}

print(factorial(5))  // 120
```

### Function as Values

```naab
fn apply(callback, value) {
    return callback(value)
}

fn double(x) {
    return x * 2
}

let result = apply(double, 5)  // 10
```

---

## Control Flow

### If Statements

```naab
let age = 25

if (age >= 18) {
    print("Adult")
} else {
    print("Minor")
}
```

### Nested If

```naab
let score = 85

if (score >= 90) {
    print("A")
} else if (score >= 80) {
    print("B")
} else if (score >= 70) {
    print("C")
} else {
    print("F")
}
```

### While Loops

```naab
let count = 0

while (count < 5) {
    print(count)
    count = count + 1
}
// Prints: 0 1 2 3 4
```

### For Loops

```naab
let numbers = [1, 2, 3, 4, 5]

for (num in numbers) {
    print(num)
}
// Prints: 1 2 3 4 5
```

### Break and Continue

```naab
// Break - exit loop
let i = 0
while (true) {
    if (i >= 5) {
        break
    }
    print(i)
    i = i + 1
}

// Continue - skip iteration
for (x in [1, 2, 3, 4, 5]) {
    if (x % 2 == 0) {
        continue  // Skip even numbers
    }
    print(x)  // Prints: 1 3 5
}
```

---

## Error Handling

### Try/Catch

```naab
try {
    // Risky operation
    let result = 10 / 0
} catch (error) {
    print("Error occurred: " + error)
}
```

### Try/Catch/Finally

```naab
let file = "data.txt"

try {
    // Open and read file
    let data = read_file(file)
    process(data)
} catch (error) {
    print("Failed to process file: " + error)
} finally {
    // Always executed
    print("Cleanup complete")
}
```

### Throwing Errors

```naab
fn divide(a, b) {
    if (b == 0) {
        throw "Division by zero"
    }
    return a / b
}

try {
    let result = divide(10, 0)
} catch (error) {
    print("Error: " + error)  // "Error: Division by zero"
}
```

---

## Standard Library

NAAb provides 13 built-in modules:

### Core Modules

**io** - File I/O operations:
```naab
import "stdlib" as std

// Read file
let content = std.read_file("data.txt")

// Write file
std.write_file("output.txt", "Hello")

// Check exists
let exists = std.exists("data.txt")
```

**json** - JSON parsing and serialization:
```naab
import "stdlib" as std

// Parse JSON
let obj = std.parse('{"name": "Alice", "age": 30}')

// Stringify
let json = std.stringify(obj)
```

**http** - HTTP requests:
```naab
import "stdlib" as std

// GET request
let response = std.get("https://api.example.com/data")

// POST request
let result = std.post("https://api.example.com/create", {
    "name": "Bob",
    "email": "bob@example.com"
})
```

### Data Modules

**string** - String manipulation (see [Strings](#strings) section)

**array** - Array operations (see [Arrays](#arrays) section)

**math** - Mathematical functions:
```naab
import "stdlib" as std

let abs_val = std.abs(-5)         // 5
let floor_val = std.floor(3.7)    // 3.0
let ceil_val = std.ceil(3.2)      // 4.0
let round_val = std.round(3.6)    // 4.0
let max_val = std.max(5, 10)      // 10
let min_val = std.min(5, 10)      // 5
let pow_val = std.pow(2, 3)       // 8.0
let sqrt_val = std.sqrt(16)       // 4.0
```

**collections** - Advanced data structures

### System Modules

**time** - Time operations:
```naab
import "stdlib" as std

let now = std.now()
let timestamp = std.timestamp()
```

**env** - Environment variables:
```naab
import "stdlib" as std

let home = std.getenv("HOME")
std.setenv("MY_VAR", "value")
```

**csv** - CSV file handling

**regex** - Regular expressions

**crypto** - Cryptographic functions

**file** - Advanced file operations

---

## Multi-File Applications

### Exporting Functions

Create `math_utils.naab`:

```naab
export fn add(a, b) {
    return a + b
}

export fn multiply(a, b) {
    return a * b
}
```

### Importing Functions

Create `main.naab`:

```naab
import {add, multiply} from "./math_utils.naab"

let sum = add(5, 3)        // 8
let product = multiply(5, 3)  // 15

print("Sum: " + sum)
print("Product: " + product)
```

### Wildcard Imports

```naab
import * as math from "./math_utils.naab"

let sum = math.add(5, 3)
let product = math.multiply(5, 3)
```

### Aliasing

```naab
import {add as plus, multiply as times} from "./math_utils.naab"

let result = plus(5, times(2, 3))  // 11
```

---

## Block Assembly

### Searching for Blocks

```bash
~/naab-lang blocks search "validate email"
```

Output:
```
Search results for "validate email":

1. BLOCK-PY-09145 (python)
   Validate email address format
   Input: string → Output: bool
   Score: 0.95

2. BLOCK-JS-03421 (javascript)
   Email validation with regex
   Input: string → Output: bool
   Score: 0.87
```

### Listing Available Blocks

```bash
~/naab-lang blocks list
```

Output:
```
Total blocks: 24,483

By language:
  Python:     8,234 blocks
  C++:        7,621 blocks
  JavaScript: 5,142 blocks
  Go:         2,341 blocks
  Rust:         845 blocks
  Java:         300 blocks
```

### Using Blocks in Code

```naab
use BLOCK-PY-09145 as validate_email

let email = "alice@example.com"
let valid = validate_email(email)

if (valid) {
    print("Valid email")
} else {
    print("Invalid email")
}
```

### Validating Block Composition

```bash
~/naab-lang validate "BLOCK-PY-09145,BLOCK-JS-03421"
```

Output shows type compatibility and suggestions for adapters if types don't match.

---

## Advanced Features

### Pipeline Syntax

Chain operations with `|>`:

```naab
let result = data
    |> normalize
    |> validate
    |> process
    |> format

// Equivalent to:
let temp1 = normalize(data)
let temp2 = validate(temp1)
let temp3 = process(temp2)
let result = format(temp3)
```

Example with arrays:

```naab
let numbers = [1, 2, 3, 4, 5]

let result = numbers
    |> filter_evens
    |> double_values
    |> sum_all

print(result)
```

### Type Annotations

```naab
fn add(x: int, y: int): int {
    return x + y
}

let numbers: array<int> = [1, 2, 3]
let config: dict<string, any> = {"debug": true}
```

Supported types:
- `int`, `float`, `string`, `bool`, `void`, `any`
- `array<T>` - homogeneous arrays
- `dict<K,V>` - key-value maps
- `function<T1, T2, ..., R>` - function types

---

## Best Practices

### 1. Use Type Annotations for Public APIs

```naab
// Good
export fn calculate_discount(price: float, percent: float): float {
    return price * (1 - percent / 100)
}

// Less clear
export fn calculate_discount(price, percent) {
    return price * (1 - percent / 100)
}
```

### 2. Handle Errors Gracefully

```naab
// Good
try {
    let data = std.read_file("config.json")
    let config = std.parse(data)
    process(config)
} catch (error) {
    print("Failed to load config: " + error)
    use_defaults()
}

// Bad - unhandled errors crash program
let data = std.read_file("config.json")
let config = std.parse(data)
```

### 3. Use Descriptive Variable Names

```naab
// Good
let user_email = "alice@example.com"
let total_price = 99.99
let is_valid = true

// Bad
let e = "alice@example.com"
let t = 99.99
let v = true
```

### 4. Break Down Complex Functions

```naab
// Good
fn process_order(order) {
    let validated = validate_order(order)
    let calculated = calculate_totals(validated)
    let formatted = format_receipt(calculated)
    return formatted
}

// Bad - too much in one function
fn process_order(order) {
    // 50 lines of validation, calculation, formatting...
}
```

### 5. Use Constants for Magic Numbers

```naab
// Good
let TAX_RATE = 0.08
let total = price * (1 + TAX_RATE)

// Bad
let total = price * 1.08
```

### 6. Leverage the Standard Library

```naab
// Good - use stdlib
import "stdlib" as std
let upper = std.upper(text)

// Bad - reinvent the wheel
fn to_upper(text) {
    // Manual uppercase implementation...
}
```

### 7. Organize Code into Modules

```naab
// utils/string_helpers.naab
export fn capitalize(text) { ... }
export fn reverse(text) { ... }

// utils/math_helpers.naab
export fn average(numbers) { ... }
export fn median(numbers) { ... }

// main.naab
import * as strings from "./utils/string_helpers.naab"
import * as math from "./utils/math_helpers.naab"
```

---

## Governance

NAAb includes a built-in governance engine that enforces project-level policies on polyglot code blocks. Place a `govern.json` file in your project directory to enable it.

### Quick Example

```json
{
  "version": "3.0",
  "mode": "enforce",
  "code_quality": {
    "no_secrets": { "level": "hard" },
    "no_oversimplification": { "level": "soft" },
    "no_hallucinated_apis": { "level": "advisory" }
  }
}
```

Key features:
- **50+ built-in checks** for security, code quality, and LLM anti-drift
- **Three enforcement levels**: HARD (block), SOFT (overridable), ADVISORY (warn)
- **CI/CD integration** via `--governance-sarif` and `--governance-junit` flags
- **Custom rules** with regex patterns for project-specific policies

For the full governance reference, see [Chapter 21: Governance and LLM Code Quality](docs/book/chapter21.md).

---

## Next Steps

- Read the [NAAb Book](docs/book/) for comprehensive documentation (21 chapters)
- See [Quick Start](docs/book/QUICK_START.md) for getting started
- Browse [examples/](examples/) for real-world examples

---

## Getting Help

- **Documentation**: `docs/book/`
- **Examples**: `examples/`
- **Tests**: `tests/`

Run `naab-lang --help` for command-line options.
