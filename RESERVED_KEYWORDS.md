# NAAb Reserved Keywords

**Last Updated:** 2026-01-25

## Full List of Reserved Keywords

These identifiers cannot be used as variable names, function names, or struct/enum names:

### Control Flow
- `if`
- `else`
- `for`
- `in`
- `while`
- `break`
- `continue`
- `match`
- `return`

### Error Handling
- `try`
- `catch`
- `throw`
- `finally`

### Type System
- `struct`
- `class`
- `enum`
- `new`
- `ref`

### Functions
- `function` (alias: `fn`)
- `async`
- `await`
- `method`

### Modules
- `use`
- `as`
- `module`
- `export`
- `import`

### Variables
- `let`
- `const`

### Special
- `config` ⚠️ **Common variable name - avoid!**
- `main`
- `init`
- `null`
- `true`
- `false`

## Common Pitfalls

### ⚠️ `config` is Reserved
```naab
// ❌ ERROR: config is a reserved keyword
let config = { ... }

// ✅ CORRECT: Use a different name
let configuration = { ... }
let app_config = { ... }
let settings = { ... }
```

### ⚠️ `data` is NOT Reserved (Safe to Use)
```naab
// ✅ OK: data is not reserved
let data = [1, 2, 3]
```

### ⚠️ `result` is NOT Reserved (Safe to Use)
```naab
// ✅ OK: result is not reserved
let result = compute()
```

## Case Sensitivity

Keywords are **case-sensitive**:
```naab
let Config = { ... }  // ✅ OK (uppercase C)
let config = { ... }  // ❌ ERROR (lowercase c is reserved)
```

## Future Reserved Words

These may become reserved in future versions:
- `async` (currently reserved but not fully implemented)
- `await` (currently reserved but not fully implemented)
- `mut` (not currently reserved, but may be added)

## See Also

- Language Specification (TODO)
- Error Messages Reference (TODO)
