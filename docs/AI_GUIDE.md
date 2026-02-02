# AI Guide for LLMs - NAAb Programming Language

**Target Audience**: AI assistants, LLMs, and automated tools working with the NAAb codebase

**Last Updated**: December 30, 2024
**NAAb Version**: 0.1.0

---

## Table of Contents

1. [Quick Orientation](#quick-orientation)
2. [Codebase Structure](#codebase-structure)
3. [Key Files Reference](#key-files-reference)
4. [Architectural Patterns](#architectural-patterns)
5. [Common Modification Patterns](#common-modification-patterns)
6. [Testing Requirements](#testing-requirements)
7. [Build System](#build-system)
8. [Common Pitfalls](#common-pitfalls)
9. [Where to Find Things](#where-to-find-things)
10. [Adding Features](#adding-features)
11. [Debugging Guide](#debugging-guide)

---

## Quick Orientation

### What is NAAb?

NAAb is a modern interpreted programming language with:
- C++17 implementation
- Multi-file support (import/export)
- Exception handling (try/catch/finally/throw)
- Pipeline operator (|>)
- 13 standard library modules
- REST API server
- Comprehensive CLI tools

### Essential First Steps for LLMs

1. **Read MASTER_CHECKLIST.md** - See what's implemented and current project status
2. **Read README.md** - Understand project scope and features
3. **Read this file** - Learn codebase patterns and conventions
4. **Check tests/** - See expected behavior through tests

### Critical Files to Know

```
/storage/emulated/0/Download/.naab/naab_language/
├── MASTER_CHECKLIST.md       # Project status (100% complete)
├── README.md                  # Main documentation
├── CMakeLists.txt            # Build configuration
├── include/naab/             # Public headers
│   ├── ast.h                # AST node definitions
│   ├── lexer.h              # Lexer interface
│   ├── parser.h             # Parser interface
│   ├── interpreter.h        # Interpreter interface
│   └── stdlib.h             # Standard library
└── src/                      # Implementation
    ├── lexer/               # Tokenization
    ├── parser/              # Syntax analysis
    ├── interpreter/         # Execution engine
    └── stdlib/              # Built-in functions
```

---

## Codebase Structure

### Directory Layout

```
naab_language/
├── include/naab/          # Public C++ headers
│   ├── ast.h             # AST nodes: Program, Function, Block, etc.
│   ├── lexer.h           # Lexer class, Token enum
│   ├── parser.h          # Parser class, parse methods
│   ├── interpreter.h     # Interpreter, Environment, Value types
│   ├── type_system.h     # Type inference and checking
│   ├── stdlib.h          # Standard library interface
│   ├── block_loader.h    # Block registry and loading
│   └── ...               # Other public interfaces
│
├── src/                   # Implementation files
│   ├── lexer/
│   │   └── lexer.cpp     # Tokenization logic
│   ├── parser/
│   │   ├── parser.cpp    # Parsing logic
│   │   └── ast_nodes.cpp # AST node implementations
│   ├── interpreter/
│   │   ├── interpreter.cpp  # Main interpreter loop
│   │   ├── environment.cpp  # Variable scope management
│   │   └── evaluator.cpp    # Expression evaluation
│   ├── stdlib/
│   │   ├── stdlib.cpp       # Standard library registration
│   │   ├── string_impl.cpp  # String module
│   │   ├── array_impl.cpp   # Array module
│   │   ├── http_impl.cpp    # HTTP module
│   │   ├── json_impl.cpp    # JSON module
│   │   └── ...              # Other modules
│   ├── runtime/
│   │   ├── block_loader.cpp     # Block registry
│   │   ├── block_registry.cpp   # Block database
│   │   ├── module_resolver.cpp  # Import/export
│   │   └── ...
│   └── cli/
│       └── main.cpp      # Command-line interface
│
├── tests/                 # Test suite
│   ├── unit/             # GoogleTest C++ unit tests
│   ├── integration/      # Integration tests
│   ├── benchmarks/       # Performance benchmarks
│   └── *.naab            # End-to-end test files
│
├── docs/                  # Documentation
│   ├── tutorials/        # Step-by-step guides
│   └── references/       # API references
│
├── external/             # Third-party dependencies
│   ├── fmt/             # Formatting library
│   ├── spdlog/          # Logging library
│   └── googletest/      # Testing framework
│
├── archive/              # Archived files
│   ├── session_summaries/  # Old session logs
│   └── old_docs/          # Deprecated documentation
│
└── scripts/              # Utility scripts
    └── ...
```

### Module Boundaries

**Core Language Pipeline**:
```
Source Code (.naab)
    ↓
Lexer (src/lexer/lexer.cpp)
    ↓
Tokens
    ↓
Parser (src/parser/parser.cpp)
    ↓
AST (include/naab/ast.h)
    ↓
Interpreter (src/interpreter/interpreter.cpp)
    ↓
Execution
```

**Standard Library**: Loaded at runtime, registered in `stdlib.cpp`

**Block System**: SQLite database with 24,483 blocks, accessed via `block_loader.h`

**Module System**: Import/export handled by `module_resolver.cpp`

---

## Key Files Reference

### Core Language Files

#### include/naab/ast.h
**Purpose**: AST node definitions for all language constructs

**Key Types**:
- `ASTNode` - Base class for all nodes
- `Program` - Root node containing statements
- `FunctionDeclaration` - Function definitions
- `IfStatement` - Conditional execution
- `WhileStatement` - Loop constructs
- `TryStatement` - Exception handling
- `ImportStatement` - Module imports
- `BinaryExpression` - Operators (+, -, *, /, &&, ||, etc.)
- `CallExpression` - Function calls
- `PipelineExpression` - Pipeline operator (|>)

**When to modify**: Adding new syntax constructs or language features

#### src/lexer/lexer.cpp
**Purpose**: Tokenization - converts source text to tokens

**Key Functions**:
- `Lexer::tokenize()` - Main tokenization entry point
- `skipWhitespace()` - Skip whitespace and comments
- `readIdentifier()` - Read keywords and identifiers
- `readNumber()` - Parse numeric literals
- `readString()` - Parse string literals

**Token Types**: See `Token` enum in `include/naab/lexer.h`

**When to modify**: Adding new operators, keywords, or literal types

#### src/parser/parser.cpp
**Purpose**: Parsing - converts tokens to AST

**Key Functions**:
- `Parser::parse()` - Entry point, returns Program node
- `parseStatement()` - Parse statements
- `parseExpression()` - Parse expressions (handles precedence)
- `parsePipelineExpression()` - Parse pipeline operator
- `parseFunctionDeclaration()` - Parse function definitions
- `parseTryCatch()` - Parse exception handling

**When to modify**: Adding new syntax or changing parsing rules

#### src/interpreter/interpreter.cpp
**Purpose**: Execution engine - evaluates AST nodes

**Key Functions**:
- `Interpreter::interpret()` - Main entry point
- `executeStatement()` - Execute statements
- `evaluateExpression()` - Evaluate expressions
- `executeBlock()` - Handle scoped blocks
- `executeTryCatch()` - Exception handling

**Value Types**: See `Value` variant in `include/naab/interpreter.h`
- `nullptr_t` - null
- `bool` - true/false
- `int64_t` - integers
- `double` - floating point
- `std::string` - strings
- `std::vector<Value>` - arrays
- `std::map<std::string, Value>` - objects/dicts
- `Function` - function objects

**When to modify**: Changing runtime behavior or adding new operations

#### src/stdlib/stdlib.cpp
**Purpose**: Standard library registration

**Pattern**:
```cpp
void registerStdLib(std::shared_ptr<Environment> env) {
    // String module
    env->define("std.length", std::make_shared<Value>(
        NativeFunction([](const std::vector<Value>& args) {
            // Implementation
        })
    ));

    // Register all 13 modules
}
```

**Modules Registered**:
1. string (12 functions)
2. array (16 functions)
3. math
4. io
5. json
6. http
7. time
8. env
9. csv
10. regex
11. crypto
12. file
13. collections

**When to modify**: Adding new standard library functions

---

## Architectural Patterns

### 1. Visitor Pattern (AST Traversal)

The interpreter uses visitor pattern to traverse AST:

```cpp
// In interpreter.cpp
void Interpreter::executeStatement(const ASTNode* node) {
    if (auto ifStmt = dynamic_cast<const IfStatement*>(node)) {
        // Handle if statement
    } else if (auto whileStmt = dynamic_cast<const WhileStatement*>(node)) {
        // Handle while loop
    } // ... other node types
}
```

**Pattern**: Use `dynamic_cast` to determine node type, then process accordingly.

### 2. Environment Chain (Scoping)

Variable scope managed through linked environments:

```cpp
class Environment {
    std::map<std::string, std::shared_ptr<Value>> variables;
    std::shared_ptr<Environment> parent; // Lexical parent scope

public:
    void define(const std::string& name, std::shared_ptr<Value> value);
    std::shared_ptr<Value> get(const std::string& name); // Searches up chain
};
```

**Pattern**: New scope = new Environment with parent pointer.

### 3. Native Function Registration

Standard library functions registered as NativeFunction objects:

```cpp
env->define("std.function_name", std::make_shared<Value>(
    NativeFunction([](const std::vector<Value>& args) -> Value {
        // Validate arguments
        if (args.size() != expected_count) {
            throw RuntimeError("Wrong number of arguments");
        }

        // Extract arguments
        auto arg1 = std::get<Type>(args[0]);

        // Perform operation
        // ...

        // Return result
        return result_value;
    })
));
```

**Pattern**: Lambda captures nothing, validates args, performs operation, returns Value.

### 4. Exception Handling

Custom exception types for different error categories:

```cpp
class RuntimeError : public std::runtime_error { /* ... */ };
class TypeError : public std::runtime_error { /* ... */ };
class BreakException : public std::exception { /* ... */ };
class ContinueException : public std::exception { /* ... */ };
class ReturnException : public std::exception {
    Value value; // Carries return value
};
```

**Pattern**: Throw specific exception types, catch in `executeTryCatch()`.

### 5. Value Variant Pattern

Values use `std::variant` for type safety:

```cpp
using Value = std::variant<
    std::monostate,        // null
    bool,                  // boolean
    int64_t,               // integer
    double,                // float
    std::string,           // string
    Array,                 // array
    Object,                // object/dict
    Function,              // function
    NativeFunction         // native function
>;
```

**Pattern**: Use `std::get<Type>(value)` to extract, `std::holds_alternative<Type>(value)` to check.

---

## Common Modification Patterns

### Adding a New Operator

**1. Add token type** (include/naab/lexer.h):
```cpp
enum class TokenType {
    // Existing tokens...
    STAR_STAR,  // ** for exponentiation
};
```

**2. Update lexer** (src/lexer/lexer.cpp):
```cpp
case '*':
    if (peek() == '*') {
        advance(); // consume second *
        tokens.push_back({TokenType::STAR_STAR, "**", line, column});
    } else {
        tokens.push_back({TokenType::STAR, "*", line, column});
    }
    break;
```

**3. Update parser** (src/parser/parser.cpp):
```cpp
std::shared_ptr<ASTNode> Parser::parseExponentiationExpression() {
    auto left = parseUnaryExpression();

    while (match(TokenType::STAR_STAR)) {
        auto op = previous().value;
        auto right = parseUnaryExpression();
        left = std::make_shared<BinaryExpression>(left, op, right);
    }

    return left;
}
```

**4. Update interpreter** (src/interpreter/interpreter.cpp):
```cpp
Value Interpreter::evaluateBinaryExpression(const BinaryExpression* expr) {
    // ... existing operators ...

    if (expr->op == "**") {
        return std::pow(
            std::get<double>(left),
            std::get<double>(right)
        );
    }
}
```

**5. Add tests** (tests/unit/test_operators.cpp):
```cpp
TEST(InterpreterTest, ExponentiationOperator) {
    EXPECT_EQ(interpret("print(2 ** 3)"), "8");
    EXPECT_EQ(interpret("print(10 ** 2)"), "100");
}
```

### Adding a New Standard Library Function

**1. Decide which module** (string, array, math, etc.)

**2. Add to stdlib registration** (src/stdlib/stdlib.cpp or module-specific file):

```cpp
// In src/stdlib/string_impl.cpp or stdlib.cpp
env->define("std.reverse", std::make_shared<Value>(
    NativeFunction([](const std::vector<Value>& args) -> Value {
        // Validate arguments
        if (args.size() != 1) {
            throw RuntimeError("reverse() takes exactly 1 argument");
        }

        if (!std::holds_alternative<std::string>(args[0])) {
            throw TypeError("reverse() requires string argument");
        }

        // Extract argument
        auto str = std::get<std::string>(args[0]);

        // Perform operation
        std::reverse(str.begin(), str.end());

        // Return result
        return str;
    })
));
```

**3. Add tests** (tests/unit/test_stdlib_string.cpp):
```cpp
TEST(StdLibTest, StringReverse) {
    EXPECT_EQ(interpret("print(std.reverse('hello'))"), "olleh");
    EXPECT_EQ(interpret("print(std.reverse(''))"), "");
}
```

**4. Update documentation** (docs/API_REFERENCE.md):
```markdown
#### std.reverse(str)
Reverses a string.

**Parameters:**
- `str` (string) - String to reverse

**Returns:** Reversed string

**Example:**
```naab
let reversed = std.reverse("hello")  // "olleh"
```
```

### Adding a New Statement Type

**1. Define AST node** (include/naab/ast.h):
```cpp
struct SwitchStatement : public ASTNode {
    std::shared_ptr<ASTNode> expression;
    std::vector<std::pair<std::shared_ptr<ASTNode>, std::vector<std::shared_ptr<ASTNode>>>> cases;
    std::vector<std::shared_ptr<ASTNode>> defaultCase;

    void accept(ASTVisitor* visitor) override;
};
```

**2. Add token/keyword** (lexer):
```cpp
// In keyword map
{"switch", TokenType::SWITCH},
{"case", TokenType::CASE},
{"default", TokenType::DEFAULT},
```

**3. Add parser method** (src/parser/parser.cpp):
```cpp
std::shared_ptr<ASTNode> Parser::parseSwitchStatement() {
    expect(TokenType::SWITCH);
    expect(TokenType::LEFT_PAREN);
    auto expr = parseExpression();
    expect(TokenType::RIGHT_PAREN);
    expect(TokenType::LEFT_BRACE);

    // Parse cases...

    return std::make_shared<SwitchStatement>(expr, cases, defaultCase);
}
```

**4. Add interpreter execution** (src/interpreter/interpreter.cpp):
```cpp
void Interpreter::executeSwitchStatement(const SwitchStatement* stmt) {
    auto value = evaluateExpression(stmt->expression.get());

    // Find matching case
    for (const auto& [caseExpr, caseBody] : stmt->cases) {
        auto caseValue = evaluateExpression(caseExpr.get());
        if (valuesEqual(value, caseValue)) {
            executeBlock(caseBody);
            return;
        }
    }

    // Execute default case if no match
    executeBlock(stmt->defaultCase);
}
```

**5. Add tests**:
```cpp
TEST(ParserTest, SwitchStatement) {
    auto ast = parse("switch(x) { case 1: print('one'); break; default: print('other'); }");
    // Verify AST structure
}

TEST(InterpreterTest, SwitchExecution) {
    EXPECT_EQ(interpret("let x = 1; switch(x) { case 1: print('one'); }"), "one");
}
```

---

## Testing Requirements

### Test Categories

**1. Unit Tests** (tests/unit/)
- C++ GoogleTest framework
- Test individual functions and classes
- 263 tests currently (75% passing - 66 need alignment)

**2. Integration Tests** (tests/integration/)
- Multi-file applications
- Pipeline execution
- Composition validation
- Error propagation
- 97+ tests

**3. End-to-End Tests** (tests/*.naab)
- Complete NAAb programs
- Test language features in realistic scenarios
- 7 tests (100% passing)

**4. Performance Benchmarks** (tests/benchmarks/)
- Search performance (<100ms target)
- Interpreter performance
- Pipeline validation (<10ms target)
- API response time (<200ms target)
- Memory usage patterns
- 5 comprehensive benchmarks

### Running Tests

```bash
# Unit tests
~/naab_unit_tests

# Integration tests
cd tests/integration
./run_all_tests.sh

# End-to-end tests
./run_tests.sh

# Performance benchmarks
cd tests/benchmarks
./run_all_benchmarks.sh

# All tests
./run_all_tests.sh  # (if exists)
```

### Test Requirements for Changes

**Adding a new operator**:
- ✅ Lexer unit test
- ✅ Parser unit test
- ✅ Interpreter unit test
- ✅ Integration test with real code
- ✅ Edge cases (nulls, type errors)

**Adding stdlib function**:
- ✅ Function unit test
- ✅ Argument validation test
- ✅ Type error test
- ✅ Integration test in .naab file
- ✅ Edge cases (empty input, nulls)

**Adding statement type**:
- ✅ Parser test (AST structure)
- ✅ Interpreter test (execution)
- ✅ Scoping test (if applicable)
- ✅ Exception handling test
- ✅ Integration test

**Minimum Test Coverage**: Aim for >90% code coverage for new code.

---

## Build System

### CMake Configuration

**Main file**: `CMakeLists.txt` in root directory

**Key sections**:

```cmake
cmake_minimum_required(VERSION 3.15)
project(naab_language VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# External dependencies
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)

# Add subdirectories
add_subdirectory(external/fmt)
add_subdirectory(external/spdlog)
add_subdirectory(external/googletest)

# Main executable
add_executable(naab-lang
    src/cli/main.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/interpreter/interpreter.cpp
    # ... more sources
)

# Link libraries
target_link_libraries(naab-lang
    SQLite::SQLite3
    OpenSSL::SSL
    fmt::fmt
    spdlog::spdlog
)
```

### Building

```bash
# Configure
cmake -B build

# Build (parallel with 4 jobs)
cmake --build build -j4

# Install binaries
cp build/naab-lang ~/
cp build/naab-repl ~/
```

### Adding New Source Files

**1. Create file** (e.g., `src/optimizer/optimizer.cpp`)

**2. Add to CMakeLists.txt**:
```cmake
add_executable(naab-lang
    # Existing files...
    src/optimizer/optimizer.cpp
)
```

**3. Rebuild**:
```bash
cmake --build build -j4
```

### Dependencies

**Required**:
- CMake 3.15+
- C++17 compiler (g++ or clang++)
- SQLite3
- OpenSSL

**Bundled** (in external/):
- fmt - Formatting library
- spdlog - Logging library
- GoogleTest - Testing framework

---

## Common Pitfalls

### 1. Value Type Extraction Without Checking

**❌ Wrong**:
```cpp
auto str = std::get<std::string>(value); // Throws if not string
```

**✅ Correct**:
```cpp
if (!std::holds_alternative<std::string>(value)) {
    throw TypeError("Expected string");
}
auto str = std::get<std::string>(value);
```

### 2. Forgetting to Update All Pipeline Stages

When adding a feature, update ALL stages:
1. ✅ Lexer (token)
2. ✅ Parser (AST node)
3. ✅ Interpreter (execution)
4. ✅ Tests
5. ✅ Documentation

Missing any stage causes incomplete feature.

### 3. Memory Management with Shared Pointers

**❌ Wrong**:
```cpp
ASTNode* node = new FunctionDeclaration(); // Raw pointer, potential leak
```

**✅ Correct**:
```cpp
auto node = std::make_shared<FunctionDeclaration>(); // Automatic cleanup
```

### 4. Exception Safety in Native Functions

**❌ Wrong**:
```cpp
NativeFunction([](const std::vector<Value>& args) -> Value {
    auto str = std::get<std::string>(args[0]); // Can throw
    // No error message
})
```

**✅ Correct**:
```cpp
NativeFunction([](const std::vector<Value>& args) -> Value {
    if (args.size() != 1) {
        throw RuntimeError("function() takes exactly 1 argument");
    }
    if (!std::holds_alternative<std::string>(args[0])) {
        throw TypeError("function() requires string argument");
    }
    auto str = std::get<std::string>(args[0]);
    // ...
})
```

### 5. Parser Lookahead Confusion

**❌ Wrong**:
```cpp
if (current().type == TokenType::PLUS) {
    advance(); // Token already consumed!
}
```

**✅ Correct**:
```cpp
if (match(TokenType::PLUS)) {
    // match() already consumed the token
    // Use previous() to get it
}
```

### 6. Environment Scope Issues

**❌ Wrong**:
```cpp
// Creating new environment but not setting parent
auto newEnv = std::make_shared<Environment>();
// Variables from outer scope not accessible!
```

**✅ Correct**:
```cpp
auto newEnv = std::make_shared<Environment>(currentEnv); // Pass parent
```

### 7. Forgetting Short-Circuit Evaluation

Logical operators (&&, ||) must short-circuit:

**❌ Wrong**:
```cpp
if (expr->op == "&&") {
    auto left = evaluateExpression(expr->left.get());
    auto right = evaluateExpression(expr->right.get()); // Always evaluates!
    return std::get<bool>(left) && std::get<bool>(right);
}
```

**✅ Correct**:
```cpp
if (expr->op == "&&") {
    auto left = evaluateExpression(expr->left.get());
    if (!std::get<bool>(left)) return false; // Short-circuit
    auto right = evaluateExpression(expr->right.get());
    return std::get<bool>(right);
}
```

---

## Where to Find Things

### "How do I add a new operator?"
- **Token**: `include/naab/lexer.h` (TokenType enum)
- **Lexing**: `src/lexer/lexer.cpp` (tokenize method)
- **Parsing**: `src/parser/parser.cpp` (parseExpression methods)
- **Execution**: `src/interpreter/interpreter.cpp` (evaluateBinaryExpression)
- **Tests**: `tests/unit/test_operators.cpp`

### "How do I add a standard library function?"
- **Registration**: `src/stdlib/stdlib.cpp` or module-specific file
- **Implementation**: Inline lambda in registration or separate file
- **Tests**: `tests/unit/test_stdlib_*.cpp`
- **Docs**: `docs/API_REFERENCE.md`

### "How does import/export work?"
- **Import parsing**: `src/parser/parser.cpp` (parseImportStatement)
- **Module resolution**: `src/runtime/module_resolver.cpp`
- **Export handling**: Part of module resolution
- **Tests**: `tests/integration/multi_file/`

### "How does exception handling work?"
- **AST nodes**: `include/naab/ast.h` (TryStatement)
- **Parsing**: `src/parser/parser.cpp` (parseTryCatch)
- **Execution**: `src/interpreter/interpreter.cpp` (executeTryCatch)
- **Exception types**: `include/naab/interpreter.h`
- **Tests**: `tests/unit/test_exception_handling.cpp`

### "How does the pipeline operator work?"
- **Token**: `TokenType::PIPE` in lexer.h
- **Parsing**: `src/parser/parser.cpp` (parsePipelineExpression)
- **Execution**: `src/interpreter/interpreter.cpp` (evaluatePipelineExpression)
- **Tests**: `tests/integration/test_pipeline.cpp`

### "Where is the block registry?"
- **Interface**: `include/naab/block_loader.h`
- **Implementation**: `src/runtime/block_registry.cpp`
- **Database**: `.naab/blocks.db` (SQLite, 24,483 blocks)
- **Usage**: CLI `blocks` command, API `/api/v1/blocks`

### "How do I debug the interpreter?"
- **Add logging**: Use spdlog (already included)
- **Debug builds**: `cmake -DCMAKE_BUILD_TYPE=Debug -B build`
- **GDB**: `gdb ~/naab-lang` then `run test.naab`
- **Print AST**: `~/naab-lang parse test.naab`
- **Type check**: `~/naab-lang check test.naab`

---

## Adding Features

### Step-by-Step: Adding a New Language Feature

**Example**: Adding a `for-in` loop

**Step 1: Design**
- Syntax: `for (item in array) { body }`
- Semantics: Iterate over array elements
- Error handling: Non-iterable type error

**Step 2: Add Token/Keyword**
```cpp
// include/naab/lexer.h
enum class TokenType {
    // ...
    FOR,
    IN,
};

// src/lexer/lexer.cpp
{"for", TokenType::FOR},
{"in", TokenType::IN},
```

**Step 3: Define AST Node**
```cpp
// include/naab/ast.h
struct ForInStatement : public ASTNode {
    std::string variable;  // Loop variable name
    std::shared_ptr<ASTNode> iterable;  // Array to iterate
    std::vector<std::shared_ptr<ASTNode>> body;  // Loop body

    void accept(ASTVisitor* visitor) override;
};
```

**Step 4: Add Parser**
```cpp
// src/parser/parser.cpp
std::shared_ptr<ASTNode> Parser::parseForInStatement() {
    expect(TokenType::FOR);
    expect(TokenType::LEFT_PAREN);

    auto varToken = expect(TokenType::IDENTIFIER);
    std::string variable = varToken.value;

    expect(TokenType::IN);

    auto iterable = parseExpression();

    expect(TokenType::RIGHT_PAREN);
    expect(TokenType::LEFT_BRACE);

    std::vector<std::shared_ptr<ASTNode>> body;
    while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
        body.push_back(parseStatement());
    }

    expect(TokenType::RIGHT_BRACE);

    return std::make_shared<ForInStatement>(variable, iterable, body);
}

// Add to parseStatement():
if (match(TokenType::FOR)) {
    return parseForInStatement();
}
```

**Step 5: Add Interpreter**
```cpp
// src/interpreter/interpreter.cpp
void Interpreter::executeForInStatement(const ForInStatement* stmt) {
    auto iterable = evaluateExpression(stmt->iterable.get());

    // Validate iterable type
    if (!std::holds_alternative<Array>(iterable)) {
        throw TypeError("for-in requires iterable (array)");
    }

    auto array = std::get<Array>(iterable);

    // Create new scope for loop variable
    auto loopEnv = std::make_shared<Environment>(currentEnv);
    auto previousEnv = currentEnv;
    currentEnv = loopEnv;

    // Iterate over array
    for (const auto& item : array) {
        // Set loop variable
        currentEnv->define(stmt->variable, std::make_shared<Value>(item));

        try {
            // Execute loop body
            for (const auto& node : stmt->body) {
                executeStatement(node.get());
            }
        } catch (const BreakException&) {
            break;  // Exit loop
        } catch (const ContinueException&) {
            continue;  // Next iteration
        }
    }

    // Restore environment
    currentEnv = previousEnv;
}

// Add to executeStatement():
if (auto forInStmt = dynamic_cast<const ForInStatement*>(node)) {
    executeForInStatement(forInStmt);
    return;
}
```

**Step 6: Add Tests**
```cpp
// tests/unit/test_for_in.cpp
TEST(ParserTest, ForInStatement) {
    auto ast = parse("for (x in [1, 2, 3]) { print(x); }");
    ASSERT_NE(ast, nullptr);
    // Verify AST structure
}

TEST(InterpreterTest, ForInLoop) {
    auto output = interpret("for (x in [1, 2, 3]) { print(x); }");
    EXPECT_EQ(output, "1\n2\n3\n");
}

TEST(InterpreterTest, ForInWithBreak) {
    auto output = interpret(R"(
        for (x in [1, 2, 3, 4, 5]) {
            if (x == 3) { break; }
            print(x);
        }
    )");
    EXPECT_EQ(output, "1\n2\n");
}

TEST(InterpreterTest, ForInTypeError) {
    EXPECT_THROW(
        interpret("for (x in 42) { print(x); }"),
        TypeError
    );
}
```

**Step 7: Update Documentation**
```markdown
<!-- README.md -->
### For-In Loops
```naab
for (item in [1, 2, 3, 4, 5]) {
    print(item)
}
```

<!-- docs/API_REFERENCE.md -->
## For-In Statement

Iterates over elements in an array.

**Syntax:**
```naab
for (variable in iterable) {
    // body
}
```

**Example:**
```naab
let numbers = [10, 20, 30]
for (num in numbers) {
    print(num)
}
// Output: 10, 20, 30
```
```

**Step 8: Integration Test**
```naab
// tests/test_for_in.naab
// Test for-in loop

let fruits = ["apple", "banana", "cherry"]
for (fruit in fruits) {
    print(fruit)
}

// Expected output:
// apple
// banana
// cherry
```

**Step 9: Rebuild and Test**
```bash
cmake --build build -j4
cp build/naab-lang ~/
~/naab_unit_tests
./run_tests.sh
```

---

## Debugging Guide

### Debugging Lexer Issues

**Symptom**: Unexpected token errors

**Debug approach**:
1. Run with `parse` command to see tokens:
   ```bash
   ~/naab-lang parse test.naab
   ```

2. Add logging in lexer:
   ```cpp
   #include <spdlog/spdlog.h>

   void Lexer::tokenize() {
       // ...
       spdlog::debug("Token: {} = '{}'", tokenTypeToString(type), value);
   }
   ```

3. Check keyword map in lexer.cpp

### Debugging Parser Issues

**Symptom**: Parse errors, wrong AST structure

**Debug approach**:
1. Print AST structure:
   ```bash
   ~/naab-lang parse test.naab
   ```

2. Add logging in parser:
   ```cpp
   std::shared_ptr<ASTNode> Parser::parseStatement() {
       spdlog::debug("Parsing statement, current token: {}", current().value);
       // ...
   }
   ```

3. Check precedence rules in `parseExpression()`

4. Verify `match()`, `expect()`, `check()` usage

### Debugging Interpreter Issues

**Symptom**: Wrong output, crashes, type errors

**Debug approach**:
1. Use GDB:
   ```bash
   gdb ~/naab-lang
   (gdb) run test.naab
   (gdb) bt  # Backtrace on crash
   ```

2. Add value logging:
   ```cpp
   Value Interpreter::evaluateExpression(const ASTNode* node) {
       auto result = /* evaluation */;
       spdlog::debug("Expression result: {}", valueToString(result));
       return result;
   }
   ```

3. Check environment scope:
   ```cpp
   spdlog::debug("Current environment variables: {}",
       currentEnv->getAllVariables());
   ```

4. Verify exception handling (try/catch)

### Debugging Standard Library Functions

**Symptom**: Function not found, wrong behavior

**Debug approach**:
1. Check registration in `stdlib.cpp`:
   ```cpp
   env->define("std.function_name", /* ... */);
   ```

2. Verify function name spelling

3. Check argument count validation:
   ```cpp
   if (args.size() != expected) {
       throw RuntimeError("Wrong number of arguments");
   }
   ```

4. Test in REPL:
   ```bash
   ~/naab-repl
   > std.function_name(arg1, arg2)
   ```

### Debugging Build Issues

**Symptom**: Compilation errors, linker errors

**Debug approach**:
1. Clean build:
   ```bash
   rm -rf build
   cmake -B build
   cmake --build build -j4
   ```

2. Check missing headers:
   ```cpp
   #include <memory>  // For shared_ptr
   #include <variant> // For std::variant
   #include <vector>  // For std::vector
   ```

3. Check CMakeLists.txt has all source files

4. Verify dependencies installed:
   ```bash
   pkg install sqlite openssl
   ```

### Debugging Test Failures

**Symptom**: Tests fail, unexpected behavior

**Debug approach**:
1. Run single test:
   ```bash
   ~/naab_unit_tests --gtest_filter=InterpreterTest.SpecificTest
   ```

2. Add debug output to test:
   ```cpp
   TEST(InterpreterTest, MyTest) {
       std::cout << "Debug: Testing..." << std::endl;
       // ...
   }
   ```

3. Compare expected vs actual output

4. Check test file syntax (for .naab tests)

---

## Best Practices for LLMs

### 1. Always Verify Before Modifying

```
✅ Read file first
✅ Understand current implementation
✅ Check related files (lexer, parser, interpreter)
✅ Review existing patterns
✅ Then make changes
```

### 2. Follow Established Patterns

Don't invent new patterns. Use existing code as template:

- **New operator?** Copy existing operator pattern
- **New statement?** Copy existing statement pattern
- **New stdlib function?** Copy existing function pattern

### 3. Test Everything

Every change needs:
- ✅ Unit test
- ✅ Integration test
- ✅ Edge cases
- ✅ Error cases

No exceptions.

### 4. Update Documentation

Changed code? Update:
- ✅ README.md (if user-facing feature)
- ✅ API_REFERENCE.md (if stdlib function)
- ✅ This AI_GUIDE.md (if architectural change)
- ✅ Code comments (if complex logic)

### 5. Maintain Consistency

- **Naming**: Follow existing conventions (camelCase for variables, PascalCase for classes)
- **Error messages**: Clear, helpful, consistent format
- **Code style**: Match surrounding code
- **File organization**: Keep related code together

### 6. Handle Errors Properly

Every function should:
- ✅ Validate inputs
- ✅ Throw appropriate exception types
- ✅ Provide helpful error messages
- ✅ Clean up resources on error

### 7. Consider Performance

- Use `std::move()` for large values
- Prefer references over copies
- Avoid unnecessary allocations
- Profile before optimizing

### 8. Think About Backwards Compatibility

- Don't break existing .naab programs
- Add features, don't remove them
- Deprecate before removing
- Test with existing examples

---

## Quick Reference

### File Modification Checklist

**Adding operator**: Lexer → Parser → Interpreter → Tests
**Adding statement**: AST → Lexer → Parser → Interpreter → Tests
**Adding stdlib function**: stdlib.cpp → Tests → Docs
**Fixing bug**: Reproduce → Fix → Add regression test

### Common Commands

```bash
# Build
cmake -B build && cmake --build build -j4

# Install
cp build/naab-lang ~/

# Test
~/naab_unit_tests
./run_tests.sh

# Debug
gdb ~/naab-lang

# Run
~/naab-lang run test.naab

# Parse
~/naab-lang parse test.naab

# REPL
~/naab-repl
```

### Key Directories

```
include/naab/        → Public headers (interfaces)
src/                 → Implementation (logic)
tests/unit/          → C++ unit tests
tests/integration/   → Integration tests
tests/benchmarks/    → Performance tests
docs/                → Documentation
```

### Getting Help

1. **Check MASTER_CHECKLIST.md** - See what's implemented
2. **Check README.md** - High-level overview
3. **Read this file** - Detailed guidance
4. **Read source code** - Best documentation is code
5. **Run tests** - See expected behavior

---

## Final Notes for LLMs

**Philosophy**: NAAb is a modern, clean language implementation. Keep it that way.

**Priorities**:
1. **Correctness** - Must work correctly
2. **Clarity** - Code should be readable
3. **Consistency** - Follow existing patterns
4. **Completeness** - Test thoroughly
5. **Performance** - Optimize when needed

**Remember**:
- Every feature has 4 parts: Lexer → Parser → Interpreter → Tests
- Always validate inputs and handle errors
- Test edge cases and error conditions
- Document user-facing changes
- Keep the codebase clean and organized

**When in doubt**:
- Look at existing code for patterns
- Read tests to understand expected behavior
- Ask for clarification if requirements are unclear
- Prefer simple, clear solutions over clever ones

---

**End of AI Guide**

For questions or updates, see:
- MASTER_CHECKLIST.md - Project status
- README.md - User documentation
- DEVELOPER_GUIDE.md - Developer documentation

**Last Updated**: December 30, 2024
**NAAb Version**: 0.1.0
