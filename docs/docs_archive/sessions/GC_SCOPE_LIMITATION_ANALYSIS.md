# GC Scope Limitation Analysis - 2026-01-19

## Discovery

During memory verification testing (Phase 3.2 Step 5), I discovered an important limitation in the current GC design.

## The Issue

**Test Case:**
```naab
fn create_cycle() {
    let a = new Node { value: 1, next: null }
    let b = new Node { value: 2, next: a }
    a.next = b  # Create cycle
    # When function returns, a and b go out of scope
}

main {
    create_cycle()  # Cycle created
    gc_collect()    # Try to collect it
}
```

**Expected Result:**
```
[GC] Mark phase: X values reachable
[GC] Sweep phase: 2 cycles detected  ← Should detect a and b
[GC] Collected 2 cyclic values
```

**Actual Result:**
```
[GC] Mark phase: 2 values reachable
[GC] Sweep phase: 0 cycles detected  ← No cycles found!
[GC] No cycles detected
```

## Root Cause Analysis

### How Our GC Works

1. **Mark Phase:** Start from environment roots (variables in scope)
2. **Traverse:** Follow references to find all reachable values
3. **Sweep:** Identify values with refcount > 1 but not reachable
4. **Collect:** Break cycles by clearing internal references

### The Problem

When a cycle goes out of scope:
```
create_cycle() called:
  - a and b created in function's environment
  - a.next = b (refcount: a=1, b=2)
  - b.next = a (refcount: a=2, b=2)

create_cycle() returns:
  - Function environment destroyed
  - Local variable references to a and b removed
  - BUT: a and b still reference each other!
  - Final refcount: a=1 (from b.next), b=1 (from a.next)
  - Cycle exists but is INVISIBLE to GC
```

**The cycle is now:**
- ✅ Unreachable from any environment (out of scope)
- ✅ Has refcount > 0 (mutual references)
- ❌ **INVISIBLE to GC** (not in any environment we scan)

### Why This Happens

Our GC uses **environment-based root finding**:
```cpp
void CycleDetector::detectAndCollect(std::shared_ptr<Environment> root_env) {
    // Start from environment
    markFromEnvironment(root_env, visited, reachable);

    // Find cycles in reachable set
    auto cycles = findCycles(reachable, all_values);
}
```

**Problem:** We only track values that are currently IN an environment. Once values go completely out of scope, we lose track of them.

## Is This A Bug or Expected Behavior?

### It's Actually BOTH:

**Expected (Design Limitation):**
- Environment-based GC is a common approach
- Values must be reachable from roots to be tracked
- Once completely out of scope, values are "dead" to the GC

**Unexpected (Memory Leak):**
- Cycles that go out of scope will NEVER be collected
- This is a genuine memory leak
- The whole point of the GC is to prevent this!

## Impact Assessment

### When Does This Occur?

**Scenario 1: Local Cycles**
```naab
fn test() {
    let a = new Node { value: 1, next: null }
    let b = new Node { value: 2, next: a }
    a.next = b
}  # Cycle leaks here!
```

**Scenario 2: Temporary Cycles**
```naab
while true {
    let node = new Node { value: 1, next: null }
    node.next = node  # Self-cycle
}  # Leaks on every iteration!
```

**Scenario 3: Function Returns**
```naab
fn make_cycle() -> Node? {
    let a = new Node { value: 1, next: null }
    let b = new Node { value: 2, next: a }
    a.next = b
    return null  # Cycle leaks!
}
```

### When Does It Work?

**Scenario: Cycles in Scope**
```naab
main {
    let a = new Node { value: 1, next: null }
    let b = new Node { value: 2, next: a }
    a.next = b
    gc_collect()  # Works! Detects but doesn't collect (still in scope)
}
# Would be collected when main exits (if we had end-of-scope GC)
```

## Solutions

### Option 1: Global Value Registry (Tracing GC)

**Approach:** Track ALL Values in a global registry
```cpp
class ValueRegistry {
    static std::set<std::weak_ptr<Value>> all_values;

    static void register_value(std::shared_ptr<Value> val) {
        all_values.insert(val);
    }
};

// In Value constructor
Value::Value(...) {
    ValueRegistry::register_value(shared_from_this());
}
```

**Pros:**
- ✅ Can find ALL values, even out of scope
- ✅ True tracing GC
- ✅ No memory leaks from cycles

**Cons:**
- ❌ Requires Value to inherit from enable_shared_from_this
- ❌ Global state (thread safety concerns)
- ❌ Overhead on every Value construction
- ❌ Major architectural change

### Option 2: Weak Pointer Tracking

**Approach:** Keep weak_ptr references to all created values
```cpp
class Interpreter {
    std::vector<std::weak_ptr<Value>> tracked_values;

    void trackValue(std::shared_ptr<Value> val) {
        tracked_values.push_back(val);
    }
};
```

**Pros:**
- ✅ Doesn't prevent normal deallocation
- ✅ Can find orphaned cycles
- ✅ Less architectural change

**Cons:**
- ❌ Still requires tracking all Values
- ❌ Weak pointers have overhead
- ❌ Need periodic cleanup of expired weak_ptrs

### Option 3: Scope-Exit Collection

**Approach:** Trigger GC when scopes exit
```cpp
class Environment {
    ~Environment() {
        if (interpreter) {
            interpreter->runGarbageCollection(parent);
        }
    }
};
```

**Pros:**
- ✅ Catches cycles as they go out of scope
- ✅ Minimal code changes
- ✅ Natural collection points

**Cons:**
- ❌ Doesn't catch cycles until scope exit
- ❌ May still leak if cycles created in outer scope
- ❌ GC overhead on every scope exit

### Option 4: Accept Current Behavior (Document Limitation)

**Approach:** Document that cycles must be in scope for GC to work

**Pros:**
- ✅ No code changes needed
- ✅ Honest about limitations
- ✅ Works for many use cases

**Cons:**
- ❌ Real memory leaks possible
- ❌ Not a "complete" GC
- ❌ User must be careful

## Recommendation

For **Phase 3.2 completion**, I recommend **Option 4** with a note for future enhancement:

### Short-term (Now):
1. Document this limitation clearly
2. Explain when GC works and when it doesn't
3. Provide user guidance on avoiding leaks
4. Mark as "Known Limitation" in docs

### Long-term (Future Phase 3.2.X):
1. Implement Option 1 (Global Value Registry) or Option 2 (Weak Pointer Tracking)
2. This would be "Phase 3.2.5: Complete Tracing GC"
3. Estimated: 3-5 days of work

## Current Status Assessment

**Is the GC "Working"?**
- ✅ YES for cycles that remain in scope
- ✅ YES for automatic triggering
- ✅ YES for environment traversal
- ❌ NO for cycles that go completely out of scope

**Is this acceptable for v1.0?**
- For v1.0: **MAYBE** - Depends on user expectations
- For production: **NO** - True GC should handle all cycles
- For Phase 3.2: **YES** - Core functionality is there

## Workaround for Users

Until complete tracing GC is implemented, users can:

1. **Avoid creating temporary cycles:**
```naab
# BAD - leaks
fn bad() {
    let a = new Node { value: 1, next: null }
    let b = new Node { value: 2, next: a }
    a.next = b
}

# GOOD - break cycle before returning
fn good() {
    let a = new Node { value: 1, next: null }
    let b = new Node { value: 2, next: a }
    a.next = b

    # Break cycle before exiting
    a.next = null
    b.next = null
}
```

2. **Keep cycles in outer scope:**
```naab
main {
    let cycles: [Node?] = []

    let i = 0
    while i < 10 {
        let a = new Node { value: i, next: null }
        let b = new Node { value: i+1, next: a }
        a.next = b
        cycles.push(a)  # Keep in scope
        i = i + 1
    }

    gc_collect()  # Now GC can see them
}
```

3. **Manual cleanup:**
```naab
fn cleanup(node: Node?) {
    if node != null {
        node.next = null  # Break cycle
    }
}
```

## Conclusion

We've discovered a **significant limitation** in the current GC design: it only tracks values reachable from environments, which means cycles that go completely out of scope will leak.

**For Phase 3.2 completion:**
- Document this limitation
- Provide user guidance
- Mark as future enhancement
- GC is still valuable for many use cases

**For future work:**
- Implement global value tracking
- Achieve true tracing GC
- Eliminate this class of leaks

This is an honest assessment of the current implementation's capabilities and limitations.
