# Zig Polyglot Executor

Execute [Zig](https://ziglang.org/) code within NAAb scripts using inline code blocks.

## Overview

The Zig executor compiles Zig code to native binaries and executes them. Zig is a systems programming language designed as a modern C replacement with safety features and excellent performance.

## Syntax

```naab
let result = <<zig
// Zig code here
>>
```

## Features

- **Compiled Performance** - Zig compiles to native code via LLVM, achieving C-like performance
- **Memory Safety** - Manual memory management with compile-time safety checks
- **No Hidden Control Flow** - Explicit error handling, no exceptions
- **C Interop** - Seamless integration with C libraries
- **Error Handling** - Compilation errors are caught and reported as NAAb exceptions
- **Thread-Safe** - Parallel Zig blocks work correctly with atomic temp file management

## Requirements

- Zig compiler (`zig`) must be installed and in PATH
- **Install:**
  ```bash
  # Download from official site
  wget https://ziglang.org/download/0.11.0/zig-linux-x86_64-0.11.0.tar.xz
  tar xf zig-linux-x86_64-0.11.0.tar.xz
  sudo mv zig-linux-x86_64-0.11.0 /usr/local/zig
  echo 'export PATH=$PATH:/usr/local/zig' >> ~/.bashrc

  # Or via package manager (if available)
  # snap install zig --classic --beta
  ```
- **Verify:** `zig version`

## Examples

### Simple Expression

```naab
let sum = <<zig
const result = 10 + 20;
>>
print(sum)  // Output: 30
```

### Multi-line with Functions

```naab
let factorial = <<zig
const std = @import("std");

fn factorial(n: u32) u32 {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

const result = factorial(5);
>>
print("5! = " + string.str(factorial))  // Output: 5! = 120
```

### Array Operations

```naab
let array_sum = <<zig
const std = @import("std");

const numbers = [_]i32{1, 2, 3, 4, 5};
var sum: i32 = 0;
for (numbers) |num| {
    sum += num;
}
>>
print("Sum: " + string.str(array_sum))  // Output: Sum: 15
```

### Error Handling

```naab
try {
    <<zig
    // Invalid Zig code
    const x: i32 = "string";  // Type error
    >>
} catch (e) {
    print("Compilation failed: " + e)
}
```

## Implementation Details

### Compilation Process

1. NAAb writes Zig code to a thread-safe temporary `.zig` file
2. Compiles with: `zig build-exe source.zig -femit-bin=binary`
3. Executes the compiled binary
4. Captures stdout/stderr
5. Parses output and converts to NAAb value
6. Cleans up temporary files

### Code Wrapping

The executor automatically wraps code in a `main` function if needed:

```zig
const std = @import("std");
pub fn main() !void {
    // Your code here
}
```

For return values, the last expression is wrapped with `stdout.print()`.

### Compiler Flags

- `build-exe` - Compile to executable
- `-femit-bin=path` - Specify output binary path

### Thread Safety

- Uses atomic counter + thread ID for unique temp file names
- Parallel Zig blocks execute independently without conflicts

### Output Handling

- **Stdout** becomes the return value
- **Stderr** captured for error messages
- Automatic type conversion: tries boolean → int → double → string

## Error Hints

The executor provides helpful hints for common Zig compilation errors:

### Type Errors

```
Hint: Zig type error.
- Zig has strict compile-time type checking
- Use explicit type annotations: const x: i32 = 5;
- Use type coercion: @intCast(), @floatCast(), etc.
- Check function return types match declarations
```

### Undeclared Identifiers

```
Hint: Zig identifier not declared.
- Check spelling and imports
- Use const for compile-time constants, var for runtime variables
- Import standard library: const std = @import("std");
```

### Compile-time Evaluation Errors

```
Hint: Zig compile-time evaluation error.
- const values must be known at compile time
- Use var for runtime-computed values
- comptime keyword forces compile-time evaluation
```

### Error Handling

```
Hint: Zig error not handled.
- Zig requires explicit error handling with try or catch
- Functions that can error use ! in signature: fn foo() !void
- Use try before error-returning function calls
```

## Performance

Zig code runs at **C-level performance** after compilation:

```naab
let zig_result = <<zig
const std = @import("std");

fn sumTo(n: i32) i32 {
    var total: i32 = 0;
    var i: i32 = 1;
    while (i <= n) : (i += 1) {
        total += i;
    }
    return total;
}

const result = sumTo(1_000_000);
>>
// Very fast - near C performance
```

### Overhead

- **First execution:** ~50-200ms (compilation time)
- **Binary execution:** <10ms (native code)
- **Note:** Each execution recompiles (no caching yet)

## Limitations

### Current Implementation

- ✅ Simple expressions and multi-line code
- ✅ Functions and error handling
- ✅ Standard library access
- ✅ Sandbox integration
- ✅ Comprehensive error hints
- ❌ No variable binding from NAAb (`<<zig[x, y]` not implemented)
- ❌ No FFI/dlopen support
- ❌ No binary caching

### System Requirements

- Zig compiler must be installed
- Cannot run on systems without `zig` in PATH
- Compilation adds overhead compared to interpreted languages

## Best Practices

### When to Use Zig

- **Systems programming** - Low-level control with safety
- **Performance-critical** code requiring C-level speed
- **C interop** - Calling or being called by C code
- **Explicit control** - When you want no hidden allocations or control flow

### When NOT to Use Zig

- Quick scripting (use Python instead)
- Web development (use JavaScript instead)
- When compilation overhead is unacceptable
- On systems without Zig compiler

### Code Style

```naab
// Good: Clear, idiomatic Zig
let result = <<zig
const std = @import("std");

fn processData(items: []const i32) i32 {
    var sum: i32 = 0;
    for (items) |item| {
        sum += item * 2;
    }
    return sum;
}

const data = [_]i32{1, 2, 3, 4, 5};
const result = processData(&data);
>>

// Avoid: Trying to write C in Zig
let bad = <<zig
int process_data(int* items, int len) {  // This is C syntax
    return sum(items, len);
}
>>
```

## Comparison with Other Executors

| Feature | Zig | Nim | Go | Rust | C++ |
|---------|-----|-----|-----|------|-----|
| **Speed** | ⚡⚡⚡ | ⚡⚡⚡ | ⚡⚡⚡ | ⚡⚡⚡ | ⚡⚡⚡ |
| **Syntax** | Moderate | Simple | Moderate | Complex | Complex |
| **Compile Time** | Fast | Fast | Fast | Slow | Moderate |
| **Memory Safety** | Manual + Checks | Manual | GC | Safe | Manual |
| **C Interop** | Excellent | Good | Via cgo | Via FFI | Native |

## Troubleshooting

### "zig: command not found"

```bash
# Install Zig
wget https://ziglang.org/download/0.11.0/zig-linux-x86_64-0.11.0.tar.xz
tar xf zig-linux-x86_64-0.11.0.tar.xz
sudo mv zig-linux-x86_64-0.11.0 /usr/local/zig
export PATH=$PATH:/usr/local/zig
```

### "Zig compilation failed"

Check the error message for syntax issues:

```naab
try {
    <<zig
    // Your code here
    >>
} catch (e) {
    print("Error: " + e)
    // Shows compilation errors with helpful hints
}
```

### Type Errors

Zig has strict compile-time type checking:

```naab
// Bad - type mismatch
let bad = <<zig
const x: i32 = "string";  // Error!
>>

// Good - correct types
let good = <<zig
const x: i32 = 42;
const y: []const u8 = "string";
>>
```

## See Also

- [Zig Language Official Docs](https://ziglang.org/documentation/master/)
- [Zig Learn](https://ziglearn.org/)
- [NAAb Polyglot Overview](../polyglot.md)
- [Nim Executor](./nim.md) - Similar compiled pattern
- [Go Executor](./go.md) - Similar compiled pattern
- [Rust Executor](./rust.md) - Similar compiled pattern

## Future Enhancements

Planned features for future versions:

1. **Variable Binding** - `<<zig[x, y] ... >>`
2. **Binary Caching** - Cache compiled binaries by code hash
3. **FFI Support** - Call Zig functions via dlopen
4. **Incremental Compilation** - Faster recompilation

---

**Zig Executor Status:** ✅ Production Ready

**Added in:** NAAb v0.2.2

**Maintained by:** NAAb Core Team
