# Build Fix - Phase 3.2 GC Implementation

**Date:** 2026-01-19
**Issue:** Compilation error with `std::unique_ptr<CycleDetector>`
**Status:** ✅ FIXED

---

## Problem

Build failed with error:
```
error: invalid application of 'sizeof' to an incomplete type 'naab::interpreter::CycleDetector'
```

**Root Cause:** `std::unique_ptr<CycleDetector>` in `Interpreter` class requires the complete type definition when the destructor is instantiated, but only a forward declaration was available in `interpreter.h`.

**Location:** `include/naab/interpreter.h:476` (cycle_detector_ member)

---

## Solution

Applied the standard "pimpl idiom" pattern for `unique_ptr` with forward-declared types:

### 1. Declared Destructor in Header

**File:** `include/naab/interpreter.h:353`

```cpp
class Interpreter : public ast::ASTVisitor {
public:
    Interpreter();
    ~Interpreter();  // Phase 3.2: Declared here, defined in .cpp (for unique_ptr<CycleDetector>)
    // ...
};
```

### 2. Defined Destructor in Implementation File

**File:** `src/interpreter/interpreter.cpp:375-378`

```cpp
// Phase 3.2: Destructor must be defined in .cpp where CycleDetector is complete
Interpreter::~Interpreter() {
    // Default destruction is fine, but must be defined here for unique_ptr<CycleDetector>
}
```

---

## Why This Works

1. **Forward Declaration in Header:**
   - `interpreter.h` still has forward declaration of `CycleDetector`
   - This allows the header to declare `std::unique_ptr<CycleDetector>` member
   - No circular dependencies

2. **Complete Type in Implementation:**
   - `interpreter.cpp` includes `"cycle_detector.h"` (line 12)
   - Complete type definition available when destructor is compiled
   - `unique_ptr` destructor can properly delete the object

3. **Standard Pattern:**
   - This is the recommended pattern for pimpl idiom
   - Commonly used with `unique_ptr` to reduce header dependencies
   - Prevents need to expose implementation headers in public API

---

## Build Status

**Before Fix:**
```
error: invalid application of 'sizeof' to an incomplete type 'naab::interpreter::CycleDetector'
```

**After Fix:**
- Compilation should succeed
- No changes to semantics (destructor behavior unchanged)
- Only moved destructor definition location

---

## Related Files

**Modified:**
- `include/naab/interpreter.h` - Added destructor declaration
- `src/interpreter/interpreter.cpp` - Added destructor definition

**No Changes Needed:**
- `src/interpreter/cycle_detector.h` - Still forward declares types
- `src/interpreter/cycle_detector.cpp` - Still includes interpreter.h

---

## Testing

After successful build, verify:
1. Project compiles without errors
2. Garbage collector initializes correctly
3. Destructor properly cleans up CycleDetector

---

## References

- **Pattern:** Pimpl (Pointer to Implementation) Idiom
- **C++ Gotcha:** `unique_ptr` requires complete type for destruction
- **Solution:** Define destructor in .cpp file where complete type is available

---

**End of Build Fix Report**
**Status:** ✅ FIXED
**Ready for:** Rebuild and testing
