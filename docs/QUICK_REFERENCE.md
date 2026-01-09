# NAAb Quick Reference

**One-page reference for NAAb syntax and features**

---

## Basic Syntax

```naab
# Comments start with #

# Variables
let x: int = 42
let name = "Alice"        # Type inference

# Functions
fn add(a: int, b: int) -> int {
    return a + b
}

# Default parameters
fn greet(name: string = "World") -> string {
    return "Hello, " + name + "!"
}

# Main entry point
main {
    print("Hello, NAAb!")
}
```

---

## Types

| Type | Example | Description |
|------|---------|-------------|
| `int` | `42` | 64-bit integer |
| `double` | `3.14` | 64-bit float |
| `bool` | `true`, `false` | Boolean |
| `string` | `"hello"` | UTF-8 string |
| `array<T>` | `[1, 2, 3]` | Array of type T |
| `map<K,V>` | `{"a": 1}` | Key-value map |
| `void` | - | No return value |

---

## Operators

```naab
# Arithmetic
a + b    # Addition
a - b    # Subtraction
a * b    # Multiplication
a / b    # Division
a % b    # Modulo

# Comparison
a == b   # Equality
a != b   # Inequality
a < b    # Less than
a > b    # Greater than
a <= b   # Less than or equal
a >= b   # Greater than or equal

# Logical
a && b   # AND
a || b   # OR
!a       # NOT
```

---

## Control Flow

```naab
# If statement
if condition {
    # true branch
} else {
    # false branch
}

# While loop
while condition {
    # loop body
}

# For loop (planned)
for item in collection {
    # loop body
}
```

---

## Standard Library

```naab
import string
import json
import http

# String module
string.length(s)              # Get length
string.trim(s)                # Remove whitespace
string.upper(s)               # To uppercase
string.lower(s)               # To lowercase
string.split(s, delim)        # Split by delimiter
string.join(arr, delim)       # Join array
string.contains(s, substr)    # Check substring
string.starts_with(s, prefix) # Check prefix
string.ends_with(s, suffix)   # Check suffix
string.replace(s, old, new)   # Replace all

# JSON module
json.parse(text)              # Parse JSON string
json.stringify(obj)           # Convert to JSON

# HTTP module
http.get(url)                 # HTTP GET request
http.post(url, body)          # HTTP POST (planned)
```

---

## Blocks

```naab
# Import a block
use BLOCK-CPP-MATH as math

# Call block function
let result = math.add(10, 20)

# Create custom block (JSON)
{
  "id": "BLOCK-CPP-MY-FUNC",
  "language": "cpp",
  "code": "extern \"C\" { int func() { return 42; } }"
}
```

---

## Commands

```bash
# Run program
naab-lang run program.naab

# Start REPL
naab-repl

# List blocks
naab-lang list-blocks

# Show block details
naab-lang show-block BLOCK-CPP-MATH

# Version info
naab-lang version
```

---

## Examples

### Hello World
```naab
main {
    print("Hello, World!")
}
```

### Variables & Functions
```naab
fn double(x: int) -> int {
    return x * 2
}

main {
    let result = double(21)
    print(result)  # 42
}
```

### Using Standard Library
```naab
import string

main {
    let text = "hello"
    let upper = string.upper(text)
    print(upper)  # "HELLO"
}
```

### Using Blocks
```naab
use BLOCK-CPP-MATH as math

main {
    let sum = math.add(10, 20)
    print(sum)  # 30
}
```

### Cross-Language
```naab
use BLOCK-CPP-FAST as cpp
use BLOCK-JS-FORMAT as js
use BLOCK-PY-DATA as py

main {
    let data = py.load([1, 2, 3])
    let result = cpp.process(data)
    let output = js.format(result)
    print(output)
}
```

---

## Common Patterns

### String Processing
```naab
import string

let text = "  Hello, World!  "
let cleaned = string.trim(text)
let parts = string.split(cleaned, ", ")
let joined = string.join(parts, " | ")
```

### JSON Parsing
```naab
import json
import http

let response = http.get("https://api.example.com/data")
let data = json.parse(response)
# Use data...
```

### Error Handling (Basic)
```naab
fn safe_divide(a: int, b: int) -> double {
    if b == 0 {
        print("Error: Division by zero")
        return 0.0
    }
    return a / b
}
```

---

## Tips

✅ **Do:**
- Use type annotations for clarity
- Use meaningful variable names
- Import only needed modules
- Cache expensive block calls

❌ **Don't:**
- Make excessive cross-language calls in loops
- Use magic numbers (use constants)
- Ignore return values
- Mix tabs and spaces

---

## Getting Help

- **User Guide**: [USER_GUIDE.md](USER_GUIDE.md)
- **API Reference**: [API_REFERENCE.md](API_REFERENCE.md)
- **Architecture**: [ARCHITECTURE.md](ARCHITECTURE.md)
- **Tutorials**: [tutorials/](tutorials/)

---

**Quick Reference v0.1.0** | [Full Documentation](README.md)
