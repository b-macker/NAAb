# NAAb User Guide

**Version:** 0.1.0
**Last Updated:** December 25, 2024

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [Your First Program](#your-first-program)
3. [Language Basics](#language-basics)
4. [Working with Blocks](#working-with-blocks)
5. [Standard Library](#standard-library)
6. [Cross-Language Programming](#cross-language-programming)
7. [Best Practices](#best-practices)
8. [Troubleshooting](#troubleshooting)

---

## Getting Started

### Installation

#### Prerequisites

- C++ compiler (clang++ or g++)
- CMake 3.15+
- SQLite3
- Python 3.8+ (optional, for Python blocks)
- pkg-config (optional, for library detection)

#### Build from Source

```bash
# Clone repository
cd /path/to/naab_language

# Create build directory
mkdir build && cd build

# Configure
cmake ..

# Build
cmake --build . -j$(nproc)

# Executables will be in build/:
# - naab-lang        (main interpreter)
# - naab-repl        (REPL)
# - naab-doc         (documentation generator)
```

#### Verify Installation

```bash
$ ./naab-lang version
NAAb Language v0.1.0
C++ Executor: Available
JavaScript Executor: Available
Python Executor: Available
Blocks loaded: 24,486
```

---

### Project Structure

```
my-naab-project/
├── src/
│   ├── main.naab          # Main program
│   ├── utils.naab         # Utility functions
│   └── config.naab        # Configuration
├── blocks/                # Custom blocks
│   ├── BLOCK-CPP-CUSTOM.json
│   └── BLOCK-JS-HELPER.json
└── data/
    └── input.csv          # Data files
```

---

## Your First Program

### Hello World

Create `hello.naab`:

```naab
main {
    print("Hello, World!")
}
```

Run:

```bash
$ naab-lang run hello.naab
Hello, World!
```

### Hello with Variables

```naab
main {
    let name = "Alice"
    let greeting = "Hello, " + name + "!"
    print(greeting)
}
```

Output:
```
Hello, Alice!
```

### Hello with Functions

```naab
fn greet(name: string) -> string {
    return "Hello, " + name + "!"
}

main {
    let message = greet("Bob")
    print(message)
}
```

---

## Language Basics

### Variables

#### Declaration

```naab
# Basic declaration with type
let age: int = 25
let name: string = "Alice"
let score: double = 95.5

# Type inference
let count = 10           # Inferred as int
let message = "hello"    # Inferred as string
```

#### Mutability

```naab
# Variables are mutable
let x = 10
x = 20  # OK

# Constants (planned)
const PI = 3.14159
# PI = 3.14  # Error: Cannot reassign constant
```

---

### Data Types

#### Primitives

```naab
let integer: int = 42
let floating: double = 3.14
let flag: bool = true
let text: string = "hello"
```

#### Arrays

```naab
# Array literal
let numbers: array<int> = [1, 2, 3, 4, 5]

# Access elements
let first = numbers[0]  # Planned

# Nested arrays
let matrix: array<array<int>> = [
    [1, 2, 3],
    [4, 5, 6]
]
```

#### Maps

```naab
# Map literal
let ages: map<string, int> = {
    "Alice": 30,
    "Bob": 25,
    "Charlie": 35
}

# Access values
let alice_age = ages["Alice"]  # Planned
```

---

### Functions

#### Basic Function

```naab
fn add(a: int, b: int) -> int {
    return a + b
}

main {
    let sum = add(10, 20)
    print(sum)  # 30
}
```

#### Default Parameters

```naab
fn greet(name: string = "World", prefix: string = "Hello") -> string {
    return prefix + ", " + name + "!"
}

main {
    print(greet())                    # "Hello, World!"
    print(greet("Alice"))             # "Hello, Alice!"
    print(greet("Bob", "Hi"))         # "Hi, Bob!"
}
```

#### Return Values

```naab
fn divide(a: int, b: int) -> double {
    return a / b
}

fn get_status() -> string {
    return "OK"
}

# Void return (no return value)
fn log_message(msg: string) -> void {
    print(msg)
}
```

---

### Control Flow

#### If Statements

```naab
fn check_age(age: int) -> string {
    if age < 18 {
        return "Minor"
    } else {
        return "Adult"
    }
}

main {
    let status = check_age(25)
    print(status)  # "Adult"
}
```

#### While Loops

```naab
fn count_to_five() -> void {
    let i = 1
    while i <= 5 {
        print(i)
        i = i + 1
    }
}
```

#### For Loops (Planned)

```naab
fn iterate_array() -> void {
    let numbers = [1, 2, 3, 4, 5]
    for num in numbers {
        print(num)
    }
}
```

---

### Operators

#### Arithmetic

```naab
let sum = a + b          # Addition
let diff = a - b         # Subtraction
let product = a * b      # Multiplication
let quotient = a / b     # Division
let remainder = a % b    # Modulo
```

#### Comparison

```naab
let equal = a == b       # Equality
let not_equal = a != b   # Inequality
let less = a < b         # Less than
let less_eq = a <= b     # Less than or equal
let greater = a > b      # Greater than
let greater_eq = a >= b  # Greater than or equal
```

#### Logical

```naab
let and_result = a && b  # Logical AND
let or_result = a || b   # Logical OR
let not_result = !a      # Logical NOT
```

---

## Working with Blocks

### What are Blocks?

**Blocks** are reusable code units written in C++, JavaScript, or Python that can be imported and used in NAAb programs.

### Importing Blocks

```naab
# Import block with alias
use BLOCK-CPP-MATH as math

main {
    let sum = math.add(10, 20)
    print(sum)  # 30
}
```

### Finding Blocks

```bash
# List all available blocks
$ naab-lang list-blocks

# Search for blocks
$ naab-lang search-blocks "math"

# Show block details
$ naab-lang show-block BLOCK-CPP-MATH
```

### Using Block Functions

```naab
use BLOCK-CPP-MATH as math

main {
    # Call block functions
    let sum = math.add(10, 20)
    let product = math.multiply(5, 6)
    let power = math.power(2.0, 8.0)

    print(sum)       # 30
    print(product)   # 30
    print(power)     # 256.0
}
```

---

### Creating Custom Blocks

#### C++ Block

Create `my-math-block.json`:

```json
{
  "id": "BLOCK-CPP-MY-MATH",
  "name": "My Math Utilities",
  "language": "cpp",
  "code": "extern \"C\" {\n    int factorial(int n) {\n        if (n <= 1) return 1;\n        return n * factorial(n - 1);\n    }\n}",
  "category": "math",
  "validation_status": "validated"
}
```

Use in program:

```naab
use BLOCK-CPP-MY-MATH as mymath

main {
    let result = mymath.factorial(5)
    print(result)  # 120
}
```

#### JavaScript Block

```json
{
  "id": "BLOCK-JS-UTILS",
  "name": "JavaScript Utilities",
  "language": "javascript",
  "code": "function reverse_string(s) { return s.split('').reverse().join(''); }",
  "category": "string"
}
```

Use:

```naab
use BLOCK-JS-UTILS as jsutils

main {
    let reversed = jsutils.reverse_string("hello")
    print(reversed)  # "olleh"
}
```

#### Python Block

```json
{
  "id": "BLOCK-PY-DATA",
  "name": "Python Data Processing",
  "language": "python",
  "code": "def process_list(items):\n    return [x * 2 for x in items]",
  "category": "data"
}
```

Use:

```naab
use BLOCK-PY-DATA as pydata

main {
    let doubled = pydata.process_list([1, 2, 3, 4])
    print(doubled)  # [2, 4, 6, 8]
}
```

---

## Standard Library

### Importing Modules

```naab
import string
import json
import http

main {
    # Now can use string.*, json.*, http.*
}
```

### String Module

```naab
import string

main {
    let text = "  Hello, World!  "

    # String operations
    let len = string.length(text)           # 17
    let trimmed = string.trim(text)         # "Hello, World!"
    let upper = string.upper(trimmed)       # "HELLO, WORLD!"
    let lower = string.lower(upper)         # "hello, world!"

    # Splitting and joining
    let parts = string.split("a,b,c", ",")  # ["a", "b", "c"]
    let joined = string.join(parts, "|")    # "a|b|c"

    # Searching
    let has = string.contains(text, "World")     # true
    let starts = string.starts_with(text, "  H") # true
    let ends = string.ends_with(text, "!  ")     # true
}
```

### JSON Module

```naab
import json

main {
    # Parse JSON
    let data = json.parse('{"name": "Alice", "age": 30}')

    # Stringify JSON
    let obj = {"city": "New York", "zip": 10001}
    let text = json.stringify(obj)
    print(text)  # {"city":"New York","zip":10001}
}
```

### HTTP Module

```naab
import http

main {
    # GET request
    let response = http.get("https://api.github.com/users/octocat")
    let data = json.parse(response)
    print(data)
}
```

---

## Cross-Language Programming

### Combining Languages

NAAb allows you to seamlessly call code written in different languages:

```naab
# Python for data processing
use BLOCK-PY-PROCESS as process

# C++ for performance
use BLOCK-CPP-COMPUTE as compute

# JavaScript for formatting
use BLOCK-JS-FORMAT as format

main {
    # Python processes the data
    let data = process.load_csv("data.csv")

    # C++ does heavy computation
    let results = compute.analyze(data)

    # JavaScript formats the output
    let formatted = format.create_report(results)

    print(formatted)
}
```

### Language-Specific Features

#### When to Use C++

- **Performance-critical code**: Numerical computations, algorithms
- **System integration**: File I/O, network operations
- **Existing C++ libraries**: LLVM, Clang, Boost

**Example:**
```naab
use BLOCK-CPP-MATRIX as matrix

main {
    let result = matrix.multiply_large_matrices(a, b)
}
```

#### When to Use JavaScript

- **String manipulation**: Text processing, formatting
- **JSON handling**: API responses, configuration
- **Quick prototyping**: Fast iteration

**Example:**
```naab
use BLOCK-JS-TEXT as text

main {
    let html = text.generate_html_report(data)
}
```

#### When to Use Python

- **Data science**: NumPy, pandas-style operations
- **Machine learning**: Model inference
- **Scripting**: File processing, automation

**Example:**
```naab
use BLOCK-PY-ML as ml

main {
    let prediction = ml.predict(model, features)
}
```

---

## Best Practices

### Code Organization

#### Separate Concerns

```naab
# config.naab
fn get_api_url() -> string {
    return "https://api.example.com"
}

# data.naab
fn fetch_data() -> string {
    import http
    let url = get_api_url()
    return http.get(url)
}

# main.naab
import json

main {
    let response = fetch_data()
    let data = json.parse(response)
    process_data(data)
}
```

#### Use Meaningful Names

```naab
# Bad
let x = 10
let f = fn(a) { return a * 2 }

# Good
let user_count = 10
let double_value = fn(value: int) -> int {
    return value * 2
}
```

---

### Error Handling

#### Check Return Values

```naab
import json

fn safe_parse(text: string) -> map<string, any> {
    # TODO: Add try-catch when available
    return json.parse(text)
}
```

#### Validate Input

```naab
fn divide(a: int, b: int) -> double {
    if b == 0 {
        print("Error: Division by zero")
        return 0.0
    }
    return a / b
}
```

---

### Performance Tips

#### Use Appropriate Types

```naab
# Use int for counters (faster than double)
let count: int = 0

# Use double only when needed
let ratio: double = 3.14159
```

#### Minimize Cross-Language Calls

```naab
# Bad: Multiple calls
for i in range(1000) {
    let result = cpp_block.compute(i)  # 1000 cross-language calls
}

# Good: Batch processing
let results = cpp_block.compute_batch([1..1000])  # 1 call
```

#### Cache Block Results

```naab
# Bad: Repeated expensive calls
let r1 = expensive_block.compute(data)
let r2 = expensive_block.compute(data)  # Duplicate work

# Good: Cache results
let cached_result = expensive_block.compute(data)
let r1 = cached_result
let r2 = cached_result
```

---

## Troubleshooting

### Common Errors

#### "Block not found"

**Error:**
```
Error: Block BLOCK-CPP-MATH not found
```

**Solutions:**
1. Check block ID spelling
2. Verify block exists: `naab-lang show-block BLOCK-CPP-MATH`
3. Check database path

---

#### "Type mismatch"

**Error:**
```
Error: Type mismatch: expected int, got string
```

**Solutions:**
1. Check function signature
2. Convert types explicitly
3. Use type annotations

---

#### "Undefined variable"

**Error:**
```
Error: Undefined variable 'calcuate'
  Suggestion: Did you mean 'calculate'?
```

**Solutions:**
1. Check spelling
2. Ensure variable is declared
3. Check scope

---

#### "Compilation failed" (C++ blocks)

**Error:**
```
fatal error: 'llvm/IR/Value.h' file not found
```

**Solutions:**
1. Install missing library: `pkg install llvm`
2. Check library detection: Run with debug flags
3. Add library to system path

---

### Debug Mode

```bash
# Run with verbose output
$ naab-lang run --verbose program.naab

# Show compilation commands
$ naab-lang run --debug-compile program.naab
```

---

### Getting Help

#### Built-in Help

```bash
# General help
$ naab-lang help

# Command-specific help
$ naab-lang help run
$ naab-lang help repl
```

#### Documentation

- **Architecture**: [ARCHITECTURE.md](ARCHITECTURE.md)
- **API Reference**: [API_REFERENCE.md](API_REFERENCE.md)
- **Tutorials**: [tutorials/](tutorials/)

#### Community

- **GitHub Issues**: Report bugs and feature requests
- **Discussions**: Ask questions and share examples

---

## Next Steps

### Tutorials

1. [Tutorial 1: Hello World](tutorials/01-hello-world.md)
2. [Tutorial 2: Variables and Functions](tutorials/02-variables-functions.md)
3. [Tutorial 3: Working with Blocks](tutorials/03-working-with-blocks.md)
4. [Tutorial 4: Standard Library](tutorials/04-standard-library.md)
5. [Tutorial 5: Cross-Language Programming](tutorials/05-cross-language.md)
6. [Tutorial 6: Building a Complete Application](tutorials/06-complete-app.md)

### Examples

Explore the `examples/` directory for:
- **hello_world.naab**: Basic program
- **calculator.naab**: Function examples
- **web_scraper.naab**: HTTP and JSON
- **data_pipeline.naab**: Cross-language data processing
- **api_server.naab**: Building an API (planned)

### Advanced Topics

- **Custom Block Creation**: Build your own reusable blocks
- **Performance Optimization**: Profiling and optimization techniques
- **Testing**: Unit testing NAAb programs (planned)
- **Deployment**: Distributing NAAb applications (planned)

---

**Happy Coding with NAAb!** 🚀

*For detailed API information, see [API_REFERENCE.md](API_REFERENCE.md)*
*For system architecture, see [ARCHITECTURE.md](ARCHITECTURE.md)*
