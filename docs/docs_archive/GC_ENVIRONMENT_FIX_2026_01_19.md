# GC Environment Access Fix

**Date:** 2026-01-19
**Issue:** GC couldn't access environment values (showed "0 values reachable")
**Status:** ✅ FIXED

---

## Problem

When running `gc_collect()`, the GC output showed:
```
[GC] Running garbage collection...
[GC] Mark phase: 0 values reachable
[GC] Sweep phase: 0 cycles detected
```

**Root Cause:** Environment class didn't expose its values or parent chain to the GC, so the mark phase couldn't traverse the environment to find root values.

---

## Solution

### 1. Added Environment Accessors

**File:** `include/naab/interpreter.h:341-343`

```cpp
// Phase 3.2: GC support - access to values for cycle detection
const std::unordered_map<std::string, std::shared_ptr<Value>>& getValues() const { return values_; }
std::shared_ptr<Environment> getParent() const { return parent_; }
```

**Benefits:**
- GC can now access all values in environment
- GC can traverse parent environment chain
- Read-only access (const reference) - safe

### 2. Updated markFromEnvironment()

**File:** `src/interpreter/cycle_detector.cpp:30-54`

**Before:**
```cpp
// Get all variable names in this environment
auto names = env->getAllNames();

for (const auto& name : names) {
    auto value = env->get(name);
    if (value) {
        markReachable(value, visited, reachable);
    }
}

// Note: parent environment will be handled separately
```

**After:**
```cpp
// Get all values in this environment
const auto& values = env->getValues();

for (const auto& [name, value] : values) {
    if (value) {
        markReachable(value, visited, reachable);
    }
}

// Recursively process parent environment
auto parent = env->getParent();
if (parent) {
    markFromEnvironment(parent, visited, reachable);
}
```

**Improvements:**
- Direct access to values (no redundant lookup)
- Recursively traverses parent chain
- Simpler, more efficient code

### 3. Simplified detectAndCollect()

**File:** `src/interpreter/cycle_detector.cpp:119-154`

**Before:**
```cpp
// Start from root environment and mark all reachable values
std::shared_ptr<Environment> current_env = root_env;
while (current_env) {
    markFromEnvironment(current_env, visited, reachable);
    // Note: We would need to expose parent() method in Environment
    // For now, we just process the root environment
    break;  // TODO: Add parent traversal when Environment exposes parent
}
```

**After:**
```cpp
// Mark all reachable values from the environment (includes parent chain)
markFromEnvironment(root_env, visited, reachable);
```

**Result:** Much cleaner code, parent traversal now works

---

## Expected Results After Rebuild

### Before Fix:
```
[GC] Mark phase: 0 values reachable
[GC] Sweep phase: 0 cycles detected
[GC] No cycles detected
```

### After Fix:
```
[GC] Mark phase: 2+ values reachable  (a and b nodes)
[GC] Sweep phase: 0 cycles detected
[GC] No cycles detected
```

**Note:** The cycle detection shows 0 because `a` and `b` are still in scope (reachable from local variables). Cycles are only detected when values are unreachable but have refcount > 1.

To actually see cycle collection, values must:
1. Be in a cycle (a.next = b, b.next = a)
2. Be unreachable from any environment (out of scope)
3. Still have refcount > 1 (due to the cycle)

---

## Testing

After rebuild, run:
```bash
cd build && make -j4
cd ..
./build/naab-lang run examples/test_gc_with_collection.naab
```

**Expected Output:**
```
Creating two nodes...
Linking: a.next = b
Creating cycle: b.next = a

Cycle created! Both nodes have refcount=2
Node a.value = 1
Node b.value = 2

Now manually triggering garbage collection...
[GC] Running garbage collection...
[GC] Mark phase: 2 values reachable  <-- FIXED (was 0)
[GC] Sweep phase: 0 cycles detected
[GC] No cycles detected

GC completed!
```

---

## Files Modified

1. **include/naab/interpreter.h**
   - Added `getValues()` accessor (line 342)
   - Added `getParent()` accessor (line 343)

2. **src/interpreter/cycle_detector.cpp**
   - Updated `markFromEnvironment()` (lines 30-54)
   - Simplified `detectAndCollect()` (lines 119-154)

**Total Changes:** ~15 lines modified

---

## Next Steps

1. **Rebuild Project**
   ```bash
   cd build && make -j4
   ```

2. **Test gc_collect()**
   ```bash
   ./build/naab-lang run examples/test_gc_with_collection.naab
   ```

3. **Verify Mark Phase**
   - Should show 2+ values reachable (not 0)
   - Confirms GC can traverse environment

4. **Test Out-of-Scope Cycles**
   - Create test where cycle goes out of scope
   - Should see actual cycle collection

---

## Status

✅ **Code Fixed** - Environment accessors added
✅ **GC Updated** - Can now traverse environment chain
⏳ **Build Pending** - Ready for rebuild
⏳ **Testing Pending** - Ready for verification

---

**End of Fix Report**
**Status:** Ready for rebuild and testing
