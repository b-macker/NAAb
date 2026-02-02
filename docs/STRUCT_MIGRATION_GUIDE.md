# NAAb Struct Migration Guide

**Version:** 1.0
**Date:** 2026-01-12
**Target Release:** NAAb 0.2.0

## Overview

This guide documents the migration path for NAAb code to support the new struct feature. The struct implementation introduces a syntax change that requires updating existing `.naab` files that use struct-like patterns.

## Breaking Changes

### Struct Literal Syntax Requires `new` Keyword

**Old Behavior (NAAb 0.1.x):**
```naab
# This was interpreted as a map literal
let p = Point { x: 10, y: 20 }
# Equivalent to: { "x": 10, "y": 20 }
```

**New Behavior (NAAb 0.2.0+):**
```naab
# Struct declaration
struct Point {
    x: INT;
    y: INT;
}

# Map literal (unchanged)
let m = { x: 10, y: 20 }

# Struct literal (NEW - requires 'new' keyword)
let p = new Point { x: 10, y: 20 }
```

### Why This Change?

The `new` keyword disambiguates between:
- **Map literals**: `{ key: value }` - anonymous key-value pairs
- **Struct literals**: `new StructName { field: value }` - typed structures with defined fields

This prevents parser ambiguity and enables proper type checking.

## Compatibility Rules

### 1. Map Literals (No Change)
```naab
# These continue to work as before:
let config = { host: "localhost", port: 8080 }
let data = { name: "Alice", age: 30 }
```

### 2. Struct Declarations (New Feature)
```naab
# NEW: Define struct types
struct Person {
    name: STRING;
    age: INT;
    email: STRING;
}
```

### 3. Struct Instantiation (Requires Migration)
```naab
# OLD (treated as map):
let user = Person { name: "Bob", age: 25 }

# NEW (must use 'new'):
let user = new Person { name: "Bob", age: 25 }
```

### 4. Struct Member Access (No Change)
```naab
# Member access syntax unchanged:
print(user.name)
print(user.age)
```

## Migration Steps

### Step 1: Identify Affected Files

Search for potential struct usage patterns:

```bash
# Find files with identifier followed by brace literals
cd ~/.naab/language
grep -r "[A-Z][a-zA-Z]* {" --include="*.naab" packages/

# Common patterns to look for:
# - ConfigManager { ... }
# - Point { ... }
# - User { ... }
```

### Step 2: Determine If It's a Struct or Map

**Indicators of struct usage:**
- Variable name starts with uppercase (e.g., `Point`, `User`, `Config`)
- Fields have specific types expected
- Used with member access like `obj.field`

**Indicators of map usage:**
- Anonymous data structure
- Dynamic keys
- Used as generic key-value store

### Step 3: Update Struct Literals

For each identified struct usage:

```naab
# BEFORE:
let point = Point { x: 10, y: 20 }

# AFTER:
let point = new Point { x: 10, y: 20 }
```

### Step 4: Verify Type Definitions Exist

If using structs, ensure they're declared:

```naab
# Add at module level (before functions/main)
struct Point {
    x: INT;
    y: INT;
}
```

### Step 5: Test

```bash
cd ~/.naab/language/build
./naab-lang run /path/to/updated/file.naab
```

## Known Affected Files

Based on the project structure, these files likely need migration:

### 1. `packages/naab-core/src/naab/core/config_manager.naab`

**Status:** Requires migration
**Struct Types:** `ConfigManager`, `PluginConfig`

**Before:**
```naab
let cm = ConfigManager {
    port: 8080,
    host: "localhost"
}
```

**After:**
```naab
struct ConfigManager {
    port: INT;
    host: STRING;
}

let cm = new ConfigManager {
    port: 8080,
    host: "localhost"
}
```

### 2. `packages/naab-core/src/naab/core/unified_registry.naab`

**Status:** Requires migration
**Struct Types:** `Registry`, `Component`

**Before:**
```naab
let reg = Registry { components: [] }
```

**After:**
```naab
struct Registry {
    components: array;
}

let reg = new Registry { components: [] }
```

### 3. User Modules in `packages/` Directory

**Action:** Each user package should be reviewed for struct patterns.

**Command to find all .naab files:**
```bash
find ~/.naab/language/packages -name "*.naab" -type f
```

## Migration Checklist

Use this checklist for each file:

- [ ] File identified for migration
- [ ] Struct declarations added at module level
- [ ] All struct literals updated with `new` keyword
- [ ] Map literals preserved (no `new` keyword)
- [ ] File compiles without errors
- [ ] Tests pass (if applicable)
- [ ] Comparative tests match (for core modules)

## Testing After Migration

### Unit Tests
```bash
cd ~/.naab/language/build
ctest --output-on-failure
```

### Integration Tests
```bash
# Test specific module
./naab-lang run packages/naab-core/src/naab/core/config_manager.naab

# Run comparative tests
cd /data/data/com.termux/files/home/NAAB_PROJECT/GEMINI
python3 run_comparative_tests.py
```

### Expected Outcomes
- ✅ All unit tests pass
- ✅ Files with structs parse successfully
- ✅ Comparative tests show MATCH results
- ✅ No parser errors about semicolons or struct syntax

## Common Migration Errors

### Error 1: Missing `new` Keyword

**Error Message:**
```
Error: Undefined variable 'Point' at line 5
```

**Cause:** Using struct name without `new` keyword

**Fix:**
```naab
# Wrong:
let p = Point { x: 1 }

# Correct:
let p = new Point { x: 1 }
```

### Error 2: Missing Struct Declaration

**Error Message:**
```
Error: Undefined struct 'Point' at line 10
```

**Cause:** Using struct literal without declaring the struct type

**Fix:**
```naab
# Add declaration before usage:
struct Point {
    x: INT;
}

let p = new Point { x: 1 }
```

### Error 3: Incorrect Field Types

**Error Message:**
```
Error: Type mismatch for field 'x': expected INT, got STRING
```

**Cause:** Initializing field with wrong type

**Fix:**
```naab
# Wrong:
let p = new Point { x: "hello" }

# Correct:
let p = new Point { x: 42 }
```

### Error 4: Missing Required Fields

**Error Message:**
```
Error: Missing required field 'y' in struct 'Point'
```

**Cause:** Not initializing all required fields

**Fix:**
```naab
struct Point {
    x: INT;
    y: INT;  # Required field
}

# Wrong:
let p = new Point { x: 1 }

# Correct:
let p = new Point { x: 1, y: 2 }
```

## Backward Compatibility

### What Still Works

- ✅ Map literals (no changes)
- ✅ Member access syntax
- ✅ All existing stdlib functions
- ✅ Cross-language FFI (maps still marshal)
- ✅ JSON serialization

### What Requires Changes

- ❌ Struct-like patterns without `new` keyword
- ⚠️ Code assuming uppercase identifiers are variables (now might be struct types)

## Rollback Strategy

If migration issues occur:

### Option 1: Revert to NAAb 0.1.x

```bash
cd ~/.naab/language
git checkout v0.1.x
cd build
cmake .. && make -j4
```

### Option 2: Use Feature Flag (Advanced)

Temporarily disable struct feature:

```cpp
// In include/naab/config.h
#define NAAB_ENABLE_STRUCT_TYPES 0  // Disable structs
```

**Note:** This breaks struct-dependent code but allows old code to run.

## Support and Resources

### Documentation
- Grammar: `docs/grammar.md`
- Struct Guide: `docs/struct_guide.md`
- API Reference: `docs/API_REFERENCE.md`

### Example Files
- Basic struct: `/usr/tmp/test_struct_working.naab`
- Advanced: `tests/unit/struct_test.cpp`

### Getting Help

If you encounter migration issues:

1. Check error message against "Common Migration Errors" section above
2. Review `docs/struct_guide.md` for syntax examples
3. Run with verbose logging: `./naab-lang --verbose run file.naab`
4. Check comparative tests: `python3 run_comparative_tests.py`

## Timeline

**Migration Deadline:** Before NAAb 0.2.0 release
**Estimated Time per File:** 10-30 minutes
**Testing Time:** 5-10 minutes per file

## Appendix: Automated Migration Script

For bulk migration, use this helper script:

```bash
#!/bin/bash
# migrate_structs.sh - Add 'new' keyword to struct literals

# Usage: ./migrate_structs.sh file.naab

if [ $# -ne 1 ]; then
    echo "Usage: $0 <file.naab>"
    exit 1
fi

FILE="$1"

# Backup original
cp "$FILE" "$FILE.backup"

# Pattern: Uppercase identifier followed by { ... }
# This is a heuristic - manual review still required
sed -i.bak 's/\([[:space:]]\)\([A-Z][a-zA-Z0-9]*\)[[:space:]]*{/\1new \2 {/g' "$FILE"

echo "Migration complete. Original backed up to $FILE.backup"
echo "⚠️  IMPORTANT: Review changes manually before committing!"
```

**Warning:** This script is a helper only. Manual review is required to ensure map literals aren't incorrectly modified.

## Conclusion

The struct feature brings type safety and better semantics to NAAb. While it requires updating existing code with the `new` keyword, the migration is straightforward and provides long-term benefits:

- ✅ Type checking for data structures
- ✅ Better IDE support (future)
- ✅ Clearer distinction between maps and typed records
- ✅ Foundation for OOP features

For questions or issues, refer to the documentation or test examples provided in the project.

---

**Document Status:** Complete and Actionable
**Last Updated:** 2026-01-12
**Maintainer:** NAAb Language Team
