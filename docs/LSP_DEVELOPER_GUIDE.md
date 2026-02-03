# NAAb LSP Developer Guide

## Architecture Overview

The NAAb LSP server is built in C++17 and follows the Language Server Protocol specification version 3.17.

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

## Data Flow

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

## Adding New Features

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

## Code Style Guidelines

### Naming Conventions

- **Classes**: PascalCase (`DocumentManager`, `HoverProvider`)
- **Functions**: camelCase (`handleCompletion`, `getSymbols`)
- **Variables**: snake_case (`doc_manager_`, `symbol_table_`)
- **Constants**: SCREAMING_SNAKE_CASE (`MAX_COMPLETIONS`)
- **Files**: snake_case (`document_manager.cpp`)

### Header Guards

Use `#pragma once` instead of include guards.

### Includes

Order:
1. Corresponding header (for .cpp files)
2. C++ standard library
3. Third-party libraries (nlohmann/json)
4. Project headers (naab/*)
5. Local headers (LSP headers)

```cpp
#include "hover_provider.h"  // Corresponding header
#include <iostream>          // C++ standard
#include <nlohmann/json.hpp> // Third-party
#include "naab/ast.h"        // Project
#include "document_manager.h" // Local
```

### Formatting

- **Indentation**: 4 spaces (no tabs)
- **Braces**: Opening brace on same line
- **Line length**: Prefer <100 characters
- **Spacing**: Space after keywords, around operators

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

- Use `std::optional<T>` for functions that may fail
- Avoid exceptions in hot paths
- Log errors to stderr with `std::cerr`

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

- Prefer stack allocation over heap
- Use smart pointers (`unique_ptr`, `shared_ptr`) over raw pointers
- Use RAII for resource management

```cpp
// Good
std::unique_ptr<Document> doc = std::make_unique<Document>(...);

// Bad
Document* doc = new Document(...);
// ... (easy to forget delete)
```

## Testing

### Unit Tests

Location: `tests/lsp/lsp_integration_test.cpp`

Framework: Google Test

```cpp
TEST(ComponentTest, TestName) {
    // Arrange
    Document doc("file:///test.naab", "main { let x = 42 }", 1);

    // Act
    auto result = someFunction(doc);

    // Assert
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->value, 42);
}
```

### Manual Testing

Use Python scripts:
- `test_completion.py` - Test completion
- `test_definition.py` - Test go-to-definition
- `test_hover.py` - Test hover

### Running Tests

```bash
# Build tests
cmake --build build --target lsp_integration_test

# Run all tests
./build/lsp_integration_test

# Run specific test
./build/lsp_integration_test --gtest_filter="CompletionProviderTest.KeywordCompletion"
```

## Debugging

### Enable Debug Logging

Add debug output:
```cpp
std::cerr << "[ComponentName] Debug info: " << value << "\n";
```

### GDB Debugging

```bash
# Build with debug symbols
cmake -DCMAKE_BUILD_TYPE=Debug -B build
cmake --build build --target naab-lsp

# Run under GDB
gdb ./build/naab-lsp

# Set breakpoints
(gdb) break LSPServer::handleCompletion
(gdb) run
```

### Valgrind Memory Check

```bash
# Check for memory leaks
valgrind --leak-check=full ./build/naab-lsp < test_input.txt

# Profile memory usage
valgrind --tool=massif ./build/naab-lsp < test_input.txt
ms_print massif.out.<pid>
```

## Performance Optimization

### Profiling

```bash
# CPU profiling with perf
perf record ./build/naab-lsp < test_input.txt
perf report

# Flame graph
perf script | flamegraph.pl > flamegraph.svg
```

### Common Bottlenecks

1. **Symbol table lookup**: O(log n) with std::map, O(1) with unordered_map
2. **Text search**: Use Boyer-Moore for large texts
3. **Parsing**: Cache parse results, debounce updates
4. **JSON serialization**: Minimize object copies

### Optimization Checklist

- [ ] Profile before optimizing
- [ ] Cache expensive computations
- [ ] Use const references to avoid copies
- [ ] Minimize allocations in hot paths
- [ ] Use move semantics where appropriate
- [ ] Consider async operations for slow tasks

## Contributing

### Workflow

1. **Fork** the repository
2. **Create branch** from `main`: `git checkout -b feature/my-feature`
3. **Make changes** following code style
4. **Write tests** for new functionality
5. **Run tests**: `./build/lsp_integration_test`
6. **Commit** with descriptive message
7. **Push** to your fork
8. **Create PR** with description

### Pull Request Guidelines

**Title Format:**
```
[LSP] Add find references support
```

**Description Template:**
```markdown
## Summary
Implements find references feature for the LSP server.

## Changes
- Added ReferencesProvider class
- Updated LSPServer with handleReferences
- Added integration tests

## Testing
- All existing tests pass
- New tests: ReferencesProviderTest (3 tests)

## Performance
- Find references: ~5ms for typical file

## Breaking Changes
None
```

### Code Review Checklist

- [ ] Code follows style guide
- [ ] All tests pass
- [ ] New features have tests
- [ ] No memory leaks (valgrind)
- [ ] Documentation updated
- [ ] Commit messages are clear

## Release Process

See `LSP_RELEASE_CHECKLIST.md` for full details.

Quick summary:
1. Update version numbers
2. Run full test suite
3. Build release binaries
4. Create git tag
5. Update documentation
6. Publish VS Code extension

## Useful Resources

### LSP Specification
- https://microsoft.github.io/language-server-protocol/

### Tools
- **LSP Inspector**: https://github.com/microsoft/language-server-protocol-inspector
- **VS Code LSP Sample**: https://github.com/microsoft/vscode-extension-samples/tree/main/lsp-sample

### Similar Projects
- **rust-analyzer**: https://github.com/rust-lang/rust-analyzer
- **clangd**: https://clangd.llvm.org/
- **gopls**: https://github.com/golang/tools/tree/master/gopls

---

**Last Updated:** February 3, 2026
**Version:** 0.1.0
