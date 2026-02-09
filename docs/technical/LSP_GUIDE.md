# NAAb Language Server Protocol (LSP) Guide

## Overview

The NAAb Language Server Protocol (LSP) implementation provides rich IDE features for the NAAb programming language, including autocomplete, hover information, go-to-definition, and real-time diagnostics. This guide covers both user-facing features and developer-centric aspects of contributing to the NAAb LSP server.

---

## User Features

### 1. Autocomplete (Code Completion)

**What it does:** Provides intelligent code suggestions as you type.

**How to use:**
- Type any code in a `.naab` file.
- Press `Ctrl+Space` (or wait for automatic trigger).
- Select from the completion list.

**What you get:**
- **Keywords**: `fn`, `let`, `if`, `for`, `while`, `struct`, `enum`, etc.
- **Functions**: All defined functions with signatures.
- **Variables**: All variables in scope.
- **Types**: `int`, `string`, `bool`, `float`, `void`, `list`, `dict`, etc.

**Example:**
```naab
fn greet(name: string) -> string {
    return "Hello"
}

main {
    let x = g|  // Press Ctrl+Space here
    // Shows: greet, let, if, for, etc.
}
```

### 2. Hover Information

**What it does:** Shows type information and function signatures when you hover over symbols.

**How to use:**
- Hover your mouse over any function, variable, or type.
- Wait briefly for the hover popup.

**What you get:**
- Variable types: `let x: int`.
- Function signatures: `fn add(a: int, b: int) -> int`.
- Struct definitions: `struct Point`.

**Example:**
```naab
let myVar: int = 42

// Hover over "myVar" shows:
// ```naab
// let myVar: int
// ```
```

### 3. Go-to-Definition

**What it does:** Jumps to where a symbol is defined.

**How to use:**
- Ctrl+Click (or F12) on any function name, variable, or type.
- Editor jumps to the definition location.

**Example:**
```naab
fn calculate() -> int {
    return 42
}

main {
    let x = calculate()  // Ctrl+Click on "calculate"
    // Jumps to line 1 where function is defined
}
```

### 4. Document Symbols (Outline)

**What it does:** Shows a hierarchical outline of all symbols in the file.

**How to use:**
- Press `Ctrl+Shift+O` (VS Code).
- Or open the "Outline" panel in the sidebar.

**What you see:**
- All functions.
- All structs with their fields.
- All enums with their variants.
- Main block.

**Example outline:**
```
📄 example.naab
├── 🔧 add(a: int, b: int) -> int
├── 🔧 greet(name: string) -> string
├── 📦 Point
│   ├── x: int
│   └── y: int
└── 🔧 main
```

### 5. Real-time Diagnostics

**What it does:** Shows errors and warnings as you type.

**What you get:**
- **Parse errors**: Red squiggles for syntax errors.
- **Type errors**: Red squiggles for type mismatches.
- **Error messages**: Detailed descriptions with line numbers.

**Example:**
```naab
main {
    let x: int = "hello"  // Red squiggle
    // Error: Type error: expected int, got string
}
```

---

## Installation & Setup (User)

### Prerequisites

- NAAb compiler installed.
- NAAb LSP server (`naab-lsp`) built and in your system's `PATH`.

### Building the LSP Server

```bash
cd ~/.naab/language
cmake --build build --target naab-lsp
sudo cp build/naab-lsp /usr/local/bin/  # Or add to PATH manually
```

### Verify Installation

```bash
which naab-lsp
# Should output: /usr/local/bin/naab-lsp

naab-lsp --version  # (if implemented)
```

### Editor Setup

The NAAb LSP server follows the standard Language Server Protocol, meaning it can be integrated with any editor that supports LSP.

#### VS Code

1.  **Install Extension:**
    ```bash
    cd vscode-naab
    npm install
    npm run compile
    npx vsce package
    code --install-extension naab-0.1.0.vsix
    ```
2.  **Configure Settings:**
    -   Open Settings (Ctrl+,).
    -   Search for "naab".
    -   Set `naab.lsp.serverPath` if `naab-lsp` is not in your system's `PATH`.
3.  **Verify:**
    -   Open a `.naab` file.
    -   Check the status bar for a "NAAb" indicator.
    -   Try autocomplete (Ctrl+Space).

#### Neovim

Add to your Neovim config (Lua):

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

Verify:
```vim
:LspInfo  " Should show naab-lsp attached
```

#### Emacs (lsp-mode)

Add to your Emacs config:

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

### Configuration

#### Server Settings

The LSP server can be configured via command-line flags or environment variables:

```bash
# Example: Set log level
export NAAB_LSP_LOG_LEVEL=debug
naab-lsp

# Example: Disable debouncing (not recommended for general use)
export NAAB_LSP_NO_DEBOUNCE=1
naab-lsp
```

#### Client Settings (VS Code Example)

```json
{
  "naab.lsp.serverPath": "/path/to/naab-lsp",
  "naab.trace.server": "verbose"  // For debugging communication
}
```

---

## Troubleshooting (User)

### Issue: LSP Server Not Starting

**Symptoms:**
- No autocomplete, no diagnostics.
- Editor shows "LSP not connected" or similar.

**Solutions:**

1.  **Check server installation:**
    ```bash
    which naab-lsp
    ```
    Ensure `naab-lsp` is in your system's `PATH`.
2.  **Check server runs manually:**
    ```bash
    echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | naab-lsp
    # Should output a JSON response (even an error response is fine, as long as it's valid JSON)
    # Example: {"jsonrpc":"2.0","id":1,"result":{"capabilities":{...}}} or {"jsonrpc":"2.0","id":1,"error":{...}}
    ```
3.  **Check editor logs:**
    -   VS Code: Output panel → "NAAb Language Server".
    -   Neovim: `:LspLog`.
    -   Emacs: `*lsp-log*` buffer.
4.  **Enable verbose logging:**
    In your client configuration (e.g., VS Code `settings.json`), set `"naab.trace.server": "verbose"` to get detailed logs.

### Issue: Autocomplete Not Working

**Possible causes:**
1.  File not saved with `.naab` extension.
2.  Syntax errors in the code preventing parsing.
3.  LSP server crashed.

**Solutions:**
1.  Save the file as `*.naab`.
2.  Fix any obvious syntax errors (check diagnostics if they are working).
3.  Restart your editor or the LSP server.

### Issue: Slow Performance

**Possible causes:**
1.  Very large file (>10,000 lines).
2.  Many symbols (>1,000 functions/variables) in a single file.

**Solutions:**
1.  Split large files into smaller, more manageable modules.
2.  Reduce the symbol count in a single file.
3.  Consider disabling features selectively (if client supports it, currently not directly configurable in server).

### Issue: Diagnostics Not Updating

**Possible causes:**
1.  Debouncing delay (default 300ms).
2.  Server crashed.
3.  Parser error within the document.

**Solutions:**
1.  Wait a moment after typing; diagnostics update after you pause.
2.  Check server logs for errors.
3.  Restart the LSP server.

---

## Performance Tips (User)

### Debouncing

The LSP server debounces diagnostics by a default of 300ms. This means:
-   **Fast typing**: Diagnostics update after you stop typing.
-   **Slow typing**: You may see multiple updates.
-   **Benefit**: Reduced CPU usage during rapid typing, leading to a smoother editing experience.

### Caching

Completion results and ASTs are aggressively cached.
-   **First request**: May take a few milliseconds (e.g., ~5ms) to compute.
-   **Repeated request**: Subsequent requests for the same context are very fast (<1ms).
-   **Cache invalidation**: Automatic upon document changes.

### Best Practices

1.  **Keep files under 5,000 lines** for optimal performance.
2.  **Use incremental saves** (rather than save-on-every-keystroke if configurable) to reduce unnecessary processing.
3.  **Close unused files** in your editor to reduce the memory and CPU footprint of the LSP server.
4.  If performance is extremely critical, you might explore client-side options to disable certain LSP features.

---

## Known Limitations (User)

### Current Limitations

1.  **Multi-file Support**:
    -   Can only jump to definitions within the same file.
    -   No cross-file symbol resolution for features like go-to-definition, find references across modules.
    -   *Workaround*: Use your editor's text search for cross-file navigation.
2.  **Member Access Completions**:
    -   `obj.` completions (e.g., for struct fields) are not fully implemented.
    -   May show an empty list for struct fields or generic suggestions.
    -   *Workaround*: Type field names manually.
3.  **Function Parameter Scope**:
    -   Parameters within a function body are sometimes treated as `any` in terms of type checking context.
    -   This does not typically affect basic completion or hover but can impact advanced type-aware features.
    -   *Workaround*: Use explicit type annotations for clarity.
4.  **Find References**: Not yet implemented.
    -   *Workaround*: Use your editor's global text search.
5.  **Rename Refactoring**: Not yet implemented.
    -   *Workaround*: Use your editor's find-and-replace functionality.

### Planned Features (Future Releases)

-   Multi-file workspace support.
-   Find all references.
-   Rename symbol refactoring.
-   Code actions (quick fixes, refactorings).
-   Semantic highlighting.
-   Inlay hints (inline type annotations).

---

## FAQ (User)

### Q: Why is autocomplete slow?

**A:** The very first request for completions in a new context can take a few milliseconds (~5ms) as the server computes results. Subsequent requests for the same context are much faster (<1ms) due to caching. If it's consistently slow, check for syntax errors in your code or if you are working with extremely large files.

### Q: Can I use this with [editor]?

**A:** Yes! The NAAb LSP server follows the standard LSP protocol. Any editor with robust LSP client support can use it (e.g., VS Code, Neovim, Emacs, Sublime Text).

### Q: How do I update the extension?

**A:** For VS Code, you typically update the extension via the Extensions view. For manual updates or development:
```bash
cd vscode-naab
git pull
npm install
npm run compile
npx vsce package
code --install-extension naab-0.1.0.vsix --force
```

### Q: Can I disable specific features?

**A:** Not currently supported directly via server configuration. All implemented features are enabled by default. Some editor clients might offer granular control over LSP features.

### Q: Does this work offline?

**A:** Yes! The LSP server runs locally on your machine and does not require an internet connection for its core functionality.

### Q: How much memory does it use?

**A:** Typical usage ranges from 10-50 MB of RAM. Memory consumption will increase with the size and number of files open in your workspace.

---

## Getting Help (User)

### Resources

-   **NAAb Repository Documentation**: The `docs/` folder in the NAAb GitHub repository.
-   **GitHub Issues**: https://github.com/naab-lang/naab/issues for bug reports and feature requests.
-   **GitHub Discussions**: https://github.com/naab-lang/naab/discussions for general questions and community interaction.

### Reporting Bugs

When reporting issues, please include the following information to help developers diagnose and fix the problem quickly:
1.  NAAb compiler version: `naab --version`.
2.  LSP server version: `naab-lsp --version`.
3.  Your editor and its version.
4.  Your operating system.
5.  A minimal reproduction code example (the smallest code snippet that triggers the bug).
6.  LSP server logs (enable verbose mode in client settings and attach the relevant output).

---

## Architecture Overview (Developer)

The NAAb LSP server is built in C++17 and rigorously follows the Language Server Protocol specification version 3.17.

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        LSP Client                            │
│                  (VS Code, Neovim, Emacs)                    │
└─────────────────────────┬───────────────────────────────────┘
                          │ JSON-RPC over stdin/stdout
┌─────────────────────────▼───────────────────────────────────┐
│                    LSP Server (naab-lsp)                     │
│  ┌────────────────────────────────────────────────────────┐ │
│  │              JsonRpcTransport                          │ │
│  │   (Read/Write JSON-RPC messages)                       │ │
│  └────────────────────┬───────────────────────────────────┘ │
│  ┌────────────────────▼───────────────────────────────────┐ │
│  │              LSPServer                                 │ │
│  │   - Message routing                                    │ │
│  │   - Lifecycle management                               │ │
│  │   - Debouncing thread                                  │ │
│  └──┬────────────┬────────────┬────────────┬─────────────┘ │
│     │            │            │            │               │
│  ┌──▼──┐     ┌──▼──┐     ┌──▼──┐     ┌──▼──┐            │
│  │Docs │     │Symb │     │Hover│     │Comp │            │
│  │Mgr  │     │Prov │     │Prov │     │Prov │            │
│  └──┬──┘     └──┬──┘     └──┬──┘     └──┬──┘            │
│     │            │            │            │               │
│  ┌──▼────────────▼────────────▼────────────▼────────────┐ │
│  │              Document (AST + Symbol Table)            │ │
│  │   - Parser (NAAb AST)                                 │ │
│  │   - Type Checker                                      │ │
│  │   - Symbol Table                                      │ │
│  └───────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### Component Responsibilities

| Component | Responsibility | Files |
|-----------|----------------|-------|
| **JsonRpcTransport** | Parse/serialize JSON-RPC messages, stdio I/O | `json_rpc.h/cpp` |
| **LSPServer** | Message routing, lifecycle, debouncing | `lsp_server.h/cpp` |
| **DocumentManager** | Track open documents, trigger parsing | `document_manager.h/cpp` |
| **Document** | Hold AST, symbol table, diagnostics | `document_manager.h/cpp` |
| **SymbolProvider** | Extract symbols from AST | `symbol_provider.h/cpp` |
| **HoverProvider** | Generate hover information | `hover_provider.h/cpp` |
| **CompletionProvider** | Generate completions | `completion_provider.h/cpp` |
| **DefinitionProvider** | Find symbol definitions | `definition_provider.h/cpp` |

---

## Data Flow (Developer)

### 1. Document Open/Change

```
Client: textDocument/didOpen
    ↓
LSPServer::handleDidOpen()
    ↓
DocumentManager::open()
    ↓
Document constructor:
    - Lexer::tokenize()
    - Parser::parseProgram() → AST
    - buildSymbolTable() → Symbol Table
    - TypeChecker::check() → Diagnostics
    ↓
scheduleUpdate() → Debounce thread (300ms)
    ↓
publishDiagnostics()
    ↓
Client: textDocument/publishDiagnostics
```

### 2. Completion Request

```
Client: textDocument/completion
    ↓
LSPServer::handleCompletion()
    ↓
CompletionProvider::getCompletions()
    ↓
Check cache (key: uri + pos + version)
    Cache hit? → Return cached result
    Cache miss ↓
analyzeContext() → Determine context type
    ↓
completeExpression() / completeTypeAnnotation() / completeMemberAccess()
    ↓
getKeywordCompletions() + getSymbolCompletions()
    ↓
Cache result
    ↓
Return CompletionList
    ↓
Client: completion response with items
```

### 3. Go-to-Definition Request

```
Client: textDocument/definition
    ↓
LSPServer::handleDefinition()
    ↓
DefinitionProvider::getDefinition()
    ↓
findSymbolAtPosition():
    - Get line text
    - Extract identifier at cursor
    - SymbolTable::lookup(identifier)
    ↓
Create Location from Symbol.location
    ↓
Return vector<Location>
    ↓
Client: definition response with locations
```

---

## Adding New Features (Developer)

### Example: Implement Find References

#### 1. Create Provider Class

**File:** `tools/naab-lsp/references_provider.h`

```cpp
#pragma once

#include "document_manager.h"
#include <vector>

namespace naab {
namespace lsp {

// LSP Location structure (already defined in definition_provider.h)
// using Location from there

class ReferencesProvider {
public:
    ReferencesProvider();

    // Find all references to symbol at position
    std::vector<Location> findReferences(
        const Document& doc,
        const Position& pos,
        bool includeDeclaration
    );

private:
    // Find all occurrences of identifier in document
    std::vector<Location> findOccurrences(
        const Document& doc,
        const std::string& identifier
    );
};

} // namespace lsp
} // namespace naab
```

#### 2. Implement Logic

**File:** `tools/naab-lsp/references_provider.cpp`

```cpp
#include "references_provider.h"
#include <sstream>

namespace naab {
namespace lsp {

ReferencesProvider::ReferencesProvider() = default;

std::vector<Location> ReferencesProvider::findReferences(
    const Document& doc,
    const Position& pos,
    bool includeDeclaration) {

    // 1. Find symbol at position
    std::string line_text = doc.getLineText(pos.line);
    std::string identifier;
    // ... extract identifier logic ...

    if (identifier.empty()) return {};

    // 2. Find all occurrences in document
    return findOccurrences(doc, identifier);
}

std::vector<Location> ReferencesProvider::findOccurrences(
    const Document& doc,
    const std::string& identifier) {

    std::vector<Location> locations;
    const std::string& text = doc.getText();

    // Simple text search (could use AST for accuracy)
    std::istringstream iss(text);
    std::string line;
    int line_num = 0;

    while (std::getline(iss, line)) {
        size_t pos = 0;
        while ((pos = line.find(identifier, pos)) != std::string::npos) {
            // Verify it's a whole word, not part of another identifier
            bool is_start = (pos == 0 || !std::isalnum(line[pos-1]));
            bool is_end = (pos + identifier.length() == line.length() ||
                          !std::isalnum(line[pos + identifier.length()]));

            if (is_start && is_end) {
                Location loc;
                loc.uri = doc.getUri();
                loc.range = Range{
                    {line_num, static_cast<int>(pos)},
                    {line_num, static_cast<int>(pos + identifier.length())}
                };
                locations.push_back(loc);
            }
            pos++;
        }
        line_num++;
    }

    return locations;
}

} // namespace lsp
} // namespace naab
```

#### 3. Integrate into LSP Server

**File:** `tools/naab-lsp/lsp_server.h`

```cpp
#include "references_provider.h"

class LSPServer {
    // ... existing members ...
private:
    ReferencesProvider references_provider_;  // Add this
};
```

**File:** `tools/naab-lsp/lsp_server.cpp`

```cpp
void LSPServer::dispatchRequest(const RequestMessage& request) {
    // ... existing handlers ...
    else if (request.method == "textDocument/references") {
        handleReferences(request);
    }
}

void LSPServer::handleReferences(const RequestMessage& request) {
    auto params = request.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    Position pos = Position::fromJson(params["position"]);
    bool includeDeclaration = params["context"]["includeDeclaration"].get<bool>();

    Document* doc = doc_manager_.getDocument(uri);
    if (!doc) {
        sendResponse(request.id, json::array());
        return;
    }

    auto locations = references_provider_.findReferences(*doc, pos, includeDeclaration);

    json locations_json = json::array();
    for (const auto& loc : locations) {
        locations_json.push_back(loc.toJson());
    }

    sendResponse(request.id, locations_json);
}
```

#### 4. Update Server Capabilities

**File:** `tools/naab-lsp/lsp_server.cpp`

```cpp
json ServerCapabilities::toJson() const {
    return {
        // ... existing capabilities ...
        {"referencesProvider", true}  // Add this
    };
}
```

#### 5. Add to CMakeLists.txt

```cmake
add_executable(naab-lsp
    # ... existing files ...
    tools/naab-lsp/references_provider.cpp
)
```

#### 6. Write Tests

**File:** `tests/lsp/lsp_integration_test.cpp`

```cpp
TEST(ReferencesProviderTest, FindVariableReferences) {
    Document doc("file:///test.naab",
                 "main { let x = 42\nlet y = x + x }",
                 1);

    ReferencesProvider provider;
    auto refs = provider.findReferences(doc, Position{0, 11}, true);

    // Should find 3 references: declaration + 2 uses
    EXPECT_EQ(refs.size(), 3);
}
```

---

## Code Style Guidelines (Developer)

### Naming Conventions

-   **Classes**: PascalCase (`DocumentManager`, `HoverProvider`).
-   **Functions**: camelCase (`handleCompletion`, `getSymbols`).
-   **Variables**: snake_case (`doc_manager_`, `symbol_table_`).
-   **Constants**: SCREAMING_SNAKE_CASE (`MAX_COMPLETIONS`).
-   **Files**: snake_case (`document_manager.cpp`).

### Header Guards

Use `#pragma once` instead of traditional include guards.

### Includes

Order of includes should be:
1.  Corresponding header (for `.cpp` files).
2.  C++ standard library headers.
3.  Third-party library headers (e.g., `nlohmann/json`).
4.  Project headers (e.g., `naab/*`).
5.  Local LSP headers.

```cpp
#include "hover_provider.h"  // Corresponding header
#include <iostream>          // C++ standard
#include <nlohmann/json.hpp> // Third-party
#include "naab/ast.h"        // Project
#include "document_manager.h" // Local
```

### Formatting

-   **Indentation**: 4 spaces (no tabs).
-   **Braces**: Opening brace on the same line as the statement.
-   **Line length**: Prefer <100 characters.
-   **Spacing**: Space after keywords, around operators.

```cpp
// Good
if (condition) {
    doSomething();
}

// Bad
if(condition){
doSomething();
}
```

### Error Handling

-   Use `std::optional<T>` for functions that may fail gracefully.
-   Avoid throwing exceptions in performance-critical paths.
-   Log errors and warnings to `stderr` using `std::cerr`.

```cpp
// Good
std::optional<Symbol> findSymbol(const std::string& name) {
    auto result = symbol_table_.lookup(name);
    if (!result) {
        std::cerr << "[Warning] Symbol not found: " << name << "\n";
        return std::nullopt;
    }
    return result;
}
```

### Memory Management

-   Prefer stack allocation over heap for small, short-lived objects.
-   Use smart pointers (`std::unique_ptr`, `std::shared_ptr`) to manage heap-allocated objects, avoiding raw pointers.
-   Apply RAII (Resource Acquisition Is Initialization) for robust resource management.

```cpp
// Good
std::unique_ptr<Document> doc = std::make_unique<Document>(...);

// Bad (prone to leaks)
Document* doc = new Document(...);
// ... (easy to forget delete)
```

---

## Testing (Developer)

### Unit Tests

**Location:** `tests/lsp/lsp_integration_test.cpp`
**Framework:** Google Test

```cpp
TEST(ComponentTest, TestName) {
    // Arrange: Set up test conditions
    Document doc("file:///test.naab", "main { let x = 42 }", 1);

    // Act: Execute the code under test
    auto result = someFunction(doc);

    // Assert: Verify the outcome
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value, 42);
}
```

### Manual Testing

Use provided Python scripts for quick manual verification:
-   `test_completion.py` - Test completion functionality.
-   `test_definition.py` - Test go-to-definition.
-   `test_hover.py` - Test hover information.

### Running Tests

```bash
# Build tests
cmake --build build --target lsp_integration_test

# Run all tests
./build/lsp_integration_test

# Run a specific test (e.g., for completion provider)
./build/lsp_integration_test --gtest_filter="CompletionProviderTest.KeywordCompletion"
```

---

## Debugging (Developer)

### Enable Debug Logging

Add debug output using `std::cerr` or a dedicated logging framework:
```cpp
std::cerr << "[ComponentName] Debug info: " << value << "\n";
```

### GDB Debugging

```bash
# Build with debug symbols enabled
cmake -DCMAKE_BUILD_TYPE=Debug -B build
cmake --build build --target naab-lsp

# Run the LSP server under GDB
gdb ./build/naab-lsp

# Set breakpoints (e.g., at a handler function)
(gdb) break LSPServer::handleCompletion
(gdb) run  # Start the LSP server process
```

### Valgrind Memory Check

Use Valgrind to detect memory leaks and errors:
```bash
# Check for memory leaks
valgrind --leak-check=full ./build/naab-lsp < test_input.txt

# Profile memory usage
valgrind --tool=massif ./build/naab-lsp < test_input.txt
ms_print massif.out.<pid>
```

---

## Performance Optimization (Developer)

### Profiling

Use system profilers to identify performance bottlenecks:
```bash
# CPU profiling with perf
perf record ./build/naab-lsp < test_input.txt
perf report

# Generate a Flame Graph for visual analysis
perf script | flamegraph.pl > flamegraph.svg
```

### Common Bottlenecks

1.  **Symbol table lookup**: Optimize for O(1) average case (e.g., using `std::unordered_map` instead of `std::map`).
2.  **Text search**: Employ efficient algorithms like Boyer-Moore for large text documents.
3.  **Parsing**: Implement caching of parse results and debounce updates to avoid redundant work.
4.  **JSON serialization**: Minimize object copies and optimize `nlohmann::json` usage.

### Optimization Checklist

-   [ ] **Profile before optimizing**: Always identify the actual bottleneck before attempting optimizations.
-   [ ] **Cache expensive computations**: Store and reuse results of costly operations.
-   [ ] **Use `const` references**: Avoid unnecessary data copies by passing arguments by `const` reference.
-   [ ] **Minimize allocations**: Reduce heap allocations in hot paths.
-   [ ] **Use move semantics**: Leverage `std::move` to transfer ownership efficiently.
-   [ ] **Consider async operations**: For long-running or blocking tasks, evaluate asynchronous execution.

---

## Contributing (Developer)

### Workflow

1.  **Fork** the NAAb repository on GitHub.
2.  **Create a new branch** from `main`: `git checkout -b feature/my-new-feature`.
3.  **Implement your changes** adhering to the project's code style guidelines.
4.  **Write tests** for any new functionality or bug fixes.
5.  **Run all tests**: `./build/lsp_integration_test`.
6.  **Commit your changes** with a descriptive message.
7.  **Push** your branch to your forked repository.
8.  **Create a Pull Request (PR)** to the main NAAb repository with a clear description of your changes.

### Pull Request Guidelines

Use a clear and concise title, following this format:
```
[LSP] Add find references support
```

Provide a detailed description using a template similar to this:
```markdown
## Summary
This PR implements the find references feature for the NAAb LSP server.

## Changes
-   Added `ReferencesProvider` class with `findReferences` logic.
-   Integrated `ReferencesProvider` into `LSPServer` via `handleReferences` method.
-   Updated `ServerCapabilities` to advertise `referencesProvider` support.
-   Modified `CMakeLists.txt` to include new source files.

## Testing
-   All existing LSP unit tests pass.
-   Added new integration tests for `ReferencesProviderTest` (3 new tests).

## Performance
-   Initial benchmarks show `findReferences` completes within ~5ms for a typical 500-line file.

## Breaking Changes
None.
```

### Code Review Checklist

-   [ ] Code adheres to the project's style guide.
-   [ ] All existing tests pass.
-   [ ] New features have corresponding unit and/or integration tests.
-   [ ] No new memory leaks detected (verified with Valgrind).
-   [ ] Relevant documentation (user and/or developer) is updated.
-   [ ] Commit messages are clear, concise, and descriptive.

---

## Release Process (Developer)

For full details on the LSP server release process, refer to `LSP_RELEASE_CHECKLIST.md`.

Quick summary:
1.  Update version numbers in relevant files.
2.  Run the full test suite (`lsp_integration_test`).
3.  Build release binaries (optimized, no debug symbols).
4.  Create a git tag for the new release.
5.  Update documentation to reflect new features or changes.
6.  Publish the VS Code extension (if applicable).

---

## Useful Resources (Developer)

### LSP Specification
-   The official Language Server Protocol specification: [https://microsoft.github.io/language-server-protocol/](https://microsoft.github.io/language-server-protocol/)

### Tools
-   **LSP Inspector**: A tool to inspect LSP communication: [https://github.com/microsoft/language-server-protocol-inspector](https://github.com/microsoft/language-server-protocol-inspector)
-   **VS Code LSP Sample**: Reference implementation for VS Code extensions: [https://github.com/microsoft/vscode-extension-samples/tree/main/lsp-sample](https://github.com/microsoft/vscode-extension-samples/tree/main/lsp-sample)

### Similar Projects (for inspiration)
-   **rust-analyzer**: [https://github.com/rust-lang/rust-analyzer](https://github.com/rust-lang/rust-analyzer)
-   **clangd**: [https://clangd.llvm.org/](https://clangd.llvm.org/)
-   **gopls**: [https://github.com/golang/tools/tree/master/gopls](https://github.com/golang/tools/tree/master/gopls)

---

**Last Updated:** February 3, 2026
**Version:** 0.1.0
