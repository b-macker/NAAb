# Stack Trace Implementation - Phase 3.1

**Implemented:** 2026-01-25
**Status:** ✅ Complete

## Overview

NAAb now provides full stack trace support for runtime errors, showing the complete call chain with filenames and line numbers across module boundaries.

## Features

### 1. Complete Call Chain
- Shows all function calls from error point back to entry point
- Maintains stack across module boundaries
- Displays in reverse chronological order (most recent first)

### 2. Source Location Information
- **Filename**: Shows relative or absolute path to source file
- **Line Number**: Shows exact line where function was defined
- **Function Name**: Shows name of each function in the call chain

### 3. Cross-Module Support
- Tracks calls across different `.naab` files
- Shows full file paths for imported modules
- Maintains accurate source locations

## Example Output

```
RuntimeError: Undefined variable: undefined_var
Stack trace:
  at outer (test_stack_trace_complete.naab:14)
  at middle (test_stack_trace_complete.naab:9)
  at deepest (test_stack_trace_complete.naab:4)
```

### Cross-Module Example

```
RuntimeError: Undefined variable: undefined_variable
Stack trace:
  at orchestrate (test_error_main.naab:4)
  at will_fail (/path/to/test_error_module.naab:2)
```

## Implementation Details

### Modified Files

1. **`include/naab/interpreter.h`** (Lines 73-93)
   - Added `source_file` and `source_line` fields to `FunctionValue` struct
   - Updated constructor to accept source location parameters

2. **`src/interpreter/interpreter.cpp`**
   - **Lines 1168-1178**: Store source location when defining functions
   - **Lines 459-487**: Save/restore `current_file_` during function calls
   - **Lines 2354-2415**: Added file tracking for generic function calls
   - **Lines 467, 2367**: Use actual line numbers in `pushStackFrame()`
   - **Lines 515-518**: Simplified `createError()` (removed debug output)

3. **`src/parser/parser.cpp`** (Line 473)
   - Include filename in `SourceLocation` when creating `FunctionDecl` AST nodes

4. **`src/cli/main.cpp`** (Lines 191-194)
   - Catch `NaabError` and print formatted stack trace

### Key Components

#### StackFrame Structure
```cpp
struct StackFrame {
    std::string function_name;  // Function name
    std::string file_path;      // Source file
    int line_number;            // Line in source
    int column_number;          // Column in source (optional)
};
```

#### Error Flow
1. **Function Definition**: Parser captures `SourceLocation` with filename
2. **Function Storage**: `FunctionValue` stores source file and line
3. **Function Call**: `current_file_` is updated to function's source file
4. **Stack Push**: `pushStackFrame()` captures function name, file, and line
5. **Error Creation**: `createError()` captures current call stack
6. **Error Display**: `NaabError::formatError()` prints formatted stack trace

## Testing

### Test Files

1. **`test_simple_stack.naab`**: Single file, 3-level nested calls
2. **`test_error_main.naab`** + **`test_error_module.naab`**: Cross-module errors
3. **`test_stack_trace_complete.naab`**: Comprehensive nested call test

### Test Results

All tests passing ✅:
- ✅ Single file stack traces show correct line numbers
- ✅ Cross-module stack traces show full file paths
- ✅ Nested calls show complete call chain
- ✅ Line numbers match actual function definitions

## Limitations

### Current Limitations
1. **Column numbers**: Currently set to 0 (not tracked in function definitions)
2. **Type inference errors**: Errors during type inference show partial stack (will be fixed when type inference is improved)

### Future Enhancements
- [ ] Add column number tracking for more precise error locations
- [ ] Improve type inference to avoid executing code during definition phase
- [ ] Add local variable values to stack frames (for debugging)
- [ ] Support for inline code block stack traces (Python, JS, etc.)

## Usage

No special syntax required - stack traces are automatically displayed for all runtime errors:

```naab
function level3() {
    return nonexistent_var  // Error here
}

function level2() {
    return level3()
}

function level1() {
    return level2()
}

main {
    let result = level1()  // Stack trace will show: level1 → level2 → level3
}
```

## Integration with Error Handling

Stack traces are included in `NaabError` exceptions, which can be:
- Caught with try/catch (when implemented)
- Logged for debugging
- Displayed to users for error reporting

## Performance Impact

Minimal:
- Stack frames are lightweight (just strings and integers)
- Push/pop operations are O(1)
- Only active during function calls
- Cleaned up automatically when functions return

## Comparison with Other Languages

| Language | Stack Trace Quality | NAAb Status |
|----------|-------------------|-------------|
| Python   | ✅ Full file:line:function | ✅ Implemented |
| JavaScript | ✅ Full stack with source maps | ✅ Implemented |
| Rust     | ✅ Full with panic backtraces | ✅ Implemented |
| Go       | ✅ Full with goroutine info | ✅ Single-threaded only |

## Phase Status

**Phase 3.1: Proper Error Handling** - Stack Traces
- [x] Implement stack frame tracking
- [x] Add source location to function definitions
- [x] Track current file during cross-module calls
- [x] Format and display stack traces
- [x] Test single-file scenarios
- [x] Test cross-module scenarios
- [x] Clean up debug output

**Next Steps**: Phase 3.1 - Try/Catch/Throw error handling
