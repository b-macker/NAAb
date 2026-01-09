# Phase 6: C++ Block Engine & Multi-Language Support

**Status**: Starting
**Estimated Time**: ~12 hours
**Focus**: Complete C++ block execution, add multi-language support

---

## Overview

Phase 5 delivered a complete standard library and polished REPL. Phase 6 focuses on **completing the core block assembly vision**: full C++ block execution and expanding to JavaScript blocks.

**Goal**: Enable NAAb programs to seamlessly use blocks from C++, Python, and JavaScript.

---

## Current State

### ✅ Working (Python Blocks)
```naab
use BLOCK-PY-00001 as api_response
let result = api_response.create({"status": "ok"})
print(result.status)
```

### ⚠️ Partial (C++ Blocks)
```naab
use BLOCK-CPP-VECTOR as vec_ops
let sum = vec_ops.sum([1, 2, 3])  # Stub execution only
```

**Problem**: CppExecutor only runs stub code, doesn't actually compile/load blocks.

### ❌ Not Implemented (JavaScript Blocks)
```naab
use BLOCK-JS-DOM as dom
let elem = dom.createElement("div")  # Not implemented
```

---

## Priorities

### 6a. Complete C++ Block Execution ⏭️
**Time**: ~4 hours | **Impact**: High | **Priority**: 1

**Current State**: `src/runtime/cpp_executor.cpp` has stub implementation

**What's Missing**:
1. Dynamic compilation (g++ or clang++)
2. Shared library loading (dlopen/dlsym)
3. Symbol resolution and calling conventions
4. Type marshalling (NAAb ↔ C++ types)
5. Error handling for compilation failures

**Implementation Plan**:

#### Step 1: Dynamic Compilation (1.5 hours)
Create temporary C++ file and compile to .so:

```cpp
class CppCompiler {
public:
    // Compile C++ code to shared library
    std::string compile(const std::string& code, const std::string& block_id);

private:
    std::string createTempFile(const std::string& code);
    bool runCompiler(const std::string& source_file,
                     const std::string& output_file);
    void cleanup(const std::string& temp_file);
};
```

**Output**: `/tmp/naab_BLOCK-CPP-12345.so`

#### Step 2: Dynamic Loading (1 hour)
Load shared library and resolve symbols:

```cpp
class CppLibrary {
public:
    CppLibrary(const std::string& so_path);
    ~CppLibrary();

    void* getSymbol(const std::string& name);

private:
    void* handle_;
    std::string path_;
};
```

#### Step 3: Type Marshalling (1 hour)
Convert between NAAb and C++ types:

```cpp
class TypeMarshaller {
public:
    // NAAb → C++
    int toInt(const Value& val);
    double toDouble(const Value& val);
    std::string toString(const Value& val);
    std::vector<Value> toArray(const Value& val);

    // C++ → NAAb
    Value fromInt(int i);
    Value fromDouble(double d);
    Value fromString(const std::string& s);
    Value fromArray(const std::vector<Value>& arr);
};
```

#### Step 4: Function Calling (30 minutes)
Invoke C++ functions with proper calling convention:

```cpp
Value CppExecutor::call(const std::string& function_name,
                        const std::vector<Value>& args) {
    // 1. Resolve symbol
    auto* func_ptr = library_->getSymbol(function_name);

    // 2. Marshal arguments
    std::vector<void*> cpp_args = marshaller_.marshal(args);

    // 3. Call function (platform-specific)
    void* result = invokeFunction(func_ptr, cpp_args);

    // 4. Marshal result back
    return marshaller_.fromCpp(result);
}
```

**Testing**:
```naab
# test_cpp_block.naab
use BLOCK-CPP-VECTOR as vec

let result = vec.sum([1, 2, 3, 4, 5])
print(result)  # Should print 15
```

**Deliverables**:
- `src/runtime/cpp_compiler.cpp` (new)
- `src/runtime/cpp_library.cpp` (new)
- `src/runtime/type_marshaller.cpp` (new)
- Updated `src/runtime/cpp_executor.cpp`
- `test_cpp_execution.cpp`
- `CPP_ENGINE.md` documentation

---

### 6b. JavaScript Block Engine ⏭️
**Time**: ~3 hours | **Impact**: High | **Priority**: 2

Add JavaScript execution using V8 or QuickJS engine.

**Library Choice**: QuickJS (lighter than V8, ~600KB)

**Why QuickJS?**
- Lightweight (vs V8's ~20MB)
- Embeddable
- ES2020 support
- No external dependencies
- Active development

**Implementation**:

#### Step 1: Integrate QuickJS (1 hour)
```bash
# Download QuickJS
wget https://bellard.org/quickjs/quickjs-2021-03-27.tar.xz
tar xf quickjs-2021-03-27.tar.xz
mv quickjs-2021-03-27 external/quickjs
cd external/quickjs && make libquickjs.a
```

#### Step 2: JavaScript Executor (1.5 hours)
```cpp
class JsExecutor {
public:
    JsExecutor();
    ~JsExecutor();

    Value execute(const std::string& code);
    Value call(const std::string& function_name,
               const std::vector<Value>& args);

private:
    JSRuntime* rt_;
    JSContext* ctx_;

    JSValue toJSValue(const Value& val);
    Value fromJSValue(JSValue val);
};
```

#### Step 3: Integration (30 minutes)
Update interpreter to route JavaScript blocks:

```cpp
// In Interpreter::loadBlock()
if (block.language == "javascript") {
    auto executor = std::make_unique<JsExecutor>();
    executor->execute(block.code);
    // Store in environment
}
```

**Example**:
```naab
# Use JavaScript block
use BLOCK-JS-UTILS as jsutil

let reversed = jsutil.reverseString("hello")
print(reversed)  # "olleh"
```

**Testing**:
```cpp
// test_js_execution.cpp
TEST(JsExecutor, BasicExecution) {
    JsExecutor executor;
    std::string code = R"(
        function add(a, b) {
            return a + b;
        }
    )";
    executor.execute(code);

    auto result = executor.call("add", {Value(5), Value(3)});
    EXPECT_EQ(result.asInt(), 8);
}
```

**Deliverables**:
- `external/quickjs/` (library)
- `src/runtime/js_executor.cpp`
- `include/naab/js_executor.h`
- `test_js_execution.cpp`
- `JS_ENGINE.md` documentation

---

### 6c. Block Language Registry ⏭️
**Time**: ~2 hours | **Impact**: Medium | **Priority**: 3

Centralize language-specific executors.

**Current Problem**: Interpreter hardcodes Python/C++ checks

**Solution**: Language registry pattern

```cpp
class LanguageRegistry {
public:
    void registerExecutor(const std::string& language,
                          std::unique_ptr<Executor> executor);

    Executor* getExecutor(const std::string& language);

    std::vector<std::string> supportedLanguages() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Executor>> executors_;
};

// Usage
registry.registerExecutor("python", std::make_unique<PyExecutor>());
registry.registerExecutor("cpp", std::make_unique<CppExecutor>());
registry.registerExecutor("javascript", std::make_unique<JsExecutor>());

// Later
auto* executor = registry.getExecutor(block.language);
executor->execute(block.code);
```

**Benefits**:
- Easy to add new languages
- Clean separation of concerns
- Plugin-like architecture

**Deliverables**:
- `src/runtime/language_registry.cpp`
- `include/naab/language_registry.h`
- Updated interpreter to use registry
- `LANGUAGE_REGISTRY.md`

---

### 6d. Block Testing Framework ⏭️
**Time**: ~2 hours | **Impact**: Medium | **Priority**: 4

Test individual blocks before using them.

**Goal**: Ensure blocks work correctly in isolation

**Implementation**:

```cpp
// Block test format (JSON)
{
  "block_id": "BLOCK-PY-00001",
  "tests": [
    {
      "name": "create_returns_object",
      "code": "result = api_response.create({'status': 'ok'})",
      "assertions": [
        {"type": "equals", "value": "result.status", "expected": "ok"}
      ]
    }
  ]
}
```

**Test Runner**:
```cpp
class BlockTester {
public:
    TestResults runTests(const std::string& block_id);

private:
    bool loadTestDefinition(const std::string& block_id);
    bool runSingleTest(const BlockTest& test);
    bool checkAssertion(const Assertion& assertion, const Value& result);
};
```

**CLI Tool**:
```bash
naab-test BLOCK-PY-00001
# Output:
# Running tests for BLOCK-PY-00001...
# ✓ create_returns_object
# ✓ handles_empty_data
# ✗ validates_schema (expected: true, got: false)
#
# 2 passed, 1 failed
```

**Deliverables**:
- `src/testing/block_tester.cpp`
- `src/cli/test_main.cpp`
- `examples/block_tests/BLOCK-PY-00001.test.json`
- `BLOCK_TESTING.md`

---

### 6e. Performance Profiling ⏭️
**Time**: ~1.5 hours | **Impact**: Low | **Priority**: 5

Add built-in performance profiling.

**Features**:
- Function call timing
- Block load time tracking
- Memory usage monitoring
- Execution statistics

**Implementation**:

```cpp
class Profiler {
public:
    void startFunction(const std::string& name);
    void endFunction(const std::string& name);

    void startBlock(const std::string& block_id);
    void endBlock(const std::string& block_id);

    ProfileReport generateReport();

private:
    std::unordered_map<std::string, Timer> timers_;
    std::vector<ProfileEntry> entries_;
};
```

**Usage**:
```naab
# Enable profiling
:profile on

let result = expensive_computation()

# Show profile
:profile report

# Output:
# Function Calls:
#   expensive_computation: 245ms (1 call)
#   helper_function: 12ms (15 calls)
#
# Block Loading:
#   BLOCK-PY-00001: 5ms
#   BLOCK-CPP-12345: 125ms (compilation)
```

**Deliverables**:
- `src/profiling/profiler.cpp`
- `include/naab/profiler.h`
- REPL integration (`:profile` commands)
- `PROFILING.md`

---

## Phase 6 Timeline

```
Week 1:
  Day 1-2: C++ compilation infrastructure (Step 1-2)
  Day 3: Type marshalling and function calling (Step 3-4)
  Day 4: JavaScript engine integration (QuickJS)
  Day 5: JavaScript executor implementation

Week 2:
  Day 1: Language registry refactoring
  Day 2: Block testing framework
  Day 3: Performance profiling
  Day 4-5: Testing, documentation, polish
```

---

## Success Criteria

- [ ] C++ blocks compile and execute dynamically
- [ ] JavaScript blocks execute via QuickJS
- [ ] All three languages (Python, C++, JS) work in same program
- [ ] Language registry makes adding new languages easy
- [ ] Block testing framework validates block correctness
- [ ] Performance profiling shows bottlenecks

---

## Example: Multi-Language Program

After Phase 6, this should work:

```naab
# polyglot.naab - Mix Python, C++, and JavaScript

use BLOCK-PY-API as api        # Python: API handling
use BLOCK-CPP-VECTOR as vec    # C++: Fast math
use BLOCK-JS-FORMAT as fmt     # JavaScript: Formatting

main {
    # Fetch data (Python)
    let data = api.fetch("https://example.com/numbers")

    # Process with C++ (fast)
    let sum = vec.sum(data.numbers)
    let avg = vec.average(data.numbers)

    # Format output (JavaScript)
    let report = fmt.template({
        "total": sum,
        "average": avg,
        "count": data.numbers.length
    })

    print(report)
}
```

Output:
```
Total: 15,234
Average: 152.34
Count: 100 items
```

---

## Dependencies

### External Libraries

1. **QuickJS** (JavaScript engine)
   ```bash
   wget https://bellard.org/quickjs/quickjs-2021-03-27.tar.xz
   tar xf quickjs-2021-03-27.tar.xz
   cd quickjs-2021-03-27 && make libquickjs.a
   ```

2. **dl (dynamic loading)** - Already available on Linux/Android
   ```cpp
   #include <dlfcn.h>  // dlopen, dlsym, dlclose
   ```

3. **g++/clang++** - Already installed
   ```bash
   pkg install clang  # Or already have it
   ```

---

## Testing Strategy

### C++ Engine Tests

```cpp
TEST(CppExecutor, CompileSimpleFunction) {
    std::string code = R"(
        extern "C" int add(int a, int b) {
            return a + b;
        }
    )";

    CppExecutor executor;
    executor.compile(code, "TEST-001");
    auto result = executor.call("add", {Value(5), Value(3)});
    EXPECT_EQ(result.asInt(), 8);
}

TEST(CppExecutor, HandleCompilationError) {
    std::string code = "invalid C++ code!!!";

    CppExecutor executor;
    EXPECT_THROW(executor.compile(code, "TEST-002"), CompilationError);
}
```

### JavaScript Engine Tests

```cpp
TEST(JsExecutor, BasicExecution) {
    JsExecutor executor;
    std::string code = R"(
        function greet(name) {
            return "Hello, " + name;
        }
    )";
    executor.execute(code);

    auto result = executor.call("greet", {Value("NAAb")});
    EXPECT_EQ(result.asString(), "Hello, NAAb");
}
```

### Multi-Language Integration Test

```naab
# test_polyglot.naab
use BLOCK-PY-MATH as pymath
use BLOCK-CPP-MATH as cppmath
use BLOCK-JS-MATH as jsmath

main {
    let a = 10
    let b = 5

    # All should return 15
    assert(pymath.add(a, b) == 15)
    assert(cppmath.add(a, b) == 15)
    assert(jsmath.add(a, b) == 15)

    print("All languages working!")
}
```

---

## Risk Mitigation

### Risks

1. **C++ Compilation Slow**: Compiling blocks at runtime may be slow
   - Mitigation: Cache compiled .so files, only recompile on changes
   - Fallback: Precompiled blocks option

2. **QuickJS Size**: May increase binary size
   - Mitigation: Static linking only needed parts (~600KB acceptable)
   - Fallback: Make JavaScript support optional (compile flag)

3. **Type Marshalling Complex**: Converting types may have edge cases
   - Mitigation: Start with simple types (int, double, string), expand gradually
   - Fallback: Explicit type conversion functions

4. **Platform Dependencies**: Dynamic loading varies by OS
   - Mitigation: Abstract dlopen/LoadLibrary behind interface
   - Fallback: Linux/Android focus first, Windows later

---

## Deliverables

### Code Deliverables

1. **C++ Engine**
   - `src/runtime/cpp_compiler.cpp`
   - `src/runtime/cpp_library.cpp`
   - `src/runtime/type_marshaller.cpp`
   - Updated `src/runtime/cpp_executor.cpp`

2. **JavaScript Engine**
   - `src/runtime/js_executor.cpp`
   - `include/naab/js_executor.h`

3. **Infrastructure**
   - `src/runtime/language_registry.cpp`
   - `include/naab/language_registry.h`

4. **Testing**
   - `src/testing/block_tester.cpp`
   - `src/cli/test_main.cpp`

5. **Profiling**
   - `src/profiling/profiler.cpp`
   - `include/naab/profiler.h`

### Test Deliverables

- `test_cpp_execution.cpp`
- `test_js_execution.cpp`
- `test_language_registry.cpp`
- `test_block_tester.cpp`
- `test_polyglot.naab` (integration)

### Documentation Deliverables

- `CPP_ENGINE.md` (C++ compilation and execution)
- `JS_ENGINE.md` (JavaScript integration)
- `LANGUAGE_REGISTRY.md` (Adding new languages)
- `BLOCK_TESTING.md` (Testing framework)
- `PROFILING.md` (Performance profiling)
- `PHASE_6_COMPLETE.md` (Final report)

---

## Metrics

Track progress:

- **C++ Blocks**: Compile time <5 seconds per block
- **JavaScript Execution**: Startup overhead <100ms
- **Type Marshalling**: Conversion overhead <1ms
- **Multi-Language**: No performance penalty for mixed programs
- **Block Tests**: Test execution <1 second per block
- **Profiling**: Overhead <5% when enabled

---

## Future Work (Phase 7+)

After Phase 6, consider:

1. **More Languages**: Rust, Go, Ruby via FFI
2. **Remote Blocks**: Fetch blocks from HTTP registries
3. **Dependency Resolution**: Automatic block dependency handling
4. **Package Manager**: `naab install BLOCK-ID`
5. **Debugger**: Step through multi-language code
6. **LSP Server**: IDE integration
7. **JIT Optimization**: LLVM backend for hot paths

---

**Phase 6 starts now!**

Let's complete the core block assembly vision: **seamless multi-language block execution**.
