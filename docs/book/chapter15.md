# Chapter 15: Performance Optimization

Performance is a key concern for any system orchestrator. NAAb is designed to be fast, but knowing how to optimize your code—and when to offload tasks to native modules—is crucial.

## 15.1 Benchmarking Your Code

The `time` module is your primary tool for measuring performance. A simple benchmarking pattern involves recording the start and end time of an operation.

```naab
use time as time

fn benchmark_operation() {
    let start = time.now_millis()
    
    // Operation to measure
    let list = [1, 2, 3]
    let i = 0
    while i < 1000 {
        list[0] = i
        i = i + 1
    }
    
    let end = time.now_millis()
    print("Operation took:", end - start, "ms")
}
```

## 15.2 Profiling and Hot Paths

When optimizing, focus on "hot paths"—the sections of code executed most frequently (e.g., inside loops).

1.  **Use Native Modules**: As discussed in Chapter 7, operations like sorting or filtering arrays are 10-100x faster using `array.sort` than writing a manual sort in NAAb or Python.
2.  **Avoid Excessive Crossing**: Calling a polyglot block has a small overhead (marshaling data). Avoid calling a Python block inside a tight loop if possible. Instead, pass the entire dataset to the block and process it there.

## 15.3 Optimizing Polyglot Blocks

*   **Batch Processing**: Pass large arrays to foreign blocks rather than calling the block once per item.
*   **Inline C++**: For CPU-bound tasks that cannot be handled by the Standard Library, use `<<cpp>>` blocks. NAAb compiles these to native machine code, providing C++ speed.

## 15.4 Inline Code Caching

NAAb automatically caches the compilation results of `<<cpp>>` blocks. This means the first time your program runs, there might be a delay for compilation, but subsequent runs will be instant as the cached binary is reused.

## 15.5 Interpreter Optimizations

The NAAb runtime includes built-in optimizations to speed up execution without any code changes from you:
*   **Variable Lookup Caching**: Repeated access to the same variable is optimized.
*   **Function Call Caching**: Frequent function calls are accelerated.
*   **Hot Path Optimization**: The interpreter detects and optimizes frequently executed loops and operations.
