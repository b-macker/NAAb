# NAAb Language Implementation Learnings

## Parallel Polyglot Execution (Completed ✅ - Task #25)

**Status:** FULLY IMPLEMENTED AND TESTED

### Implementation Complete:

1. **Dependency Analyzer** ✅
   - `include/naab/polyglot_dependency_analyzer.h`
   - `src/interpreter/polyglot_dependency_analyzer.cpp`
   - Detects RAW/WAW/WAR dependencies between polyglot blocks
   - Groups independent blocks for parallel execution
   - **Bug fixed:** Proper block comparison in group conflict detection

2. **Variable Snapshot System** ✅
   - `Interpreter::VariableSnapshot` struct (interpreter.h:612-628)
   - Thread-safe deep copy using existing `copyValue()` method
   - Prevents race conditions during parallel execution

3. **Integration with Interpreter** ✅
   - Modified CompoundStmt visitor with statement-to-group mapping
   - Added `executePolyglotGroupParallel()` method
   - Uses existing `PolyglotAsyncExecutor::executeParallel()`
   - **Bug fixed:** Store full statement pointer, not just InlineCodeExpr

4. **Variable Binding** ✅
   - Serializes snapshot variables using `serializeValueForLanguage()`
   - Language-specific syntax (Python, JS, Rust, C++, C#, Shell)
   - Strips common indentation and prepends declarations

5. **Testing & Validation** ✅
   - ✅ Independent blocks execute in parallel (Test 1)
   - ✅ Sequential dependencies execute in order (Test 2)
   - ✅ Variable binding works correctly (Test 3)
   - ✅ Results stored properly in environment

### Test Results:
```
Test 1: Three independent blocks (a, b, c) - PASS ✅
Test 2: Sequential dependency (data → sum) - PASS ✅
Test 3: Independent blocks reading same variable - PASS ✅
```

### Key Bugs Fixed:
1. **Dependency analyzer:** Fixed block comparison in conflict detection (was using wrong index)
2. **Variable binding:** Store full statement pointer to execute VarDeclStmt (not just InlineCodeExpr)
3. **Type conversion:** Convert unique_ptr<Stmt> vector to raw pointer vector for analyzer

### Key Learning: Async infrastructure already existed in `polyglot_async_executor.cpp` - this was primarily an integration task!

## Pipeline Operator Fix (Completed)
**Problem:** Pipeline operator (`|>`) was failing with "wrong number of arguments" error.

**Root Cause:** BinaryExpr visitor was evaluating BOTH left and right sides before the switch statement. For `100 |> subtract(50)`, the right side (`subtract(50)`) was evaluated as a normal function call before pipeline handling, causing it to fail.

**Solution:** Added special handling for Pipeline operator (like And/Or/Assign) to skip right-side evaluation at lines 1792-1803 in `interpreter.cpp`. The right side is only evaluated within the Pipeline case where we can prepend the piped value.

**Key Code Location:** `src/interpreter/interpreter.cpp:1792-1803`

## Error Message Improvements (Completed)
Implemented comprehensive debugging improvements:

### 1. "Did You Mean?" Suggestions
- Created `src/utils/string_utils.cpp` with Levenshtein distance algorithm
- Updated stdlib modules (string, array, math) to suggest similar function names
- Example: `string.uppre()` → "Did you mean: upper?"

### 2. Better Function Argument Errors
- Enhanced error at 2 locations in `interpreter.cpp` (lines 503-526, 2370-2387)
- Shows function signature with parameter names
- Displays expected vs provided argument count
- Example: "Function: subtract(a, b)" instead of just "expects 2 args"

### 3. Polyglot Error Context
- Wrapped Python errors in `python_executor.cpp` with code preview
- Wrapped JavaScript errors in `js_executor.cpp` with code preview
- Both show: clear header, original error, first 200 chars of code, helpful hints

**Files Modified:**
- `include/naab/utils/string_utils.h` (new)
- `src/utils/string_utils.cpp` (new)
- `src/stdlib/string_impl.cpp`
- `src/stdlib/array_impl.cpp`
- `src/stdlib/math_impl.cpp`
- `src/interpreter/interpreter.cpp` (2 locations)
- `src/runtime/python_executor.cpp`
- `src/runtime/js_executor.cpp`
- `CMakeLists.txt` (added string_utils.cpp)

## Important Patterns

### When Updating Error Messages
1. Add `#include "naab/utils/string_utils.h"` to use suggestions
2. Add `#include <sstream>` for ostringstream
3. Use `naab::utils::findSimilar()` to find close matches
4. Use `naab::utils::formatSuggestions()` to format output
5. Always list available functions/options

### BinaryExpr Evaluation Order
- And, Or, Assign, and Pipeline operators need special handling
- They should NOT evaluate right side before the switch statement
- Evaluate left only, handle right in the specific case

### Polyglot Error Wrapping Pattern
```cpp
catch (const error& e) {
    std::ostringstream oss;
    oss << "Error in [Language] polyglot block:\n"
        << "  [Language] error: " << e.what() << "\n";

    // Add code preview
    std::string preview = code.substr(0, std::min(code.size(), size_t(200)));
    oss << "  Block preview:\n";
    std::istringstream code_stream(preview);
    std::string line;
    while (std::getline(code_stream, line)) {
        oss << "    " << line << "\n";
    }

    oss << "\n  Hint: [Helpful message]";
    throw std::runtime_error(oss.str());
}
```

## Exception Propagation Fix (Completed)
**Problem:** Exceptions from polyglot blocks (Python, JavaScript) were not being caught by NAAb try/catch statements.

**Root Cause:** The TryStmt visitor at line 1670 in `interpreter.cpp` caught `std::exception` but immediately re-threw it instead of executing the catch block. Polyglot errors throw `std::runtime_error`, which is a `std::exception`, but they were being re-thrown at line 1702 instead of being handled.

**Solution:** Modified the `std::exception` catch handler (lines 1700-1727) to execute the catch block instead of re-throwing:
1. Create error value from exception message
2. Bind error to catch variable
3. Execute catch body
4. Handle nested exceptions properly

**Key Code Location:** `src/interpreter/interpreter.cpp:1700-1727`

**Files Modified:**
- `src/interpreter/interpreter.cpp` (TryStmt visitor)

## Polyglot Timeout Configuration (Completed)
**Problem:** Polyglot block timeouts were hardcoded to 30 seconds in multiple locations, making it impossible to configure for different use cases.

**Solution:** Added configurable timeout settings to both executors:
1. Added `timeout_seconds_` member variable (default: 30)
2. Added `setTimeout(int seconds)` and `getTimeout()` methods
3. Replaced all hardcoded 30-second values with `timeout_seconds_`
4. Updated audit logging to use configurable timeout value

**Key Code Locations:**
- JavaScript: `include/naab/js_executor.h`, `src/runtime/js_executor.cpp`
- Python: `include/naab/python_executor.h`, `src/runtime/python_executor.cpp`

**Files Modified:**
- `include/naab/js_executor.h` (added timeout config)
- `src/runtime/js_executor.cpp` (3 locations updated)
- `include/naab/python_executor.h` (added timeout config)
- `src/runtime/python_executor.cpp` (2 locations updated)

## Missing Stdlib Functions (Completed)
Implemented missing standard library functions:

### 1. string.reverse
- Added to string module at `src/stdlib/string_impl.cpp`
- Reverses a string using std::reverse
- Usage: `string.reverse("hello")` → "olleh"

### 2. env.set
- Added as alias for `env.set_var` in env module
- Sets environment variables
- Usage: `env.set("VAR", "value")`

### 3. array.pop
- Already existed, no changes needed

**Files Modified:**
- `src/stdlib/string_impl.cpp` (added reverse function)
- `src/stdlib/env_impl.cpp` (added set alias)

## Lambda Expression Support (Completed)
**Problem:** No support for inline lambda/anonymous functions in NAAb.

**Solution:** Implemented full lambda expression support using `function(params) -> type { body }` syntax:
1. Added `LambdaExpr` AST node to `include/naab/ast.h`
2. Added `NodeKind::LambdaExpr` to enum
3. Implemented `parseLambdaExpr()` in parser (reuses function parameter parsing)
4. Added interpreter visitor that creates anonymous `FunctionValue` with closure capture
5. Lambda distinguishes from function declaration by checking if `function` is followed by `LPAREN` (not `IDENTIFIER`)

**Key Code Locations:**
- AST: `include/naab/ast.h` (LambdaExpr class, lines 879-897)
- Parser: `src/parser/parser.cpp` (parseLambdaExpr at line 643-707, primary check at 1603-1607)
- Interpreter: `src/interpreter/interpreter.cpp` (visit(LambdaExpr&) at line 3451-3502)
- AST nodes: `src/parser/ast_nodes.cpp` (accept method)

**Syntax Examples:**
```naab
// Simple lambda
let double = function(x: int) -> int { return x * 2 }

// Multiple parameters
let add = function(a: int, b: int) -> int { return a + b }

// No parameters
let greet = function() -> string { return "Hello!" }
```

**Files Modified:**
- `include/naab/ast.h` (LambdaExpr class, forward decl, NodeKind, visitor method)
- `src/parser/ast_nodes.cpp` (accept implementation)
- `include/naab/parser.h` (parseLambdaExpr declaration)
- `src/parser/parser.cpp` (parseLambdaExpr implementation, primary check)
- `include/naab/interpreter.h` (visit declaration)
- `src/interpreter/interpreter.cpp` (visit implementation)

**Enhanced Call Expression Support:**
- Extended CallExpr visitor to handle any expression type as callee (not just IdentifierExpr/MemberExpr)
- Now supports calling lambdas stored in arrays: `operations[0](5)`
- Supports higher-order functions returning lambdas: `makeMultiplier(3)(4)`
- Location: `src/interpreter/interpreter.cpp:2375-2389`

## Python Polyglot Indentation Fix (Completed)
**Problem:** First line of Python polyglot blocks had its leading whitespace stripped by the lexer, causing IndentationError when all lines should have equal indentation.

**Root Cause:** The lexer at lines 367-374 in `lexer.cpp` was skipping ALL whitespace (spaces, tabs, newlines) after the language name. This stripped indentation from the first line of code while preserving it on subsequent lines.

**Solution:** Modified lexer to only skip newlines (not spaces/tabs) after the language name:
1. Changed line 368 condition to only match `\n` and `\r`, not spaces/tabs
2. Updated interpreter at lines 3387-3413 to include first line in min_indent calculation
3. Now all lines are treated equally for indentation stripping

**Key Code Locations:**
- Lexer: `src/lexer/lexer.cpp:367-374` (whitespace skipping after language name)
- Interpreter: `src/interpreter/interpreter.cpp:3387-3413` (common indentation stripping)

**Files Modified:**
- `src/lexer/lexer.cpp` (only skip newlines, not spaces)
- `src/interpreter/interpreter.cpp` (include first line in indentation calc)

## Test Results
All improvements verified working:
- ✅ Pipeline operator: `100 |> subtract(50)` = 50
- ✅ "Did you mean?": `string.uppre()` suggests "upper"
- ✅ Function args: Shows "Function: subtract(a, b)"
- ✅ Python errors: Shows code preview with division by zero
- ✅ JavaScript errors: Shows code preview with undefined variable
- ✅ Exception propagation: Python exceptions caught by try/catch
- ✅ Exception propagation: JavaScript exceptions caught by try/catch
- ✅ string.reverse: "hello" → "olleh"
- ✅ env.set: Sets environment variables successfully
- ✅ IO functions: read, write, exists all working
- ✅ Lambda syntax: `function(x: int) -> int { return x * 2 }` works
- ✅ Lambda with multiple params: `function(a: int, b: int) -> int { return a + b }`
- ✅ Lambda with no params: `function() -> string { return "Hello!" }`
- ✅ Lambdas in arrays: `operations[0](5)` works correctly
- ✅ Higher-order functions: `makeMultiplier(3)(4)` returns functions that work
- ✅ Python indentation: All lines with equal indentation work correctly
- ✅ Python functions: Nested indentation preserved properly
