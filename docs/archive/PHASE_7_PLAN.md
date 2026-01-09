# Phase 7: Interpreter Integration & Block Loading

## Overview

Phase 7 integrates the multi-language execution system built in Phase 6 into the NAAb interpreter and REPL, enabling actual block loading and execution in NAAb programs.

**Goal**: Make the block assembly vision real - load and execute blocks from any language in NAAb programs.

**Duration**: ~1 week
**Priority**: HIGH (Core functionality)

---

## What We Have (Phase 6 Complete)

✅ **C++ Executor** - Dynamic compilation and execution
✅ **JavaScript Executor** - QuickJS integration
✅ **Language Registry** - Plugin architecture for executors
✅ **Testing Framework** - Automated block testing
✅ **Profiling System** - Performance measurement

**Missing**: Integration with actual NAAb interpreter and REPL!

---

## Phase 7 Components

### 7a. Interpreter Block Loading ⭐ (Priority 1)
**Time**: ~3 hours | **Impact**: Critical

Integrate language registry with interpreter's `use` statement.

**Current State**:
```cpp
// Interpreter::loadBlock() - hardcoded Python only
if (block.language == "python") {
    // Execute Python block
}
```

**Target State**:
```cpp
// Interpreter::loadBlock() - uses registry
auto& registry = LanguageRegistry::instance();
auto* executor = registry.getExecutor(block.language);
executor->execute(block.code);
```

**Implementation**:

1. **Modify Interpreter::loadBlock()**
   ```cpp
   void Interpreter::loadBlock(const std::string& block_id, const std::string& alias) {
       // 1. Query block registry (database)
       auto block = block_loader_.loadBlock(block_id);

       // 2. Get executor from language registry
       auto& registry = runtime::LanguageRegistry::instance();
       auto* executor = registry.getExecutor(block.language);

       if (!executor) {
           throw std::runtime_error("Unsupported language: " + block.language);
       }

       // 3. Execute block code
       if (!executor->execute(block.code)) {
           throw std::runtime_error("Failed to execute block: " + block_id);
       }

       // 4. Store in environment
       auto block_value = std::make_shared<BlockValue>(block_id, executor);
       environment_->setVariable(alias, block_value);
   }
   ```

2. **Create BlockValue Class**
   ```cpp
   class BlockValue : public Value {
   public:
       BlockValue(const std::string& block_id, runtime::Executor* executor);

       // Call method on block
       std::shared_ptr<Value> callMethod(
           const std::string& method,
           const std::vector<std::shared_ptr<Value>>& args
       ) override;

   private:
       std::string block_id_;
       runtime::Executor* executor_;
   };
   ```

3. **Update Interpreter::visitMethodCall()**
   ```cpp
   std::shared_ptr<Value> Interpreter::visitMethodCall(MethodCallNode* node) {
       auto object = evaluate(node->object);

       if (auto block = std::dynamic_pointer_cast<BlockValue>(object)) {
           return block->callMethod(node->method, args);
       }

       // ... existing code ...
   }
   ```

**Deliverables**:
- Updated `src/interpreter/interpreter.cpp`
- New `include/naab/block_value.h`
- New `src/interpreter/block_value.cpp`
- Test: `examples/test_block_loading.naab`

---

### 7b. REPL Block Commands ⭐ (Priority 2)
**Time**: ~2 hours | **Impact**: High

Add block loading and management commands to REPL.

**Commands**:

```naab
# Load block
:load BLOCK-JS-MATH as math
> Loaded BLOCK-JS-MATH (javascript) as 'math'

# List loaded blocks
:blocks
> Loaded blocks:
>   math: BLOCK-JS-MATH (javascript)
>   api: BLOCK-PY-API (python)

# Block info
:info math
> Block: BLOCK-JS-MATH
> Language: javascript
> Functions: add, multiply, divide
> Status: ready

# Reload block
:reload math
> Reloaded math

# Unload block
:unload math
> Unloaded math

# List available languages
:languages
> Supported languages:
>   - cpp
>   - javascript
>   (use :register <lang> to add more)
```

**Implementation**:

```cpp
// In repl.cpp or repl_commands.cpp
void ReplCommandHandler::handleCommand(const std::string& cmd) {
    if (cmd.starts_with(":load ")) {
        handleLoadBlock(cmd);
    } else if (cmd == ":blocks") {
        handleListBlocks();
    } else if (cmd.starts_with(":info ")) {
        handleBlockInfo(cmd);
    } else if (cmd.starts_with(":reload ")) {
        handleReloadBlock(cmd);
    } else if (cmd.starts_with(":unload ")) {
        handleUnloadBlock(cmd);
    } else if (cmd == ":languages") {
        handleListLanguages();
    }
}
```

**Deliverables**:
- `src/repl/repl_commands.cpp`
- `include/naab/repl_commands.h`
- Updated `src/repl/repl.cpp`
- Documentation: `REPL_COMMANDS.md`

---

### 7c. Executor Registration ⭐ (Priority 3)
**Time**: ~1 hour | **Impact**: Medium

Initialize language registry on startup with available executors.

**Startup Sequence**:

```cpp
// In main.cpp
int main() {
    // Initialize language registry
    auto& registry = runtime::LanguageRegistry::instance();

    // Register available executors
    registry.registerExecutor("cpp",
        std::make_unique<runtime::CppExecutorAdapter>());

    registry.registerExecutor("javascript",
        std::make_unique<runtime::JsExecutorAdapter>());

    // Check for Python availability
    #ifdef PYTHON_AVAILABLE
    registry.registerExecutor("python",
        std::make_unique<runtime::PyExecutorAdapter>());
    #endif

    fmt::print("NAAb v{}.{}.{}\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    fmt::print("Languages: {}\n",
               fmt::join(registry.supportedLanguages(), ", "));

    // Start interpreter/REPL
    // ...
}
```

**Deliverables**:
- Updated `src/cli/main.cpp`
- Updated `src/repl/repl.cpp`
- Startup banner showing supported languages

---

### 7d. Block Examples ⭐ (Priority 4)
**Time**: ~2 hours | **Impact**: High

Create example programs demonstrating multi-language blocks.

**Example 1: C++ Math Block**
```naab
# examples/cpp_math.naab
use BLOCK-CPP-MATH as math

main {
    let sum = math.add(10, 20)
    let product = math.multiply(5, 7)

    print("Sum:", sum)
    print("Product:", product)
}
```

**Example 2: JavaScript Utilities**
```naab
# examples/js_utils.naab
use BLOCK-JS-STRING as str

main {
    let greeting = str.format("Hello, {}!", "World")
    let upper = str.toUpper(greeting)

    print(upper)
}
```

**Example 3: Multi-Language (Polyglot)**
```naab
# examples/polyglot.naab
use BLOCK-CPP-VECTOR as vec    # C++: Fast math
use BLOCK-JS-FORMAT as fmt     # JavaScript: Formatting

main {
    # Process with C++ (fast)
    let numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    let sum = vec.sum(numbers)
    let avg = vec.average(numbers)

    # Format output (JavaScript)
    let report = fmt.template({
        "total": sum,
        "average": avg,
        "count": numbers.length
    })

    print(report)
}
```

**Deliverables**:
- `examples/cpp_math.naab`
- `examples/js_utils.naab`
- `examples/polyglot.naab`
- `examples/blocks/` (sample block JSON files)

---

### 7e. Integration Testing ⭐ (Priority 5)
**Time**: ~2 hours | **Impact**: Critical

Test complete end-to-end block loading and execution.

**Test Cases**:

1. **Load C++ Block**
   ```cpp
   TEST(Integration, LoadCppBlock) {
       Interpreter interp;
       interp.execute("use BLOCK-CPP-MATH as math");
       auto result = interp.execute("math.add(5, 3)");
       EXPECT_EQ(result->toInt(), 8);
   }
   ```

2. **Load JavaScript Block**
   ```cpp
   TEST(Integration, LoadJsBlock) {
       Interpreter interp;
       interp.execute("use BLOCK-JS-MATH as math");
       auto result = interp.execute("math.multiply(7, 6)");
       EXPECT_EQ(result->toInt(), 42);
   }
   ```

3. **Multi-Language Program**
   ```cpp
   TEST(Integration, MultiLanguage) {
       Interpreter interp;
       interp.execute("use BLOCK-CPP-MATH as cpp_math");
       interp.execute("use BLOCK-JS-MATH as js_math");

       auto cpp_result = interp.execute("cpp_math.add(10, 20)");
       auto js_result = interp.execute("js_math.add(10, 20)");

       EXPECT_EQ(cpp_result->toInt(), 30);
       EXPECT_EQ(js_result->toInt(), 30);
   }
   ```

4. **Error Handling**
   ```cpp
   TEST(Integration, UnsupportedLanguage) {
       Interpreter interp;
       EXPECT_THROW(
           interp.execute("use BLOCK-RUBY-001 as rb"),
           std::runtime_error
       );
   }
   ```

**Deliverables**:
- `test_integration.cpp`
- Test blocks in `examples/blocks/`
- Integration test suite

---

## Architecture Updates

### Before Phase 7

```
┌──────────────┐
│ Interpreter  │ (Hardcoded Python only)
└──────────────┘

┌──────────────┐
│ REPL         │ (No block commands)
└──────────────┘

┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│ CppExecutor  │     │ JsExecutor   │     │ Language     │
│              │     │              │     │ Registry     │
└──────────────┘     └──────────────┘     └──────────────┘
(Not connected to interpreter)
```

### After Phase 7

```
┌─────────────────────────────────────────────────────────┐
│ Interpreter                                             │
│ • loadBlock(id, alias) → uses Language Registry         │
│ • callMethod() → dispatches to correct executor         │
└────────────────────────┬────────────────────────────────┘
                         ↓
        ┌────────────────────────────────────┐
        │ Language Registry (Singleton)       │
        │ • getExecutor(language)             │
        │ • Registered executors:             │
        │   - cpp: CppExecutorAdapter         │
        │   - javascript: JsExecutorAdapter   │
        └────────────────────────────────────┘
                         ↓
        ┌────────────────┬────────────────┐
        ↓                ↓                ↓
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ CppExecutor  │  │ JsExecutor   │  │ PyExecutor   │
└──────────────┘  └──────────────┘  └──────────────┘

┌─────────────────────────────────────────────────────────┐
│ REPL                                                    │
│ • :load <block> as <alias>                              │
│ • :blocks, :info, :reload, :unload                      │
│ • :languages                                            │
└─────────────────────────────────────────────────────────┘
```

---

## Success Criteria

- [ ] Load C++ blocks with `use BLOCK-CPP-XXX as name`
- [ ] Load JavaScript blocks with `use BLOCK-JS-XXX as name`
- [ ] Call methods on loaded blocks: `name.method(args)`
- [ ] REPL commands work: `:load`, `:blocks`, `:info`, etc.
- [ ] Multi-language programs execute correctly
- [ ] Error messages for unsupported languages
- [ ] Integration tests pass

---

## Timeline

```
Day 1-2: Interpreter block loading integration
Day 3: REPL block commands
Day 4: Executor registration and startup
Day 5: Block examples and documentation
Day 6-7: Integration testing and polish
```

---

## Dependencies

- Phase 6 complete (✅ Done)
- Block registry (SQLite database)
- Block JSON files in `blocks/library/`

---

## Testing Strategy

### Unit Tests
- Interpreter::loadBlock()
- BlockValue::callMethod()
- REPL command parsing

### Integration Tests
- End-to-end block loading
- Multi-language execution
- Error handling

### Manual Tests
- REPL commands
- Example programs
- Performance profiling

---

## Documentation Deliverables

- `INTERPRETER_INTEGRATION.md` - How block loading works
- `REPL_COMMANDS.md` - REPL block commands reference
- `BLOCK_EXAMPLES.md` - Sample programs
- `PHASE_7_COMPLETE.md` - Final report

---

## Future Work (Phase 8+)

After Phase 7:

1. **Python Executor Integration** - Add PyExecutorAdapter
2. **Remote Block Repositories** - Fetch blocks via HTTP
3. **Dependency Resolution** - Automatic block dependencies
4. **Block Versioning** - Handle multiple block versions
5. **Package Manager** - `naab install <block-id>`
6. **IDE Integration** - LSP server for editors
7. **Debugger** - Step through multi-language code

---

**Phase 7 starts now!**

Let's make the block assembly vision real by connecting all the pieces we built in Phase 6.
