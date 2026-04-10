# NAAb Architecture

## Overview

NAAb is a polyglot governance-aware scripting language with dual execution engines (bytecode VM and tree-walking interpreter), NaN-boxed values, and a comprehensive static analysis pipeline.

## Pipeline

```
Source → Lexer → Parser → AST → [Type Checker] → Compiler → VM
                                                 ↘ Interpreter (tree-walk)
                        ↘ Governance Engine (static analysis)
```

## Core Components

### Lexer (`src/lexer/lexer.cpp`)
- Hand-written scanner with keyword map
- Supports string interpolation (`${expr}`, `f"{expr}"`)
- Polyglot inline code blocks (`<<python ... >>`)
- Block IDs (`BLOCK-CPP-12345`)

### Parser (`src/parser/parser.cpp`)
- Recursive descent with Pratt-style expression parsing
- Panic-mode error recovery (reports multiple errors)
- ~3400 lines covering all NAAb syntax

### AST (`include/naab/ast.h`)
- ~50 node types (expressions, statements, declarations)
- Visitor pattern for traversal
- Source location tracking for error messages

### Bytecode VM (`src/vm/vm.cpp`, `include/naab/vm.h`)
- Stack-based with ~60 opcodes
- NaN-boxed values (8 bytes each, inline int/double/bool/null)
- Computed goto dispatch (GCC/Clang), switch fallback (MSVC)
- ~8x faster than tree-walker
- Default execution engine

### Compiler (`src/vm/compiler.cpp`)
- Single-pass AST → bytecode
- Local variable resolution at compile time
- Closure upvalue capture
- Constant folding for arithmetic

### Tree-Walking Interpreter (`src/interpreter/interpreter.cpp`)
- Visitor pattern over AST nodes
- `result_` (NaabVal) holds last evaluated value
- Environment chain for scoping
- Used for: BLOCK-* imports, debugging, fallback

### NaN-Boxing (`include/naab/naab_val.h`)
- 8-byte `NaabVal` type using IEEE 754 NaN payload
- Inline: int (32-bit), double, bool, null
- Pointer: string, list, dict, function, generator
- Zero `fromLegacy()`/`toLegacy()` calls outside naab_val.cpp

### Governance Engine (`src/runtime/governance.cpp`)
- 50+ static checks (secrets, placeholders, SQL injection, XSS, etc.)
- 3-tier enforcement: HARD (block), SOFT (override), ADVISORY (warn)
- Taint tracking through variable assignments and function calls
- SARIF/JUnit/JSON report output
- `govern.json` configuration with quality gates and environment overlays

### Module System (`src/runtime/module_resolver.cpp`)
- `use module_name` — Rust-style imports
- `import { x } from "file"` — ES6-style imports
- `use BLOCK-CPP-12345 as Name` — block registry imports
- Thread-safe module cache with `std::shared_mutex`

### Stdlib (`src/stdlib/*_impl.cpp`)
- 17 modules: array, string, math, file, json, time, env, net, crypto, log, uuid, validate, process, regex, collections, http, io
- 150+ functions
- 204 error messages with "Did you mean?" suggestions

### Polyglot Runtime (`src/runtime/`)
- 12 executor adapters: Python, JavaScript, Rust, C++, C#, Go, Nim, Shell, Ruby, PHP, R, Lua
- Subprocess-based execution with timeout and resource limits
- Variable binding via `<<python[x, y] ... >>` syntax
- Output tainted by default for governance

## Key Design Decisions

1. **VM is default** — tree-walker available via `--tree-walk`
2. **Governance is always loaded** — `govern.json` discovered from script directory upward
3. **Polyglot output is always tainted** — prevents trust laundering through external code
4. **NaN-boxing everywhere** — single 8-byte value type, no allocation for primitives
5. **fail-closed security** — lockfile signatures, env var blocking, shell injection prevention
