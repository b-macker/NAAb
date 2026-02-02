# NAAb Benchmarking Suite

## Overview

This directory contains a benchmarking framework for measuring the performance of the NAAb language interpreter. The benchmarks are divided into micro-benchmarks (low-level operations) and macro-benchmarks (realistic workloads).

## Structure

```
benchmarks/
├── micro/                      # Micro-benchmarks
│   ├── 01_variables_simple.naab  # Variable operations
│   ├── 02_arithmetic.naab        # Arithmetic operations
│   ├── 03_functions.naab         # Function calls
│   └── 04_strings.naab           # String operations
├── macro/                      # Macro-benchmarks
│   └── sorting.naab              # Sorting algorithms (LIMITED - see below)
├── results/                    # Benchmark results (timestamped)
├── run_all_benchmarks.sh       # Runner script for all benchmarks
└── README.md                   # This file
```

## Running Benchmarks

### Individual Benchmark
```bash
../build/naab-lang run micro/01_variables_simple.naab
```

### All Benchmarks
```bash
./run_all_benchmarks.sh
```

Results are saved to `results/benchmark_YYYYMMDD_HHMMSS.txt`.

## Benchmark Descriptions

### Micro-Benchmarks

1. **01_variables_simple.naab** - Variable Operations
   - Variable assignment (100K iterations)
   - Variable access and addition (100K iterations)
   - Variable reassignment (100K iterations)
   - Tests basic variable handling performance

2. **02_arithmetic.naab** - Arithmetic Operations
   - Addition (100K iterations)
   - Multiplication (100K iterations)
   - Division (100K iterations)
   - Mixed operations (100K iterations)
   - Tests basic arithmetic performance

3. **03_functions.naab** - Function Calls
   - Empty function calls (100K iterations)
   - Functions with arguments (100K iterations)
   - Recursive functions (Fibonacci, 100 iterations of fib(20))
   - Tests function call overhead

4. **04_strings.naab** - String Operations
   - String concatenation (10K iterations)
   - String length (10K iterations)
   - String substring (10K iterations)
   - Case conversion (20K iterations)
   - String contains (10K iterations)
   - String repeat (10K iterations)
   - Tests string manipulation performance

### Macro-Benchmarks

1. **sorting.naab** - Sorting Algorithms
   - **STATUS: LIMITED FUNCTIONALITY**
   - Tests bubble sort with arrays of sizes: 10, 50, 100, 200
   - **LIMITATION**: Cannot complete due to missing array element assignment

## Known Limitations

### Critical Language Features Missing

The benchmarking effort revealed several missing language features that limit what can be benchmarked:

1. **❌ Range Operator (`..`)**
   - Syntax like `for i in 0..100` is not supported
   - **Workaround**: Use `while` loops instead
   - **Impact**: All for-range loops must be rewritten

2. **❌ Time Module**
   - No `time.milliseconds()` or similar timing functions
   - **Workaround**: None - can only measure by observation
   - **Impact**: Cannot measure precise execution time within NAAb code

3. **❌ List Member Methods**
   - No `list.length()` - must use `array.length(list)`
   - No `list.append(value)` - must use `array.push(list, value)`
   - **Workaround**: Use array module functions
   - **Impact**: More verbose code, functional style required

4. **❌ Array Element Assignment (CRITICAL)**
   - Cannot do `arr[i] = value`
   - **Workaround**: None available
   - **Impact**: Cannot implement in-place algorithms (sorting, modification, etc.)
   - **Blocks**: Most macro-benchmarks requiring array manipulation

### Implications for Benchmarking

Due to these limitations:

- **Micro-benchmarks**: ✅ Work with modifications (while loops, array module)
- **Macro-benchmarks**: ⚠️ Severely limited
  - Cannot benchmark sorting algorithms (requires array assignment)
  - Cannot benchmark array manipulation algorithms
  - Limited to read-only or append-only operations
- **Timing**: ⏱️ Manual observation only (no programmatic timing)

## Workarounds Used

### 1. While Loops Instead of Range-Based For Loops

**Original (not supported):**
```naab
for i in 0..iterations {
    # code
}
```

**Current (working):**
```naab
let i = 0
while i < iterations {
    # code
    i = i + 1
}
```

### 2. Array Module Functions

**Original (not supported):**
```naab
let len = my_list.length()
my_list.append(value)
```

**Current (working):**
```naab
use array as array

let len = array.length(my_list)
my_list = array.push(my_list, value)
```

### 3. Manual Timing

Since there's no time module, benchmarks simply report "Completed N operations" without timing data. External timing tools must be used.

## Future Improvements Needed

To create a complete benchmarking suite, the following language features are required:

### High Priority
1. **Array element assignment** - Blocks all in-place algorithms
2. **Time module** - Essential for performance measurement
3. **Range operator** - Improves code readability

### Medium Priority
4. List method syntax (`.length()`, `.append()`)
5. Better iteration constructs

### Recommended Next Steps

1. **Implement array element assignment** in the interpreter
   - This is the most critical blocker for benchmarking
   - Required for: sorting, matrix operations, graph algorithms, etc.

2. **Add basic time module** with at least:
   - `time.milliseconds()` or `time.nanoseconds()`
   - `time.now()` for timestamps

3. **Add range operator to lexer/parser**
   - Token: DOT_DOT or RANGE
   - Syntax: `0..100` creates range object
   - Integration with for loops

4. **Expand benchmark suite** once features are available:
   - More sorting algorithms (quicksort, mergesort)
   - Data structure operations (trees, graphs)
   - I/O benchmarks
   - Polyglot code performance

## Baseline Performance (Observed)

Since programmatic timing is not available, here are rough observations:

- **Variables (100K)**: Completes quickly (~seconds)
- **Arithmetic (100K)**: Completes quickly (~seconds)
- **Functions (100K)**: Completes quickly (~seconds)
- **Strings (10K)**: Slower due to string allocations (~seconds)

*Note: Precise timing requires implementation of time module.*

## Contributing

When adding new benchmarks:

1. Avoid using unsupported features (see limitations above)
2. Use `while` loops instead of `for i in 0..n`
3. Use `array` module functions for list operations
4. Document any workarounds in comments
5. Keep iterations reasonable (10K-100K for most operations)

## License

Part of the NAAb language project.
