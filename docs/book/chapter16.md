# Chapter 16: Tooling and the Development Environment

A productive development environment relies on robust tooling. NAAb provides essential command-line tools for executing, interacting with, and managing your code. While advanced IDE integrations are under development, the core command-line interface (CLI) and Read-Eval-Print Loop (REPL) are fully functional.

## 16.1 The `naab-lang` CLI

The `naab-lang` executable is the primary interface for compiling, running, and managing NAAb projects. It supports various commands:

*   **`naab-lang run <file.naab>`**: Executes a NAAb program. This is the most common command for running your applications.
*   **`naab-lang parse <file.naab>`**: Parses a NAAb program and prints its Abstract Syntax Tree (AST). Useful for understanding how NAAb interprets your code.
*   **`naab-lang check <file.naab>`**: Performs a static type check on your NAAb program, identifying type errors without executing the code.
*   **`naab-lang validate <block1,block2,...>`**: Validates the compatibility of a sequence of blocks, ensuring their input and output types align. This is crucial for constructing robust pipelines.
*   **`naab-lang stats`**: Displays overall usage statistics for blocks and other NAAb components.
*   **`naab-lang blocks list`**: Lists statistics about all available blocks in the local registry.
*   **`naab-lang blocks search <query>`**: Searches the block registry for components matching a specific query.
*   **`naab-lang blocks info <block_id>`**: Displays detailed metadata for a specific block ID.
*   **`naab-lang blocks index [path]`**: Builds or rebuilds the search index for the block registry.
*   **`naab-lang version`**: Displays the current version of the NAAb interpreter and its build information.
*   **`naab-lang help`**: Shows a comprehensive list of all commands and their usage.

**Example Usage:**

```bash
# Run your program
naab-lang run my_app.naab

# Check a file for type errors
naab-lang check my_library.naab

# List all available blocks
naab-lang blocks list
```

## 16.2 The `naab-repl` (Read-Eval-Print Loop)

The `naab-repl` is an interactive console that allows you to execute NAAb code snippets one line at a time. It's an invaluable tool for:

*   **Experimentation**: Quickly test language features, syntax, and logic.
*   **Debugging**: Inspect variable values and test functions without running a full program.
*   **Learning**: Explore NAAb interactively.

To start the REPL:

```bash
naab-repl
```

Once inside, you can type NAAb statements and expressions:

```naab
> let x = 10
> let y = x * 2
> print(y)
20
> fn add(a, b) { return a + b }
> add(5, 7)
12
```

The REPL also supports special commands, usually prefixed with a colon (`:`):

*   **`:reset`**: Resets the interpreter's state, clearing all declared variables and functions.
*   **`:exit`** or **`:quit`**: Exits the REPL.

## 16.3 Code Formatter: `naab-fmt`

NAAb includes a comprehensive code formatter that automatically formats your code according to a consistent style guide. The formatter ensures code readability and eliminates style debates across teams.

### 16.3.1 Basic Usage

```bash
# Format a single file in-place
naab-lang fmt myprogram.naab

# Format all files in a directory
naab-lang fmt src/**/*.naab

# Check if files are formatted (CI mode)
naab-lang fmt --check src/**/*.naab

# Show formatting differences without modifying files
naab-lang fmt --diff myprogram.naab
```

### 16.3.2 Configuration

The formatter reads settings from `.naabfmt.toml` in your project root:

```toml
[style]
indent_width = 4              # Number of spaces per indent (2 or 4)
max_line_length = 100         # Maximum line length before wrapping
semicolons = "never"          # "always", "never", or "as-needed"
trailing_commas = true        # Add trailing commas in multi-line lists

[braces]
function_brace_style = "same_line"     # K&R style
control_flow_brace_style = "same_line"

[wrapping]
wrap_function_params = "auto"   # "auto", "always", or "never"
wrap_struct_fields = "auto"
align_wrapped_params = true
```

### 16.3.3 Features

*   **Idempotent**: Formatting the same code twice produces identical results
*   **Semantic preservation**: Never changes program behavior
*   **Configurable styles**: Customize indentation, line length, braces, and more
*   **Multi-line formatting**: Intelligently wraps long function calls, struct literals, and arrays
*   **Polyglot block preservation**: Never modifies code inside `<<>>` blocks

For complete details, see `docs/FORMATTER_GUIDE.md`.

## 16.4 Interactive Debugger

The NAAb debugger provides step-through debugging with breakpoints, variable inspection, and watch expressions.

### 16.4.1 Starting the Debugger

Launch any NAAb program in debug mode:

```bash
naab-lang run --debug myprogram.naab
```

### 16.4.2 Setting Breakpoints

Set breakpoints directly in your code using special comments:

```naab
fn processData(items: list<int>) -> int {
    let sum = 0
    for item in items {
        // breakpoint
        sum = sum + item    // Debugger pauses here
    }
    return sum
}
```

You can also set breakpoints at runtime:

```
(debug) b myfile.naab:15
Breakpoint set at myfile.naab:15
```

### 16.4.3 Debugger Commands

When execution pauses at a breakpoint, you enter the debugger REPL:

| Command | Shortcut | Description |
|---------|----------|-------------|
| `continue` | `c` | Continue execution until next breakpoint |
| `step` | `s` | Step to next line (enters functions) |
| `next` | `n` | Step over function calls |
| `vars` | `v` | Show all local variables |
| `print <expr>` | `p <expr>` | Evaluate and print expression |
| `watch <expr>` | `w <expr>` | Add watch expression |
| `breakpoint <file>:<line>` | `b <file>:<line>` | Set breakpoint |
| `help` | `h` | Show all commands |
| `quit` | `q` | Exit debugger |

### 16.4.4 Example Debug Session

```bash
$ naab-lang run --debug fibonacci.naab

Breakpoint hit at fibonacci.naab:14

(debug) p a
0

(debug) p b
1

(debug) w temp
Added watch: temp

(debug) c
[Step 15] temp=1
[Step 16] temp=2
[Step 17] temp=3
...
```

For comprehensive debugging techniques, see `docs/DEBUGGING_GUIDE.md`.

## 16.5 Code Quality Hints

NAAb includes an integrated linter that provides quality hints across five categories:

### 16.5.1 Hint Categories

*   **Performance**: Detects inefficient patterns like O(n²) array concatenation in loops
*   **Best Practices**: Flags overly long functions, missing error handling, empty catch blocks
*   **Security**: Identifies SQL injection risks, XSS vulnerabilities, hardcoded secrets
*   **Maintainability**: Warns about deep nesting, code duplication, magic numbers
*   **Readability**: Suggests simplifications for complex conditions and long parameter lists

### 16.5.2 Enhanced Error Messages

The parser and interpreter provide contextual error messages with fix suggestions:

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

### 16.5.3 LLM Best Practices

NAAb detects common mistakes made by Large Language Models when generating code:

*   Unnecessary type annotations (over-specification)
*   Incorrect polyglot block syntax (missing variable lists)
*   Wrong module import patterns (using `import` instead of `use`)
*   JavaScript/Python idioms that don't work in NAAb

See `docs/LLM_BEST_PRACTICES.md` for a complete guide to generating correct NAAb code.

## 16.6 Future Tools and IDE Integration

Additional tools are planned or under active development:

*   **Language Server Protocol (LSP)**: An LSP server will provide rich IDE features like intelligent autocompletion, real-time diagnostics, hover information, go-to-definition, and refactoring support in popular editors like VS Code, Neovim, and Sublime Text.
*   **Package Manager**: A package manager (`naab-pkg`) will simplify dependency management, allowing easy installation, updating, and publishing of NAAb modules and libraries.
*   **Documentation Generator**: Automatically generate API documentation from code comments and type signatures.
