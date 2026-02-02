# NAAb Language - Developer Guide

**Version**: 0.2.0
**Last Updated**: January 29, 2026

This guide helps developers contribute to the NAAb language implementation. It covers building from source, understanding the architecture, common pitfalls, and the documentation workflow.

## Table of Contents

1. [Building from Source](#building-from-source)
2. [Architecture Overview](#architecture-overview)
3. [Common Pitfalls](#common-pitfalls)
4. [Contributing Documentation](#contributing-documentation)
5. [Common C++ Modifications](#common-cpp-modifications)
6. [Testing Your Changes](#testing-your-changes)

---

## Building from Source

### Prerequisites

```bash
# Required tools
- CMake 3.15+
- C++17 compatible compiler (GCC 9+, Clang 10+)
- Python 3.8+ with development headers
- Node.js 14+ (for JavaScript executor)
- fmt library
```

### Build Steps

```bash
# 1. Navigate to project directory
cd ~/.naab/language

# 2. Create build directory
mkdir -p build
cd build

# 3. Configure with CMake
cmake ..

# 4. Build the project
cmake --build . --target naab-lang -j$(nproc)

# 5. Test the binary
./naab-lang --help
```

### Build Targets

```bash
# Main executable
cmake --build build --target naab-lang

# Clean build (recommended when changing headers)
rm -rf build && mkdir build && cd build && cmake .. && cmake --build .
```

### Common Build Issues

**Issue 1: Python headers not found**
```bash
# Install Python development headers
# Debian/Ubuntu:
apt install python3-dev

# Termux:
pkg install python
```

**Issue 2: fmt library not found**
```bash
# Install fmt
# Debian/Ubuntu:
apt install libfmt-dev

# Termux:
pkg install fmt
```

**Issue 3: Compilation errors with C++17 features**
```bash
# Ensure your compiler supports C++17
g++ --version  # Should be 9.0+
clang++ --version  # Should be 10.0+
```

---

## Architecture Overview

### High-Level Components

```
┌─────────────────────────────────────────────────┐
│                   CLI (main.cpp)                │
│  - Argument parsing                             │
│  - Script execution orchestration               │
└───────────────┬─────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────┐
│              Lexer (lexer.cpp)                  │
│  - Tokenizes source code                        │
│  - Recognizes keywords, operators, literals     │
└───────────────┬─────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────┐
│              Parser (parser.cpp)                │
│  - Builds Abstract Syntax Tree (AST)            │
│  - Handles expressions, statements, types       │
└───────────────┬─────────────────────────────────┘
                │
                ▼
┌─────────────────────────────────────────────────┐
│           Interpreter (interpreter.cpp)         │
│  - AST visitor pattern                          │
│  - Executes NAAb code directly                  │
│  - Manages environments (scoping)               │
│  - Coordinates with stdlib and executors        │
└───────────────┬─────────────────────────────────┘
                │
        ┌───────┴────────┬──────────────┐
        ▼                ▼              ▼
┌──────────────┐  ┌─────────────┐  ┌─────────────┐
│   Standard   │  │  Polyglot   │  │   Module    │
│   Library    │  │  Executors  │  │   System    │
│  (stdlib/)   │  │  (runtime/) │  │ (modules/)  │
└──────────────┘  └─────────────┘  └─────────────┘
```

### Key Directories

```
language/
├── src/
│   ├── cli/              # Command-line interface
│   │   └── main.cpp      # Entry point, argument parsing
│   ├── lexer/            # Tokenization
│   │   ├── lexer.cpp
│   │   └── lexer.h
│   ├── parser/           # Syntax analysis
│   │   ├── parser.cpp
│   │   └── ast.h         # AST node definitions
│   ├── interpreter/      # Execution engine
│   │   ├── interpreter.cpp
│   │   └── cycle_detector.cpp  # Garbage collection
│   ├── stdlib/           # Standard library modules
│   │   ├── stdlib.cpp    # Module registration
│   │   ├── io_impl.cpp   # I/O operations
│   │   ├── json_impl.cpp # JSON parsing
│   │   ├── string_impl.cpp
│   │   ├── env_impl.cpp  # Environment variables
│   │   └── ...
│   └── runtime/          # Polyglot execution
│       ├── language_registry.cpp
│       ├── python_executor.cpp
│       ├── js_executor.cpp
│       └── cpp_executor.cpp
├── include/naab/         # Public headers
│   ├── interpreter.h
│   ├── stdlib.h
│   ├── stdlib_new_modules.h
│   └── ...
├── docs/
│   ├── book/             # Official documentation
│   │   ├── chapter*.md   # Documentation chapters
│   │   └── verification/ # Verification tests
│   └── DEVELOPER_GUIDE.md (this file)
└── CMakeLists.txt        # Build configuration
```

### Data Flow Example

```cpp
// User runs: ./naab-lang run script.naab arg1 arg2

1. CLI (main.cpp)
   - Parses: run script.naab arg1 arg2
   - Reads file: script.naab
   - Creates Lexer with source code

2. Lexer
   - Tokenizes source: use io\nmain { io.write("Hello") }
   - Returns: [USE, IDENTIFIER(io), MAIN, LBRACE, ...]

3. Parser
   - Builds AST from tokens
   - Creates: Program { uses: [ModuleUseStmt], main_block: MainBlock }

4. Interpreter
   - Visits AST nodes
   - Executes: visit(ModuleUseStmt) -> loads stdlib module
   - Executes: visit(MainBlock) -> runs main code
   - Calls: stdlib->getModule("io")->call("write", ["Hello"])

5. StdLib Module (IOModule)
   - Executes: std::cout << "Hello"
   - Returns: null value
```

---

## Common Pitfalls

This section documents issues encountered during development and their solutions.

### 1. Circular Dependencies

**Problem:** Header files including each other creates circular dependencies.

**Solution:** Use forward declarations and include only in .cpp files.

```cpp
// GOOD: Forward declaration in header
// interpreter.h
namespace naab { namespace stdlib { class StdLib; } }
class Interpreter {
    std::unique_ptr<stdlib::StdLib> stdlib_;
};

// interpreter.cpp
#include "naab/stdlib.h"  // Include in implementation
```

### 2. Header Include Paths

**Problem:** Headers not found during compilation.

**Solution:** Use consistent include paths relative to `include/` directory.

```cpp
// CORRECT: All includes relative to include/ directory
#include "naab/interpreter.h"
#include "naab/stdlib.h"
#include "naab/stdlib_new_modules.h"
```

### 3. Module Registration

**Problem:** New stdlib modules not accessible in NAAb code.

**Solution:** Register module in `stdlib.cpp`:

```cpp
// src/stdlib/stdlib.cpp
void StdLib::registerModules() {
    modules_["io"] = std::make_shared<IOModule>();
    modules_["json"] = std::make_shared<JSONModule>();

    // NEW: Add your module here
    modules_["mymodule"] = std::make_shared<MyModule>();
}
```

### 4. Reserved Keywords

**Problem:** Using reserved keywords as variable names.

**Solution:** Check `lexer.cpp` for reserved keywords and avoid them:

```cpp
// src/lexer/lexer.cpp - keywords_ map
keywords_["config"] = TokenType::CONFIG;
keywords_["main"] = TokenType::MAIN;
keywords_["fn"] = TokenType::FUNCTION;
```

### 5. Python Polyglot Multi-Line Issues

**Problem:** Multi-line Python dicts in polyglot blocks cause syntax errors.

**Solution:** Use single-line dict syntax:

```naab
// GOOD
let stats = <<python[data]
{"mean": sum(data) / len(data), "min": min(data)}
>>

// BAD
let stats = <<python[data]
{
    "mean": sum(data) / len(data),
    "min": min(data)
}
>>
```

### 6. Callback Pattern for Interpreter Access

**Problem:** Stdlib modules need access to interpreter state.

**Solution:** Use callback pattern (see `EnvModule::ArgsProvider` example):

```cpp
class EnvModule : public Module {
public:
    using ArgsProvider = std::function<std::vector<std::string>()>;

    void setArgsProvider(ArgsProvider provider) {
        args_provider_ = std::move(provider);
    }

private:
    ArgsProvider args_provider_;
};

// In Interpreter constructor:
env_mod->setArgsProvider([this]() { return this->script_args_; });
```

---

## Contributing Documentation

NAAb uses a **book + verification** pattern to ensure documentation accuracy.

### Documentation Structure

```
docs/book/
├── chapter01.md              # Introduction
├── chapter02.md              # Types and Data Structures
├── ...
└── verification/
    ├── ISSUES.md             # Known issues tracker
    ├── ch01_intro/
    │   ├── hello.naab        # Runnable test
    │   └── entry_point_errors.md
    ├── ch02_basics/
    │   ├── dictionaries.naab
    │   └── structs_vs_dicts.naab
    └── ...
```

### The Book + Verification Pattern

**Rule:** Every feature documented in the book MUST have a working verification test.

**Why?**
- Prevents documentation drift
- Provides runnable examples
- Catches regressions

### Adding New Documentation

**Step 1: Update the chapter**
```markdown
## 8.3 The `string` Module

Use `string.length()` to get the length of a string:

\`\`\`naab
use string as str

main {
    let text = "Hello!"
    let len = str.length(text)
    print("Length:", len)
}
\`\`\`
```

**Step 2: Create verification test**
```naab
// docs/book/verification/ch08_text_math/string_test.naab

use io
use string as str

main {
    io.write("=== String Test ===\n")

    let text = "Hello!"
    let len = str.length(text)

    if len == 6 {
        io.write("✓ string.length() works\n")
    }
}
```

**Step 3: Run the test**
```bash
./build/naab-lang run docs/book/verification/ch08_text_math/string_test.naab
```

**Step 4: Document in ISSUES.md**
```markdown
| ISS-XXX | Ch 08 | String Functions | ✅ Resolved | Test: `ch08_text_math/string_test.naab` (passing). |
```

---

## Common C++ Modifications

### Adding a New Stdlib Function

**Example: Adding `string.reverse()` function**

**Step 1: Update hasFunction()**
```cpp
// src/stdlib/string_impl.cpp

bool StringModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "length", "substring", "concat",
        "reverse"  // Add new function
    };
    return functions.count(name) > 0;
}
```

**Step 2: Implement in call()**
```cpp
std::shared_ptr<interpreter::Value> StringModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // ... existing functions ...

    if (function_name == "reverse") {
        if (args.size() != 1) {
            throw std::runtime_error("reverse() takes exactly 1 argument");
        }

        std::string str = getString(args[0]);
        std::string reversed(str.rbegin(), str.rend());

        return makeString(reversed);
    }
}
```

**Step 3: Test**
```bash
cmake --build build --target naab-lang

echo 'use string as str
main {
    print(str.reverse("hello"))
}' > test.naab

./build/naab-lang run test.naab  # Output: olleh
```

---

## Testing Your Changes

### Manual Testing

```bash
# 1. Build
cmake --build build --target naab-lang

# 2. Create test script
cat > test.naab << 'EOF'
use io

main {
    io.write("Hello!\n")
}
EOF

# 3. Run test
./build/naab-lang run test.naab
```

### Verification Testing

```bash
# Run all verification tests in a chapter
for file in docs/book/verification/ch08_text_math/*.naab; do
    echo "Testing: $file"
    ./build/naab-lang run "$file"
done
```

### Debugging Tips

**Enable verbose output:**
```bash
./build/naab-lang run script.naab --verbose
```

**Profile execution:**
```bash
./build/naab-lang run script.naab --profile
```

**Use fmt::print for debugging:**
```cpp
fmt::print("[DEBUG] Variable: {}\n", value);
```

---

## Best Practices

### Code Style

- Classes: `PascalCase`
- Functions: `camelCase`
- Variables: `snake_case`
- Constants: `UPPER_SNAKE_CASE`

### Error Messages

```cpp
// GOOD: Descriptive
throw std::runtime_error("reverse() takes exactly 1 argument, got " +
                        std::to_string(args.size()));

// BAD: Vague
throw std::runtime_error("Wrong arguments");
```

### Memory Management

```cpp
// GOOD: Smart pointers
auto val = std::make_shared<Value>();

// BAD: Raw pointers
Value* val = new Value();
delete val;
```

---

## Summary Checklist

When contributing a new feature:

- [ ] Feature implemented in C++
- [ ] Code follows project style
- [ ] No circular dependencies
- [ ] Header includes correct
- [ ] Module registered (if applicable)
- [ ] Chapter documentation updated
- [ ] Verification test created
- [ ] Verification test passes
- [ ] ISSUES.md updated
- [ ] No compiler warnings

**Remember:** If it's not tested, it's not done!
