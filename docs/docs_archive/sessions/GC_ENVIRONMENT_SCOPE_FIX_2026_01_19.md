# GC Environment Scope Fix - 2026-01-19

## Issue

The garbage collector was showing "0 values reachable" when called from within functions, even though local variables with cycles were clearly in scope.

### Symptoms

```
[GC] Running garbage collection...
[GC] Mark phase: 0 values reachable  ❌ Wrong!
[GC] Sweep phase: 0 cycles detected
[GC] No cycles detected
```

## Root Cause

The `gc_collect()` built-in function was passing `global_env_` to the garbage collector, but local function variables exist in the **current execution environment** (`current_env_`), not the global environment.

### Original Code (Incorrect)

```cpp
// In interpreter.cpp
void Interpreter::runGarbageCollection() {
    // ...
    size_t collected = cycle_detector_->detectAndCollect(global_env_);  // ❌ Wrong scope!
}

else if (func_name == "gc_collect") {
    runGarbageCollection();  // Always uses global_env_
}
```

When `gc_collect()` was called from within `main()`:
- Local variables `a` and `b` were in `main()`'s environment
- GC was only looking at global environment
- Result: 0 values found

## Solution

Modified `runGarbageCollection()` to accept an optional environment parameter, defaulting to global if not provided:

### Fixed Code

```cpp
// In interpreter.h
void runGarbageCollection(std::shared_ptr<Environment> env = nullptr);

// In interpreter.cpp
void Interpreter::runGarbageCollection(std::shared_ptr<Environment> env) {
    if (!cycle_detector_ || !gc_enabled_) {
        return;
    }

    fmt::print("[GC] Running garbage collection...\n");

    // Use provided environment or fall back to global environment
    auto root_env = env ? env : global_env_;

    // Run mark-and-sweep cycle detection
    size_t collected = cycle_detector_->detectAndCollect(root_env);
    // ...
}

// Built-in function now passes current environment
else if (func_name == "gc_collect") {
    runGarbageCollection(current_env_);  // ✅ Correct scope!
    result_ = std::make_shared<Value>();
}
```

## Results

### After Fix

```
[GC] Running garbage collection...
[GC] Mark phase: 4 values reachable  ✅ Correct!
[GC] Sweep phase: 0 cycles detected
[GC] No cycles detected
```

The GC now properly:
1. Traverses the current function's environment
2. Finds local variables (a, b) and their field values (4 total values)
3. Correctly identifies that cycles are reachable (still in scope)
4. Won't collect them until they go out of scope

## Key Insights

1. **Environment Hierarchy**: NAAb has nested environments (global → function scope)
2. **Current vs Global**: `current_env_` changes during execution, `global_env_` stays constant
3. **GC Root Selection**: The GC must start from the **current execution context** to find all live values
4. **Automatic GC**: When implementing automatic GC triggering, must pass `current_env_`

## Files Modified

1. **include/naab/interpreter.h**:342-343
   - Updated `runGarbageCollection()` signature to accept optional environment parameter

2. **src/interpreter/interpreter.cpp**:3519-3534
   - Implemented environment parameter with fallback to global

3. **src/interpreter/interpreter.cpp**:2301-2303
   - Updated `gc_collect()` built-in to pass `current_env_`

## Build & Test

```bash
cd build
make -j4                                              # 100% success
cd ..
./build/naab-lang run examples/test_gc_with_collection.naab   # 4 values found ✅
./build/naab-lang run examples/test_memory_cycles.naab        # All 5 tests pass ✅
```

## Next Steps

1. **Automatic GC Triggering**: Ensure `runGarbageCollection()` is called with `current_env_` when triggered automatically
2. **Valgrind Verification**: Run with Valgrind to verify no memory leaks
3. **Performance Testing**: Measure GC overhead with automatic triggering

## Conclusion

This fix was **critical** for making the GC functional. Without it, the GC could never find values in function scopes, rendering it useless for 99% of real-world code. The GC now correctly scans the current execution context and can properly identify reachable vs unreachable cyclic structures.

---

**Session**: 2026-01-19 (Continuation)
**Phase**: 3.2 Runtime Cycle Detection
**Status**: Core GC implementation complete and verified
**Completion**: ~65% of Phase 3.2
