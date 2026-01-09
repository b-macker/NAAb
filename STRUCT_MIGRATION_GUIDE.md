# NAAb Struct Implementation Migration Guide

## Overview
This guide covers the migration path for NAAb code when struct support is added.

## Syntax Changes

### Old Syntax (Pre-Struct)
```naab
// Map literal
let point = { x: 10, y: 20 }
```

### New Syntax (Post-Struct)
```naab
// Map literal (unchanged)
let map = { x: 10, y: 20 }

// Struct declaration
struct Point {
    x: INT;
    y: INT;
}

// Struct literal (requires 'new' keyword)
let point = new Point { x: 10, y: 20 }
```

## Breaking Changes

### 1. Struct Literals Require 'new' Keyword
**Why**: Disambiguates struct literals from map literals
**Migration**: Add `new` keyword before type name in struct instantiations

**Before**:
```naab
struct ConfigManager { port: INT; }
let cm = ConfigManager { port: 8080 }  // ❌ Will fail
```

**After**:
```naab
struct ConfigManager { port: INT; }
let cm = new ConfigManager { port: 8080 }  // ✅ Correct
```

### 2. Struct Field Declarations
Fields must have explicit types with semicolons

```naab
struct Data {
    value: INT;          // ✅ Correct
    name: STRING;        // ✅ Correct
    items: array<INT>;   // ✅ Correct
}
```

## Migration Steps for Existing .naab Files

### For config_manager.naab:
1. Struct declarations already correct (no changes needed)
2. Update all struct instantiations to use `new` keyword
3. Verify field types match struct definitions

### For unified_registry.naab:
1. Same as config_manager.naab
2. Check map vs struct usage

## Compatibility Notes

- Map literals (`{ key: value }`) remain unchanged
- Existing code without structs continues to work
- No changes to arrays, functions, or other language features

## Testing Your Migration

```bash
# Parse check
~/.naab/language/build/naab-lang parse your_file.naab

# Full run
~/.naab/language/build/naab-lang run your_file.naab
```

## Rollback Plan

If issues occur:
1. Revert to pre-struct version: `git checkout <pre-struct-commit>`
2. File list backed up in `tests_baseline_pre_struct/`
