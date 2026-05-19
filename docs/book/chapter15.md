# Chapter 15: Performance Optimization

Performance is a key concern for any system orchestrator. NAAb is designed to be fast by default, with a bytecode VM as the primary execution engine. This chapter covers the architecture that makes NAAb fast and how to optimize your code.

## 15.1 Bytecode VM

NAAb uses a **stack-based bytecode VM** as its default execution engine, providing ~8x faster execution than the legacy tree-walking interpreter.

The compilation pipeline:

```
Source → Lexer → Parser → AST → Compiler → Bytecode → VM
```

The VM is the default — no flags needed. To fall back to the tree-walker (useful for debugging or features not yet compiled to bytecode), use:

```bash
naab --tree-walk app.naab
```

### Computed Goto Dispatch

On GCC and Clang, the VM uses **computed goto** (`__label__` addresses) for opcode dispatch instead of a `switch` statement. This eliminates branch prediction overhead and provides ~20-30% faster dispatch compared to switch-based interpreters.

### Constant Folding

The compiler performs constant folding during compilation — expressions like `3 + 4` or `"hello" + " world"` are evaluated at compile time, producing a single constant in the bytecode.

## 15.2 NaN-Boxing

NAAb values use **NaN-boxing** — encoding type information in the unused bits of IEEE 754 NaN values. This means:

- **8 bytes per value** — int, double, bool, and null are stored inline with zero heap allocation
- **No boxing/unboxing overhead** — primitive values are manipulated directly without wrapping in objects
- **Cache-friendly** — the value stack is a contiguous array of 8-byte entries

Factory methods: `NaabVal::makeInt(42)`, `NaabVal::makeString("hi")`, `NaabVal::makeBool(true)`

## 15.3 Benchmarking Your Code

The `time` module is your primary tool for measuring performance:

```naab
use time

fn benchmark(name, iterations, body) {
    let start = time.now_millis()
    for i in 0..iterations {
        body()
    }
    let elapsed = time.now_millis() - start
    print(f"{name}: {elapsed}ms ({iterations} iterations)")
}

main {
    benchmark("array push", 10000, fn() {
        let arr = []
        for i in 0..100 { arr.push(i) }
    })
}
```

## 15.4 Profiling

Use the `--profile` flag to get execution timing information:

```bash
naab --profile app.naab
```

For fine-grained timing within your code, use `debug.timer()`:

```naab
main {
    debug.timer("data_processing")
    // ... expensive operation ...
    debug.timer("data_processing")  // prints elapsed time
}
```

## 15.5 Optimizing Polyglot Blocks

Polyglot blocks have startup overhead (process spawning, data marshaling). Optimize by:

1. **Batch processing** — pass large arrays to foreign blocks rather than calling the block once per item:

```naab
// SLOW: one Python call per item
for item in data {
    let result = <<python[item]
    item * 2
    >>
}

// FAST: one Python call for all items
let results = <<python[data]
[x * 2 for x in data]
>>
```

2. **Persistent runtimes** — keep interpreter state across multiple calls using the `runtime` keyword, avoiding repeated process startup.

3. **Use the right language** — Python for data science, Rust/C++ for computation, JavaScript for string processing. Avoid using heavy languages for simple operations that NAAb can handle natively.

4. **Inline C++** — for CPU-bound tasks, `<<cpp>>` blocks compile to native machine code. Compilation is cached, so subsequent runs reuse the compiled binary.

## 15.6 General Tips

- **Avoid excessive polyglot crossing** — calling a polyglot block inside a tight loop is slow. Move the loop inside the block.
- **Use stdlib functions** — `array.sort`, `array.filter_fn`, `array.map_fn` are implemented in C++ and significantly faster than equivalent NAAb loops.
- **Minimize copies** — NAAb uses value semantics. Re-assigning nested dicts/arrays inside loops creates copies. Use indexed access (`arr[i] = val`) to modify in place when possible.
- **GC tuning** — `--gc-threshold N` controls when garbage collection runs. Higher values reduce GC frequency at the cost of memory. `--gc-stats` prints GC statistics for tuning.
