# Chapter 16: Tooling and the Development Environment

A productive development environment relies on robust tooling. NAAb provides a full suite of command-line tools for running, formatting, debugging, and analyzing your code, plus an LSP server for IDE integration.

## 16.1 The `naab-lang` CLI

The `naab-lang` executable is the primary interface for running and managing NAAb programs.

### 16.1.1 Commands

*   **`naab-lang run <file.naab>`**: Executes a NAAb program. You can also pass `<file.naab>` directly without the `run` subcommand — NAAb auto-detects `.naab` files.
*   **`naab-lang parse <file.naab>`**: Parses a NAAb program and prints its Abstract Syntax Tree (AST). Useful for understanding how NAAb interprets your code.
*   **`naab-lang check <file.naab>`**: Performs a static type check, identifying type errors without executing the code.
*   **`naab-lang fmt <file.naab>`**: Formats code according to the project's style configuration (see section 16.3).
*   **`naab-lang validate <block1,block2,...>`**: Validates block compatibility, ensuring input and output types align for pipelines.
*   **`naab-lang stats`**: Displays usage statistics for blocks and other components.
*   **`naab-lang blocks list`**: Lists all available blocks in the local registry.
*   **`naab-lang blocks search <query>`**: Searches the block registry for matching components.
*   **`naab-lang blocks info <block_id>`**: Displays detailed metadata for a specific block.
*   **`naab-lang blocks index [path]`**: Builds or rebuilds the block search index.
*   **`naab-lang api [port]`**: Starts a REST API server for programmatic access.
*   **`naab-lang init`**: Creates a `naab.toml` project manifest in the current directory.
*   **`naab-lang manifest check`**: Validates an existing `naab.toml` manifest.
*   **`naab-lang version`**: Displays the interpreter version and build information.
*   **`naab-lang help`**: Shows all commands and options.

### 16.1.2 Runtime Options

These flags modify how `naab-lang run` executes your program:

| Flag | Short | Description |
|------|-------|-------------|
| `--verbose` | `-v` | Enable verbose output |
| `--debug` | `-d` | Enable interactive debugger (see section 16.4) |
| `--profile` | `-p` | Enable performance profiling |
| `--explain` | | Explain execution step-by-step |
| `--no-color` | | Disable colored error messages |
| `--pipe` | | Pipe mode: `io.write()` goes to stderr, `io.output()` goes to stdout (useful for JSON pipelines) |

### 16.1.3 Security Options

NAAb provides sandboxing controls for running untrusted code:

| Flag | Default | Description |
|------|---------|-------------|
| `--sandbox-level <level>` | `standard` | Security level: `restricted`, `standard`, `elevated`, or `unrestricted` |
| `--timeout <seconds>` | `30` | Execution timeout per polyglot block |
| `--memory-limit <MB>` | `512` | Memory limit per polyglot block |
| `--allow-network` | disabled | Enable network access for polyglot blocks |

**Example Usage:**

```bash
# Run a program
naab-lang my_app.naab

# Run with verbose output and profiling
naab-lang run --verbose --profile my_app.naab

# Run untrusted code in restricted sandbox with tight limits
naab-lang run --sandbox-level restricted --timeout 5 --memory-limit 64 untrusted.naab
```

## 16.2 The REPL (Read-Eval-Print Loop)

The REPL is an interactive console for executing NAAb code one statement at a time. It is useful for experimentation, debugging, and learning.

To start the REPL:

```bash
naab-repl
```

Once inside, type NAAb statements and expressions:

```naab
> let x = 10
> let y = x * 2
> print(y)
20
> fn add(a, b) { return a + b }
> add(5, 7)
12
```

The REPL supports special commands prefixed with a colon:

*   **`:reset`**: Clears all declared variables and functions.
*   **`:exit`** or **`:quit`**: Exits the REPL.

## 16.3 Code Formatter

NAAb includes an auto-formatter that enforces consistent style across projects. The formatter is idempotent, semantically preserving, and never modifies code inside polyglot `<<>>` blocks.

### 16.3.1 Basic Usage

```bash
# Format a single file in-place
naab-lang fmt myprogram.naab

# Format all files in a directory
naab-lang fmt src/**/*.naab

# Check if files are formatted (CI mode, exit code 1 if not)
naab-lang fmt --check src/**/*.naab

# Show what would change without modifying files
naab-lang fmt --check --diff myprogram.naab
```

### 16.3.2 Configuration

The formatter reads settings from `.naabfmt.toml` in your project root:

```toml
[style]
indent_width = 4              # 2 or 4 spaces (default: 4)
max_line_length = 100         # Maximum line length before wrapping (default: 100)
semicolons = "never"          # "always", "never", or "as-needed" (default: "never")
trailing_commas = true        # Trailing commas in multi-line lists (default: true)

[braces]
function_brace_style = "same_line"        # "same_line" (K&R) or "next_line" (Allman)
control_flow_brace_style = "same_line"

[spacing]
blank_lines_between_declarations = 1
blank_lines_between_sections = 2
space_before_function_paren = false

[wrapping]
wrap_function_params = "auto"   # "auto", "always", or "never"
wrap_struct_fields = "auto"
wrap_array_elements = "auto"
align_wrapped_params = true
```

### 16.3.3 Editor Integration

**VS Code** — Add to `.vscode/settings.json`:

```json
{
  "[naab]": {
    "editor.formatOnSave": true,
    "editor.defaultFormatter": "naab.naab-fmt"
  }
}
```

**Vim/Neovim** — Format on save:

```vim
autocmd BufWritePre *.naab !naab-fmt %
```

### 16.3.4 CI Integration

**GitHub Actions:**

```yaml
- name: Check formatting
  run: naab-lang fmt --check src/**/*.naab
```

**Pre-commit hook** — Create `.git/hooks/pre-commit`:

```bash
#!/bin/bash
naab-fmt --check $(git diff --cached --name-only --diff-filter=ACM | grep '\.naab$')
```

## 16.4 Interactive Debugger

The NAAb debugger provides step-through debugging with breakpoints, variable inspection, and watch expressions.

### 16.4.1 Starting the Debugger

Launch any NAAb program in debug mode:

```bash
naab-lang run --debug myprogram.naab
```

### 16.4.2 Setting Breakpoints

Set breakpoints in code using special comments:

```naab
fn processData(items) {
    let sum = 0
    let i = 0
    while i < array.length(items) {
        // breakpoint
        sum = sum + items[i]    // Debugger pauses here
        i = i + 1
    }
    return sum
}
```

Or set breakpoints at runtime from the debugger prompt:

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

### 16.4.5 Debugging Techniques

**Print debugging** — The simplest approach for quick inspection:

```naab
main {
    let data = [1, 2, 3, 4, 5]
    print("data before: " + string.join(data, ", "))

    // ... operations on data ...

    print("data after: " + string.join(data, ", "))
}
```

**Try-catch debugging** — Wrap risky operations to see error details:

```naab
main {
    try {
        riskyOperation()
    } catch (e) {
        print("Error: " + e)
    }
}
```

**The `debug` module** — Use `debug.inspect()` and `debug.type()` for runtime introspection:

```naab
main {
    let data = {"name": "Alice", "scores": [90, 85, 92]}
    print(debug.inspect(data))    // Shows full structure
    print(debug.type(data))       // "dict"
    print(debug.type(data["scores"]))  // "array"
}
```

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
        "name": "Alice",
        "age": 30
    }

Instead of:
    let person = {
        name: "Alice",
        age: 30
    }

Note: Use structs for fixed schemas, dictionaries for dynamic data.
```

### 16.5.3 Common Mistake Detection

NAAb detects common patterns from other languages and provides targeted suggestions:

*   Using `function` instead of `fn`
*   Using `import` instead of `use`
*   Using `console.log()` instead of `print()`
*   Calling camelCase stdlib functions (`toUpperCase`) instead of NAAb names (`string.upper`)
*   Missing variable lists in polyglot blocks (`<<python code >>` instead of `<<python[vars] code >>`)

## 16.6 Language Server Protocol (LSP)

NAAb includes an LSP server (`naab-lsp`) that provides IDE features for editors that support the Language Server Protocol.

### 16.6.1 Features

The LSP server currently supports:

*   **Code Completion**: Suggests keywords (`fn`, `let`, `if`, `while`, `struct`), defined functions with signatures, in-scope variables, and built-in types. Triggered automatically or with `Ctrl+Space`.
*   **Hover Information**: Shows type information and function signatures when hovering over symbols.
*   **Go-to-Definition**: Jump to where a function, variable, or type is defined (`Ctrl+Click` or `F12`).
*   **Document Symbols (Outline)**: Shows a hierarchical outline of all functions, structs, enums, and the main block (`Ctrl+Shift+O` in VS Code).
*   **Real-time Diagnostics**: Parse errors and type errors appear as red squiggles as you type. The server debounces updates by 300ms to reduce CPU usage during rapid typing.

### 16.6.2 Installation

Build the LSP server from source:

```bash
cd ~/.naab/language
cmake --build build --target naab-lsp
```

Then ensure `naab-lsp` is in your PATH or configure your editor to point to `build/naab-lsp`.

### 16.6.3 Editor Setup

**VS Code:**

Install the NAAb extension from the `vscode-naab/` directory:

```bash
cd vscode-naab
npm install && npm run compile
npx vsce package
code --install-extension naab-0.2.0.vsix
```

Configure the server path in VS Code settings if needed:

```json
{
  "naab.lsp.serverPath": "/path/to/naab-lsp",
  "naab.trace.server": "verbose"
}
```

**Neovim** (with nvim-lspconfig):

```lua
require('lspconfig').configs.naab = {
  default_config = {
    cmd = {'naab-lsp'},
    filetypes = {'naab'},
    root_dir = function(fname)
      return vim.fn.getcwd()
    end,
  },
}
require('lspconfig').naab.setup{}
```

**Emacs** (lsp-mode):

```elisp
(require 'lsp-mode)
(add-to-list 'lsp-language-id-configuration '(naab-mode . "naab"))
(lsp-register-client
 (make-lsp-client
  :new-connection (lsp-stdio-connection "naab-lsp")
  :major-modes '(naab-mode)
  :server-id 'naab-lsp))
(add-hook 'naab-mode-hook #'lsp)
```

### 16.6.4 Architecture

The LSP server is built in C++17 and communicates over JSON-RPC via stdin/stdout. Its internal components are:

| Component | Responsibility |
|-----------|---------------|
| `JsonRpcTransport` | Parse and serialize JSON-RPC messages over stdio |
| `LSPServer` | Message routing, lifecycle management, debouncing |
| `DocumentManager` | Track open documents, trigger parsing on changes |
| `CompletionProvider` | Generate keyword, function, and variable completions |
| `HoverProvider` | Produce type and signature information |
| `DefinitionProvider` | Locate symbol definitions in the AST |
| `SymbolProvider` | Extract document symbols for the outline view |

When a document is opened or changed, the server runs the lexer, parser, and type checker to produce an AST, symbol table, and diagnostics. Completion results are cached and invalidated on document change.

### 16.6.5 Known Limitations

*   **Single-file scope**: Go-to-definition and completions work within a single file only. Cross-file symbol resolution is not yet implemented.
*   **Member access**: Completions after `.` (struct field access) are not fully implemented.
*   **Find references and rename**: Not yet implemented. Use editor text search as a workaround.

## 16.7 Future Tools

Additional tools are planned for future releases:

*   **Package Manager** (`naab-pkg`): Dependency management for installing, updating, and publishing NAAb modules.
*   **Documentation Generator**: Automatic API documentation from code comments and type signatures.
*   **Semantic Highlighting**: LSP-based token coloring that understands NAAb semantics.
