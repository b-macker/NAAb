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

**Prerequisites:**

*   CMake 3.15+
*   C++17 compatible compiler (GCC 9+, Clang 10+)
*   Python 3.8+ with development headers (`python3-dev` on Debian/Ubuntu, `python` on Termux)
*   fmt library (`libfmt-dev` on Debian/Ubuntu, `fmt` on Termux)

**Build steps:**

1.  **Clone the Repository**:
    ```bash
    git clone https://github.com/b-macker/NAAb.git
    cd naab
    ```

2.  **Build from Source**:
    ```bash
    cmake -B build
    cmake --build build --target naab-lang -j$(nproc)
    ```
    This compiles the `naab-lang` interpreter into the `build/` directory.

3.  **Verify Installation**:
    ```bash
    ./build/naab-lang --version
    ```
    You should see output like `NAAb Block Assembly Language v0.2.0-dev`.

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

If you encounter any errors during installation or execution, check the project's `README.md` for troubleshooting tips.

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

## 1.5 Architecture and Execution Model

NAAb programs are interpreted. The `naab-lang` tool reads your source code and executes it directly. For polyglot blocks, NAAb hands off code to the appropriate language executor.

### 1.5.1 The Execution Pipeline

```
Source File (.naab)
       ↓
┌──────────────────────────────────┐
│  Lexer (src/lexer/lexer.cpp)     │  Tokenizes source into keywords,
│  Keywords, identifiers, literals │  operators, and polyglot blocks
└──────────────┬───────────────────┘
               ↓
┌──────────────────────────────────┐
│  Parser (src/parser/parser.cpp)  │  Recursive descent parser builds
│  Builds Abstract Syntax Tree     │  a tree of expression/statement nodes
└──────────────┬───────────────────┘
               ↓
┌──────────────────────────────────┐
│  Interpreter (src/interpreter/)  │  Visitor pattern traverses the AST.
│  AST Visitor + Environment       │  Native code runs directly; polyglot
│                                  │  blocks dispatch to executors.
└──────────┬───────────┬───────────┘
           ↓           ↓
┌─────────────────┐  ┌────────────────────────────┐
│  Standard       │  │  Polyglot Executors         │
│  Library        │  │  (src/runtime/)             │
│  (12 modules)   │  │  Python, JS, Bash, C++,    │
│                 │  │  Rust, Go, Ruby, C#, PHP    │
└─────────────────┘  └────────────────────────────┘
```

Each stage in the pipeline:

1.  **Lexing**: Scans your `.naab` file into a stream of tokens. Recognizes NAAb keywords, polyglot block delimiters (`<<python`, `>>`), and keyword aliases (`fn`/`func`/`function` all map to the same token).
2.  **Parsing**: Builds an Abstract Syntax Tree (AST) from the token stream. Each node represents a language construct (function declaration, if-statement, polyglot block, etc.).
3.  **Interpretation**: The interpreter walks the AST using the visitor pattern. For native NAAb code, it executes directly. For polyglot blocks (`<<python ... >>`), it dispatches to the appropriate executor.
4.  **Memory Management**: NAAb uses C++ smart pointers (RAII) for automatic memory management. You do not need to manually allocate or free memory.

### 1.5.2 Polyglot Execution

When the interpreter encounters a polyglot block, it routes execution based on the language tag:

*   **Embedded executors** (JavaScript via QuickJS, Python via CPython): Run within the NAAb process for low overhead.
*   **Subprocess executors** (Bash, Ruby, Go, PHP): Run as separate OS processes, communicating via stdin/stdout pipes.
*   **Compiled executors** (C++, Rust, C#): Compile source to a temporary binary, execute it, and capture the output.

Data flows between NAAb and polyglot blocks through a type marshalling layer that converts NAAb values (integers, strings, arrays, dictionaries) to their equivalents in the target language and back.

### 1.5.3 Project Structure

For contributors or those exploring the source:

```
language/
├── src/
│   ├── cli/              # Command-line interface (main.cpp)
│   ├── lexer/            # Tokenization (lexer.cpp)
│   ├── parser/           # Syntax analysis (parser.cpp)
│   ├── interpreter/      # Execution engine (interpreter.cpp)
│   ├── stdlib/           # Standard library modules (12 modules)
│   ├── runtime/          # Polyglot executors (9 languages)
│   └── debugger/         # Interactive debugger
├── include/naab/         # Public C++ headers
├── tools/naab-lsp/       # LSP server for IDE integration
├── vscode-naab/          # VS Code extension
├── docs/book/            # This book and verification tests
└── CMakeLists.txt        # Build configuration
```

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
