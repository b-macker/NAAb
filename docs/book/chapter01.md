# Chapter 1: Introduction to NAAb

## 1.1 What is NAAb? The Orchestration Philosophy

NAAb (Not Another Assembly Block) is a modern, high-performance programming language designed from the ground up to excel in **polyglot orchestration**. In today's complex software landscape, no single language is perfectly suited for every task. NAAb embraces this reality by providing a seamless, type-safe environment to integrate and execute code written in multiple programming languages—all within a single, cohesive application.

The core philosophy of NAAb can be summarized as: **Orchestrate in NAAb, Compute in the Best Language for the Task.**

This means you leverage NAAb's robust control flow, module system, and fast native Standard Library for the overall application logic, data flow, and glue code. For computationally intensive tasks, domain-specific logic, or to tap into existing rich ecosystems (like Python's data science libraries or C++'s performance), you seamlessly invoke code from those languages directly within your NAAb program.

## 1.2 Key Features at a Glance

*   **Polyglot by Design**: Execute code blocks in C++, JavaScript, Python, Bash, Go, Rust, Ruby, and C# natively.
*   **Type-Safe Composition**: NAAb's type system helps validate the compatibility of data and function calls across language boundaries, catching errors early.
*   **Native Performance**: A lightning-fast C++ core and Standard Library (stdlib) ensure that NAAb's orchestration layer introduces minimal overhead.
*   **Block Assembly**: A powerful package management system allows you to discover, integrate, and reuse pre-built code blocks from a vast registry, fostering modularity and rapid development.
*   **Pipeline Syntax**: A clear `|>` operator for chaining operations, enhancing readability and functional composition.
*   **Modern Language Constructs**: Features like structs, first-class functions, and robust error handling provide a familiar yet powerful development experience.

## 1.3 Setting Up Your NAAb Development Environment

NAAb is built to be lean and efficient. The primary tool for development is the `naab-lang` command-line interpreter.

### 1.3.1 Installation

To get NAAb up and running, follow these steps. This assumes you have `git` and `cmake` installed on your system.

1.  **Clone the Repository**:
    ```bash
    git clone https://github.com/naab-lang/naab.git
    cd naab
    ```
    *(Note: The exact repository path may vary based on your system setup.)*

2.  **Build from Source**:
    NAAb uses `cmake` for its build system.
    ```bash
    cmake -B build
    cmake --build build -j$(nproc) # Use all available cores for faster build
    ```
    This will compile the `naab-lang` interpreter and associated tools into the `build/` directory.

3.  **Verify Installation**:
    You can directly run the interpreter from the `build/` directory.
    ```bash
    ./build/naab-lang --version
    ```
    You should see output indicating the NAAb version.

### 1.3.2 The NAAb REPL (Read-Eval-Print Loop)

For interactive experimentation and quick testing of NAAb code, you can use the `naab-repl` tool, also found in the `build/` directory.

```bash
./build/naab-repl
```
Once in the REPL, you can type NAAb expressions and statements:
```naab
> let x = 10
> print(x * 2)
20
> "Hello, " + "World!"
Hello, World!
```

## 1.4 Your First NAAb Program: "Hello, World!"

Let's write the canonical first program to ensure everything is working correctly.

Create a new file named `hello.naab` with the following content:

```naab
// hello.naab
main {
    print("Hello, NAAb!")
}
```

Now, execute this program using the `naab-lang` interpreter:

```bash
./build/naab-lang run hello.naab
```

You should see the output:

```
Hello, NAAb!
```

If you encounter any errors during installation or execution, refer to the project's `README.md` or seek assistance from the NAAb community forums.

### 1.4.1 Understanding the Entry Point: `main {}`

**IMPORTANT:** NAAb uses `main { ... }` as the program entry point. This is a special top-level construct, **not a function definition**.

✅ **Correct syntax:**
```naab
main {
    print("Hello, NAAb!")
}
```

❌ **Common mistake (will fail):**
```naab
fn main() {  // ERROR: Parser expects a function name after 'fn'
    print("Hello, NAAb!")
}
```

**Why this design?**

The `main` block is a special language construct that serves as the entry point. It's not a function you define—it's a keyword that marks where program execution begins. This design choice:

1. **Simplifies syntax** - No need to declare parameter types or return values for the entry point
2. **Distinguishes purpose** - Makes it immediately clear that `main` is special, not just another function
3. **Reduces verbosity** - `main { ... }` is cleaner than `fn main() -> void { ... }`

If you see an error like:
```
Parse error: Expected function name Got: 'main'
```

This means you've written `fn main()` when you should use `main {}`.

**Verification:** See `docs/book/verification/ch01_intro/hello.naab` for a working example.

## 1.5 The NAAb Compilation and Execution Model

NAAb programs are interpreted, meaning the `naab-lang` tool reads your source code and executes it directly. However, for polyglot blocks (which we'll explore in Part II), NAAb dynamically compiles or executes code in its native environment.

At a high level, the process is:

1.  **Lexing**: The `naab-lang` tool scans your `.naab` file, breaking it down into a stream of tokens (keywords, identifiers, operators, etc.).
2.  **Parsing**: These tokens are then structured into an Abstract Syntax Tree (AST), which represents the hierarchical structure of your program.
3.  **Semantic Analysis**: The AST is checked for type correctness, variable scope, and other semantic rules.
4.  **Interpretation**: The interpreter traverses the AST, executing the operations defined in your program.
    *   For native NAAb code, this is direct execution within the NAAb runtime.
    *   For `<<lang` blocks, the interpreter hands off the code to the relevant language-specific executor (e.g., a Python interpreter, a C++ compiler, a JavaScript engine).
5.  **Memory Management**: NAAb employs automatic garbage collection. This means you, as the developer, do not need to manually allocate or free memory for most objects. The runtime automatically reclaims memory that is no longer in use, simplifying development and reducing memory-related bugs.

This hybrid approach allows NAAb to maintain fast native execution for its core logic while providing unparalleled flexibility for integrating diverse language ecosystems.

## 1.6 Common Beginner Mistakes

When learning NAAb, developers coming from other languages often encounter a few common pitfalls. Here are the most frequent mistakes and how to avoid them:

### 1.6.1 Using `fn main()` Instead of `main {}`

❌ **Mistake:**
```naab
fn main() {  // ERROR!
    print("Hello!")
}
```

✅ **Correct:**
```naab
main {
    print("Hello!")
}
```

**Why:** As explained in Section 1.4.1, `main` is a special entry point construct, not a function.

---

### 1.6.2 Forgetting to Import Standard Library Modules

❌ **Mistake:**
```naab
main {
    io.write("Hello!\n")  // ERROR: Undefined variable: io
}
```

✅ **Correct:**
```naab
use io

main {
    io.write("Hello!\n")
}
```

**Why:** All modules, including standard library modules (io, string, json, etc.), must be imported with `use` statements. They are not automatically available.

---

### 1.6.3 Using Unquoted Keys in Dictionary Literals

❌ **Mistake:**
```naab
let person = {
    name: "Alice",  // ERROR: Undefined variable: name
    age: 30
}
```

✅ **Correct:**
```naab
let person = {
    "name": "Alice",
    "age": 30
}
```

**Why:** Dictionary keys must be quoted strings in NAAb. Unquoted identifiers are treated as variable references. We'll cover this in detail in Chapter 2.

---

### 1.6.4 Using Dot Notation to Access Dictionary Values

❌ **Mistake:**
```naab
let data = {"key": "value"}
print(data.key)  // ERROR: This doesn't work for dictionaries
```

✅ **Correct:**
```naab
let data = {"key": "value"}
print(data["key"])  // Use bracket notation
```

**Why:** Dot notation (`.`) is for struct fields, not dictionary keys. Use bracket notation `["key"]` for dictionaries. See Chapter 2 for the distinction.

---

### 1.6.5 Assuming Polyglot Blocks Don't Work in Functions

**This is not a mistake—it works!** A common misconception is that polyglot blocks can only be used in `main {}`. This is false.

✅ **Polyglot blocks work everywhere:**
```naab
fn calculate() -> int {
    let result = <<python
x = 10
y = 20
x + y
    >>
    return result
}

export fn process_data(data: list<int>) -> float {
    let avg = <<python[data]
import numpy as np
np.array(data).mean()
    >>
    return avg
}
```

Polyglot blocks can be used in:
- The `main {}` block
- Local functions
- Exported functions
- Functions in external module files

We'll explore polyglot programming in depth in Chapter 5.

---

## Quick Tips for Success

1. **Use `main {}` not `fn main()`** - The entry point is a special construct
2. **Always import modules** - Even stdlib modules need `use` statements
3. **Quote dictionary keys** - Use `{"key": value}` syntax
4. **Bracket notation for dicts** - Use `dict["key"]` not `dict.key`
5. **Read error messages carefully** - NAAb provides helpful hints in error messages
6. **Check the verification tests** - `docs/book/verification/` has working examples for every feature

---

**Next Chapter:** In [Chapter 2](chapter02.md), we'll dive deep into NAAb's type system, including variables, primitives, collections (arrays and dictionaries), and user-defined structs. We'll clarify the important distinction between structs (which use dot notation) and dictionaries (which use bracket notation).
