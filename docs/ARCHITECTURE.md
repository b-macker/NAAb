# NAAb Language Architecture

**Version:** 0.1.0
**Last Updated:** December 25, 2024
**Status:** Production Ready

---

## Table of Contents

1. [Overview](#overview)
2. [System Architecture](#system-architecture)
3. [Core Components](#core-components)
4. [Data Flow](#data-flow)
5. [Block System](#block-system)
6. [Cross-Language Execution](#cross-language-execution)
7. [Type System](#type-system)
8. [Standard Library](#standard-library)
9. [Build System](#build-system)
10. [Performance Characteristics](#performance-characteristics)

---

## Overview

### What is NAAb?

**NAAb** (Not Another Abstract Bytecode) is a block assembly language that enables seamless composition of code blocks written in multiple programming languages (C++, JavaScript, Python) within a single program.

### Key Features

- **Multi-language Support**: Write blocks in C++, JavaScript, or Python
- **Block-based Architecture**: Reusable code blocks with automatic dependency management
- **Cross-language Calling**: Call C++ from JavaScript, Python from C++, etc.
- **Automatic Library Detection**: Detects required libraries from includes
- **Type Marshalling**: Automatic conversion between language type systems
- **Standard Library**: 13 built-in modules (io, json, http, collections, etc.)
- **Performance**: Sub-microsecond cross-language calls

### Design Philosophy

1. **Simplicity**: Easy to learn, hard to misuse
2. **Composability**: Small blocks combine to create complex systems
3. **Performance**: Native execution speed with minimal overhead
4. **Safety**: Strong typing with runtime checks
5. **Extensibility**: Easy to add new languages and libraries

---

## System Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    NAAb Program (.naab)                  │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│                  Lexer (Tokenization)                    │
│              • Keywords, identifiers, literals           │
│              • Block IDs (BLOCK-CPP-12345)              │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│                  Parser (AST Generation)                 │
│              • Function declarations                     │
│              • Block imports (use ... as ...)           │
│              • Expression parsing                        │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│              Semantic Analyzer (Type Checking)           │
│              • Symbol table management                   │
│              • Type inference                            │
│              • Error reporting with fuzzy matching       │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│                  Interpreter (Execution)                 │
│              • Environment management                    │
│              • Function calling                          │
│              • Block loading and execution               │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│              Language Registry & Executors               │
│   ┌───────────────┬───────────────┬──────────────────┐  │
│   │ C++ Executor  │ JS Executor   │ Python Executor  │  │
│   │ (clang++)     │ (QuickJS)     │ (CPython)        │  │
│   └───────────────┴───────────────┴──────────────────┘  │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│              Type Marshaller & Value System              │
│         • int, double, bool, string                      │
│         • arrays, maps                                   │
│         • blocks, functions                              │
└─────────────────────────────────────────────────────────┘
```

### Component Layers

```
┌─────────────────────────────────────────────────────┐
│              User Code Layer (.naab files)          │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│           Language Frontend (Lexer/Parser)          │
│              AST, Semantic Analysis                 │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│          Runtime Layer (Interpreter)                │
│       Environment, Block Loader, StdLib             │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│        Execution Layer (Language Executors)         │
│        C++ (clang++), JS (QuickJS), Py (CPython)   │
└─────────────────────────────────────────────────────┘
                        ↓
┌─────────────────────────────────────────────────────┐
│          Platform Layer (OS, Libraries)             │
│         dlopen, libffi, pybind11, pthread           │
└─────────────────────────────────────────────────────┘
```

---

## Core Components

### 1. Lexer (naab::lexer)

**Purpose:** Tokenize source code into meaningful tokens

**Location:** `src/lexer/lexer.cpp`

**Key Classes:**
- `Lexer`: Main tokenizer class
- `Token`: Token representation (type, value, location)
- `TokenType`: Enum of all token types

**Features:**
- Block ID recognition (`BLOCK-CPP-12345`)
- Multi-line string support
- Comment handling
- Newline significance tracking

**Example:**
```naab
use BLOCK-CPP-MATH as math
// Tokens: USE, BLOCK_ID("BLOCK-CPP-MATH"), AS, IDENTIFIER("math")

let x = 42
// Tokens: LET, IDENTIFIER("x"), EQ, NUMBER(42)
```

---

### 2. Parser (naab::parser)

**Purpose:** Build Abstract Syntax Tree from tokens

**Location:** `src/parser/parser.cpp`

**Key Classes:**
- `Parser`: Main parser class
- `ast::*`: AST node types (Expr, Stmt, Decl)

**AST Node Types:**
```cpp
// Expressions
CallExpr       // function(args)
BinaryExpr     // a + b
MemberExpr     // object.member
IdentifierExpr // variable_name

// Statements
LetStmt        // let x = value
ReturnStmt     // return value
IfStmt         // if condition { ... }

// Declarations
FunctionDecl   // fn name(params) { ... }
UseStmt        // use BLOCK-ID as name
MainBlock      // main { ... }
```

**Features:**
- Recursive descent parsing
- Error recovery
- Default parameter support (`param: type = default`)
- Type annotations

---

### 3. Semantic Analyzer (naab::semantic)

**Purpose:** Type checking and symbol resolution

**Location:** `src/semantic/analyzer.cpp`

**Key Components:**

**Symbol Table:**
```cpp
class SymbolTable {
    std::unordered_map<std::string, Symbol> symbols_;
    SymbolTable* parent_;  // For nested scopes
};
```

**Type Checker:**
```cpp
class TypeChecker {
    bool checkType(ast::Expr* expr, ast::Type expected);
    ast::Type inferType(ast::Expr* expr);
};
```

**Error Reporter:**
```cpp
class ErrorReporter {
    void reportError(SourceLocation loc, std::string message);
    std::string suggestFix(std::string name);  // Fuzzy matching
};
```

**Features:**
- Type inference
- Scope validation
- Unused variable detection
- Fuzzy error suggestions (Did you mean "calculate"?)

---

### 4. Interpreter (naab::interpreter)

**Purpose:** Execute AST and manage runtime environment

**Location:** `src/interpreter/interpreter.cpp`

**Key Classes:**

```cpp
class Interpreter : public ast::Visitor {
    Environment* global_env_;     // Global variables
    Environment* current_env_;    // Current scope
    BlockLoader* block_loader_;   // Load blocks from DB

    void visit(ast::FunctionDecl& node) override;
    void visit(ast::CallExpr& node) override;
    void visit(ast::BinaryExpr& node) override;
    // ... all AST node visitors
};
```

**Environment:**
```cpp
class Environment {
    std::unordered_map<std::string, std::shared_ptr<Value>> vars_;
    Environment* parent_;

    void define(std::string name, std::shared_ptr<Value> value);
    std::shared_ptr<Value> get(std::string name);
};
```

**Value System:**
```cpp
struct Value {
    std::variant<
        std::monostate,                           // void/null
        int,                                      // integers
        double,                                   // floats
        bool,                                     // booleans
        std::string,                              // strings
        std::vector<std::shared_ptr<Value>>,     // arrays
        std::unordered_map<...>,                 // maps
        std::shared_ptr<BlockValue>,             // loaded blocks
        std::shared_ptr<FunctionValue>,          // user functions
        std::shared_ptr<PythonObjectValue>       // Python objects
    > data;
};
```

---

### 5. Block System

**Purpose:** Load and manage reusable code blocks

**Location:** `src/runtime/block_loader.cpp`

**Database Schema:**
```sql
CREATE TABLE blocks (
    id TEXT PRIMARY KEY,           -- BLOCK-CPP-12345
    name TEXT,                     -- Human-readable name
    language TEXT,                 -- cpp, javascript, python
    code TEXT,                     -- Source code
    category TEXT,                 -- math, string, http, etc.
    validation_status TEXT,        -- validated, pending, failed
    token_count INTEGER            -- For usage tracking
);
```

**Block Metadata:**
```cpp
struct BlockMetadata {
    std::string id;                // BLOCK-CPP-12345
    std::string name;              // "Math Utilities"
    std::string language;          // "cpp"
    std::string category;          // "math"
    std::string validation_status; // "validated"
    int token_count;               // 500
};
```

**Block Loading Flow:**
```
.naab file: use BLOCK-CPP-MATH as math
       ↓
BlockLoader::loadBlock("BLOCK-CPP-MATH")
       ↓
Query SQLite database
       ↓
Create BlockValue with metadata and code
       ↓
Assign executor based on language
       ↓
Store in interpreter environment
       ↓
Ready to call: math.add(10, 20)
```

**Block Registry:**
- SQLite database at `~/.naab/naab/data/naab.db`
- 24,486 blocks total
- Languages: C++ (23,906), Python (553), JavaScript (2), others (25)

---

### 6. Language Executors

#### C++ Executor (naab::runtime::CppExecutor)

**Purpose:** Compile and execute C++ blocks

**Location:** `src/runtime/cpp_executor.cpp`

**Compilation Flow:**
```
C++ source code
    ↓
BlockEnricher::detectLibraries(code)
    ↓
Extract #include directives → ["llvm", "clang", "spdlog"]
    ↓
CppExecutor::buildLibraryFlags(libs)
    ↓
Generate linker flags: "-lLLVM -lclang -lspdlog"
    ↓
clang++ -fPIC -shared -O2 [flags] code.cpp -o block.so
    ↓
dlopen(block.so)
    ↓
dlsym("function_name")
    ↓
Call function with marshalled arguments
```

**Key Methods:**
```cpp
bool compileBlock(
    const std::string& block_id,
    const std::string& code,
    const std::string& entry_function,
    const std::vector<std::string>& dependencies
);

std::shared_ptr<Value> callFunction(
    const std::string& block_id,
    const std::string& function_name,
    const std::vector<std::shared_ptr<Value>>& args
);
```

**Library Detection:**
- Automatic detection of 20+ libraries
- Mapping: `#include <llvm/IR/...>` → `-lLLVM -lLLVMSupport`
- Supports: LLVM, Clang, spdlog, fmt, OpenMP, pthread, sqlite3, curl, boost, gtest, etc.

**Cache:**
- Compiled `.so` files stored in `~/.naab_cpp_cache/`
- Reused across runs for performance

---

#### JavaScript Executor (naab::runtime::JsExecutor)

**Purpose:** Execute JavaScript blocks via embedded QuickJS

**Location:** `src/runtime/js_executor.cpp`

**QuickJS Integration:**
```cpp
class JsExecutor {
    JSRuntime* runtime_;
    JSContext* context_;

    bool execute(const std::string& code);
    std::shared_ptr<Value> callFunction(
        const std::string& name,
        const std::vector<std::shared_ptr<Value>>& args
    );
};
```

**Execution:**
```
JavaScript code
    ↓
JS_Eval(context, code, strlen(code), filename, flags)
    ↓
Store functions in global scope
    ↓
Call: JS_GetGlobalObject → JS_GetPropertyStr → JS_Call
    ↓
Marshal JS_Value → NAAb Value
```

**Type Marshalling:**
```cpp
JS_Value naabToJS(Value* val) {
    if (int* i = std::get_if<int>(&val->data))
        return JS_NewInt32(ctx, *i);
    if (std::string* s = std::get_if<std::string>(&val->data))
        return JS_NewString(ctx, s->c_str());
    // ... other types
}
```

---

#### Python Executor (naab::runtime::PythonExecutor)

**Purpose:** Execute Python blocks via embedded CPython

**Location:** `src/runtime/python_executor.cpp`

**CPython Integration via pybind11:**
```cpp
class PythonExecutor {
    bool execute(const std::string& code);
    std::shared_ptr<Value> callFunction(
        const std::string& name,
        const std::vector<std::shared_ptr<Value>>& args
    );
};
```

**Execution:**
```
Python code
    ↓
PyRun_SimpleString(code.c_str())
    ↓
Store objects in __main__ module
    ↓
Call: PyObject_GetAttrString → PyObject_CallObject
    ↓
Marshal PyObject* → NAAb Value
```

**Type Marshalling:**
```cpp
PyObject* naabToPython(Value* val) {
    if (int* i = std::get_if<int>(&val->data))
        return PyLong_FromLong(*i);
    if (std::string* s = std::get_if<std::string>(&val->data))
        return PyUnicode_FromString(s->c_str());
    // ... other types
}
```

---

### 7. Cross-Language Bridge

**Purpose:** Enable seamless calling between languages

**Location:** `src/runtime/cross_language_bridge.cpp`

**Call Chain Example:**
```naab
# Python block
def calculate(x):
    return x * 2

# C++ block
int process(int n) {
    return n + 10;
}

# JavaScript block
function format(val) {
    return "Result: " + val;
}

# NAAb program
main {
    let a = python_block.calculate(5)    # Python → 10
    let b = cpp_block.process(a)         # C++ → 20
    let c = js_block.format(b)           # JS → "Result: 20"
    print(c)
}
```

**Execution Flow:**
```
NAAb Interpreter
    ↓
Python: calculate(5)
    → CPython evaluates → returns PyObject*
    → Marshal to Value(int=10)
    ↓
C++: process(10)
    → dlsym + ffi call → returns int
    → Marshal to Value(int=20)
    ↓
JavaScript: format(20)
    → QuickJS evaluates → returns JS_Value
    → Marshal to Value(string="Result: 20")
    ↓
Return to NAAb interpreter
```

**Type Marshaller:**
```cpp
class TypeMarshaller {
    std::shared_ptr<Value> pythonToNaab(PyObject* obj);
    std::shared_ptr<Value> jsToNaab(JSContext* ctx, JSValue val);
    std::shared_ptr<Value> cppToNaab(void* ptr, std::string type);

    PyObject* naabToPython(Value* val);
    JSValue naabToJS(JSContext* ctx, Value* val);
    void* naabToCpp(Value* val, std::string type);
};
```

**Performance:**
- Python → C++: ~0.178 μs per call
- C++ → JavaScript: ~0.004 μs per call
- Total overhead: < 1 μs for typical chains

---

## Data Flow

### Program Execution Flow

```
1. Source File (.naab)
   ↓
2. Lexer → Tokens
   ↓
3. Parser → AST
   ↓
4. Semantic Analyzer → Type-checked AST
   ↓
5. Interpreter → Execution
   ↓
   ├─ Load blocks from DB
   │  ├─ C++ blocks → CppExecutor → Compile → dlopen → Execute
   │  ├─ JS blocks → JsExecutor → QuickJS → Execute
   │  └─ Py blocks → PythonExecutor → CPython → Execute
   ↓
6. Results → Console output
```

### Block Execution Flow

```
1. use BLOCK-ID as name
   ↓
2. BlockLoader queries SQLite
   ↓
3. Load metadata and code
   ↓
4. Assign executor based on language
   ↓
5. Create BlockValue in environment
   ↓
6. Call: name.function(args)
   ↓
7. Executor compiles/evaluates (if not cached)
   ↓
8. Marshal arguments to target language
   ↓
9. Execute function
   ↓
10. Marshal result back to NAAb Value
    ↓
11. Return to caller
```

---

## Type System

### Primitive Types

```naab
let i: int = 42                    # 64-bit signed integer
let d: double = 3.14               # 64-bit floating point
let b: bool = true                 # Boolean
let s: string = "hello"            # UTF-8 string
let v: void = null                 # Void/null
```

### Composite Types

```naab
let arr: array<int> = [1, 2, 3]
let map: map<string, int> = {"a": 1, "b": 2}
```

### Function Types

```naab
fn add(a: int, b: int) -> int {
    return a + b
}

let f: function<(int, int) -> int> = add
```

### Type Inference

```naab
let x = 42           # Inferred as int
let s = "hello"      # Inferred as string
let arr = [1, 2, 3]  # Inferred as array<int>
```

### Type Marshalling Rules

| NAAb Type | C++ Type | JavaScript Type | Python Type |
|-----------|----------|-----------------|-------------|
| int | int / int64_t | Number | int |
| double | double | Number | float |
| bool | bool | Boolean | bool |
| string | std::string | String | str |
| array | std::vector | Array | list |
| map | std::unordered_map | Object | dict |

---

## Standard Library

### Available Modules (13 total)

1. **io** - Input/output operations
2. **json** - JSON parsing and serialization
3. **http** - HTTP client operations
4. **collections** - Data structures
5. **string** - String manipulation (12 functions)
6. **array** - Array operations
7. **math** - Mathematical functions
8. **time** - Time and date operations
9. **env** - Environment variables
10. **csv** - CSV file handling
11. **regex** - Regular expressions
12. **crypto** - Cryptographic functions
13. **file** - File system operations

### Example: String Module

```cpp
// src/stdlib/string_impl.cpp (268 lines)

// 12 functions:
- length(s: string) -> int
- substring(s: string, start: int, end: int) -> string
- concat(s1: string, s2: string) -> string
- split(s: string, delimiter: string) -> array<string>
- join(arr: array<string>, delimiter: string) -> string
- trim(s: string) -> string
- upper(s: string) -> string
- lower(s: string) -> string
- replace(s: string, old: string, new: string) -> string
- contains(s: string, substr: string) -> bool
- starts_with(s: string, prefix: string) -> bool
- ends_with(s: string, suffix: string) -> bool
```

**Usage:**
```naab
import string

main {
    let text = "  Hello, World!  "
    let trimmed = string.trim(text)      # "Hello, World!"
    let upper = string.upper(trimmed)    # "HELLO, WORLD!"
    let parts = string.split(upper, ", ") # ["HELLO", "WORLD!"]
}
```

---

## Build System

### CMake Structure

```cmake
naab_lang/
├── CMakeLists.txt              # Root build configuration
├── src/
│   ├── lexer/                  # naab_lexer library
│   ├── parser/                 # naab_parser library
│   ├── semantic/               # naab_semantic library
│   ├── interpreter/            # naab_interpreter library
│   ├── runtime/                # naab_runtime library
│   ├── stdlib/                 # naab_stdlib library
│   └── cli/                    # naab-lang executable
├── include/naab/               # Public headers
├── external/                   # Third-party libraries
│   ├── abseil-cpp/
│   ├── fmt/
│   ├── spdlog/
│   ├── quickjs-2021-03-27/
│   └── json/
└── tests/                      # Test executables
```

### Library Dependencies

```
naab-lang executable
    ↓
naab_interpreter
    ├─ naab_parser
    │   └─ naab_lexer
    │       └─ absl::strings
    ├─ naab_semantic
    │   └─ fmt::fmt
    ├─ naab_runtime
    │   ├─ naab_block_enricher
    │   ├─ SQLite3
    │   ├─ quickjs
    │   └─ Python3 (optional)
    └─ naab_stdlib
        ├─ fmt::fmt
        ├─ spdlog::spdlog
        ├─ curl
        └─ OpenSSL (optional)
```

### Build Commands

```bash
# Configure
mkdir build && cd build
cmake ..

# Build
cmake --build . -j$(nproc)

# Executables produced:
# - naab-lang          (main interpreter)
# - naab-repl          (REPL)
# - naab-repl-opt      (optimized REPL)
# - naab-repl-rl       (REPL with readline)
# - naab-doc           (documentation generator)
# - enrich_tool        (block enricher)
# - test_*             (test executables)
```

---

## Performance Characteristics

### Compilation Times

| Component | Time | Details |
|-----------|------|---------|
| Full build (cold) | ~45s | All libraries + executables |
| Incremental build | ~5s | Single file change |
| C++ block compile | < 1s | Simple block with stdlib |
| C++ block compile | 2-5s | Complex block with LLVM |

### Execution Performance

| Operation | Time | Notes |
|-----------|------|-------|
| Lexer | ~0.1ms | Per 1000 tokens |
| Parser | ~0.5ms | Per 1000 tokens |
| Semantic analysis | ~0.2ms | Per 100 symbols |
| Interpreter overhead | ~10μs | Per function call |
| C++ → C++ call | ~0.001μs | Native speed |
| Python → C++ call | ~0.178μs | Marshalling overhead |
| C++ → JS call | ~0.004μs | QuickJS overhead |
| JS → Python call | ~0.200μs | Combined overhead |

### Memory Usage

| Component | Memory | Notes |
|-----------|--------|-------|
| Binary size | 6.2 MB | naab-lang executable |
| Runtime baseline | ~10 MB | Before loading blocks |
| Per C++ block | ~5 KB | Compiled .so in cache |
| Per JS block | ~1 KB | QuickJS bytecode |
| Per Python block | ~2 KB | CPython objects |
| SQLite DB | ~50 MB | 24,486 blocks |

### Scalability

- **Blocks loaded**: Tested up to 1,000 blocks in single program
- **Nested calls**: Tested up to 10 levels deep
- **Concurrent blocks**: Thread-safe (with limitations)
- **Cache size**: Unlimited (until disk full)

---

## Security Considerations

### Code Execution

- **Sandboxing**: None currently - blocks run with full privileges
- **Mitigation**: Only load trusted blocks from verified sources

### Input Validation

- **SQL Injection**: Parameterized queries used throughout
- **Path Traversal**: Block IDs validated (must match `BLOCK-[A-Z]+-[0-9]+`)
- **Code Injection**: Parser validates all input

### Library Loading

- **Dynamic Loading**: Uses `dlopen` with `RTLD_LAZY | RTLD_LOCAL`
- **Symbol Resolution**: Only loads extern "C" symbols
- **Dependencies**: Library flags validated against whitelist

---

## Future Enhancements

### Planned Features

1. **JIT Compilation**: Compile hot paths to native code
2. **Parallel Execution**: True multi-threading support
3. **Remote Blocks**: Load blocks from network sources
4. **Block Versioning**: Support multiple versions of same block
5. **Debugging**: Full debugger with breakpoints, stepping
6. **Profiling**: Built-in profiler for performance analysis

### Language Features

1. **Async/Await**: Asynchronous function support
2. **Generators**: Yield-based iteration
3. **Pattern Matching**: Match expressions
4. **Macros**: Compile-time code generation
5. **Traits**: Interface-like contracts

---

## Appendix

### Glossary

- **Block**: Reusable code unit in a specific language
- **Executor**: Component that runs code in a specific language
- **Marshaller**: Converts values between language type systems
- **Environment**: Symbol table for variable lookup
- **AST**: Abstract Syntax Tree - intermediate representation

### References

- **QuickJS**: https://bellard.org/quickjs/
- **pybind11**: https://pybind11.readthedocs.io/
- **Abseil**: https://abseil.io/
- **fmt**: https://fmt.dev/
- **spdlog**: https://github.com/gabime/spdlog

### Version History

- **0.1.0** (Dec 25, 2024): Initial architecture documentation
  - Default parameters support
  - Library detection (20+ libraries)
  - LLVM/Clang support
  - Comprehensive test suite

---

**End of Architecture Documentation**

*For API reference, see [API_REFERENCE.md](API_REFERENCE.md)*
*For user guide, see [USER_GUIDE.md](USER_GUIDE.md)*
*For tutorials, see [tutorials/](tutorials/)*
