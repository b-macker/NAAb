# NAAb Block Assembly Language - Architecture

## Core Concept

NAAb is the world's first **block assembly language** where programs are written by importing and composing reusable code blocks from any programming language.

## Three-Layer Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Layer 1: BLOCK STORAGE                                 │
│  Decomposed code stored as JSON files                   │
├─────────────────────────────────────────────────────────┤
│  • Database: /storage/emulated/0/Download/.naab/naab/   │
│    data/naab.db                                          │
│  • Blocks: /storage/emulated/0/Download/.naab/naab/     │
│    blocks/library/                                       │
│                                                          │
│  Block Structure (JSON):                                │
│  {                                                       │
│    "id": "BLOCK-PY-00001",                              │
│    "language": "python",                                │
│    "code": "class APIResponse: ...",                    │
│    "metadata": {...}                                    │
│  }                                                       │
│                                                          │
│  Total blocks: 24,167                                   │
│    - Python: 237 blocks                                 │
│    - C++: 23,903 blocks (LLVM/Clang source)            │
│    - Others: PHP, Ruby, Swift, TypeScript, etc.         │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│  Layer 2: EXECUTION ENGINES                             │
│  Execute code FROM block JSON files                     │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  ✅ Python Engine (PyExecutor)                          │
│     - Reads Python code from block JSON                │
│     - Executes via embedded Python interpreter          │
│     - Supports method chaining and state                │
│                                                          │
│  ⚠️ C++ Engine (CppExecutor)                            │
│     - Reads C++ code from block JSON                    │
│     - Compiles to .so dynamically                       │
│     - Loads and executes via dlopen/dlsym               │
│     - Status: Partial (stub execution)                  │
│                                                          │
│  ❌ Future Engines                                       │
│     - JavaScript, Rust, Go, etc.                        │
│                                                          │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│  Layer 3: NAAb LANGUAGE                                 │
│  Assembly/glue language for composing blocks            │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  Syntax:                                                │
│    use BLOCK-PY-00001 as api_response                   │
│    use BLOCK-PY-00005 as validator                      │
│                                                          │
│    main {                                               │
│        let response = api_response.create(data)         │
│        let valid = validator.check(response)            │
│        print(valid)                                     │
│    }                                                     │
│                                                          │
│  Features:                                              │
│    - Variables (let, const)                             │
│    - Functions                                          │
│    - Control flow (if, for, while)                      │
│    - Method chaining                                    │
│    - Block composition                                  │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

## Infrastructure vs Blocks

### INFRASTRUCTURE (Hardcoded in Stdlib)

These are **tools for managing blocks**, not user blocks themselves:

```
naab_stdlib:
  ├── JSON Module    ← Parse block JSON files
  ├── IO Module      ← Read/write block files
  ├── HTTP Module    ← Fetch remote blocks
  └── Collections    ← Manage block collections
```

**Why hardcoded?**
- JSON parsing is needed to READ all blocks (circular dependency)
- IO operations are needed to LOAD blocks
- These are infrastructure, not user code

### BLOCKS (Loaded Dynamically)

User code decomposed from projects:

```
BLOCK-PY-00001: APIResponse class
BLOCK-PY-00002: Database connector
BLOCK-CPP-12345: Vector operations
BLOCK-JS-00042: DOM manipulation
```

**Why dynamic?**
- User code, not infrastructure
- Versioned and updateable
- Language-agnostic

## Data Flow

### Loading a Block

```
1. NAAb code:      use BLOCK-PY-00001 as api
                        ↓
2. BlockLoader:    Query database → Get metadata
                        ↓
3. BlockLoader:    Read JSON file → Extract "code" field
                        ↓
4. Interpreter:    Check language = "python"
                        ↓
5. PyExecutor:     Execute Python code
                        ↓
6. NAAb:          Store as 'api' in environment
```

### Calling Block Functions

```
NAAb code:         let result = api.create(data)
                        ↓
Interpreter:       Resolve 'api' → BlockValue
                        ↓
Interpreter:       Check language → "python"
                        ↓
PyExecutor:        Call Python method via PyObject
                        ↓
Return:            Convert result to NAAb Value
```

## File Structure

```
/storage/emulated/0/Download/.naab/
├── naab/
│   ├── data/
│   │   └── naab.db                    # Block registry SQLite
│   └── blocks/
│       └── library/
│           ├── python/                # Python block JSONs
│           ├── cpp/                   # C++ block JSONs
│           ├── javascript/
│           └── ...
│
└── naab_language/                     # Interpreter source
    ├── src/
    │   ├── cli/                       # Main CLI
    │   ├── lexer/                     # Tokenizer
    │   ├── parser/                    # AST builder
    │   ├── semantic/                  # Type checker
    │   ├── interpreter/               # Executor
    │   ├── runtime/
    │   │   ├── block_loader.cpp       # Load blocks from DB
    │   │   ├── cpp_executor.cpp       # C++ engine
    │   │   └── ...
    │   └── stdlib/
    │       ├── json_impl.cpp          # JSON infrastructure
    │       ├── io.cpp                 # IO infrastructure
    │       └── stdlib.cpp             # Module registry
    │
    ├── include/naab/                  # Headers
    ├── external/                      # Dependencies
    │   ├── json/                      # nlohmann/json
    │   ├── fmt/
    │   ├── spdlog/
    │   └── abseil-cpp/
    └── examples/                      # Sample .naab programs
```

## Current Status

### ✅ Working
- Block registry (SQLite database)
- Block metadata storage (JSON files)
- Python block execution (PyExecutor)
- NAAb language parser/interpreter
- Stdlib infrastructure (JSON, IO)
- Method chaining
- Type checking
- Error reporting

### ⚠️ Partial
- C++ block execution (stub only)
- REPL (basic, needs optimization)

### ❌ Not Implemented
- JavaScript/Rust/Go engines
- HTTP remote block fetching
- Block versioning/dependency resolution
- Performance optimization
- Documentation generator

## Key Design Decisions

### 1. JSON is Infrastructure, Not a Block

**Wrong approach (circular):**
```
BLOCK-JSON.json → Contains JSON parser code
└── But JSON parser is needed to READ this file!
```

**Correct approach:**
```
stdlib/json_impl.cpp → Hardcoded JSON parser
└── Used to READ all block JSON files
```

### 2. Blocks Store Code, Engines Execute It

**Blocks don't execute themselves** - they're just data:
```json
{
  "id": "BLOCK-PY-00001",
  "code": "class APIResponse: ..."
}
```

**Engines execute the code FROM blocks:**
```cpp
PyExecutor::execute(block.code);
```

### 3. Separation of Concerns

```
BlockLoader:     WHERE is the code? (database/filesystem)
Engines:         HOW to execute? (Python/C++/etc.)
Interpreter:     WHEN to execute? (call flow)
Stdlib:          WHAT infrastructure? (JSON/IO/etc.)
```

## Example Program

```naab
# polyglot.naab - Mix Python and C++ blocks

use BLOCK-PY-00001 as api_response
use BLOCK-CPP-VECTOR as vec_ops

main {
    # Python block
    let data = api_response.create({
        "status": "success",
        "items": [1, 2, 3, 4, 5]
    })

    # C++ block (when implemented)
    let sum = vec_ops.sum(data.items)

    print("Total:", sum)
}
```

## Benefits of Block Assembly

1. **Language Agnostic**: Use any language for any task
2. **Code Reuse**: Share blocks across projects
3. **Versioning**: Track block versions independently
4. **Modularity**: Small, focused blocks
5. **Discovery**: Searchable block registry
6. **Composition**: Build complex programs from simple blocks

## Future Roadmap

1. Complete C++ engine (full function calling)
2. Add JavaScript engine
3. Remote block repositories
4. Dependency resolution
5. Block testing framework
6. Performance optimization
7. IDE integration
8. Block marketplace

---

**NAAb** = World's first block assembly language
- Blocks = Code (any language)
- Engines = Executors (per language)
- NAAb = Glue (assembly language)
