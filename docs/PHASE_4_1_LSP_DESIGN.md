# Phase 4.1: Language Server Protocol (LSP) - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** High - Full LSP server implementation
**Estimated Effort:** 3-4 weeks implementation
**Priority:** HIGH - Essential for IDE integration and developer experience

This document outlines the complete design for implementing a Language Server Protocol (LSP) server for NAAb, enabling rich IDE features like autocomplete, hover information, go-to-definition, and real-time diagnostics.

---

## Current Problem

**No IDE Support:**
- No autocomplete for keywords, functions, variables
- No type information on hover
- No go-to-definition
- No find references
- No real-time error checking
- Manual error discovery (run program to see errors)

**Impact:** Poor developer experience, slow development velocity, high friction for adoption.

---

## Language Server Protocol Overview

### What is LSP?

The Language Server Protocol is a standardized protocol between development tools (editors/IDEs) and language servers that provide language-specific features.

**Architecture:**
```
┌─────────────┐         JSON-RPC over       ┌──────────────────┐
│   VS Code   │ <────── stdio/TCP/pipes ───> │   naab-lsp       │
│   Neovim    │                              │  (LSP Server)    │
│   Emacs     │                              │                  │
└─────────────┘                              │  - Parser        │
                                             │  - Type Checker  │
                                             │  - Symbol Table  │
                                             │  - Diagnostics   │
                                             └──────────────────┘
```

**Benefits:**
- Write once, work with all LSP-compatible editors
- Standardized protocol (maintained by Microsoft/community)
- Rich ecosystem of client implementations

---

## Phase 4.1 Goals

### Must-Have Features (v1.0)

1. **Autocomplete** - Complete keywords, variables, functions, struct fields
2. **Hover** - Show type information and documentation
3. **Diagnostics** - Real-time syntax and type errors
4. **Go-to-Definition** - Jump to symbol definition
5. **Document Symbols** - Outline view of file

### Should-Have Features (v1.1)

6. **Find References** - Find all usages of symbol
7. **Rename** - Rename symbol across workspace
8. **Code Actions** - Quick fixes and refactorings

### Nice-to-Have Features (v1.2+)

9. **Semantic Highlighting** - Context-aware syntax coloring
10. **Inlay Hints** - Show inferred types inline
11. **Code Lens** - Show reference counts, run buttons

---

## Design: naab-lsp Architecture

### Project Structure

```
tools/naab-lsp/
├── main.cpp                    # Entry point, LSP server loop
├── lsp_server.h/cpp           # LSP protocol implementation
├── document_manager.h/cpp     # Track open documents
├── completion_provider.h/cpp  # Autocomplete logic
├── hover_provider.h/cpp       # Hover information
├── definition_provider.h/cpp  # Go-to-definition
├── diagnostics_provider.h/cpp # Error checking
├── symbol_provider.h/cpp      # Document symbols
└── workspace.h/cpp            # Workspace management
```

### Core Components

#### 1. LSP Server (lsp_server.cpp)

**Responsibilities:**
- JSON-RPC message handling
- LSP lifecycle (initialize, initialized, shutdown, exit)
- Route requests to providers
- Send responses to client

**Implementation:**
```cpp
class LSPServer {
public:
    LSPServer();

    // Lifecycle
    void initialize(const InitializeParams& params);
    void shutdown();

    // Document synchronization
    void didOpen(const DidOpenTextDocumentParams& params);
    void didChange(const DidChangeTextDocumentParams& params);
    void didClose(const DidCloseTextDocumentParams& params);

    // Features
    CompletionList completion(const CompletionParams& params);
    Hover hover(const HoverParams& params);
    std::vector<Location> definition(const DefinitionParams& params);
    std::vector<Diagnostic> publishDiagnostics(const std::string& uri);

    // Main loop
    void run();

private:
    DocumentManager doc_manager_;
    CompletionProvider completion_provider_;
    HoverProvider hover_provider_;
    DefinitionProvider definition_provider_;
    DiagnosticsProvider diagnostics_provider_;
    Workspace workspace_;

    void handleMessage(const json& message);
    void sendResponse(const json& response);
};
```

#### 2. Document Manager (document_manager.cpp)

**Responsibilities:**
- Track open documents in editor
- Maintain document content in memory
- Incremental updates on change
- Parse documents on open/change

**Implementation:**
```cpp
class DocumentManager {
public:
    void open(const std::string& uri, const std::string& text);
    void update(const std::string& uri, const std::vector<TextDocumentContentChangeEvent>& changes);
    void close(const std::string& uri);

    Document* getDocument(const std::string& uri);

private:
    std::map<std::string, std::unique_ptr<Document>> documents_;
};

struct Document {
    std::string uri;
    std::string text;
    std::unique_ptr<ast::Program> ast;  // Parsed AST
    std::vector<Diagnostic> diagnostics;
    SymbolTable symbol_table;
    int version;

    void parse();  // Re-parse when text changes
};
```

#### 3. Symbol Table

**Responsibilities:**
- Track all symbols in document (variables, functions, structs, enums)
- Track scope information
- Enable fast symbol lookup

**Implementation:**
```cpp
struct Symbol {
    std::string name;
    SymbolKind kind;  // Variable, Function, Struct, Enum, etc.
    SourceLocation location;
    Type type;
    std::string documentation;
    Scope* scope;
};

class SymbolTable {
public:
    void addSymbol(const Symbol& symbol);
    Symbol* findSymbol(const std::string& name, const SourceLocation& from);
    std::vector<Symbol*> getSymbolsInScope(const SourceLocation& location);
    std::vector<Symbol*> getAllSymbols();

private:
    std::vector<Symbol> symbols_;
    ScopeTree scope_tree_;
};
```

---

## Feature Implementation Details

### Feature 1: Autocomplete (textDocument/completion)

**Goal:** Suggest completions as user types.

**Triggers:**
- After typing identifier character (a-z, A-Z, 0-9, _)
- After dot (`.`) for member access
- After `::` for scope resolution
- After `<` for generic type arguments

**Completion Categories:**

1. **Keywords**
   ```naab
   f|  →  function, for, false
   ```
   - Complete language keywords based on context
   - Filter by prefix

2. **Variables in Scope**
   ```naab
   let myVariable = 42
   let result = my|  →  myVariable
   ```
   - Find all variables visible at cursor position
   - Filter by prefix

3. **Function Names**
   ```naab
   pr|  →  print, process
   ```
   - All functions in scope
   - Include parameters and return type in detail

4. **Struct Fields**
   ```naab
   let person = Person { name: "Alice", age: 30 }
   person.|  →  name, age
   ```
   - Parse struct type
   - Suggest all fields

5. **Enum Variants**
   ```naab
   Status.|  →  Pending, Active, Complete
   ```
   - Parse enum type
   - Suggest all variants

6. **Type Names**
   ```naab
   let x: s|  →  string, struct
   ```
   - Built-in types (int, float, string, bool)
   - User-defined structs/enums

**Implementation:**
```cpp
class CompletionProvider {
public:
    CompletionList getCompletions(
        const Document& doc,
        const Position& position
    ) {
        CompletionList list;

        // 1. Get token at cursor
        Token token = getTokenAtPosition(doc, position);

        // 2. Determine completion context
        CompletionContext ctx = analyzeContext(doc, position);

        // 3. Generate completions based on context
        if (ctx.isMemberAccess) {
            list = completeMember(doc, position, ctx.objectType);
        } else if (ctx.isTypePosition) {
            list = completeType(doc, position);
        } else {
            list = completeExpression(doc, position);
        }

        return list;
    }

private:
    CompletionList completeMember(const Document& doc, const Position& pos, const Type& type);
    CompletionList completeType(const Document& doc, const Position& pos);
    CompletionList completeExpression(const Document& doc, const Position& pos);
};
```

**Example Completions:**

```json
{
  "isIncomplete": false,
  "items": [
    {
      "label": "myVariable",
      "kind": 6,  // CompletionItemKind.Variable
      "detail": "int",
      "documentation": "Local variable declared at line 5"
    },
    {
      "label": "myFunction",
      "kind": 3,  // CompletionItemKind.Function
      "detail": "(x: int, y: int) -> int",
      "documentation": "Adds two numbers"
    }
  ]
}
```

---

### Feature 2: Hover (textDocument/hover)

**Goal:** Show type and documentation on hover.

**Hover Information:**

1. **Variable Hover**
   ```naab
   let myVar = 42
       ^^^^^
   Hover: myVar: int
   ```

2. **Function Hover**
   ```naab
   function add(a: int, b: int) -> int
            ^^^
   Hover:
     function add(a: int, b: int) -> int
     Adds two numbers together
   ```

3. **Type Hover**
   ```naab
   struct Person { name: string, age: int }
          ^^^^^^
   Hover:
     struct Person {
       name: string
       age: int
     }
   ```

**Implementation:**
```cpp
class HoverProvider {
public:
    std::optional<Hover> getHover(
        const Document& doc,
        const Position& position
    ) {
        // 1. Find symbol at position
        Symbol* symbol = doc.symbol_table.findSymbolAt(position);
        if (!symbol) return std::nullopt;

        // 2. Format hover content
        Hover hover;
        hover.contents = formatSymbol(symbol);
        hover.range = symbol->location.toRange();

        return hover;
    }

private:
    std::string formatSymbol(const Symbol* symbol);
};
```

**Example Hover Response:**

```json
{
  "contents": {
    "kind": "markdown",
    "value": "```naab\nfunction add(a: int, b: int) -> int\n```\n\nAdds two numbers together\n\n**Parameters:**\n- `a`: First number\n- `b`: Second number\n\n**Returns:** Sum of a and b"
  },
  "range": {
    "start": { "line": 5, "character": 9 },
    "end": { "line": 5, "character": 12 }
  }
}
```

---

### Feature 3: Diagnostics (textDocument/publishDiagnostics)

**Goal:** Show errors and warnings in real-time.

**Diagnostic Types:**

1. **Syntax Errors**
   ```naab
   let x = 42   // Missing semicolon (if required)
   function add(a: int, b:  // Missing closing paren
   ```

2. **Type Errors**
   ```naab
   let x: int = "hello"  // Type mismatch
   ```

3. **Undefined Symbols**
   ```naab
   print(unknownVar)  // Variable not defined
   ```

4. **Null Safety Violations**
   ```naab
   let x: string? = null
   let len = x.length  // Nullable access without check
   ```

**Implementation:**
```cpp
class DiagnosticsProvider {
public:
    std::vector<Diagnostic> getDiagnostics(const Document& doc) {
        std::vector<Diagnostic> diagnostics;

        // 1. Syntax errors from parser
        diagnostics = collectParseErrors(doc.ast.get());

        // 2. Type errors from type checker
        auto type_errors = runTypeChecker(doc.ast.get());
        diagnostics.insert(diagnostics.end(), type_errors.begin(), type_errors.end());

        // 3. Semantic warnings (unused vars, etc.)
        auto warnings = runSemanticAnalysis(doc.ast.get());
        diagnostics.insert(diagnostics.end(), warnings.begin(), warnings.end());

        return diagnostics;
    }

private:
    std::vector<Diagnostic> collectParseErrors(const ast::Program* program);
    std::vector<Diagnostic> runTypeChecker(const ast::Program* program);
    std::vector<Diagnostic> runSemanticAnalysis(const ast::Program* program);
};
```

**Example Diagnostic:**

```json
{
  "uri": "file:///path/to/file.naab",
  "diagnostics": [
    {
      "range": {
        "start": { "line": 5, "character": 12 },
        "end": { "line": 5, "character": 19 }
      },
      "severity": 1,  // Error
      "code": "type-mismatch",
      "message": "Cannot assign string to int\n  Expected: int\n  Got: string",
      "source": "naab"
    }
  ]
}
```

---

### Feature 4: Go-to-Definition (textDocument/definition)

**Goal:** Jump to symbol definition.

**Examples:**

1. **Variable Definition**
   ```naab
   let myVar = 42      // Definition here
   print(myVar)        // Jump to line 1
         ^^^^^
   ```

2. **Function Definition**
   ```naab
   function add(a: int, b: int) -> int { ... }  // Definition here
   let result = add(1, 2)                        // Jump to line 1
                ^^^
   ```

3. **Struct Definition**
   ```naab
   struct Person { ... }     // Definition here
   let p = Person { ... }    // Jump to line 1
           ^^^^^^
   ```

**Implementation:**
```cpp
class DefinitionProvider {
public:
    std::vector<Location> getDefinition(
        const Document& doc,
        const Position& position
    ) {
        // 1. Find symbol at cursor
        Symbol* symbol = doc.symbol_table.findSymbolAt(position);
        if (!symbol) return {};

        // 2. If this is a reference, find definition
        Symbol* definition = findDefinition(symbol);

        // 3. Return location
        Location loc;
        loc.uri = doc.uri;  // May be different file
        loc.range = definition->location.toRange();

        return { loc };
    }

private:
    Symbol* findDefinition(Symbol* symbol);
};
```

---

### Feature 5: Document Symbols (textDocument/documentSymbol)

**Goal:** Provide outline/symbol tree of document.

**Symbol Tree:**

```
File: main.naab
├── function main() -> void
├── struct Person
│   ├── name: string
│   └── age: int
├── function processData(data: string) -> Result<Data, Error>
└── enum Status
    ├── Pending
    ├── Active
    └── Complete
```

**Implementation:**
```cpp
class SymbolProvider {
public:
    std::vector<DocumentSymbol> getDocumentSymbols(const Document& doc) {
        std::vector<DocumentSymbol> symbols;

        // Walk AST and extract symbols
        for (const auto& decl : doc.ast->declarations) {
            if (auto* func = dynamic_cast<FunctionDecl*>(decl.get())) {
                symbols.push_back(createFunctionSymbol(func));
            } else if (auto* struct_decl = dynamic_cast<StructDecl*>(decl.get())) {
                symbols.push_back(createStructSymbol(struct_decl));
            }
            // ... other declaration types
        }

        return symbols;
    }

private:
    DocumentSymbol createFunctionSymbol(const FunctionDecl* func);
    DocumentSymbol createStructSymbol(const StructDecl* struct_decl);
};
```

---

## LSP Protocol Implementation

### JSON-RPC Message Format

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "textDocument/completion",
  "params": {
    "textDocument": { "uri": "file:///path/to/file.naab" },
    "position": { "line": 5, "character": 12 }
  }
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "isIncomplete": false,
    "items": [ /* completion items */ ]
  }
}
```

**Notification (no response expected):**
```json
{
  "jsonrpc": "2.0",
  "method": "textDocument/didOpen",
  "params": {
    "textDocument": {
      "uri": "file:///path/to/file.naab",
      "languageId": "naab",
      "version": 1,
      "text": "let x = 42"
    }
  }
}
```

### Message Handling

```cpp
void LSPServer::run() {
    while (true) {
        // Read message from stdin
        std::string message = readMessage();
        if (message.empty()) break;

        // Parse JSON
        json msg = json::parse(message);

        // Handle message
        handleMessage(msg);
    }
}

void LSPServer::handleMessage(const json& msg) {
    std::string method = msg["method"];

    if (method == "initialize") {
        handleInitialize(msg);
    } else if (method == "textDocument/didOpen") {
        handleDidOpen(msg);
    } else if (method == "textDocument/completion") {
        handleCompletion(msg);
    }
    // ... other methods
}
```

---

## Dependencies

### External Libraries

1. **JSON Library** - nlohmann/json (header-only)
   - Parse/serialize JSON-RPC messages
   - MIT license

2. **URI Parsing** - cpp-uri or custom
   - Parse file:// URIs
   - Convert to filesystem paths

### Internal Dependencies

1. **Parser** - existing `src/parser/parser.cpp`
   - Parse NAAb code to AST
   - Track source locations

2. **Type Checker** - new `src/semantic/type_checker.cpp`
   - Type inference and checking
   - Generate type errors

3. **Symbol Table** - new `src/semantic/symbol_table.cpp`
   - Track symbols and scopes
   - Fast symbol lookup

---

## Implementation Plan

### Week 1: Core Infrastructure (5 days)

- [ ] Set up naab-lsp project structure
- [ ] Implement JSON-RPC message handling
- [ ] Implement LSP lifecycle (initialize, shutdown)
- [ ] Implement DocumentManager
- [ ] Test: LSP server starts and accepts messages

### Week 2: Basic Features (5 days)

- [ ] Implement diagnostics (syntax errors)
- [ ] Implement document symbols
- [ ] Implement basic hover (show type)
- [ ] Test: Errors show in VS Code

### Week 3: Advanced Features (5 days)

- [ ] Implement completion provider
- [ ] Implement go-to-definition
- [ ] Implement type checker integration
- [ ] Test: All features work in VS Code

### Week 4: Polish & Testing (5 days)

- [ ] Improve completion context awareness
- [ ] Add documentation to hover
- [ ] Performance optimization
- [ ] Integration testing
- [ ] Documentation

**Total: 4 weeks**

---

## Testing Strategy

### Unit Tests

- JSON-RPC message parsing/serialization
- Symbol table operations
- Completion provider logic
- Type checker

### Integration Tests

- End-to-end LSP communication
- Parse → Type Check → Diagnostics pipeline
- Multi-document workspace

### Manual Testing

- VS Code extension
- Neovim LSP client
- Emacs lsp-mode

---

## VS Code Extension

### Extension Structure

```
vscode-naab/
├── package.json        # Extension manifest
├── src/
│   └── extension.ts    # Extension entry point
└── syntaxes/
    └── naab.tmLanguage.json  # Syntax highlighting
```

### Extension Configuration

```json
{
  "name": "naab",
  "displayName": "NAAb Language Support",
  "description": "NAAb language support for VS Code",
  "version": "0.1.0",
  "engines": { "vscode": "^1.60.0" },
  "activationEvents": [ "onLanguage:naab" ],
  "main": "./out/extension.js",
  "contributes": {
    "languages": [{
      "id": "naab",
      "extensions": [".naab"],
      "configuration": "./language-configuration.json"
    }],
    "grammars": [{
      "language": "naab",
      "scopeName": "source.naab",
      "path": "./syntaxes/naab.tmLanguage.json"
    }]
  }
}
```

---

## Performance Considerations

### Incremental Parsing

**Problem:** Re-parsing entire file on every keystroke is slow.

**Solution:** Parse only changed regions.

```cpp
void Document::updateIncremental(const std::vector<TextDocumentContentChangeEvent>& changes) {
    for (const auto& change : changes) {
        // Apply text change
        applyChange(change);

        // Re-parse only affected range
        reparseRange(change.range);
    }
}
```

### Async Operations

**Problem:** Blocking on type checking delays UI.

**Solution:** Run analysis asynchronously.

```cpp
std::future<std::vector<Diagnostic>> DiagnosticsProvider::getDiagnosticsAsync(const Document& doc) {
    return std::async(std::launch::async, [&doc]() {
        return runFullAnalysis(doc);
    });
}
```

### Caching

**Problem:** Re-computing completions/hover on every request.

**Solution:** Cache results per document version.

```cpp
class CompletionCache {
    std::map<std::pair<std::string, Position>, CompletionList> cache_;

public:
    std::optional<CompletionList> get(const std::string& uri, const Position& pos, int version);
    void put(const std::string& uri, const Position& pos, int version, const CompletionList& list);
    void invalidate(const std::string& uri);
};
```

---

## Error Handling

### Graceful Degradation

**Philosophy:** LSP server should never crash, even with malformed code.

**Strategies:**

1. **Parse Errors:** Continue parsing, create error recovery nodes
2. **Type Errors:** Report error, continue type checking
3. **Invalid Requests:** Return empty result, log error

```cpp
CompletionList CompletionProvider::getCompletions(...) {
    try {
        // Normal completion logic
        return generateCompletions();
    } catch (const std::exception& e) {
        // Log error, return empty
        logError("Completion failed: " + std::string(e.what()));
        return CompletionList();
    }
}
```

---

## Documentation

### User Documentation

- Installation guide (VS Code, Neovim, Emacs)
- Feature showcase with GIFs
- Configuration options
- Troubleshooting

### Developer Documentation

- LSP architecture overview
- Adding new features
- Testing guide
- Protocol reference

---

## Success Metrics

### Phase 4.1 Complete When:

- [x] LSP server implements core features (completion, hover, diagnostics, go-to-def, symbols)
- [x] VS Code extension published and working
- [x] Neovim configuration documented and working
- [x] Performance acceptable (<100ms for completion, <50ms for hover)
- [x] Zero crashes in 1 week of testing
- [x] Documentation complete

---

## Future Enhancements (v1.1+)

### Find References

Find all usages of a symbol across workspace.

```cpp
std::vector<Location> findReferences(const Symbol* symbol);
```

### Rename Symbol

Rename symbol and all references atomically.

```cpp
WorkspaceEdit renameSymbol(const Symbol* symbol, const std::string& newName);
```

### Code Actions

Quick fixes and refactorings.

```cpp
std::vector<CodeAction> getCodeActions(const Range& range, const std::vector<Diagnostic>& diagnostics);
```

Examples:
- "Add missing semicolon"
- "Import function from module"
- "Add null check"
- "Convert to Result type"

### Semantic Highlighting

Token-based syntax highlighting with semantic information.

### Inlay Hints

Show inferred types inline:
```naab
let x = 42     // : int (shown as inlay hint)
```

---

## Conclusion

**Phase 4.1 Status: DESIGN COMPLETE**

A full-featured LSP server will dramatically improve NAAb developer experience:
- Fast, accurate autocomplete
- Real-time error detection
- Seamless IDE integration
- Professional tooling on par with mainstream languages

**Implementation Effort:** 4 weeks

**Priority:** High (essential for adoption)

**Next Steps:** Begin implementation with core infrastructure (Week 1).

**Dependencies:** Type checker (Phase 2.4.4/2.4.5), robust parser error recovery.

Once implemented, NAAb will have IDE support comparable to TypeScript, Rust, or Go.
