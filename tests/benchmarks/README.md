# NAAb Performance Benchmarks

This directory contains performance benchmarks for the NAAb language interpreter and runtime.

## Overview

The benchmark suite tests various aspects of NAAb's performance:

1. **Block Search** - Search performance across 24,483 blocks
2. **Interpreter Execution** - Function calls, loops, arrays, recursion
3. **Pipeline Validation** - Pipeline operator performance and validation
4. **API Response Time** - REST API endpoint response times
5. **Memory Usage** - Large data structures and memory allocation patterns

## Benchmark Files

### NAAb Benchmarks

- **benchmark_search.naab** - Block search performance (target: < 100ms)
- **benchmark_interpreter.naab** - Interpreter execution benchmarks
- **benchmark_pipeline.naab** - Pipeline validation (target: < 10ms)
- **benchmark_memory.naab** - Memory usage and allocation patterns

### Shell Script Benchmarks

- **benchmark_api.sh** - REST API performance (target: < 200ms response time)

### Test Runners

- **run_all_benchmarks.sh** - Master runner for all benchmarks

## Running Benchmarks

### Run All Benchmarks

```bash
cd /storage/emulated/0/Download/.naab/naab_language/tests/benchmarks
./run_all_benchmarks.sh
```

### Run Individual Benchmarks

```bash
# Search performance
~/naab-lang run benchmark_search.naab

# Interpreter performance
~/naab-lang run benchmark_interpreter.naab

# Pipeline performance
~/naab-lang run benchmark_pipeline.naab

# Memory usage
~/naab-lang run benchmark_memory.naab

# API performance (requires server running)
./benchmark_api.sh
```

## Performance Targets

| Benchmark | Target | Description |
|-----------|--------|-------------|
| Block Search | < 100ms | Average search time across 24,483 blocks |
| Pipeline Validation | < 10ms | Average validation time per pipeline |
| API Response | < 200ms | Average API endpoint response time |
| Interpreter | General | No specific target, baseline measurements |
| Memory | General | No specific target, detect issues |

## Starting the API Server (for API benchmarks)

```bash
~/naab-lang api 8080
```

Then in another terminal:

```bash
./benchmark_api.sh
```

## Interpreting Results

### Success Indicators

- **PASS**: Benchmark meets performance targets
- **WARNING**: Benchmark completes but exceeds targets
- **FAIL**: Benchmark fails or errors out
- **TIMEOUT**: Benchmark takes too long to complete

### Example Output

```
========================================
  Interpreter Performance Benchmark
========================================

Function calls (10,000): 145ms
While loop (100,000): 234ms
Array operations (1,000): 87ms
Recursion (fib 20): 312ms
String operations (1,000): 156ms
Dict operations (1,000): 198ms
Higher-order functions: 203ms

========================================
Summary:
  Total execution time: 1335ms
  Average per benchmark: 190.7ms

  Status: PASS (all benchmarks within targets)
========================================
```

## Benchmark Details

### 1. Block Search (`benchmark_search.naab`)

Tests search performance with various query types:
- Email validation
- Parse JSON
- HTTP request
- Sort array
- Database query

Runs 10 iterations per query type.

### 2. Interpreter Execution (`benchmark_interpreter.naab`)

Tests 7 core interpreter operations:
- Function call overhead (10,000 calls)
- Loop performance (100,000 iterations)
- Array operations (1,000 elements)
- Recursion (Fibonacci 20)
- String operations (1,000 iterations)
- Dict operations (1,000 entries)
- Higher-order functions (map, filter, reduce)

### 3. Pipeline Validation (`benchmark_pipeline.naab`)

Tests 5 pipeline scenarios:
- Simple pipeline (2 stages, 1,000x)
- Long pipeline (6 stages, 1,000x)
- Array pipeline with map/filter/reduce (100x)
- Pipeline validation overhead (1,000x)
- Complex data transformation (50x)

### 4. API Performance (`benchmark_api.sh`)

Tests 5 REST API endpoints:
- Health check (50 requests)
- Simple code execution (50 requests)
- Code with computation (25 requests)
- List blocks (50 requests)
- Search blocks (50 requests)

Measures average response time using curl's built-in timing.

### 5. Memory Usage (`benchmark_memory.naab`)

Tests 6 memory scenarios:
- Large array creation (10,000 elements)
- Large dict creation (5,000 entries)
- Nested data structures (100 records)
- String concatenation (1,000 chars)
- Array copying (100 copies of 1,000 elements)
- Memory churn (1,000 allocations)

## Adding New Benchmarks

To add a new benchmark:

1. Create a new `.naab` file or `.sh` script in this directory
2. Follow the naming convention: `benchmark_<name>.naab`
3. Include clear output showing PASS/FAIL status
4. Add the benchmark to `run_all_benchmarks.sh`
5. Document it in this README

### Benchmark Template

```naab
// Performance Benchmark: <Name>
// Target: <Performance Target>

import "stdlib" as std

print("========================================")
print("  <Benchmark Name>")
print("========================================")
print("")

function benchmark(name, fn) {
    let start = std.now()
    fn()
    let end = std.now()
    let elapsed = end - start
    print(name + ": " + elapsed + "ms")
    return elapsed
}

// Your benchmark code here

print("")
print("========================================")
print("Summary:")
// Summary output

if (<condition>) {
    print("  Status: PASS")
} else {
    print("  Status: FAIL")
}
print("========================================")
```

## CI/CD Integration

These benchmarks can be integrated into CI/CD pipelines:

```bash
# Run benchmarks and exit with appropriate code
./run_all_benchmarks.sh
if [ $? -eq 0 ]; then
    echo "Performance tests passed"
else
    echo "Performance tests failed"
    exit 1
fi
```

## Performance Regression Detection

To detect performance regressions:

1. Run benchmarks on main branch and save results
2. Run benchmarks on feature branch
3. Compare results and fail if regression exceeds threshold

Example:

```bash
# Save baseline
./run_all_benchmarks.sh > baseline_results.txt

# After changes
./run_all_benchmarks.sh > current_results.txt

# Compare (manual for now, could be automated)
diff baseline_results.txt current_results.txt
```

## System Requirements

- NAAb binary in `~/naab-lang` (or set `NAAB_BIN` environment variable)
- For API benchmarks: Running API server on localhost:8080
- For shell benchmarks: bash, curl, bc

## Troubleshooting

### "Command not found: naab-lang"

Set the NAAB_BIN environment variable:

```bash
export NAAB_BIN=/path/to/naab-lang
./run_all_benchmarks.sh
```

### API benchmarks fail

Make sure the API server is running:

```bash
~/naab-lang api 8080 &
sleep 2  # Wait for server to start
./benchmark_api.sh
```

### Benchmarks timeout

Increase the timeout in the scripts:

```bash
TIMEOUT=300 ./run_all_benchmarks.sh
```

---

**Last Updated**: December 30, 2024
**NAAb Version**: 0.1.0
