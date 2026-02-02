# Phase 2.4.5: Null Safety by Default - Design Document

## Executive Summary

**Status:** DESIGN COMPLETE | IMPLEMENTATION PENDING
**Complexity:** Medium - Requires parser + runtime changes
**Estimated Effort:** 3-5 days implementation

Null safety by default makes types non-nullable unless explicitly marked, eliminating null pointer errors at compile time.

---

## Current Problem

**Current Semantics:**
```naab
let x: string = "hello"  // Can be null (no enforcement)
let y: string? = null    // Explicitly nullable

// Runtime error possible:
function getLength(s: string) -> int {
    return s.length  // If s is null, crash!
}
```

**Issue:** Non-nullable types aren't enforced, leading to runtime null pointer errors.

---

## Proposed Solution

### Option A: Non-Nullable by Default (Recommended) ✓

**Semantics (Kotlin/Swift/Rust model):**
```naab
// Non-nullable by default
let x: string = "hello"   // Cannot be null
x = null                  // COMPILE ERROR

// Nullable must be explicit
let y: string? = null     // Can be null
y = "hello"               // OK

// Safe access
if (y != null) {
    print(y.length)  // y is known non-null in this block
}
```

**Benefits:**
- ✅ Eliminates entire class of runtime errors
- ✅ Matches modern language design (Kotlin, Swift, TypeScript strict mode)
- ✅ Forces developers to handle null explicitly
- ✅ Better documentation (signature shows nullability)

**Drawbacks:**
- ⚠️ Breaking change (existing code must be updated)
- ⚠️ More verbose (must add `?` for nullable)
- ⚠️ Migration effort

**DECISION:** Choose this option (production-ready language need null safety)

### Option B: Keep Current (Nullable Opt-In)

**Current Semantics:**
```naab
let x: string = "hello"   // Can be null (runtime check only)
let y: string? = null     // Same as string (? is optional)
```

**Benefits:**
- ✅ No breaking changes
- ✅ Less verbose

**Drawbacks:**
- ❌ Doesn't solve null safety problem
- ❌ Runtime errors still possible
- ❌ Not production-ready

**DECISION:** Reject this option

---

## Design: Option A (Non-Nullable by Default)

### Type System Changes

**Type Representation:**
```cpp
struct Type {
    TypeKind kind;
    bool is_nullable = false;  // false = non-nullable by default

    // Factory methods
    static Type makeString() {
        return Type(TypeKind::String, "", false);  // Non-nullable
    }

    static Type makeNullableString() {
        return Type(TypeKind::String, "", true);  // Nullable
    }
};
```

**Semantics:**
```naab
// Type annotations
x: int       // Non-nullable int (cannot be null)
x: int?      // Nullable int (can be null)

x: string    // Non-nullable string
x: string?   // Nullable string

x: List<int>   // Non-nullable list of non-nullable ints
x: List<int>?  // Nullable list of non-nullable ints
x: List<int?>  // Non-nullable list of nullable ints
x: List<int?>? // Nullable list of nullable ints
```

### Parser Updates

**Parse `?` as part of type:**
```cpp
ast::Type Parser::parseType() {
    // Parse base type (int, string, struct, etc.)
    ast::Type type = parseBaseType();

    // Check for nullable marker
    if (match(lexer::TokenType::QUESTION)) {
        type.is_nullable = true;
    }

    return type;
}
```

**Examples:**
```naab
let x: int? = null         // Parse: int with is_nullable=true
let y: string? = "hello"   // Parse: string with is_nullable=true
let z: List<int>? = null   // Parse: List<int> with is_nullable=true
```

### Type Checking

**Null Assignment Check:**
```cpp
void checkAssignment(const Type& var_type, const Type& value_type) {
    // Cannot assign null to non-nullable
    if (!var_type.is_nullable && value_type.is_null()) {
        throw TypeError("Cannot assign null to non-nullable type");
    }

    // Cannot assign nullable to non-nullable without check
    if (!var_type.is_nullable && value_type.is_nullable) {
        throw TypeError("Cannot assign nullable to non-nullable without null check");
    }
}
```

**Examples:**
```naab
// Valid:
let x: int = 42              // OK
let y: int? = null           // OK
let z: int? = 42             // OK

// Invalid:
let a: int = null            // ERROR: Cannot assign null to non-nullable
let b: string = y            // ERROR: Cannot assign nullable to non-nullable
```

### Null Checks & Type Narrowing

**Explicit Null Check:**
```naab
let x: string? = getValue()

// Before check: x is string?
if (x != null) {
    // Inside block: x is string (type narrowed)
    print(x.length)  // OK: x is guaranteed non-null
}

// After block: x is string? again
```

**Implementation:**
```cpp
class TypeNarrowing {
public:
    // Enter scope where variable is known non-null
    void narrowToNonNull(const std::string& var_name, const Type& original_type);

    // Get narrowed type (or original if not narrowed)
    Type getNarrowedType(const std::string& var_name);

    // Exit scope (restore original type)
    void exitScope();

private:
    std::map<std::string, Type> narrowed_types_;
    std::vector<std::map<std::string, Type>> scope_stack_;
};
```

**Type Checker Integration:**
```cpp
void TypeChecker::visitIfStmt(IfStmt* stmt) {
    auto* condition = stmt->getCondition();

    // Check if condition is null check (x != null)
    if (auto* binary = dynamic_cast<BinaryExpr*>(condition)) {
        if (binary->getOp() == TokenType::NE) {  // !=
            auto* left = binary->getLeft();
            auto* right = binary->getRight();

            // If comparing identifier to null
            if (auto* ident = dynamic_cast<IdentifierExpr*>(left)) {
                if (isNullLiteral(right)) {
                    // Narrow type in then branch
                    Type original = getType(ident->getName());
                    Type narrowed = original;
                    narrowed.is_nullable = false;

                    narrowing_.narrowToNonNull(ident->getName(), narrowed);
                    visitStmt(stmt->getThenBranch());
                    narrowing_.exitScope();
                    return;
                }
            }
        }
    }

    // Default: no narrowing
    visitStmt(stmt->getThenBranch());
}
```

### Safe Access Operators

**Option 1: Explicit Checks (Current)**
```naab
let x: string? = getValue()

if (x != null) {
    print(x.length)
}
```

**Option 2: Optional Chaining (Future Enhancement)**
```naab
let x: string? = getValue()

// Safe access (returns null if x is null)
let len = x?.length  // Type: int?

// With default
let len = x?.length ?? 0  // Type: int
```

**Implementation (Future):**
```cpp
// Add QUESTION_DOT token (?.
let result = x?.field    // If x is null, result is null
let result = x?.method() // If x is null, result is null
```

### Null Coalescing Operator

**Syntax:** `??` (null coalescing)
```naab
let x: string? = getValue()
let y = x ?? "default"  // If x is null, use "default"
// Type of y: string (non-nullable)
```

**Implementation:**
```cpp
// Add QUESTION_QUESTION token (??
case TokenType::QUESTION_QUESTION:
    if (left->getType().is_nullable) {
        // Result is non-nullable (union of types)
        Type result = unify(left->getType().makeNonNullable(), right->getType());
        return result;
    }
    break;
```

---

## Migration Strategy

### Step 1: Deprecation Phase (v0.9)

**Add warnings for implicit nullable:**
```naab
let x: string = null  // WARNING: Implicit null assignment deprecated
                     // Use 'string?' for nullable types
```

**Provide migration tool:**
```bash
naab-migrate --add-nullable file.naab
```

**Tool adds `?` where null is assigned:**
```naab
// Before:
let x: string = null
let y: int = getValue()  // getValue can return null

// After:
let x: string? = null
let y: int? = getValue()
```

### Step 2: Enforcement Phase (v1.0)

**Make non-nullable strict:**
```naab
let x: string = null  // ERROR: Cannot assign null to non-nullable
let y: string? = null // OK: Nullable type
```

**Runtime Checks (Belt and Suspenders):**
```cpp
// In interpreter, when accessing non-nullable:
Value getValue(const std::string& name) {
    auto value = env_.get(name);

    // If type is non-nullable, enforce at runtime too
    if (!type.is_nullable && value.isNull()) {
        throw RuntimeError("Null value in non-nullable variable '" + name + "'");
    }

    return value;
}
```

### Step 3: Full Null Safety (v1.1+)

**Add advanced features:**
- Optional chaining (`?.`)
- Null coalescing (`??`)
- Null assertion (`x!` - "I know this is non-null")
- Smart casts (automatic narrowing)

---

## Examples

### Example 1: Basic Null Safety
```naab
// Non-nullable (safe)
function getLength(s: string) -> int {
    return s.length  // Safe: s cannot be null
}

let name = "Alice"
let len = getLength(name)  // OK

let nullable_name: string? = null
let len2 = getLength(nullable_name)  // COMPILE ERROR: nullable -> non-nullable
```

### Example 2: Handling Nullable
```naab
function safeGetLength(s: string?) -> int {
    if (s != null) {
        return s.length  // OK: s is narrowed to string (non-null)
    } else {
        return 0
    }
}

let name: string? = getValue()
let len = safeGetLength(name)  // OK: expects nullable
```

### Example 3: Null Coalescing
```naab
let name: string? = getValue()
let displayName = name ?? "Anonymous"  // string (non-nullable)
```

### Example 4: Generic Nullability
```naab
struct Box<T> {
    value: T  // T can be nullable or non-nullable
}

let box1 = Box<int> { value: 42 }         // Box<int> (non-nullable int)
let box2 = Box<int?> { value: null }      // Box<int?> (nullable int)
let box3: Box<int>? = null                // Nullable Box<int>
let box4: Box<int?>? = null               // Nullable Box<nullable int>
```

---

## Runtime Enforcement

### Interpreter Changes

**Null Access Check:**
```cpp
Value Interpreter::visitMemberExpr(MemberExpr* expr) {
    auto object = evaluate(expr->getObject());

    // If object is null and type is non-nullable, error
    if (object.isNull()) {
        Type object_type = expr->getObject()->getType();
        if (!object_type.is_nullable) {
            throw RuntimeError("Null pointer exception: accessing member '" +
                             expr->getMember() + "' on null object");
        } else {
            throw RuntimeError("Accessing member on null object (did you forget null check?)");
        }
    }

    return object.getMember(expr->getMember());
}
```

**Null Assignment Check:**
```cpp
void Interpreter::visitVarDeclStmt(VarDeclStmt* stmt) {
    auto value = evaluate(stmt->getValue());

    Type var_type = stmt->getType();

    // Runtime enforcement
    if (value.isNull() && !var_type.is_nullable) {
        throw RuntimeError("Cannot assign null to non-nullable variable '" +
                         stmt->getName() + "'");
    }

    env_.define(stmt->getName(), value, var_type);
}
```

---

## Error Messages

### Null Assignment Error
```naab
let x: string = null
```
**Error:**
```
Error at line 1: Cannot assign null to non-nullable type 'string'
  Help: Change type to nullable if null values are expected
    let x: string? = null
```

### Nullable to Non-Nullable Error
```naab
let x: string? = getValue()
let y: string = x  // ERROR
```
**Error:**
```
Error at line 2: Cannot assign nullable type 'string?' to non-nullable 'string'
  Help: Add null check before assignment
    if (x != null) {
        let y: string = x  // OK: x is known non-null
    }
  Or use null coalescing:
    let y: string = x ?? "default"
```

### Null Pointer Access
```naab
let x: string? = null
print(x.length)  // Runtime error (if not caught at compile time)
```
**Error:**
```
RuntimeError at line 2: Accessing member 'length' on null object
  Variable 'x' has type 'string?' which can be null
  Add null check:
    if (x != null) {
        print(x.length)
    }
```

---

## Testing Strategy

### Test Cases

**1. Type Checking:**
- [ ] Non-nullable types reject null
- [ ] Nullable types accept null
- [ ] Cannot assign nullable to non-nullable
- [ ] Type narrowing in if blocks
- [ ] Generic nullable types (Box<int?>)

**2. Runtime Enforcement:**
- [ ] Null access on non-nullable throws
- [ ] Null access on nullable (without check) throws
- [ ] Null check allows safe access

**3. Migration:**
- [ ] Migration tool adds ? where needed
- [ ] Legacy code produces warnings
- [ ] Warnings become errors in v1.0

**4. Advanced Features (Future):**
- [ ] Optional chaining (?.
- [ ] Null coalescing (??
- [ ] Null assertion (!
- [ ] Smart casts

---

## Integration with Existing Features

### With Generics (Phase 2.4.1)
```naab
struct Result<T, E> {
    value: T?      // Nullable T
    error: E?      // Nullable E
}

// Non-nullable Result of nullable int
let result: Result<int?, string> = Ok(null)
```

### With Union Types (Phase 2.4.2)
```naab
// Nullable union
let x: (int | string)? = null

// Union with null (equivalent to nullable)
let y: int | null = null  // Same as int?
```

### With Type Inference (Phase 2.4.4)
```naab
let x = 42           // Infer: int (non-nullable)
let y = null         // ERROR: Cannot infer (ambiguous)
let z: int? = null   // Explicit: int? (nullable)
```

---

## Comparison with Other Languages

| Language | Null Safety | Notes |
|----------|-------------|-------|
| Kotlin | Non-nullable by default | `String` vs `String?` |
| Swift | Non-nullable by default | `String` vs `String?` |
| Rust | No null (Option<T>) | `Option<T>` instead of null |
| TypeScript | Optional (strictNullChecks) | `string` vs `string \| null` |
| Java | No (NullPointerException) | Upcoming with Valhalla |
| Python | No (None everywhere) | mypy can check |
| **NAAb** | **Non-nullable by default** | `string` vs `string?` |

**NAAb follows Kotlin/Swift model** - modern, safe, practical.

---

## Implementation Checklist

### Parser Changes (1 day)
- [x] Parse `?` after type (already works)
- [ ] Default `is_nullable` to false
- [ ] Test: Types parsed correctly

### Type Checker (2 days)
- [ ] Implement null assignment checking
- [ ] Implement nullable-to-non-nullable checking
- [ ] Implement type narrowing in if blocks
- [ ] Test: Type errors caught

### Interpreter (1 day)
- [ ] Runtime null access checks
- [ ] Runtime null assignment checks
- [ ] Test: Runtime enforcement works

### Error Messages (0.5 days)
- [ ] Clear error messages for null violations
- [ ] Helpful suggestions (add `?`, add null check)
- [ ] Test: Error messages helpful

### Migration (1 day)
- [ ] Create migration tool
- [ ] Add deprecation warnings (v0.9)
- [ ] Document migration process
- [ ] Test: Migration tool works

**Total: 5.5 days**

---

## Conclusion

**Phase 2.4.5 Status: DESIGN COMPLETE**

Null safety by default will:
- Eliminate null pointer errors at compile time
- Force explicit handling of nullable values
- Make code more robust and maintainable
- Match modern language best practices

**Implementation Effort:** 5.5 days

**Priority:** High (critical for production readiness)

**Recommended Timeline:**
- v0.9: Add deprecation warnings
- v1.0: Enforce null safety strictly
- v1.1: Add advanced features (optional chaining, null coalescing)

Once implemented, NAAb will have null safety on par with Kotlin and Swift, making it a truly production-ready language.
