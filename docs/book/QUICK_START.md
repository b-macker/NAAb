# NAAb Quick Start Guide

**Get productive with NAAb in 5 minutes.**

This guide covers the essential syntax and patterns you need to start writing NAAb programs. For comprehensive details, see [The NAAb Programming Language Book](chapter01.md).

---

## Entry Point

NAAb programs start with a `main` block (NOT a function).

✅ **Correct:**
```naab
main {
    io.write("Hello, NAAb!\n")
}
```

❌ **Wrong:**
```naab
fn main() {  // ERROR: Parser expects a function name, not 'main'
    io.write("Hello, NAAb!\n")
}
```

**Why?** The `main` construct is a special top-level entry point, not a function definition.

---

## Data Types

### Primitives
```naab
let count: int = 42
let price: float = 19.99
let name: string = "Alice"
let active: bool = true
```

### Collections

**Arrays:**
```naab
let numbers = [1, 2, 3, 4, 5]
let first = numbers[0]  // Access by index

// Modify in place
numbers[1] = 10
```

**Dictionaries - Keys MUST be quoted strings:**
```naab
// ✅ Correct: Quoted keys
let person = {
    "name": "Alice",
    "age": 30,
    "active": true
}

// Access with bracket notation
let name = person["name"]

// ❌ Wrong: Unquoted keys
let broken = {
    name: "Alice",  // ERROR!
    age: 30
}

// ❌ Wrong: Dot notation on dicts
let name = person.name  // ERROR! Dot notation doesn't work on dictionaries
```

**Structs - Use dot notation for field access:**
```naab
struct Person {
    name: string
    age: int
}

main {
    let alice = new Person {
        name: "Alice",
        age: 30
    }

    // ✅ Correct: Dot notation for struct fields
    io.write(alice.name, "\n")
    alice.age = 31  // Modify field
}
```

### Quick Comparison: Structs vs. Dictionaries

| Feature | Struct | Dictionary |
|---------|--------|------------|
| **Keys** | Identifiers (unquoted) | Strings (quoted) |
| **Access** | `obj.field` | `dict["key"]` |
| **Type safety** | Compile-time checked | Runtime types |
| **When to use** | Known structure | Dynamic/JSON data |

---

## Functions

### Define Functions
```naab
fn add(x: int, y: int) -> int {
    return x + y
}

fn greet(name: string) -> string {
    return "Hello, " + name
}

main {
    let sum = add(5, 3)
    let greeting = greet("World")
}
```

### Export Functions (for modules)
```naab
// In calculator.naab
export fn double(x: int) -> int {
    return x * 2
}

export fn square(x: int) -> int {
    return x * x
}
```

---

## Module System

### Standard Library Modules

Built-in modules are available via `use` statements (just like custom modules):

```naab
use io
use string as str
use json
use time
use math

main {
    // ✅ Use after importing
    io.write("Hello\n")

    let upper = str.upper("hello")
    let now = time.now()
    let data = json.parse("{\"key\": \"value\"}")
    let result = math.sqrt(16)
}
```

**Available Standard Library Modules:**
- `io` - Input/output (`write`, `read`, `write_error`)
- `string` - String operations (`upper`, `lower`, `split`, `join`)
- `array` - Array operations (`push`, `pop`, `map_fn`, `filter_fn`)
- `json` - JSON parsing (`parse`, `stringify`)
- `time` - Time operations (`now`, `sleep`, `format`)
- `math` - Math functions (`sqrt`, `pow`, `abs`, `floor`, `ceil`)
- `fs` - File system (`read`, `write`, `exists`, `delete`)
- `env` - Environment variables (`get`, `set_var`)
- `http` - HTTP requests (`get`, `post`)

### Custom Modules

Use the `use module_name as alias` syntax:

```naab
// In main.naab
use calculator as calc
use utils/helpers as helpers

main {
    let result = calc.double(21)
    io.write("Result: ", result, "\n")
}
```

**Important:**
- Both custom modules and stdlib modules use `use module_name` or `use module_name as alias`
- There is no `import {item1, item2}` syntax - use aliases for namespacing
- Stdlib modules (io, string, json, etc.) are built-in but still need `use` statements

---

## Polyglot Blocks

Execute code from other languages inline!

### Simple Expression
```naab
main {
    // Python
    let result = <<python 42 + 8 >>

    // JavaScript
    let upper = <<javascript "hello".toUpperCase() >>

    io.write("Result: ", result, "\n")
    io.write("Upper: ", upper, "\n")
}
```

### With Variable Binding
Pass NAAb variables to inline code:

```naab
main {
    let count = 10
    let factor = 2.5

    // Pass variables with [var1, var2] syntax
    let result = <<python[count, factor]
count * factor
    >>

    io.write("Result: ", result, "\n")  // Output: 25.0
}
```

### Multi-line Code Blocks
```naab
main {
    let data = [1, 2, 3, 4, 5]

    let result = <<python[data]
import numpy as np
arr = np.array(data)
arr.mean() * 2
    >>

    io.write("Mean doubled: ", result, "\n")
}
```

### Polyglot Blocks in Functions

**YES, this works!** You can use polyglot blocks inside functions and exported functions:

```naab
// In stats.naab
export fn calculate_stats(numbers: list<int>) -> dict<string, float> {
    let result = <<python[numbers]
import numpy as np
arr = np.array(numbers)
{
    "mean": float(arr.mean()),
    "std": float(arr.std()),
    "sum": float(arr.sum())
}
    >>
    return result
}
```

### Supported Languages
- `python` - Python 3 (most features)
- `javascript` - JavaScript via QuickJS
- `bash` / `sh` - Shell commands
- `cpp` - C++ (inline compilation)
- `rust` - Rust (inline compilation)
- `go` - Go (inline compilation)
- `ruby` - Ruby
- `csharp` / `cs` - C#

---

## Control Flow

### Conditionals
```naab
main {
    let age = 25

    if age >= 18 {
        io.write("Adult\n")
    } else if age >= 13 {
        io.write("Teenager\n")
    } else {
        io.write("Child\n")
    }
}
```

### Loops

**While loop:**
```naab
main {
    let i = 0
    while i < 5 {
        io.write("Count: ", i, "\n")
        i = i + 1
    }
}
```

**For-in loop:**
```naab
main {
    let numbers = [10, 20, 30, 40]

    for num in numbers {
        io.write("Number: ", num, "\n")
    }

    // With range
    for i in 0..5 {  // Exclusive: 0, 1, 2, 3, 4
        io.write("Index: ", i, "\n")
    }

    for i in 0..=5 {  // Inclusive: 0, 1, 2, 3, 4, 5
        io.write("Index: ", i, "\n")
    }
}
```

---

## Common Beginner Mistakes

### 1. Using `fn main()`
❌ **Wrong:** `fn main() { ... }`
✅ **Right:** `main { ... }`

### 2. Unquoted Dictionary Keys
❌ **Wrong:** `{name: "Alice"}`
✅ **Right:** `{"name": "Alice"}`

### 3. Dot Notation on Dictionaries
❌ **Wrong:** `dict.key`
✅ **Right:** `dict["key"]`

### 4. Forgetting to Import Standard Library
❌ **Wrong:** Using `io.write()` without importing
✅ **Right:** Add `use io` at the top of your file

### 5. Thinking Polyglot Blocks Don't Work in Functions
✅ **They work perfectly!** Polyglot blocks can be used:
- In `main {}`
- In local functions `fn calculate() { ... }`
- In exported functions `export fn process() { ... }`
- In external module files

---

## Complete Example

```naab
// calculator.naab
export fn advanced_calc(numbers: list<float>) -> dict<string, float> {
    // Polyglot block in exported function - fully supported!
    let stats = <<python[numbers]
import numpy as np
arr = np.array(numbers)
{
    "mean": float(arr.mean()),
    "median": float(np.median(arr)),
    "std": float(arr.std())
}
    >>
    return stats
}
```

```naab
// main.naab
use calculator as calc

main {
    // Dictionary with quoted keys
    let config = {
        "threshold": 10.5,
        "enabled": true
    }

    // Array of numbers
    let data = [12.5, 15.0, 18.3, 22.1, 19.8]

    // Call exported function with polyglot block
    let stats = calc.advanced_calc(data)

    // Access dict values with bracket notation
    io.write("Mean: ", stats["mean"], "\n")
    io.write("Median: ", stats["median"], "\n")
    io.write("Std Dev: ", stats["std"], "\n")

    // Conditional based on threshold
    if stats["mean"] > config["threshold"] {
        io.write("Above threshold!\n")
    }
}
```

**Run it:**
```bash
./build/naab-lang run main.naab
```

---

## Next Steps

1. **Read the Book:** [Chapter 1: Introduction](chapter01.md)
2. **Try Examples:** `docs/book/verification/` has runnable examples
3. **Explore Projects:** See `docs/book/verification/ch0_full_projects/` for real applications

---

## Getting Help

- **Documentation:** See `docs/book/` for comprehensive guide
- **Issues:** Check `docs/book/verification/ISSUES.md` for known issues
- **Examples:** All code examples in the book are verified and runnable

---

**Happy coding with NAAb!** 🚀
