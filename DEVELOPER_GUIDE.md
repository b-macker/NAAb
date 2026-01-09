# NAAb Developer Guide

**Version**: 0.1.0
**Last Updated**: December 30, 2024

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Build System](#build-system)
3. [Project Structure](#project-structure)
4. [Core Components](#core-components)
5. [Testing Guide](#testing-guide)
6. [Code Style Guide](#code-style-guide)
7. [Contributing](#contributing)
8. [Development Workflow](#development-workflow)

---

## Architecture Overview

NAAb follows a layered architecture:

```
┌─────────────────────────────────────────┐
│          Application Layer               │
│   (REPL, CLI, REST API)                 │
└─────────────────────────────────────────┘
              ↓
┌─────────────────────────────────────────┐
│         Interpreter Layer                │
│   (Runtime, Execution, Type Checking)   │
└─────────────────────────────────────────┘
              ↓
┌─────────────────────────────────────────┐
│          Parser Layer                    │
│   (AST Construction, Syntax Analysis)   │
└─────────────────────────────────────────┘
              ↓
┌─────────────────────────────────────────┐
│          Lexer Layer                     │
│   (Tokenization, Lexical Analysis)      │
└─────────────────────────────────────────┘
              ↓
┌─────────────────────────────────────────┐
│        Support Systems                   │
│  (Block Registry, Stdlib, Type System)  │
└─────────────────────────────────────────┘
```

### Key Design Principles

1. **Modularity**: Each component is independent and testable
2. **Type Safety**: Strong type checking with optional annotations
3. **Performance**: Lazy loading, caching, and optimizations
4. **Extensibility**: Plugin architecture for blocks and modules
5. **Error Handling**: Comprehensive error reporting with stack traces

---

## Build System

### Prerequisites

```bash
# Required
cmake >= 3.15
c++ compiler with C++17 support

# Dependencies (auto-downloaded)
- fmt (formatting library)
- spdlog (logging)
- sqlite3 (database)
- OpenSSL (crypto)
- cpp-httplib (HTTP server)
- GoogleTest (unit testing)
```

### Build Commands

```bash
# Clean build
cmake -B build
cmake --build build -j4

# Debug build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4

# Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4

# With verbose output
cmake --build build --verbose
```

### Build Targets

```bash
# Main executable
cmake --build build --target naab-lang

# REPL
cmake --build build --target naab-repl

# Unit tests
cmake --build build --target naab_unit_tests

# All targets
cmake --build build --target all
```

---

## Project Structure

```
naab_language/
├── include/naab/           # Public headers
│   ├── ast.h              # AST node definitions
│   ├── lexer.h            # Lexer interface
│   ├── parser.h           # Parser interface
│   ├── interpreter.h      # Interpreter interface
│   ├── type_system.h      # Type system
│   ├── stdlib.h           # Standard library
│   └── block_loader.h     # Block registry
│
├── src/                    # Implementation
│   ├── lexer/             # Tokenization
│   ├── parser/            # Syntax analysis
│   ├── interpreter/       # Execution engine
│   ├── semantic/          # Type checking
│   ├── runtime/           # Block loading, modules
│   ├── stdlib/            # Built-in functions
│   ├── cli/               # Command-line interface
│   └── api/               # REST API
│
├── tests/                  # Test files
│   ├── unit/              # Unit tests (GoogleTest)
│   ├── test_*.naab        # End-to-end tests
│   └── run_tests.sh       # Test runner
│
├── external/               # Third-party libraries
│   ├── fmt/               # Formatting
│   ├── spdlog/            # Logging
│   └── googletest/        # Testing framework
│
├── docs/                   # Documentation
│   ├── tutorials/         # Step-by-step guides
│   └── references/        # API references
│
└── CMakeLists.txt         # Build configuration
```

---

## Core Components

### 1. Lexer (`src/lexer/lexer.cpp`)

**Purpose**: Convert source code into tokens

**Key Functions**:
```cpp
class Lexer {
    vector<Token> tokenize();          // Main entry point
    Token readIdentifier();            // Read ID tokens
    Token readNumber();                // Read numeric literals
    Token readString();                // Read string literals
};
```

**Token Types**:
- Keywords: `let`, `function`, `if`, `for`, `while`, etc.
- Literals: Numbers, strings, booleans
- Operators: `+`, `-`, `*`, `/`, `&&`, `||`, `|>`, etc.
- Delimiters: `(`, `)`, `{`, `}`, `[`, `]`

**Adding a New Token Type**:

1. Add to `TokenType` enum in `lexer.h`:
```cpp
enum class TokenType {
    // ... existing ...
    NEW_TOKEN,  // Add here
};
```

2. Add keyword mapping if needed:
```cpp
map<string, TokenType> keywords = {
    // ... existing ...
    {"newkeyword", TokenType::NEW_TOKEN},
};
```

3. Handle in tokenizer logic

---

### 2. Parser (`src/parser/parser.cpp`)

**Purpose**: Build Abstract Syntax Tree (AST) from tokens

**Key Classes**:
```cpp
class Parser {
    unique_ptr<Program> parseProgram();
    unique_ptr<Stmt> parseStmt();
    unique_ptr<Expr> parseExpr();
};
```

**Operator Precedence** (lowest to highest):
1. Assignment (`=`)
2. Logical OR (`||`)
3. Logical AND (`&&`)
4. Equality (`==`, `!=`)
5. Comparison (`<`, `<=`, `>`, `>=`)
6. Addition/Subtraction (`+`, `-`)
7. Multiplication/Division (`*`, `/`, `%`)
8. Unary (`!`, `-`)
9. Pipeline (`|>`)
10. Call/Member (`.`, `()`, `[]`)

**Adding a New Statement Type**:

1. Define AST node in `ast.h`:
```cpp
struct NewStmt : public Stmt {
    // ... fields ...
    void accept(ASTVisitor& visitor) override;
};
```

2. Add parse method in `parser.cpp`:
```cpp
unique_ptr<Stmt> Parser::parseNewStmt() {
    // ... parsing logic ...
}
```

3. Add visitor method in `interpreter.cpp`:
```cpp
void Interpreter::visit(NewStmt& stmt) {
    // ... execution logic ...
}
```

---

### 3. Interpreter (`src/interpreter/interpreter.cpp`)

**Purpose**: Execute AST nodes

**Key Methods**:
```cpp
class Interpreter : public ASTVisitor {
    void execute(const Program& program);
    void visit(IfStmt& stmt);
    void visit(WhileStmt& stmt);
    void visit(FunctionDecl& decl);
    // ... other visitors ...
};
```

**Value Representation**:
```cpp
struct Value {
    variant<monostate,  // null/undefined
            int,
            double,
            string,
            bool,
            vector<shared_ptr<Value>>,    // Array
            map<string, shared_ptr<Value>>,  // Dict
            shared_ptr<FunctionValue>     // Function
           > data;
};
```

**Environment (Scoping)**:
```cpp
class Environment {
    map<string, shared_ptr<Value>> values;
    shared_ptr<Environment> parent;  // Lexical scoping

    void define(string name, shared_ptr<Value> value);
    shared_ptr<Value> get(string name);
    void set(string name, shared_ptr<Value> value);
};
```

---

### 4. Type System (`include/naab/type_system.h`)

**Purpose**: Type checking and compatibility

**Type Representation**:
```cpp
enum class BaseType {
    Any, Void, Int, Float, String, Bool,
    Array, Dict, Function
};

class Type {
    BaseType base_;
    vector<Type> params_;  // Generic parameters

    bool isCompatibleWith(const Type& other);
    string toString();
};
```

**Type Compatibility Rules**:
- `int` → `float` (widens)
- `float` → `int` (NOT allowed - precision loss)
- `any` ↔ everything
- `array<int>` ↔ `array<int>` only
- `dict<K1,V1>` ↔ `dict<K2,V2>` if K1↔K2 and V1↔V2

---

### 5. Standard Library (`src/stdlib/`)

**Modules**:
- **string**: String manipulation (12 functions)
- **array**: Array operations (16 functions)
- **math**: Mathematical functions (8 functions)
- **io**: File I/O operations
- **json**: JSON parsing/serialization
- **http**: HTTP requests
- **time**: Time operations
- **env**: Environment variables
- **csv**: CSV parsing
- **regex**: Regular expressions
- **crypto**: Cryptographic functions
- **file**: Advanced file operations
- **collections**: Data structures

**Adding a New Stdlib Function**:

1. Add to module header (`stdlib_new_modules.h`):
```cpp
class MyModule : public Module {
    shared_ptr<Value> call(string name, vector<shared_ptr<Value>> args);
    bool hasFunction(string name);
};
```

2. Implement in `src/stdlib/my_module_impl.cpp`:
```cpp
shared_ptr<Value> MyModule::call(string name, vector<shared_ptr<Value>> args) {
    if (name == "my_function") {
        // ... implementation ...
        return make_shared<Value>(result);
    }
}
```

3. Register in StdLib constructor (`stdlib.cpp`):
```cpp
modules_["mymodule"] = make_shared<MyModule>();
```

---

## Testing Guide

### Unit Tests (GoogleTest)

**Location**: `tests/unit/`

**Running Tests**:
```bash
# Build tests
cmake --build build --target naab_unit_tests

# Run all tests
~/naab_unit_tests

# Run specific suite
~/naab_unit_tests --gtest_filter=LexerTest.*

# Brief output
~/naab_unit_tests --gtest_brief=1
```

**Writing a Unit Test**:

```cpp
// tests/unit/my_component_test.cpp
#include <gtest/gtest.h>
#include "naab/my_component.h"

TEST(MyComponentTest, BasicFunctionality) {
    MyComponent comp;
    auto result = comp.doSomething();
    EXPECT_EQ(result, expected_value);
}

TEST(MyComponentTest, ErrorHandling) {
    MyComponent comp;
    EXPECT_THROW(comp.invalidOperation(), runtime_error);
}
```

### End-to-End Tests

**Location**: `tests/test_*.naab`

**Running E2E Tests**:
```bash
./run_tests.sh
```

**Writing an E2E Test**:

Create `tests/test_new_feature.naab`:
```naab
// Test new feature
let result = new_feature_function(input)

if (result == expected) {
    print("PASS")
} else {
    print("FAIL: Expected " + expected + ", got " + result)
}
```

Add to `run_tests.sh`:
```bash
tests=(
    # ... existing tests ...
    "tests/test_new_feature.naab"
)
```

---

## Code Style Guide

### C++ Code Style

**Naming Conventions**:
```cpp
// Classes: PascalCase
class MyClass { };

// Functions/methods: camelCase
void myFunction();

// Variables: snake_case
int my_variable = 0;

// Constants: UPPER_SNAKE_CASE
const int MAX_SIZE = 100;

// Private members: trailing underscore
class Foo {
    int value_;
};
```

**Formatting**:
```cpp
// Indentation: 4 spaces (not tabs)
if (condition) {
    doSomething();
}

// Braces: Always use braces, even for single-line blocks
if (x > 0) {
    y = x;
}

// Line length: Max 100 characters
```

**Best Practices**:
```cpp
// Use smart pointers
auto ptr = make_shared<Value>(42);

// Use const correctness
const string& getName() const;

// Use nullptr, not NULL
Value* ptr = nullptr;

// Use auto for complex types
auto it = map.find(key);

// Prefer range-based for
for (const auto& item : items) {
    process(item);
}
```

### NAAb Code Style

See [Code Style Guide](docs/style_guide.md) for NAAb language conventions.

---

## Contributing

### Development Workflow

1. **Fork and Clone**:
```bash
git clone https://github.com/your-username/naab.git
cd naab/naab_language
```

2. **Create Feature Branch**:
```bash
git checkout -b feature/my-new-feature
```

3. **Make Changes**:
- Write code
- Write tests
- Update documentation

4. **Test Changes**:
```bash
# Build
cmake --build build

# Run unit tests
~/naab_unit_tests

# Run E2E tests
./run_tests.sh

# Manual testing
~/naab-lang run tests/test_my_feature.naab
```

5. **Commit Changes**:
```bash
git add .
git commit -m "Add: New feature description

- Detailed change 1
- Detailed change 2
- Fixes #123"
```

6. **Push and Create PR**:
```bash
git push origin feature/my-new-feature
# Create pull request on GitHub
```

### Commit Message Format

```
Type: Short description (50 chars)

Longer description if needed, wrapped at 72 characters.

- Bullet points for multiple changes
- Reference issues with #number

Fixes #123
Related to #456
```

**Types**:
- `Add`: New feature
- `Fix`: Bug fix
- `Update`: Change existing feature
- `Remove`: Remove feature
- `Docs`: Documentation only
- `Test`: Add/update tests
- `Refactor`: Code refactoring
- `Perf`: Performance improvement

---

## Development Environment

### Recommended IDE Setup

**VSCode**:
- C++ extension pack
- CMake Tools extension
- clangd language server

**Vim/Neovim**:
- coc-clangd for C++
- cmake.vim for CMake

### Debugging

**GDB**:
```bash
# Build with debug symbols
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Debug
gdb ~/naab-lang
(gdb) run tests/test_example.naab
(gdb) bt  # Backtrace
```

**Logging**:
```cpp
#include <spdlog/spdlog.h>

spdlog::debug("Debug message: {}", value);
spdlog::info("Info message");
spdlog::error("Error: {}", error_msg);
```

---

## Performance Optimization

### Profiling

```bash
# Build with profiling
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

# Profile
perf record ~/naab-lang run benchmark.naab
perf report
```

### Common Optimizations

1. **Lazy Loading**: Load blocks only when needed
2. **Caching**: Cache parsed ASTs and type information
3. **Move Semantics**: Use `std::move` for large objects
4. **Reserve Capacity**: `vector.reserve()` for known sizes
5. **String Views**: Use `string_view` instead of `string` when possible

---

## Release Process

1. Update version in `CMakeLists.txt`
2. Update `CHANGELOG.md`
3. Run full test suite
4. Build release binaries
5. Tag release: `git tag -a v0.1.0 -m "Release v0.1.0"`
6. Push tag: `git push origin v0.1.0`
7. Create GitHub release

---

## Getting Help

- **Issues**: https://github.com/naab-lang/naab/issues
- **Discussions**: https://github.com/naab-lang/naab/discussions
- **Documentation**: `/storage/emulated/0/Download/.naab/naab_language/docs/`

---

## License

See [LICENSE](LICENSE) file for details.
