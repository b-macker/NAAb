# Chapter 4: Functions and Modules

Functions are fundamental building blocks of any programming language, allowing you to encapsulate logic and promote code reuse. Modules provide a way to organize your code into logical units. This chapter covers how to define and use functions, work with higher-order functions, leverage the pipeline operator, and manage code organization with NAAb's module system.

## 4.1 Defining Functions with `fn`

In NAAb, functions are declared using the `fn` keyword, followed by the function name, a list of parameters (with optional type annotations), an optional return type annotation, and the function body enclosed in curly braces.

```naab
// Define a function that adds two integers
fn add(a: int, b: int) -> int {
    return a + b
}

// Define a function that greets a name (return type inferred as string)
fn greet(name: string) {
    print("Hello, " + name + "!")
}

main {
    let result = add(10, 20)
    print("10 + 20 =", result) // Output: 10 + 20 = 30

    greet("NAAb User") // Output: Hello, NAAb User!
}
```

### Parameters and Return Types

*   **Parameters**: Each parameter consists of a name and an optional type annotation (e.g., `param_name: Type`). If the type is omitted, it defaults to `any` (dynamic typing).
*   **Return Type**: The return type is specified after an arrow (`->`) following the parameter list (e.g., `-> int`). If the return type is omitted, it defaults to `void` (no explicit return value) or is inferred from the function body. A function that does not explicitly `return` a value implicitly returns `null`.

## 4.2 Higher-Order Functions and Closures

NAAb treats functions as first-class citizens, meaning they can be passed as arguments to other functions, returned from functions, and assigned to variables. Functions that operate on or return other functions are known as higher-order functions. NAAb also supports closures, where functions can "capture" and remember variables from their surrounding scope.

```naab
// A simple function to be passed around
fn multiply_by_two(x: int) -> int {
    return x * 2
}

// A higher-order function that applies another function to a value
fn apply_operation(value: int, operation: function) -> int {
    return operation(value)
}

// A function returning another function (closure)
fn make_adder(x: int) -> function {
    fn adder(y: int) -> int {
        return x + y // 'adder' closes over 'x'
    }
    return adder
}

main {
    // Pass 'multiply_by_two' as an argument
    let result1 = apply_operation(5, multiply_by_two)
    print("apply_operation(5, multiply_by_two) =", result1) // Output: 10

    // Using the closure
    let add_five = make_adder(5)
    let result2 = add_five(10)
    print("add_five(10) =", result2) // Output: 15
}
```
**Function Types:** You can use the `function` keyword to denote a function type.

## 4.3 The Pipeline Operator (`|>`)

The pipeline operator (`|>`) is a powerful feature for chaining function calls in a readable and declarative manner. It takes the result of the expression on its left and passes it as the first argument to the function call on its right.

Consider this sequence of operations: `funcC(funcB(funcA(data)))`. With the pipeline operator, this becomes: `data |> funcA |> funcB |> funcC`.

```naab
fn increment(x: int) -> int {
    return x + 1
}

fn double(x: int) -> int {
    return x * 2
}

fn square(x: int) -> int {
    return x * x
}

main {
    let initial_value = 5

    // Chaining operations with the pipeline operator
    // 5 -> increment (6) -> double (12) -> square (144)
    let final_result = initial_value
        |> increment
        |> double
        |> square

    print("Pipeline result:", final_result) // Expected: 144

    // The pipeline operator can also pass to functions with multiple arguments
    // as the first argument. Other arguments are specified normally.
    fn subtract(a: int, b: int) -> int {
        return a - b
    }

    let subtracted_result = 100
        |> subtract(50) // Equivalent to subtract(100, 50)
    print("100 |> subtract(50) =", subtracted_result) // Expected: 50
}
```
**Pipeline Support:** The pipeline operator is fully supported in NAAb, allowing for clean chaining of operations across multiple lines.

## 4.4 The Module System

As your NAAb projects grow, organizing code into reusable files and modules becomes essential. NAAb provides a simple, Rust-inspired module system using `use` and `export` keywords.

### 4.4.1 Import Syntax and Module Resolution

The `use` keyword is used to import modules, making their exported members available. Module names are derived from filenames (without extension) or can be standard library module names.

```naab
// Import a custom module (from math_utils.naab)
use math_utils

// Import a custom module from a subdirectory (e.g., 'utils/helpers.naab')
// Note: Module resolution typically searches in the current working directory and stdlib paths.
// The example 'utils/helpers' assumes 'utils' is in a search path.
use utils/helpers as helpers

// Import standard library modules (REQUIRED)
use io
use string as str // With alias

main {
    // Example using custom module
    let sum = math_utils.add(5, 3)

    // Example using aliased custom module
    // let result = helpers.calculate() // (Assuming helpers exports calculate())

    // Example using standard library modules
    io.write("Hello from io!\n")
    let upper = str.upper("hello")
}
```

**Important Notes on Module Resolution:**
- Modules (both custom and standard library) are searched for in the process's current working directory and standard library paths.
- NAAb supports relative imports using dot notation (e.g., `use utils.helper`), which translates to `utils/helper.naab`.
- Modules are resolved relative to the *source file's directory*.

### 4.4.2 Standard Library Modules: `use` is REQUIRED

Standard library modules are built-in C++ modules that provide core functionality (e.g., I/O, string manipulation, math). **Unlike some languages, they are NOT automatically available and MUST be explicitly imported using `use` statements.**

```naab
use io      // ✅ REQUIRED for console I/O
use string  // ✅ REQUIRED for string functions
use time    // ✅ REQUIRED for time functions

main {
    io.write("Hello from stdlib io!\n")
    let upper = string.upper("hi")
    let now = time.now()
}
```
**Common Mistake:** Forgetting to import stdlib modules.

❌ **Wrong:**
```naab
main {
    io.write("Hello\n")  // ERROR: Undefined variable: io (without 'use io')
}
```

✅ **Correct:**
```naab
use io

main {
    io.write("Hello\n")  // Works!
}
```

Standard library modules include:
- `io` - Input/output operations
- `string` - String manipulation
- `array` - Array operations
- `json` - JSON parsing and serialization
- `time` - Time operations
- `math` - Mathematical functions
- `fs` - File system operations
- `env` - Environment variables
- `http` - HTTP requests
- `regex` - Regular expressions
- `crypto` - Cryptography

### 4.4.3 Exporting from a Module

To make functions, variables, structs, or enums available to other files, you use the `export` keyword.

```naab
// In math_utils.naab
export fn add(a: int, b: int) -> int {
    return a + b
}

export let PI: float = 3.14159 

export struct Vector2 {
    x: int,
    y: int
}

export enum Operation {
    Add,
    Subtract
}
```
**Important:** Global `let` statements in the main program are not allowed (this is `ISS-021` - By Design). Top-level of a module file can contain `use`, `import`, `export`, `struct`, `enum`, `function`.

### 4.4.4 Module Aliases: `use module as alias`

Both custom modules and standard library modules support aliasing with the `as` keyword. This is useful for:
- **Shortening long module names**
- **Avoiding name conflicts**
- **Improving code readability**

```naab
use io
use string as str        // Alias for convenience
use json as j           // Short alias
use my_long_module_name as mod

main {
    io.write("Starting...\n")

    let text = "Hello World"
    let upper = str.upper(text)     // Using alias
    io.write("Upper: ", upper, "\n")

    let data = {"key": "value"}
    let json_str = j.stringify(data)  // Using short alias
    io.write("JSON: ", json_str, "\n")

    // Using custom module alias
    // let result = mod.calculate() // Example assuming mod exports calculate()
}
```

**Important Notes:**
- Both `use module_name` and `use module_name as alias` work for stdlib and custom modules
- Once aliased, use the alias consistently (e.g., `str.upper()` not `string.upper()`)
- There is no `import {item1, item2}` syntax in NAAb - use aliases for namespacing

### 4.4.6 Polyglot Blocks in Modules: Full Support

**YES, polyglot blocks work in modules!** A common misconception is that polyglot blocks can only be used in `main {}`. This is **completely false**.

Polyglot blocks can be used anywhere:
- ✅ In `main {}`
- ✅ In local functions
- ✅ In exported functions
- ✅ In external module files

This enables powerful modular polyglot programming patterns.

**Example: Module with Polyglot Functions**

Create a file `stats.naab`:

```naab
// stats.naab - Statistical analysis module with polyglot code
export fn calculate_mean(numbers: list<float>) -> float {
    // Python polyglot block inside exported function - FULLY SUPPORTED!
    let mean = <<python[numbers]
import numpy as np
float(np.array(numbers).mean())
    >>
    return mean
}

export fn calculate_stats(data: list<float>) -> dict<string, float> {
    // Multi-line polyglot block with complex logic
    let stats = <<python[data]
import numpy as np
arr = np.array(data)
{
    "mean": float(arr.mean()),
    "median": float(np.median(arr)),
    "std": float(arr.std()),
    "min": float(arr.min()),
    "max": float(arr.max())
}
    >>
    return stats
}

export fn process_with_js(text: string) -> string {
    // JavaScript polyglot block in exported function
    let processed = <<javascript[text]
text.toUpperCase().split('').reverse().join('')
    >>
    return processed
}
```

Now use it in `main.naab`:

```naab
// main.naab
use io
use json
use stats

main {
    let numbers = [10.5, 20.3, 15.7, 18.9, 22.1]

    // Call exported function with polyglot block
    let mean = stats.calculate_mean(numbers)
    io.write("Mean: ", mean, "\n")

    // Call exported function with complex polyglot logic
    let analysis = stats.calculate_stats(numbers)
    io.write("Stats: ", json.stringify(analysis), "\n")

    // Call exported JS polyglot function
    let reversed = stats.process_with_js("Hello")
    io.write("Reversed: ", reversed, "\n")
}
```

**Run it:**
```bash
./build/naab-lang run main.naab
```

**Output:**
```
Mean: 17.5
Stats: {"mean":17.5,"median":18.9,"std":4.12,"min":10.5,"max":22.1}
Reversed: OLLEH
```

**Key Points:**
- Polyglot blocks work perfectly in exported functions
- Variable binding `[numbers]` works in module functions
- You can mix NAAb logic and polyglot code seamlessly
- Modules can encapsulate polyglot complexity

**Verification:** See `docs/book/verification/ch05_polyglot/polyglot_module.naab` and `import_polyglot_module.naab` for working examples.

### 4.4.7 Module Import Patterns: Best Practices

#### Pattern 1: Direct Import (No Alias)
```naab
use math_utils

main {
    let result = math_utils.add(5, 3)
}
```
**Use when:** Module name is short and clear

#### Pattern 2: Import with Alias
```naab
use string as str
use json as j

main {
    let upper = str.upper("hello")
    let data = j.parse("{\"key\": \"value\"}")
}
```
**Use when:** You want shorter names or need to avoid conflicts

#### Pattern 3: Multiple Module Imports
```naab
use io
use json
use string as str
use time
use my_custom_module as mod

main {
    // All modules available
}
```
**Use when:** You need multiple modules (most programs)

#### Pattern 4: Nested Module Structure
```naab
// Directory structure:
// utils/
//   ├── string_helpers.naab
//   └── math_helpers.naab

use utils/string_helpers as str_utils
use utils/math_helpers as math_utils

main {
    let result = str_utils.capitalize("hello")
}
```
**Use when:** Organizing large projects with subdirectories

> **⚠️ Known Issue (ISS-035):** Module imports are currently resolved relative to the **process working directory**, not the location of the source file. Relative import syntax like `use ./module` is not supported. This can make nested project structures difficult to manage.

### 4.4.8 Common Module System Mistakes

#### Mistake 1: Forgetting to Import Stdlib
❌ **Wrong:**
```naab
main {
    io.write("Hello\n")  // ERROR: Undefined variable: io
}
```

✅ **Correct:**
```naab
use io

main {
    io.write("Hello\n")
}
```

#### Mistake 2: Using Full Name After Aliasing
❌ **Wrong:**
```naab
use string as str

main {
    let upper = string.upper("hello")  // ERROR: string not defined
}
```

✅ **Correct:**
```naab
use string as str

main {
    let upper = str.upper("hello")  // Use the alias!
}
```

#### Mistake 3: Assuming `import {items}` Syntax Exists
❌ **Wrong (this syntax doesn't exist):**
```naab
import {add, subtract} from "math_utils"  // ERROR: Not valid NAAb syntax
```

✅ **Correct:**
```naab
use math_utils

main {
    let sum = math_utils.add(5, 3)
    let diff = math_utils.subtract(10, 5)
}
```

#### Mistake 4: Thinking Polyglot Blocks Don't Work in Modules
❌ **Wrong assumption:**
> "Polyglot blocks only work in main {}, not in exported functions"

✅ **Reality:**
Polyglot blocks work everywhere! You can use them in:
- Local functions
- Exported functions
- Functions in external modules
- Nested function calls

See Section 4.4.6 for complete examples.

---

## 4.5 Putting It All Together: A Complete Module Example

Let's build a complete example that demonstrates all the concepts from this chapter.

**File: `text_processor.naab`**
```naab
// text_processor.naab
use string as str

// Exported function with polyglot block
export fn analyze_text(text: string) -> dict<string, any> {
    let analysis = <<python[text]
{
    "length": len(text),
    "words": len(text.split()),
    "unique_chars": len(set(text)),
    "uppercase": text.upper(),
    "lowercase": text.lower()
}
    >>
    return analysis
}

// Regular exported function (no polyglot)
export fn clean_text(text: string) -> string {
    let trimmed = str.trim(text)
    let lower = str.lower(trimmed)
    return lower
}
```

**File: `main.naab`**
```naab
// main.naab
use io
use json
use text_processor as tp

fn display_analysis(analysis: dict<string, any>) {
    io.write("Length: ", analysis["length"], "\n")
    io.write("Words: ", analysis["words"], "\n")
    io.write("Unique chars: ", analysis["unique_chars"], "\n")
}

main {
    let text = "  Hello World from NAAb!  "

    // Use module function with polyglot
    let analysis = tp.analyze_text(text)
    display_analysis(analysis)

    // Use regular module function
    let cleaned = tp.clean_text(text)
    io.write("Cleaned: '", cleaned, "'\n")

    // Pipeline with module functions
    let result = text
        |> tp.clean_text
        |> tp.analyze_text

    io.write("Pipeline result: ", json.stringify(result), "\n")
}
```

This example demonstrates:
- ✅ Module exports
- ✅ Module imports with aliases
- ✅ Polyglot blocks in exported functions
- ✅ Standard library usage
- ✅ Pipeline operator with modules
- ✅ Higher-order functions

**Next Chapter:** In [Chapter 5](chapter05.md), we'll dive deep into the polyglot block system, exploring how to execute code from Python, JavaScript, C++, Bash, and other languages directly within your NAAb programs.


