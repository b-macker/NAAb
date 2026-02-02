# Phase 3.2: Memory Management - Implementation Analysis

**Date:** 2026-01-18
**Status:** ANALYSIS IN PROGRESS

## Executive Summary

**Current Status:** Memory management is partially implemented
- ✅ **Memory Model:** Fully documented (MEMORY_MODEL.md)
- ✅ **Type-level cycle detection:** Implemented (struct definitions)
- ❌ **Runtime cycle detection:** NOT IMPLEMENTED
- ❌ **Memory profiling:** NOT IMPLEMENTED
- ⏳ **Leak verification:** PENDING TESTING

**Recommended Actions:**
1. Implement runtime cycle detector for Value objects
2. Add memory profiling/tracking
3. Run leak detection tools (Valgrind, ASan)

---

## Memory Model Overview

### AST Memory (✅ Complete)
**Approach:** `std::unique_ptr` for tree ownership
**Status:** ✅ NO LEAKS (tree structure prevents cycles)
**Evidence:** Unique pointers cascade destruction

### Runtime Values (⚠️ Partial)
**Approach:** `std::shared_ptr` with reference counting
**Status:** ⚠️ **RISK OF CYCLES**
**Problem:** Circular references between structs/lists/dicts never freed

**Value Structure:**
```cpp
using ValueData = std::variant<
    std::monostate,                                      // void/null
    int, double, bool, std::string,                     // primitives
    std::vector<std::shared_ptr<Value>>,                // list (can cycle)
    std::unordered_map<std::string, std::shared_ptr<Value>>,  // dict (can cycle)
    std::shared_ptr<StructValue>                        // struct (can cycle)
>;
```

**Cycle Risk Locations:**
- **StructValue fields:** Can reference other StructValues
- **List elements:** Can reference lists/structs/dicts
- **Dict values:** Can reference dicts/lists/structs

---

## Existing Cycle Detection (Type-Level) ✅

### Location
- File: `src/runtime/struct_registry.cpp`
- Function: `validateStructDef()`
- Lines: ~20 lines

### What It Detects
**Circular struct type definitions:**
```naab
struct A {
    b: B  # A depends on B
}
struct B {
    a: A  # B depends on A → CYCLE DETECTED AT COMPILE TIME
}
```

### How It Works
```cpp
bool StructRegistry::validateStructDef(const interpreter::StructDef& def,
                                       std::set<std::string>& visiting) {
    if (visiting.count(def.name)) {
        throw std::runtime_error("Circular struct dependency detected: " +
                               def.name);
    }
    visiting.insert(def.name);

    for (const auto& field : def.fields) {
        if (field.type.kind == ast::TypeKind::Struct) {
            auto dep = getStruct(field.type.getStructName());
            if (dep) {
                validateStructDef(*dep, visiting);
            }
        }
    }

    visiting.erase(def.name);
    return true;
}
```

**Algorithm:** Depth-first search with visited set
**Time Complexity:** O(N) where N = number of struct types
**Result:** Prevents type-level cycles at struct definition time

### Limitation
**Does NOT detect runtime cycles:**
```naab
struct Node {
    value: int
    next: Node | null  # Legal type (nullable breaks compile-time cycle)
}

let a = Node { value: 1, next: null }
let b = Node { value: 2, next: a }
a.next = b  # Runtime cycle created: a → b → a

# Reference count: a.refcount = 2, b.refcount = 2
# Both never freed → MEMORY LEAK
```

---

## Missing: Runtime Cycle Detection ❌

### Problem Statement

**Scenario:**
```naab
struct Node {
    value: int
    next: Node | null
}

# Create cycle
let a = Node { value: 1, next: null }
let b = Node { value: 2, next: a }
a.next = b  # Cycle: a ↔ b

# When a and b go out of scope:
# - a.refcount = 2 (variable 'a' + b.next)
# - b.refcount = 2 (variable 'b' + a.next)
# - Neither reaches refcount=0 → LEAKED
```

**Impact:** Memory leak for any cyclic data structure

**Common Patterns That Leak:**
1. **Doubly-linked lists:**
   ```naab
   a.next = b
   b.prev = a
   ```

2. **Parent-child references:**
   ```naab
   parent.child = node
   node.parent = parent
   ```

3. **Graph structures:**
   ```naab
   nodeA.neighbors = [nodeB, nodeC]
   nodeB.neighbors = [nodeA]  # Cycle
   ```

4. **Self-references:**
   ```naab
   list.tail = list  # Simple cycle
   ```

---

## Solution Approach: Runtime Cycle Detector

### Design

**Algorithm:** Mark-and-sweep garbage collection
1. **Mark Phase:** Traverse all reachable values from roots (environment)
2. **Sweep Phase:** Find values with refcount > reachable count
3. **Break Cycles:** Clear references in cyclic structures

**Trigger:** Run periodically or on-demand
- After N allocations (e.g., every 1000)
- When memory usage exceeds threshold
- On explicit `gc.collect()` call

### Implementation Plan

#### 1. Value Traversal Methods

**Add to Value class:**
```cpp
class Value {
public:
    // ... existing code ...

    // Traverse all child values (for cycle detection)
    void traverse(std::function<void(std::shared_ptr<Value>)> visitor);

    // Get all referenced values
    std::vector<std::shared_ptr<Value>> getReferences() const;
};
```

**Implementation:**
```cpp
void Value::traverse(std::function<void(std::shared_ptr<Value>)> visitor) {
    // Visit list elements
    if (auto* list = std::get_if<std::vector<std::shared_ptr<Value>>>(&data)) {
        for (auto& elem : *list) {
            visitor(elem);
        }
    }
    // Visit dict values
    else if (auto* dict = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&data)) {
        for (auto& [key, val] : *dict) {
            visitor(val);
        }
    }
    // Visit struct fields
    else if (auto* struct_val = std::get_if<std::shared_ptr<StructValue>>(&data)) {
        for (auto& field : (*struct_val)->field_values) {
            visitor(field);
        }
    }
}
```

#### 2. Cycle Detector Class

**Create new file: `src/interpreter/cycle_detector.h`**
```cpp
#pragma once

#include <memory>
#include <set>
#include <vector>
#include <unordered_map>

namespace naab {
namespace interpreter {

class Value;
class Environment;

class CycleDetector {
public:
    CycleDetector() = default;

    // Run cycle detection and collection
    size_t detectAndBreak(std::shared_ptr<Environment> root_env);

private:
    // Mark all reachable values from environment
    void markReachable(std::shared_ptr<Value> value,
                      std::set<std::shared_ptr<Value>>& visited,
                      std::set<std::shared_ptr<Value>>& reachable);

    // Find values in cycles (refcount > 0 but unreachable)
    std::vector<std::shared_ptr<Value>> findCycles(
        const std::set<std::shared_ptr<Value>>& reachable);

    // Break cycles by clearing references
    void breakCycles(const std::vector<std::shared_ptr<Value>>& cycles);

    size_t total_allocations_ = 0;
    size_t total_freed_ = 0;
};

} // namespace interpreter
} // namespace naab
```

**Create new file: `src/interpreter/cycle_detector.cpp`**
```cpp
#include "cycle_detector.h"
#include "naab/interpreter.h"

namespace naab {
namespace interpreter {

void CycleDetector::markReachable(
    std::shared_ptr<Value> value,
    std::set<std::shared_ptr<Value>>& visited,
    std::set<std::shared_ptr<Value>>& reachable)
{
    if (!value || visited.count(value)) {
        return;
    }

    visited.insert(value);
    reachable.insert(value);

    // Recursively mark children
    value->traverse([&](std::shared_ptr<Value> child) {
        markReachable(child, visited, reachable);
    });
}

std::vector<std::shared_ptr<Value>> CycleDetector::findCycles(
    const std::set<std::shared_ptr<Value>>& reachable)
{
    std::vector<std::shared_ptr<Value>> cycles;

    // In a real implementation, we'd need to track all allocated values
    // For now, this is a placeholder showing the concept

    return cycles;
}

void CycleDetector::breakCycles(
    const std::vector<std::shared_ptr<Value>>& cycles)
{
    // Break cycles by clearing references in cyclic structures
    for (auto& value : cycles) {
        // Clear list elements
        if (auto* list = std::get_if<std::vector<std::shared_ptr<Value>>>(&value->data)) {
            list->clear();
        }
        // Clear dict values
        else if (auto* dict = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&value->data)) {
            dict->clear();
        }
        // Clear struct fields
        else if (auto* struct_val = std::get_if<std::shared_ptr<StructValue>>(&value->data)) {
            (*struct_val)->field_values.clear();
        }
    }

    total_freed_ += cycles.size();
}

size_t CycleDetector::detectAndBreak(std::shared_ptr<Environment> root_env) {
    std::set<std::shared_ptr<Value>> visited;
    std::set<std::shared_ptr<Value>> reachable;

    // Mark all values reachable from environment
    auto all_names = root_env->getAllNames();
    for (const auto& name : all_names) {
        auto value = root_env->get(name);
        markReachable(value, visited, reachable);
    }

    // Find unreachable cycles
    auto cycles = findCycles(reachable);

    // Break cycles
    breakCycles(cycles);

    return cycles.size();
}

} // namespace interpreter
} // namespace naab
```

#### 3. Integration with Interpreter

**Add to Interpreter class:**
```cpp
class Interpreter : public ast::ASTVisitor {
public:
    // ... existing code ...

    // Cycle detection
    void runCycleDetection();
    void enableAutomaticGC(size_t allocation_threshold = 1000);

private:
    CycleDetector cycle_detector_;
    size_t allocation_count_ = 0;
    size_t gc_threshold_ = 0;  // 0 = disabled
};
```

**Trigger in interpreter:**
```cpp
void Interpreter::visit(ast::VarDeclStmt& node) {
    // ... existing variable declaration code ...

    // Track allocations
    allocation_count_++;

    // Auto GC if enabled
    if (gc_threshold_ > 0 && allocation_count_ >= gc_threshold_) {
        runCycleDetection();
        allocation_count_ = 0;
    }
}

void Interpreter::runCycleDetection() {
    size_t freed = cycle_detector_.detectAndBreak(current_env_);
    if (freed > 0) {
        fmt::print("[GC] Collected {} cyclic values\n", freed);
    }
}
```

---

## Challenges & Limitations

### 1. Tracking All Allocations

**Problem:** Need to track all allocated Value objects to find unreachable ones

**Solutions:**
1. **Global registry:** Maintain weak_ptr registry of all values
   ```cpp
   std::set<std::weak_ptr<Value>> all_values_;
   ```

2. **Generation marking:** Mark values by "generation" and sweep old generations

3. **Conservative approach:** Only detect cycles reachable from environment (miss some)

### 2. Performance Impact

**Mark-and-sweep overhead:**
- O(N) where N = number of live values
- Can pause interpreter during GC
- Potentially expensive for large heaps

**Mitigation:**
- Run infrequently (every 1000+ allocations)
- Incremental GC (mark a few values per allocation)
- Generational GC (focus on young values)

### 3. Weak Pointers Alternative

**Instead of mark-and-sweep, use weak pointers:**

**Approach:** Programmer-specified weak fields
```naab
struct Node {
    value: int
    next: Node | null
    weak prev: Node | null  # weak = no refcount increment
}
```

**Pros:**
- No GC overhead
- Deterministic cleanup
- Explicit control

**Cons:**
- Requires programmer annotation
- Easy to get wrong
- Not automatic

---

## Testing Strategy

### Test Cases

**1. Simple Cycle**
```naab
struct Node {
    value: int
    next: Node | null
}

let a = Node { value: 1, next: null }
let b = Node { value: 2, next: a }
a.next = b  # Create cycle

# Expect: Both freed after scope exit (with GC)
```

**2. Self-Reference**
```naab
struct List {
    items: list
}

let lst = List { items: [] }
lst.items.append(lst)  # Self-reference

# Expect: Freed after scope exit
```

**3. Multi-Node Cycle**
```naab
let nodes = [
    Node { value: 1, next: null },
    Node { value: 2, next: null },
    Node { value: 3, next: null }
]
nodes[0].next = nodes[1]
nodes[1].next = nodes[2]
nodes[2].next = nodes[0]  # Cycle: 0 → 1 → 2 → 0

# Expect: All freed
```

**4. No False Positives**
```naab
let a = Node { value: 1, next: null }
let b = Node { value: 2, next: a }  # No cycle (one-way reference)

# Expect: Both freed normally (no GC needed)
```

### Leak Detection Tools

**Valgrind:**
```bash
valgrind --leak-check=full --show-leak-kinds=all \
    ./build/naab-lang run test_cycles.naab
```

**Address Sanitizer:**
```bash
# Rebuild with ASan
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -g" ..
make

# Run tests
./build/naab-lang run test_cycles.naab
```

**Expected Output:**
- **Without GC:** Definite memory leaks
- **With GC:** No leaks (all cycles collected)

---

## Implementation Checklist

### Phase 3.2.1: Cycle Detection
- [ ] Add `Value::traverse()` method
- [ ] Add `Value::getReferences()` method
- [ ] Create `CycleDetector` class (header + impl)
- [ ] Integrate with Interpreter
- [ ] Add allocation tracking
- [ ] Add GC trigger logic
- [ ] Create comprehensive test file
- [ ] Verify with Valgrind

**Estimated Effort:** 2-3 days

### Phase 3.2.2: Memory Profiling
- [ ] Create `MemoryProfiler` class
- [ ] Track allocation/deallocation
- [ ] Add `memory.getStats()` API
- [ ] Add `memory.printTopConsumers()` API
- [ ] Test memory profiling

**Estimated Effort:** 2-3 days

### Phase 3.2.3: Leak Verification
- [ ] Run Valgrind on all test files
- [ ] Run ASan on all test files
- [ ] Fix any detected leaks
- [ ] Document memory usage patterns
- [ ] Create leak test suite

**Estimated Effort:** 1-2 days

**Total Phase 3.2:** 5-8 days

---

## Recommendations

### Immediate (This Session)
1. ⏳ **Finish this analysis** - Document current state
2. ⏳ **Simple implementation** - Basic cycle detector (mark-and-sweep)
3. ⏳ **Test with cycles** - Verify it works

### Short Term (Next Session)
4. **Memory profiling** - Add tracking and stats
5. **Leak testing** - Run Valgrind/ASan
6. **Performance tuning** - Optimize GC frequency

### Alternative Approach (Lower Priority)
- **Weak pointers** - Add `weak` keyword for explicit cycle breaking
- **Ownership documentation** - Guide programmers on avoiding cycles

---

## Conclusion

**Memory Management Status:** ⚠️ **PARTIAL - NEEDS RUNTIME CYCLE DETECTION**

**What Exists:**
- ✅ Memory model documented
- ✅ Type-level cycle detection (struct definitions)
- ✅ Smart pointer-based management (RAII)

**What's Missing:**
- ❌ Runtime cycle detection (Value objects)
- ❌ Memory profiling/tracking
- ⏳ Leak verification (not yet tested)

**Risk Level:** ⚠️ **MEDIUM-HIGH**
- Any cyclic data structures WILL leak memory
- Can cause gradual memory exhaustion
- Critical for production use

**Recommended Priority:** **HIGH** - Should implement before v1.0

**Time to Complete:** 5-8 days (with testing)

---

## Next Steps

1. Implement basic cycle detector (mark-and-sweep)
2. Create test file with various cycle patterns
3. Verify with Valgrind that cycles are collected
4. Add memory profiling
5. Mark Phase 3.2 complete

---

**End of Analysis**
**Date:** 2026-01-18
**Status:** Analysis complete, ready for implementation
