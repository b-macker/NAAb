# NAAb REPL Commands Reference

**Version**: 0.1.0
**Last Updated**: December 17, 2025

---

## Overview

The NAAb REPL (Read-Eval-Print Loop) provides an interactive environment for executing NAAb code and managing blocks. This document describes all available REPL commands.

---

## General Commands

### `:help`, `:h`

Show help information about available commands.

**Usage**:
```
>>> :help
```

**Output**: Displays a formatted list of all available commands with descriptions.

---

### `:exit`, `:quit`, `:q`

Exit the REPL session.

**Usage**:
```
>>> :exit
Goodbye!
```

**Note**: Also accepts standard `exit` or `quit` without the colon prefix.

---

### `:clear`, `:cls`

Clear the screen and redisplay the welcome banner.

**Usage**:
```
>>> :clear
```

**Effect**: Uses ANSI escape codes to clear the terminal screen.

---

### `:reset`

Reset the interpreter state, clearing all variables and loaded blocks.

**Usage**:
```
>>> let x = 42
>>> :reset
[INFO] Resetting interpreter state...
[SUCCESS] State reset complete
>>> print(x)
[ERROR] Undefined variable: x
```

**Effect**:
- Clears all variables
- Resets line counter
- Clears accumulated program history
- **Note**: Does not unload blocks from memory (requires restart for full cleanup)

---

##

 Block Management Commands

### `:load <block-id> as <alias>`

Load a block from the registry with a given alias.

**Syntax**:
```
:load <BLOCK-ID> as <alias>
```

**Parameters**:
- `<BLOCK-ID>`: Block identifier (e.g., `BLOCK-CPP-MATH`, `BLOCK-JS-UTIL`)
- `<alias>`: Name to use for the block in your code

**Examples**:
```naab
>>> :load BLOCK-CPP-MATH as math
[INFO] Loading block BLOCK-CPP-MATH as 'math'...
[INFO] Loaded block BLOCK-CPP-MATH as math (cpp, 150 tokens)
       Source: /path/to/block.cpp
       Code size: 1024 bytes
[INFO] Creating dedicated C++ executor for block...
[SUCCESS] Block loaded and ready as 'math'

>>> math.add(5, 10)
[CALL] Invoking block BLOCK-CPP-MATH (cpp) with 2 args
[INFO] Calling block via executor (cpp)...
[INFO] Calling function: add
[SUCCESS] Block call completed
15
```

**Notes**:
- Equivalent to `use <BLOCK-ID> as <alias>` statement
- C++ blocks create dedicated executor instances
- JS/Python blocks use shared executor from registry

---

### `:blocks`

List all loaded blocks in the current session.

**Usage**:
```
>>> :blocks

═══════════════════════════════════════════════════════════
  Loaded Blocks
═══════════════════════════════════════════════════════════

[INFO] Block listing functionality requires interpreter API extension
       Use 'use BLOCK-ID as name' to load blocks

Available in registry: ~24,000 blocks
Use :languages to see supported languages
```

**Future Enhancement**: Will display table of loaded blocks with:
- Alias name
- Block ID
- Language
- Status (ready/error)
- Function count

---

### `:info <alias>`

Display detailed information about a loaded block.

**Syntax**:
```
:info <alias>
```

**Parameters**:
- `<alias>`: Name of the loaded block

**Example**:
```
>>> :info math

Block Information: math
─────────────────────────────────────────────────────────

[INFO] Block info functionality requires interpreter API extension
       Alias: math
       Status: Requires interpreter.getBlockInfo() method
```

**Future Enhancement**: Will display:
- Block ID
- Language
- File path
- Token count
- Available functions/methods
- Documentation

---

### `:reload <alias>`

Reload a block (useful after modifying block source code).

**Syntax**:
```
:reload <alias>
```

**Parameters**:
- `<alias>`: Name of the block to reload

**Example**:
```
>>> :reload math
[INFO] Reloading block 'math'...
[INFO] Reload functionality requires interpreter API extension
       For now, use :unload then :load
```

**Workaround**:
```
>>> :unload math
>>> :load BLOCK-CPP-MATH as math
```

**Future Enhancement**: Will automatically:
1. Get original block ID from alias
2. Unload current block
3. Reload from disk
4. Re-execute with same alias

---

### `:unload <alias>`

Unload a block from the current session.

**Syntax**:
```
:unload <alias>
```

**Parameters**:
- `<alias>`: Name of the block to unload

**Example**:
```
>>> :unload math
[INFO] Unloading block 'math'...
[INFO] Unload functionality requires interpreter API extension
       Variable 'math' will be undefined on next reset
```

**Future Enhancement**: Will remove the block variable from the environment.

**Current Workaround**: Use `:reset` to clear all variables.

---

### `:languages`

Show all supported languages and their status.

**Usage**:
```
>>> :languages

═══════════════════════════════════════════════════════════
  Supported Languages
═══════════════════════════════════════════════════════════

  • cpp          ✓ ready
  • javascript   ✓ ready

Use 'use BLOCK-<LANG>-<ID> as name' to load blocks
```

**Output**:
- Language name
- Initialization status (✓ ready / ✗ not initialized)

**Note**: Languages must be registered in `main()` to appear in this list.

---

## Block Loading Examples

### C++ Block

```naab
>>> :load BLOCK-CPP-MATH as math
>>> math.add(10, 20)
30
>>> math.multiply(5, 7)
35
```

### JavaScript Block

```naab
>>> :load BLOCK-JS-STRING as str
>>> str.format("Hello, {}!", "World")
"Hello, World!"
>>> str.toUpper("hello")
"HELLO"
```

### Multiple Blocks

```naab
>>> :load BLOCK-CPP-VECTOR as vec
>>> :load BLOCK-JS-FORMAT as fmt
>>> :blocks
  vec: BLOCK-CPP-VECTOR (cpp) ✓
  fmt: BLOCK-JS-FORMAT (javascript) ✓

>>> let numbers = [1, 2, 3, 4, 5]
>>> let sum = vec.sum(numbers)
>>> let report = fmt.template({"total": sum})
>>> print(report)
```

---

## Implementation Details

### Command Parsing

Commands are parsed using simple whitespace tokenization:

```cpp
:load BLOCK-CPP-MATH as math
→ ["load", "BLOCK-CPP-MATH", "as", "math"]
```

### Execution Flow

1. **User Input**: Command starts with `:`
2. **Command Routing**: `ReplCommandHandler::handleCommand()`
3. **Command Execution**: Specific handler method called
4. **Result**: Output printed, REPL continues

### Block Loading

The `:load` command:
1. Parses command line
2. Constructs `use <block-id> as <alias>` statement
3. Tokenizes with `Lexer`
4. Parses with `Parser`
5. Executes with `Interpreter`
6. Creates BlockValue with appropriate executor (owned for C++, borrowed for JS/Python)

---

## Architecture

### ReplCommandHandler Class

```cpp
class ReplCommandHandler {
public:
    explicit ReplCommandHandler(interpreter::Interpreter& interp);

    // Process a REPL command (starts with ':')
    bool handleCommand(const std::string& cmd_line);

private:
    interpreter::Interpreter& interpreter_;

    // Command handlers
    void handleLoad(const std::string& cmd_line);
    void handleBlocks();
    void handleInfo(const std::string& alias);
    void handleReload(const std::string& alias);
    void handleUnload(const std::string& alias);
    void handleLanguages();
};
```

### Integration with REPL Session

```cpp
class ReplSession {
private:
    interpreter::Interpreter interpreter_;
    ReplCommandHandler command_handler_;  // Phase 7b

    void handleCommand(const std::string& cmd) {
        bool continue_repl = command_handler_.handleCommand(cmd);

        // Handle session-specific commands
        if (cmd == ":reset") {
            interpreter_ = interpreter::Interpreter();
            // ...
        }

        if (!continue_repl) std::exit(0);
    }
};
```

---

## Future Enhancements

### Planned for Phase 7c-7e

1. **Block Listing**: Implement `interpreter.getLoadedBlocks()` API
2. **Block Info**: Add `interpreter.getBlockInfo(alias)` API
3. **Block Unload**: Add `interpreter.unloadBlock(alias)` API
4. **Block Reload**: Implement hot-reload functionality
5. **History Commands**: `:history` integration
6. **Autocomplete**: Tab completion for commands and block names
7. **Block Search**: `:search <keyword>` to find blocks by name or function
8. **Performance Stats**: `:stats` to show execution metrics

---

## Error Handling

### Unknown Command

```
>>> :foo
[ERROR] Unknown command: :foo
        Type :help for available commands
```

### Invalid Syntax

```
>>> :load BLOCK-CPP-MATH
[ERROR] Usage: :load <block-id> as <alias>
        Example: :load BLOCK-CPP-MATH as math
```

### Block Load Failure

```
>>> :load BLOCK-INVALID as test
[INFO] Loading block BLOCK-INVALID as 'test'...
[ERROR] Failed to load block: Block not found in registry
```

---

## Files

| File | Purpose |
|------|---------|
| `include/naab/repl_commands.h` | ReplCommandHandler interface |
| `src/repl/repl_commands.cpp` | Command implementations |
| `src/repl/repl.cpp` | REPL session integration |

---

## Testing

### Manual Test Script

```bash
$ ./naab-repl

>>> :help
# Should show all commands

>>> :languages
# Should show cpp, javascript

>>> :load BLOCK-CPP-MATH as math
# Should load successfully

>>> math.add(5, 10)
# Should return 15

>>> :blocks
# Should list math block

>>> :clear
# Should clear screen

>>> :exit
# Should exit
```

---

## See Also

- [PHASE_7_PLAN.md](PHASE_7_PLAN.md) - Overall Phase 7 implementation plan
- [PHASE_7a_COMPLETE.md](PHASE_7a_COMPLETE.md) - Interpreter integration details
- [INTERPRETER_INTEGRATION.md](INTERPRETER_INTEGRATION.md) - How block loading works

---

**Phase 7b Status**: ✅ COMPLETE (Implementation)

**Next**: Phase 7c - Executor Registration on Startup
