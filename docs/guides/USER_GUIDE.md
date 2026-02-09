# NAAb Language User Guide
**Version**: 0.1.0
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
13. [Appendix: Quick Start Details](#appendix-quick-start-details)
    13.1 [Recent Updates & Current Status](#recent-updates--current-status)
    13.2 [Common Coding Patterns](#common-coding-patterns)
    13.3 [Testing Your Code - Quick Reference](#testing-your-code---quick-reference)
    13.4 [Common Errors & Fixes - Quick Reference](#common-errors--fixes---quick-reference)
    13.5 [Quick Build & Run](#quick-build--run)
    13.6 [Need Help?](#need-help)
    13.7 [Quick Checklist](#quick-checklist)

---

## Introduction

NAAb (Not Another Assembly Block) is a modern block assembly language designed for composing reusable code blocks across multiple programming languages. With 24,483 blocks in the registry, NAAb allows you to build applications by assembling pre-built, tested components.

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

```bash
# Clone repository
cd /storage/emulated/0/Download/.naab/naab_language

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
use io
use array

let numbers = [1, 2, 3, 4, 5]

// Access
print(numbers[0])  // 1

// Length
print(array.length(numbers))  // 5

// Add element
numbers = array.push(numbers, 6)  // [1, 2, 3, 4, 5, 6]

// Remove last element (modifies in-place)
let last_num = array.pop(numbers) // 6, numbers is now [1, 2, 3, 4, 5]
print("Popped: ", last_num, ", Array: ", numbers)
// Note: array.pop() modifies the array in-place and throws an error if called on an empty array.
// Check array.length before calling pop for robust code.

// Reverse
let reversed = array.reverse(numbers)  // [5, 4, 3, 2, 1]

// Check contains
let has = array.contains(numbers, 3)  // true
```

### Higher-Order Array Functions

```naab
use io
use array

// Note: array.find is not available in stdlib
// function greater_than_3(x: int) -> bool {
//     return x > 3
// }

main {
    let numbers = [1, 2, 3, 4, 5]

    // Map - transform array
    let doubled = array.map(numbers, fn(x: int) -> int { return x * 2 })  // [2, 4, 6, 8, 10]
    print(doubled)

    // Filter - select elements
    let evens = array.filter(numbers, fn(x: int) -> bool { return x % 2 == 0 })  // [2, 4]
    print(evens)

    // Reduce - aggregate
    let sum = array.reduce(numbers, fn(acc: int, x: int) -> int { return acc + x }, 0)  // 15
    print(sum)

    // Find - first match is not directly available as `array.find`
    // let found = array.find(numbers, greater_than_3)  // 4
}
```

### Strings

```naab
use io
use string

let text = "Hello, World!"

// Length
print(string.length(text))  // 13

// Case conversion
let upper = string.upper(text)  // "HELLO, WORLD!"
let lower = string.lower(text)  // "hello, world!"

// Trim whitespace
let trimmed = string.trim("  hello  ")  // "hello"

// Split
let words = string.split(text, ", ")  // ["Hello", "World!"]

// Replace
let replaced = string.replace(text, "World", "NAAb")  // "Hello, NAAb!"

// Check contains
let has = string.contains(text, "World")  // true

// Check prefix/suffix
let starts = string.starts_with(text, "Hello")  // true
let ends = string.ends_with(text, "!")  // true

// Substring
let sub = string.substring(text, 0, 5)  // "Hello"

// Find index
let index = string.index_of(text, "World")  // 7

// Repeat
let repeated = string.repeat("ha", 3)  // "hahaha"
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
fn greet(name: string) -> string { // Corrected: `fn` keyword and type annotations
    return "Hello, " + name + "!"
}

let message = greet("Alice")
print(message)  // "Hello, Alice!"
```

### Default Parameters

NAAb functions do not currently support default parameters. All arguments must be provided when calling a function.

```naab
// Correct function definition syntax
fn greet(name: string) -> string {
    return "Hello, " + name + "!"
}

main {
    print(greet("Bob")) // Must provide argument
}
```

### Recursive Functions

```naab
fn factorial(n: int) -> int { // Corrected: `fn` keyword and type annotations
    if (n <= 1) {
        return 1
    }
    return n * factorial(n - 1)
}

print(factorial(5))  // 120
```

### Function as Values

```naab
fn apply(fn_val: function, value: int) -> int { // Corrected: `fn` keyword and type annotations
    return fn_val(value)
}

fn double(x: int) -> int { // Corrected: `fn` keyword and type annotations
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

for num in numbers {
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
use io // Required for print

let file = "data.txt" // This file won't exist

fn process(data: string) { // Dummy process function
    if (data == "error") {
        throw "Simulated processing error"
    }
    print("Processing:", data)
}

main {
    try {
        // Open and read file (will throw error since file does not exist)
        // Corrected: io.read("data.txt") is not yet functional. Simulate a failing read.
        let data = "error" // Simulate data read failure
        process(data)
    } catch (error) {
        print("Failed to process file: " + error)
    } finally {
        // Always executed
        print("Cleanup complete")
    }
}
```

### Throwing Errors

```naab
fn divide(a: int, b: int) -> float { // Corrected: `fn` keyword and type annotations
    if (b == 0) {
        throw "Division by zero"
    }
    return a / b
}

main {
    try {
        let result = divide(10, 0)
    } catch (error) {
        print("Error: " + error)  // "Error: Division by zero"
    }
}
```

---

## Standard Library

NAAb provides 13 built-in modules:

### Core Modules

**io** - Console I/O operations:
```naab
use io

// Write to console
io.write("Hello to console from io!\n")
let input = io.read_line("Enter something: ")
io.write("You entered: ", input, "\n")
```

**file** - File I/O operations:
```naab
use file

// Write to file (overwrites)
file.write("output.txt", "Hello World")
print("Wrote 'Hello World' to output.txt")

// Check exists
let exists = file.exists("output.txt")
print("output.txt exists:", exists)

// Read entire file
let content = file.read("output.txt")
print("Content of output.txt: ", content)

// Cleanup
file.delete("output.txt")
print("Cleaned up output.txt")


**json** - JSON parsing and serialization:
```naab
use json

// Parse JSON
let obj = json.parse('{"name": "Alice", "age": 30}')

// Stringify
let json = json.stringify(obj)
```

**http** - HTTP requests:
```naab
use http

// GET request
let response = http.get("https://api.example.com/data")

// POST request
let result = http.post("https://api.example.com/create", {
    "name": "Bob",
    "email": "bob@example.com"
})
```

### Data Modules

**string** - String manipulation (see [Strings](#strings) section)

**array** - Array operations (see [Arrays](#arrays) section)

**math** - Mathematical functions:
```naab
use math

let abs_val = math.abs(-5)         // 5
let floor_val = math.floor(3.7)    // 3.0
let ceil_val = math.ceil(3.2)      // 4.0
let round_val = math.round(3.6)    // 4.0
let max_val = math.max(5, 10)      // 10
let min_val = math.min(5, 10)      // 5
let pow_val = math.pow(2, 3)       // 8.0
let sqrt_val = math.sqrt(16)       // 4.0
```

**collections** - Advanced data structures

### System Modules

**time** - Time operations:
```naab
use time

let now = time.now()
// Note: time.timestamp() is not available in stdlib (AI_ASSISTANT_GUIDE.md only lists time.now(), time.milliseconds(), time.nanoseconds(), time.format(), time.parse(), time.sleep())
// let timestamp = time.timestamp()
```

**env** - Environment variables:
```naab
use env

let home = env.get("HOME")
env.set("MY_VAR", "value")
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
export fn add(a: int, b: int) -> int { // Corrected: `fn` keyword and type annotations
    return a + b
}

export fn multiply(a: int, b: int) -> int { // Corrected: `fn` keyword and type annotations
    return a * b
}
```

### Importing Functions

Create `main.naab`:

```naab
use io
use math_utils // Assuming math_utils.naab is in the same directory

let sum = math_utils.add(5, 3)
let product = math_utils.multiply(5, 3)

print("Sum: ", sum)
print("Product: ", product)
```

### Wildcard Imports

```naab
use io
use math_utils as math // Assuming math_utils.naab is in the same directory

let sum = math.add(5, 3)
let product = math.multiply(5, 3)

print("Sum: ", sum)
print("Product: ", product)
```

### Aliasing

```naab
use io
use math_utils as mu // Aliasing the module

// NAAb does not support aliasing individual functions during import.
// You must refer to functions via the module alias: mu.add, mu.multiply.
let result = mu.add(5, mu.multiply(2, 3))  // 11
print("Result: ", result)
```

---

## Block Assembly

The Block Assembly System allows you to leverage a vast library of pre-built code blocks across multiple programming languages. For a comprehensive guide on searching, listing, using, and validating blocks, please refer to the dedicated [Block Assembly System guide](../reference/BLOCK_ASSEMBLY.md).

### Basic Usage Example

```naab
use BLOCK-PY-09145 as validate_email // Example Block ID

let email = "alice@example.com"
let valid = validate_email(email)

if (valid) {
    print("Valid email")
} else {
    print("Invalid email")
}
```
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
fn add(x: int, y: int) -> int { // Corrected: `fn` keyword and `->` for return type
    return x + y
}

let numbers: list<int> = [1, 2, 3]
let my_config: dict<string, any> = {"debug": true}
```

Supported types:
- `int`, `float`, `string`, `bool`, `void`, `any`
- `list<T>` - homogeneous lists (arrays are represented as lists)
- `dict<K,V>` - key-value maps
- `function<T1, T2, ..., R>` - function types

---

## Best Practices

### 1. Use Type Annotations for Public APIs

```naab
// Good
export fn calculate_discount(price: float, percent: float) -> float { // Corrected: `fn` and `->`
    return price * (1 - percent / 100)
}

// Less clear (and potentially invalid syntax if `function` is used)
// Code without type annotations should still use `fn`
export fn calculate_discount_untyped(price, percent) { // Corrected: `fn`
    return price * (1 - percent / 100)
}
```

### 2. Handle Errors Gracefully

```naab
// Good
use io
use json

fn process(config: dict<string, any>) {
    print("Processing config:", config)
}

fn use_defaults() {
    print("Using default configuration.")
}

main {
    try {
        // Simulate reading a file (io.read is non-functional)
        let data_str = '{"setting": "value"}'
        let config = json.parse(data_str)
        process(config)
    } catch (error) {
        print("Failed to load config: " + error)
        use_defaults()
    }
}
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
function process_order(order) {
    let validated = validate_order(order)
    let calculated = calculate_totals(validated)
    let formatted = format_receipt(calculated)
    return formatted
}

// Bad - too much in one function
function process_order(order) {
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
use string
let text_example = "hello world" // Define text_example
let upper = string.upper(text_example) // Use string.upper
print(upper) // Added print for observation

// Bad - reinvent the wheel
fn to_upper_manual(text_param: string) -> string { // Corrected: fn and type annotations
    // Manual uppercase implementation... (dummy implementation)
    return text_param // Just return for now
}
print(to_upper_manual("hello"))
```

### 7. Organize Code into Modules

(See the [Multi-File Applications](#multi-file-applications) section for runnable examples.)

```naab
// Example structure:
// utils/string_helpers.naab
export fn capitalize(text: string) -> string { /* ... */ }
export fn reverse(text: string) -> string { /* ... */ }

// utils/math_helpers.naab
export fn average(numbers: list<float>) -> float { /* ... */ }
export fn median(numbers: list<float>) -> float { /* ... */ }

// main.naab
use utils.string_helpers as strings
use utils.math_helpers as math
```

---

## Next Steps

- Explore the [Standard Library sections](#standard-library) in this guide for module overviews.
- Check the `examples/` directory for real-world examples.
- Visit the [Developer Guide](../technical/DEVELOPER_GUIDE.md) for architectural insights.

---

## Getting Help



- **This Guide**: `docs/guides/USER_GUIDE.md`

- **AI Assistant Guide**: `docs/guides/AI_ASSISTANT_GUIDE.md`

- **Technical Guides**: `docs/technical/` directory (e.g., [Debugging Guide](../technical/DEBUGGING_GUIDE.md))

- **Reference Material**: `docs/reference/` directory (e.g., [Block Assembly](../reference/BLOCK_ASSEMBLY.md))

- **Tutorials**: `docs/tutorials/` directory (e.g., [Getting Started Tutorial](../tutorials/GETTING_STARTED.naab))

- **Examples**: `/examples/` directory in the repository root.

- **Tests**: `/tests/` directory in the repository root.



Run `naab-lang help` for command-line options.
