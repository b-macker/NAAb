# NAAb Memory Model Documentation

## Executive Summary

**Status:** DOCUMENTED
**Current Implementation:** Basic shared_ptr-based reference counting
**Recommended Improvements:** Cycle detection, memory profiling, leak detection

---

## Memory Management Strategy

NAAb uses C++ smart pointers for memory management with clear ownership semantics.

### Core Principles

1. **Automatic Memory Management**
   - No manual malloc/free
   - No explicit delete calls
   - RAII (Resource Acquisition Is Initialization)

2. **Clear Ownership**
   - AST nodes: unique_ptr (single owner)
   - Runtime values: shared_ptr (reference counting)
   - Temporary expressions: unique_ptr → shared_ptr conversion

3. **Reference Semantics**
   - Structs can be passed by reference (`ref` keyword - Phase 2.1)
   - Default is copy-by-value (with deep copy)
   - Explicit reference prevents copies

---

## AST Memory Management

### Ownership Model

**Rule:** AST nodes use `std::unique_ptr<T>` for owned children.

**Rationale:**
- AST is a tree structure with clear parent-child relationships
- Each node is owned by exactly one parent
- Unique ownership prevents cycles and double-frees
- Move semantics enable efficient tree construction

**Example:**
```cpp
class FunctionDecl : public ASTNode {
private:
    std::string name_;                          // Owned value
    std::vector<Parameter> params_;             // Owned vector
    std::unique_ptr<Stmt> body_;               // Owned pointer (single owner)
};

class BinaryExpr : public Expr {
private:
    std::unique_ptr<Expr> left_;    // Owned (FunctionDecl owns this expr)
    std::unique_ptr<Expr> right_;   // Owned (FunctionDecl owns this expr)
};
```

### Lifetime

**Creation:** Parser creates AST nodes
**Ownership:** Parser returns unique_ptr to caller
**Lifetime:** AST lives until Program node is destroyed
**Destruction:** Automatic (unique_ptr destructor cascades)

**Guarantee:** No memory leaks in AST (assuming no cycles, which tree structure prevents).

---

## Runtime Value Memory Management

### Value Representation

**Current Design (simplified):**
```cpp
class Value {
    ValueType type_;  // Int, Float, String, Struct, List, Dict, etc.
    std::variant<int, float, std::string, StructValue, ...> data_;
};

struct StructValue {
    std::string type_name_;
    std::map<std::string, std::shared_ptr<Value>> fields_;  // Shared ownership
};
```

### Ownership Model

**Rule:** Runtime values use `std::shared_ptr<Value>` for shared ownership.

**Rationale:**
- Values can be referenced from multiple places:
  - Variables in different scopes
  - Struct fields
  - List/Dict elements
  - Function closures (future)
- Reference counting automatically frees unused values
- Enables safe aliasing

**Example:**
```naab
let x = Box { value: 42 }
let y = x  // Both x and y share the same Box (shared_ptr)

struct Container {
    data: Box
}
let c = Container { data: x }  // c.data also shares the Box
```

### Reference Semantics (Phase 2.1)

**Rule:** `ref` keyword prevents copies, passes pointer to existing value.

**Implementation:**
```cpp
// Without ref:
void assignValue(std::shared_ptr<Value> value) {
    // value is a copy of the shared_ptr (refcount incremented)
    // Modifications don't affect original unless shared_ptr points to same object
}

// With ref:
void assignValue(std::shared_ptr<Value>& value) {
    // value is a reference to the shared_ptr (no copy, same object)
    // Modifications affect the same Value object
}
```

**NAAb Code:**
```naab
function modify(data: Box) {
    // Receives a copy (separate shared_ptr, but same underlying object)
    data.value = 100  // Modifies the shared object
}

function modifyRef(ref data: Box) {
    // Receives a reference to the same shared_ptr
    data.value = 100  // Modifies the shared object
}
```

**Key Point:** In NAAb's current design, both behave similarly because shared_ptr shares the object. The `ref` keyword is semantic, preventing full deep copies when desired.

---

## Memory Leaks

### Potential Leak Sources

#### 1. Circular References ⚠️ **RISK**

**Problem:**
```naab
struct Node {
    value: int
    next: Node | null
}

let a = Node { value: 1, next: null }
let b = Node { value: 2, next: a }
a.next = b  // Cycle: a → b → a
```

**Result:** Both `a` and `b` have shared_ptr references to each other. Reference count never reaches zero. **Memory leak.**

**Status:** ❌ NOT ADDRESSED

**Solution Options:**
1. **Weak pointers** (std::weak_ptr)
   - Mark certain fields as weak (no refcount increment)
   - Requires programmer annotation or heuristic
2. **Cycle detector**
   - Periodically check for unreachable cycles
   - Tracing garbage collector approach
3. **Ownership discipline**
   - Document which fields should break cycles
   - Programmer responsibility

**Recommended:** Implement cycle detector (see Phase 3.2 pending items).

#### 2. Inline Code Subprocess Handles ⚠️ **RISK**

**Problem:**
Inline code execution spawns subprocesses (Python, JavaScript, etc.). If process handles aren't cleaned up properly, file descriptors leak.

**Status:** ⚠️ PARTIALLY ADDRESSED

**Current Implementation:**
- Subprocesses are closed after execution
- File descriptors are closed

**Potential Issues:**
- Exception during subprocess execution
- Subprocess hangs indefinitely
- Zombie processes

**Solution:** Ensure RAII for subprocess handles:
```cpp
class SubprocessHandle {
public:
    SubprocessHandle(/* args */);
    ~SubprocessHandle() {
        if (process_) {
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
        }
    }
private:
    pid_t pid_;
    // ...
};
```

#### 3. Forgotten std::shared_ptr Storage ⚠️ **RARE**

**Problem:**
If interpreter stores shared_ptr in global state and never clears it, memory won't be freed.

**Status:** ✅ LOW RISK (controlled interpreter state)

**Prevention:**
- Keep interpreter state minimal
- Clear all state between runs
- No global caches without eviction

---

## Memory Usage Patterns

### Stack vs Heap

**Stack Allocation:**
- AST during parsing (temporary, then moved to heap)
- Local variables during interpretation (Value objects, not std::shared_ptr)
- Function call frames

**Heap Allocation:**
- All AST nodes (via unique_ptr)
- All runtime values (via shared_ptr)
- String contents
- List/Dict storage

### Growth Characteristics

**AST Memory:**
- Proportional to source code size
- One-time allocation during parsing
- Constant after parsing (no growth)
- Released after interpretation

**Runtime Memory:**
- Proportional to:
  - Number of variables in scope
  - Size of data structures (lists, dicts)
  - Depth of call stack
- Can grow during execution (lists append, dicts insert)
- Released when values go out of scope (refcount = 0)

**Inline Code Memory:**
- Subprocess creation (fork overhead)
- Subprocess heap (Python/JS memory)
- Pipe buffers (stdin/stdout communication)
- **Recommendation:** Cache compiled inline code (future optimization)

---

## Phase 3.2: Memory Management Tasks

### 1. Define and Document Memory Model ✅ COMPLETE

**This document fulfills this requirement.**

Documented:
- AST ownership (unique_ptr)
- Runtime values (shared_ptr)
- Reference semantics (`ref` keyword)
- Potential leak sources

### 2. Implement Cycle Detection ❌ TODO

**Goal:** Detect and break reference cycles in structs.

**Approach:**
```cpp
class CycleDetector {
public:
    // Run periodically (e.g., after N allocations)
    void detectAndBreak() {
        std::set<std::shared_ptr<Value>> visited;
        std::set<std::shared_ptr<Value>> reachable;

        // Mark phase: find all reachable values
        for (auto& [name, value] : current_scope_) {
            markReachable(value, visited, reachable);
        }

        // Sweep phase: find unreachable values with refcount > 0
        // (These are in cycles)
        breakCycles(reachable);
    }

private:
    void markReachable(std::shared_ptr<Value> value,
                      std::set<std::shared_ptr<Value>>& visited,
                      std::set<std::shared_ptr<Value>>& reachable);
    void breakCycles(std::set<std::shared_ptr<Value>>& reachable);
};
```

**Estimated Effort:** 2-3 days

**Files to Modify:**
- `src/interpreter/interpreter.cpp` - Add cycle detector
- `src/interpreter/value.cpp` - Add traversal methods

### 3. Add Memory Profiling ❌ TODO

**Goal:** Track memory usage and identify leaks.

**Features:**
- Total memory allocated
- Peak memory usage
- Current memory usage
- Top memory-consuming values
- Allocation traces (optional, debug mode)

**API:**
```naab
// Built-in memory profiling functions
let stats = memory.getStats()
print("Total allocated: " + stats.total_allocated)
print("Current usage: " + stats.current_usage)
print("Peak usage: " + stats.peak_usage)

memory.printTopConsumers(10)  // Show top 10 largest values
```

**Implementation:**
```cpp
class MemoryProfiler {
public:
    void recordAllocation(size_t bytes, const std::string& type);
    void recordDeallocation(size_t bytes);

    MemoryStats getStats() const;
    void printReport() const;

private:
    size_t total_allocated_ = 0;
    size_t current_usage_ = 0;
    size_t peak_usage_ = 0;
    std::map<std::string, size_t> type_usage_;  // Memory by type
};
```

**Estimated Effort:** 2-3 days

**Files to Create:**
- `src/interpreter/memory_profiler.h`
- `src/interpreter/memory_profiler.cpp`

### 4. Ensure No Leaks ⏳ ONGOING

**Goal:** Verify no memory leaks in production code.

**Tools:**
- **Valgrind** (memcheck) - Detect leaks
- **Address Sanitizer** (ASan) - Detect leaks and use-after-free
- **Leak Sanitizer** (LSan) - Leak-specific tool

**Process:**
1. Run test suite under Valgrind:
   ```bash
   valgrind --leak-check=full --show-leak-kinds=all ./naab test.naab
   ```

2. Run with Address Sanitizer:
   ```bash
   clang++ -fsanitize=address -g interpreter.cpp
   ./naab test.naab
   ```

3. Check for:
   - Definite leaks (memory not freed)
   - Possible leaks (ambiguous pointers)
   - Reachable blocks (still referenced but questionable)

4. Fix leaks:
   - Add proper destructors
   - Clear collections on scope exit
   - Break cycles explicitly
   - Use RAII for resources

**Estimated Effort:** 1-2 days (for initial pass) + ongoing

**Status:** ⚠️ NOT VERIFIED

---

## Best Practices for NAAb Developers

### For Interpreter Developers

1. **Use smart pointers exclusively**
   - AST: unique_ptr
   - Runtime: shared_ptr
   - Never use raw pointers for ownership

2. **Avoid global state**
   - Keep interpreter state in Interpreter class
   - Clear state between runs
   - No static shared_ptr storage

3. **RAII for resources**
   - File handles: RAII wrapper
   - Subprocess handles: RAII wrapper
   - Network sockets: RAII wrapper

4. **Test for leaks**
   - Run Valgrind regularly
   - Use Address Sanitizer in CI
   - Profile memory in long-running tests

### For NAAb Users (Future)

1. **Avoid circular references**
   - Use weak references where appropriate
   - Document ownership in struct design
   - Consider using IDs instead of direct references

2. **Release large data**
   - Set variables to null when done
   - Clear lists/dicts explicitly if very large
   - Scope variables narrowly

3. **Profile memory usage**
   - Use built-in memory profiling (when available)
   - Monitor peak usage in production
   - Optimize hot paths

---

## Future Enhancements

### Generational GC (Future)

**Goal:** Improve performance with generational hypothesis.

**Approach:**
- Young generation: frequently allocated, short-lived
- Old generation: long-lived values
- Minor GC: collect young generation (fast)
- Major GC: collect all (slow, rare)

**Estimated Effort:** 4-6 weeks

**Priority:** Low (current approach is sufficient for now)

### Compacting GC (Future)

**Goal:** Reduce memory fragmentation.

**Approach:**
- Move values to contiguous memory
- Update all pointers
- Requires value relocation support

**Estimated Effort:** 6-8 weeks

**Priority:** Low (C++ heap allocator handles fragmentation well)

### Arena Allocation (Medium Term)

**Goal:** Faster allocation for temporary values.

**Approach:**
- Allocate temporary values in arena
- Bulk free entire arena at once
- Useful for expression evaluation

**Estimated Effort:** 2-3 weeks

**Priority:** Medium (good performance win)

---

## Conclusion

**Phase 3.2 Status:**
- ✅ Memory model documented
- ❌ Cycle detection not implemented
- ❌ Memory profiling not implemented
- ⚠️ Leak verification not performed

**Estimated Effort for Complete Phase 3.2:**
- Cycle detection: 2-3 days
- Memory profiling: 2-3 days
- Leak verification: 1-2 days
- **Total: 5-8 days**

**Priority:**
- High: Leak verification (should be done soon)
- Medium: Cycle detection (needed for production)
- Low: Memory profiling (nice to have, not critical)

**Current Status:** NAAb's memory management is solid for typical use cases (no cycles, reasonable data sizes). Production hardening requires cycle detection and leak verification.
