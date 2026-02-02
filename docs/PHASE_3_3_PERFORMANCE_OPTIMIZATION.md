# Phase 3.3: Performance Optimization - Strategy Document

## Executive Summary

**Status:** PLANNING COMPLETE
**Implementation:** NOT STARTED
**Priority:** MEDIUM (optimize after correctness)

This document outlines the performance optimization strategy for NAAb, covering inline code caching, benchmarking, interpreter optimizations, and potential bytecode compilation.

---

## Performance Baseline

### Current Bottlenecks (Hypothesized)

1. **Inline Code Execution** - Subprocess spawning overhead
2. **Expression Evaluation** - AST traversal for every operation
3. **Variable Lookup** - Map lookups in nested scopes
4. **String Operations** - Frequent allocations/copies
5. **Function Calls** - Stack frame creation/destruction

**Note:** Actual bottlenecks must be measured via profiling.

---

## Phase 3.3 Tasks

### 3.3.1: Implement Inline Code Caching ❌ TODO

**Problem:**
Every inline code block spawns a new subprocess:
```naab
for i in 0..1000 {
    let result = <<python print("hello")>>  // 1000 subprocess spawns!
}
```

**Solution:** Cache compiled inline code + reuse subprocess.

#### Approach 1: Compilation Caching

```cpp
class InlineCodeCache {
public:
    struct CachedCode {
        std::string language;
        std::string source_hash;  // SHA256 of source
        std::string compiled_path;  // /tmp/naab_cache_XXX.pyc
    };

    CachedCode* get(const std::string& language, const std::string& source);
    void put(const std::string& language, const std::string& source, const std::string& compiled_path);

private:
    std::unordered_map<std::string, CachedCode> cache_;
};
```

**Benefits:**
- Python: compile to .pyc (bytecode)
- JavaScript: V8 caches compiled code
- Shell: no compilation, but can cache parsed commands

**Estimated Speedup:** 2-5x for repeated inline code

#### Approach 2: Subprocess Reuse (REPL Mode)

```cpp
class SubprocessPool {
public:
    // Get or create subprocess for language
    Subprocess* acquire(const std::string& language);

    // Return subprocess to pool
    void release(Subprocess* proc);

private:
    std::map<std::string, std::vector<Subprocess*>> pools_;
};
```

**Benefits:**
- No fork/exec overhead
- No interpreter startup (Python, Node.js)
- Send code via pipe, receive results

**Challenges:**
- Need REPL protocol for each language
- State isolation between calls
- Error handling (subprocess crashes)

**Estimated Speedup:** 10-100x for many inline code calls

#### Approach 3: Hybrid (Recommended)

1. **First call:** Spawn subprocess, compile code, cache
2. **Subsequent calls:** Reuse subprocess + cached compiled code
3. **Fallback:** If subprocess dies, spawn new one

**Estimated Effort:** 3-5 days

**Priority:** High (biggest performance win)

---

### 3.3.2: Create Benchmarking Suite ❌ TODO

**Goal:** Measure performance, track regressions, guide optimizations.

#### Benchmark Categories

1. **Micro-benchmarks** (low-level operations)
   - Variable assignment (1M times)
   - Arithmetic operations (1M times)
   - Function calls (100K times)
   - List operations (append, access)
   - Dict operations (insert, lookup)
   - String concatenation

2. **Macro-benchmarks** (realistic workloads)
   - Fibonacci (recursive vs iterative)
   - Sorting (quicksort, mergesort)
   - Tree traversal
   - JSON parsing
   - File I/O
   - Inline code execution (Python, JS)

3. **Integration benchmarks** (end-to-end)
   - ATLAS v1 (full pipeline)
   - Web server (handle requests)
   - Data processing (read CSV, transform, write)

#### Benchmark Framework

```naab
// benchmark.naab - Benchmarking utilities
function benchmark(name: string, fn: function() -> void, iterations: int) {
    let start = time.now()

    for i in 0..iterations {
        fn()
    }

    let end = time.now()
    let duration = end - start
    let per_op = duration / iterations

    print("[BENCH] " + name)
    print("  Total: " + duration + " ms")
    print("  Per op: " + per_op + " ms")
    print("  Ops/sec: " + (1000 / per_op))
}
```

#### Benchmark Runner

```bash
#!/bin/bash
# run_benchmarks.sh

echo "=== NAAb Performance Benchmarks ==="
echo "Date: $(date)"
echo "Commit: $(git rev-parse HEAD)"
echo ""

./naab benchmarks/micro.naab > results/micro_$(date +%Y%m%d).txt
./naab benchmarks/macro.naab > results/macro_$(date +%Y%m%d).txt
./naab benchmarks/integration.naab > results/integration_$(date +%Y%m%d).txt

# Compare with baseline
python3 tools/compare_benchmarks.py results/baseline.txt results/latest.txt
```

**Estimated Effort:** 2-3 days

**Priority:** High (essential for optimization work)

---

### 3.3.3: Optimize Interpreter Hot Paths ❌ TODO

**Goal:** Identify and optimize slowest operations.

#### Step 1: Profile

```bash
# Linux: perf
perf record ./naab benchmark.naab
perf report

# macOS: Instruments
instruments -t "Time Profiler" ./naab benchmark.naab

# Cross-platform: gprof
g++ -pg interpreter.cpp
./naab benchmark.naab
gprof ./naab gmon.out
```

#### Step 2: Identify Hot Spots

Typical hot spots in tree-walking interpreters:
1. AST node visitation (virtual function calls)
2. Variable lookup (map searches)
3. Dynamic type checking
4. Memory allocation (shared_ptr creation)
5. String operations

#### Step 3: Optimize

**Optimization 1: Inline Fast Paths**
```cpp
// Before: virtual function call
value = expr->evaluate();

// After: inline common cases
if (expr->kind() == NodeKind::LiteralExpr) {
    value = static_cast<LiteralExpr*>(expr)->getValue();  // Direct access
} else {
    value = expr->evaluate();  // Fallback to virtual call
}
```

**Optimization 2: Cache Variable Lookups**
```cpp
class Scope {
public:
    // Add index-based lookup
    Value* getByIndex(size_t index) { return &values_[index]; }

private:
    std::vector<Value> values_;           // Dense array
    std::map<std::string, size_t> names_; // Name → index mapping
};
```

**Optimization 3: Specialize Common Operations**
```cpp
// Before: generic binary operation
Value binaryOp(const Value& left, TokenType op, const Value& right);

// After: specialized for int + int
Value add_int_int(int left, int right) { return Value(left + right); }

// Dispatch
if (left.isInt() && right.isInt() && op == PLUS) {
    return add_int_int(left.asInt(), right.asInt());  // Fast path
} else {
    return binaryOp(left, op, right);  // Slow path
}
```

**Estimated Speedup:** 2-5x for interpreter overhead

**Estimated Effort:** 5-10 days (iterative process)

**Priority:** Medium (after benchmarking)

---

### 3.3.4: Consider Bytecode Compiler ❌ TODO

**Goal:** Compile AST to bytecode for faster execution.

#### Why Bytecode?

**Tree-Walking Interpreter (Current):**
- Traverse AST for every operation
- Pointer chasing, poor cache locality
- Virtual function call overhead
- Flexible but slow

**Bytecode Interpreter:**
- Linear array of instructions
- Cache-friendly
- Direct dispatch (switch or computed goto)
- 5-10x faster than tree-walking

#### Design

**Bytecode Format:**
```
Opcode (1 byte) | Operand 1 (4 bytes) | Operand 2 (4 bytes) | ...
```

**Example Opcodes:**
```cpp
enum class Opcode : uint8_t {
    LOAD_CONST,    // Load constant from constant pool
    LOAD_VAR,      // Load variable by index
    STORE_VAR,     // Store to variable
    ADD,           // Pop two values, push sum
    SUB, MUL, DIV,
    CALL,          // Call function
    RETURN,        // Return from function
    JUMP,          // Unconditional jump
    JUMP_IF_FALSE, // Conditional jump
    // ...
};
```

**Compilation Example:**
```naab
let x = 10 + 20
```

**AST:**
```
VarDeclStmt
  ├── name: "x"
  └── value: BinaryExpr
      ├── left: LiteralExpr(10)
      ├── op: PLUS
      └── right: LiteralExpr(20)
```

**Bytecode:**
```
0: LOAD_CONST 0   # Load 10 (constant pool[0])
2: LOAD_CONST 1   # Load 20 (constant pool[1])
4: ADD            # Pop 20, pop 10, push 30
5: STORE_VAR 0    # Store to variable x (locals[0])
```

#### Implementation Phases

**Phase 1: Bytecode Compiler (2-3 weeks)**
- Define bytecode instruction set
- Implement AST → bytecode compiler
- Create bytecode serialization format
- Test with simple programs

**Phase 2: Bytecode Interpreter (2-3 weeks)**
- Implement bytecode execution engine
- Value stack management
- Call stack management
- Exception handling in bytecode

**Phase 3: Optimization (2-3 weeks)**
- Computed goto dispatch (faster than switch)
- Inline caching (for variable/method lookups)
- Constant folding during compilation
- Dead code elimination

**Total Effort:** 6-9 weeks

**Estimated Speedup:** 5-10x over tree-walking interpreter

**Priority:** Low (major undertaking, consider for v2.0)

#### Alternatives to Full Bytecode

**Option 1: JIT Compilation**
- Compile hot functions to machine code
- Use LLVM or similar
- 10-100x speedup for hot paths
- **Effort:** 3-6 months
- **Priority:** Very Low (v3.0+)

**Option 2: Hybrid Approach**
- Tree-walking for cold code
- Bytecode for hot code
- JIT for very hot code
- **Effort:** 4-8 weeks
- **Priority:** Low (v2.0)

---

## Performance Goals

### Target Benchmarks (vs Python 3.11)

| Benchmark | Python 3.11 | NAAb Goal | Status |
|-----------|-------------|-----------|--------|
| Fibonacci(30) | 100ms | 200ms (2x slower) | ❌ Not measured |
| List sum (1M) | 50ms | 100ms (2x slower) | ❌ Not measured |
| Dict lookup (100K) | 20ms | 40ms (2x slower) | ❌ Not measured |
| JSON parse (10KB) | 5ms | 10ms (2x slower) | ❌ Not measured |
| Inline Python call | 50ms | 10ms (5x faster*) | ❌ Not measured |

\* With subprocess reuse

**Rationale:** Being 2x slower than Python is acceptable for an interpreted language with advanced features (polyglot, structs, generics).

### Memory Goals

| Metric | Goal | Status |
|--------|------|--------|
| Peak memory (ATLAS) | < 50MB | ❌ Not measured |
| Idle memory | < 5MB | ❌ Not measured |
| Memory per value | < 64 bytes | ❌ Not measured |
| No leaks | 0 bytes leaked | ⚠️ Not verified |

---

## Recommended Implementation Order

### Short Term (Next 2 Weeks)
1. ✅ Document optimization strategy (this document)
2. 🎯 **Create benchmarking suite** (2-3 days)
3. 🎯 **Measure baseline performance** (1 day)
4. 🎯 **Implement inline code caching** (3-5 days)

### Medium Term (Next 1-2 Months)
1. Profile interpreter with real workloads
2. Optimize hot paths (iterative)
3. Verify no memory leaks (Valgrind)
4. Implement cycle detection (if needed)

### Long Term (v2.0+)
1. Consider bytecode compilation
2. Consider JIT compilation
3. Implement parallel execution (async/await)

---

## Profiling Tools

### CPU Profiling

**Linux:**
```bash
# perf (best for Linux)
perf record -g ./naab benchmark.naab
perf report

# Flame graph
perf script | flamegraph.pl > flame.svg
```

**macOS:**
```bash
# Instruments
instruments -t "Time Profiler" ./naab benchmark.naab

# Sample
sample naab 10 -f profile.txt
```

**Cross-platform:**
```bash
# gprof
g++ -pg -o naab interpreter.cpp
./naab benchmark.naab
gprof naab gmon.out > profile.txt

# Valgrind callgrind
valgrind --tool=callgrind ./naab benchmark.naab
kcachegrind callgrind.out.*
```

### Memory Profiling

**Linux:**
```bash
# Valgrind massif (heap profiler)
valgrind --tool=massif ./naab benchmark.naab
ms_print massif.out.*

# heaptrack
heaptrack ./naab benchmark.naab
heaptrack_gui heaptrack.naab.*
```

**macOS:**
```bash
# Instruments
instruments -t "Allocations" ./naab benchmark.naab

# leaks
leaks --atExit -- ./naab benchmark.naab
```

---

## Optimization Checklist

### Before Optimizing
- [ ] Create benchmarking suite
- [ ] Measure baseline performance
- [ ] Profile to find hot spots
- [ ] Set performance goals

### While Optimizing
- [ ] Focus on hottest code first (80/20 rule)
- [ ] Measure every change (no guessing)
- [ ] Keep correctness tests passing
- [ ] Document trade-offs

### After Optimizing
- [ ] Verify performance goals met
- [ ] Check for regressions in other areas
- [ ] Document optimizations (for maintenance)
- [ ] Update benchmarks with new baseline

---

## Performance vs Other Languages

### Interpreted Languages

| Language | Relative Speed | Notes |
|----------|---------------|-------|
| Python 3.11 | 1.0x (baseline) | CPython, tree-walking |
| Ruby 3.2 | 0.8x | YARV bytecode VM |
| PHP 8.2 | 1.2x | Zend Engine, JIT |
| **NAAb (current)** | **0.3-0.5x** | Tree-walking (estimate) |
| **NAAb (optimized)** | **0.5-1.0x** | With caching + opts |
| **NAAb (bytecode)** | **2-5x** | With bytecode VM |

### Compiled Languages

| Language | Relative Speed | Notes |
|----------|---------------|-------|
| JavaScript (V8) | 20-50x | JIT compilation |
| Java (HotSpot) | 30-100x | JIT compilation |
| Go | 50-100x | AOT compilation |
| Rust | 100-200x | AOT + LLVM opts |
| C | 100-300x | AOT + manual opts |

**Conclusion:** NAAb can be competitive with Python/Ruby, but will never match compiled languages. That's OK—developer productivity and polyglot features are the priority.

---

## Conclusion

**Phase 3.3 Status:**
- ✅ Strategy documented
- ❌ Benchmarking suite not created
- ❌ Baseline performance not measured
- ❌ Optimizations not implemented

**Estimated Effort for Complete Phase 3.3:**
- Benchmarking suite: 2-3 days
- Inline code caching: 3-5 days
- Hot path optimization: 5-10 days
- Bytecode compiler: 6-9 weeks (optional)
- **Total (without bytecode): 10-18 days**

**Priority:**
- High: Benchmarking suite (needed for any optimization work)
- High: Inline code caching (biggest expected win)
- Medium: Hot path optimization (iterative improvement)
- Low: Bytecode compiler (major effort, future consideration)

**Recommendation:** Start with benchmarking + caching, then profile and optimize hot paths iteratively. Defer bytecode compilation to v2.0 unless performance is unacceptable.
