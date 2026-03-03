# Julia Polyglot Executor

Execute [Julia](https://julialang.org/) code within NAAb scripts using inline code blocks.

## Overview

The Julia executor runs Julia code via subprocess execution. Julia is a high-performance dynamic language for scientific computing, combining Python-like syntax with C-level performance through JIT compilation.

## Syntax

```naab
let result = <<julia
# Julia code here
>>
```

## Features

- **JIT-Compiled Performance** - Julia compiles to native code via LLVM at runtime
- **Scientific Computing** - Built for numerical and scientific workloads
- **Rich Type System** - Dynamic typing with optional type annotations
- **Multiple Dispatch** - Methods are selected based on runtime types
- **Standard Library** - Full access to Julia's stdlib (Statistics, LinearAlgebra, etc.)
- **Error Handling** - Runtime errors are caught and reported as NAAb exceptions
- **Thread-Safe** - Parallel Julia blocks work correctly with atomic temp file management

## Requirements

- Julia runtime (`julia`) must be installed and in PATH
- **Install:**
  ```bash
  # Download from official site
  wget https://julialang-s3.julialang.org/bin/linux/x64/1.9/julia-1.9.3-linux-x86_64.tar.gz
  tar xf julia-1.9.3-linux-x86_64.tar.gz
  sudo mv julia-1.9.3 /opt/julia
  echo 'export PATH=$PATH:/opt/julia/bin' >> ~/.bashrc

  # Or via package manager
  # apt install julia        # Debian/Ubuntu
  # brew install julia       # macOS
  # pacman -S julia          # Arch
  ```
- **Verify:** `julia --version`

## Examples

### Simple Expression

```naab
let sum = <<julia
10 + 20
>>
print(sum)  // Output: 30
```

### Multi-line with Functions

```naab
let factorial = <<julia
function factorial(n)
    if n <= 1
        return 1
    end
    return n * factorial(n - 1)
end

factorial(5)
>>
print("5! = " + string.str(factorial))  // Output: 5! = 120
```

### Array Operations

```naab
let array_mean = <<julia
data = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
sum(data) / length(data)
>>
print("Mean: " + string.str(array_mean))  // Output: Mean: 5.5
```

### Using Standard Library

```naab
let stats = <<julia
using Statistics

values = [10, 20, 30, 40, 50]
result = Dict(
    "mean" => mean(values),
    "median" => median(values),
    "std" => std(values)
)
println(result)
>>
```

### Matrix Operations

```naab
let matrix_det = <<julia
using LinearAlgebra

A = [1 2; 3 4]
det(A)
>>
print("Determinant: " + string.str(matrix_det))
```

### Error Handling

```naab
try {
    <<julia
    # Division by zero
    result = 10 / 0
    >>
} catch (e) {
    print("Runtime error: " + e)
}
```

## Implementation Details

### Execution Process

1. NAAb writes Julia code to a thread-safe temporary `.jl` file
2. Executes with: `julia script.jl`
3. Captures stdout/stderr
4. Parses output and converts to NAAb value
5. Cleans up temporary files

### Code Wrapping

Julia code is executed as-is for most cases. For return values, the executor automatically wraps the last expression with `println()` if needed.

### Thread Safety

- Uses atomic counter + thread ID for unique temp file names
- Parallel Julia blocks execute independently without conflicts

### Output Handling

- **Stdout** becomes the return value
- **Stderr** captured for error messages
- Automatic type conversion: tries boolean → int → double → string

## Error Hints

The executor provides helpful hints for common Julia errors:

### Undefined Variable Errors

```
Hint: Julia variable not defined.
- Check spelling and imports
- Julia is case-sensitive
- Use 'using' to import packages: using Statistics
- Variables must be defined before use
```

### Method Errors

```
Hint: Julia method error.
- Check function argument types
- Julia uses multiple dispatch - methods are matched by types
- Use convert() for explicit type conversion
- Example: convert(Float64, 5) converts int to float
```

### Package/Argument Errors

```
Hint: Julia package or argument error.
- Install packages: using Pkg; Pkg.add("PackageName")
- Standard library: Statistics, LinearAlgebra, Dates, etc.
- Check function arguments match expected types
```

### Syntax Errors

```
Hint: Julia syntax error.
- Julia uses 'end' to close blocks (not braces)
- Use '=' for assignment, '==' for comparison
- Arrays use 1-based indexing: arr[1] (not arr[0])
- Use 'function' keyword to define functions
```

### Bounds Errors

```
Hint: Julia array index out of bounds.
- Julia uses 1-based indexing
- Use length() to get array size
- Use eachindex() to iterate safely
```

## Performance

Julia achieves **near-C performance** through JIT compilation:

```naab
let julia_result = <<julia
function sumTo(n)
    total = 0
    for i in 1:n
        total += i
    end
    return total
end

sumTo(1_000_000)
>>
// First run: slower (JIT compilation)
// Subsequent runs: very fast (compiled code cached)
```

### Overhead

- **First execution:** ~100-500ms (JIT compilation)
- **Subsequent:** ~50-100ms (compiled code reused within session)
- **Note:** Each NAAb block creates new Julia process (no session persistence yet)

## Limitations

### Current Implementation

- ✅ Simple expressions and multi-line code
- ✅ Functions and error handling
- ✅ Standard library and package imports
- ✅ Sandbox integration
- ✅ Comprehensive error hints
- ❌ No variable binding from NAAb (`<<julia[x, y]` not implemented)
- ❌ No persistent REPL context across blocks
- ❌ Each block starts fresh Julia process

### System Requirements

- Julia runtime must be installed
- Cannot run on systems without `julia` in PATH
- Some packages may require installation (`Pkg.add()`)

## Best Practices

### When to Use Julia

- **Scientific computing** - Numerical analysis, statistics, data science
- **Performance-critical math** - Linear algebra, differential equations
- **Machine learning** - When Python is too slow
- **High-performance scripting** - Need speed but want dynamic language

### When NOT to Use Julia

- Simple scripting (use Python instead)
- Web development (use JavaScript instead)
- When startup time matters (JIT compilation overhead)
- Systems programming (use Rust/Zig instead)

### Code Style

```naab
// Good: Clear, idiomatic Julia
let result = <<julia
using Statistics

function processData(items)
    filtered = filter(x -> x > 0, items)
    return mean(filtered)
end

data = [1, -2, 3, -4, 5]
processData(data)
>>

// Avoid: Trying to write Python in Julia
let bad = <<julia
def process_data(items):  # This is Python syntax
    return sum(items)
>>
```

## Comparison with Other Executors

| Feature | Julia | Python | Nim | Go | Rust |
|---------|-------|--------|-----|-----|------|
| **Speed** | ⚡⚡⚡ | ⚡ | ⚡⚡⚡ | ⚡⚡⚡ | ⚡⚡⚡ |
| **Syntax** | Simple | Simple | Simple | Moderate | Complex |
| **Startup** | Slow | Fast | Fast | Fast | Fast |
| **Math/Sci** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐ |
| **Ecosystem** | Large | Huge | Medium | Large | Large |

## Troubleshooting

### "julia: command not found"

```bash
# Install Julia
wget https://julialang-s3.julialang.org/bin/linux/x64/1.9/julia-1.9.3-linux-x86_64.tar.gz
tar xf julia-1.9.3-linux-x86_64.tar.gz
sudo mv julia-1.9.3 /opt/julia
export PATH=$PATH:/opt/julia/bin
```

### "UndefVarError: package not found"

Install missing packages:

```naab
<<julia
using Pkg
Pkg.add("Statistics")  # Install package
>>

// Then use it
let result = <<julia
using Statistics
mean([1, 2, 3, 4, 5])
>>
```

### "MethodError: no method matching"

Check function argument types:

```naab
// Bad - type mismatch
let bad = <<julia
sqrt("not a number")  // Error!
>>

// Good - correct types
let good = <<julia
sqrt(16)  // 4.0
>>
```

### Array Indexing Errors

Remember Julia uses 1-based indexing:

```naab
let first = <<julia
arr = [10, 20, 30]
arr[1]  // First element (NOT arr[0])
>>
print(first)  // Output: 10
```

## Common Patterns

### Data Analysis

```naab
let analysis = <<julia
using Statistics

data = randn(1000)  # 1000 random numbers
Dict(
    "mean" => mean(data),
    "std" => std(data),
    "min" => minimum(data),
    "max" => maximum(data)
)
>>
```

### Linear Algebra

```naab
let eigenvalues = <<julia
using LinearAlgebra

A = [4 2; 1 3]
eigvals(A)
>>
```

### Broadcasting

```naab
let squared = <<julia
data = [1, 2, 3, 4, 5]
data .^ 2  # Element-wise squaring
>>
```

## See Also

- [Julia Language Official Docs](https://docs.julialang.org/)
- [Julia Academy](https://juliaacademy.com/)
- [NAAb Polyglot Overview](../polyglot.md)
- [Python Executor](./python.md) - Similar dynamic language
- [Nim Executor](./nim.md) - Similar performance goals
- [Zig Executor](./zig.md) - Compiled alternative

## Future Enhancements

Planned features for future versions:

1. **Variable Binding** - `<<julia[x, y] ... >>`
2. **Persistent REPL** - Session context across blocks
3. **Package Pre-loading** - Cache commonly used packages
4. **Julia ↔ NAAb Type Conversion** - Rich data exchange

---

**Julia Executor Status:** ✅ Production Ready

**Added in:** NAAb v0.2.2

**Maintained by:** NAAb Core Team
