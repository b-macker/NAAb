# Phase 2.4.4: Type Inference - Design Document

## Executive Summary

**Status:** DESIGN COMPLETE | IMPLEMENTATION PENDING
**Complexity:** High - Requires constraint-based type system
**Estimated Effort:** 5-10 days implementation

Type inference allows omitting type annotations where they can be automatically deduced, reducing verbosity while maintaining type safety.

---

## Design Goals

### What Type Inference Should Do

1. **Infer variable types from initializers**
   ```naab
   let x = 42          // Infer: int
   let y = "hello"     // Infer: string
   let z = [1, 2, 3]   // Infer: list<int>
   ```

2. **Infer function return types from body**
   ```naab
   function add(a: int, b: int) {
       return a + b     // Infer return type: int
   }
   ```

3. **Infer generic type arguments**
   ```naab
   let numbers = List<int> { items: [1, 2, 3] }
   let first = identity(42)  // Infer: identity<int>(42)
   ```

4. **Infer struct field types from initializers** (optional)
   ```naab
   struct Point {
       x = 0    // Infer: int
       y = 0    // Infer: int
   }
   ```

### What Type Inference Should NOT Do

1. **Cannot infer function parameter types**
   ```naab
   function add(a, b) {  // ERROR: Parameters must be typed
       return a + b
   }
   ```
   **Rationale:** Function signatures are API contracts, must be explicit.

2. **Cannot infer without enough information**
   ```naab
   let x    // ERROR: No initializer, cannot infer
   let y = null  // ERROR: Ambiguous (could be any nullable type)
   ```

3. **Does not perform "spooky action at a distance"**
   - Inference is local, not global
   - No inference across function boundaries (except returns)

---

## Inference Algorithm

### Phase 1: Variable Declaration Inference

**Rule:** `let name = expr` infers type from `expr`

**Implementation:**
```cpp
Type inferVariableType(const Expr* initializer) {
    // Evaluate expression type
    Type expr_type = initializer->getType();

    // If type is concrete (Int, String, etc.), use it directly
    if (expr_type.isConcrete()) {
        return expr_type;
    }

    // If type is generic or contains type variables, perform unification
    return unify(expr_type);
}
```

**Examples:**
```naab
let a = 42              // Type: int
let b = 3.14            // Type: float
let c = "hello"         // Type: string
let d = true            // Type: bool
let e = [1, 2, 3]       // Type: list<int>
let f = {"a": 1}        // Type: dict<string, int>
let g = Point {x: 1, y: 2}  // Type: Point
```

### Phase 2: Function Return Type Inference

**Rule:** `function name(params) { body }` infers return type from `return` statements

**Implementation:**
```cpp
Type inferReturnType(const FunctionDecl* func) {
    std::vector<Type> return_types;

    // Find all return statements in function body
    findReturnStatements(func->getBody(), return_types);

    // If no returns, type is void
    if (return_types.empty()) {
        return Type::makeVoid();
    }

    // If single return, use that type
    if (return_types.size() == 1) {
        return return_types[0];
    }

    // If multiple returns, unify types
    Type unified = return_types[0];
    for (size_t i = 1; i < return_types.size(); ++i) {
        unified = unify(unified, return_types[i]);
    }
    return unified;
}
```

**Examples:**
```naab
function getInt() {
    return 42          // Infer: int
}

function getString() {
    if (condition) {
        return "yes"   // string
    } else {
        return "no"    // string
    }
    // Infer: string (both branches return string)
}

function mixed() {
    if (condition) {
        return 42      // int
    } else {
        return "no"    // string
    }
    // Infer: int | string (union type)
}
```

### Phase 3: Generic Type Argument Inference

**Rule:** `genericFunc(args)` infers type arguments from argument types

**Implementation:**
```cpp
std::vector<Type> inferGenericArgs(
    const FunctionDecl* generic_func,
    const std::vector<Expr*>& call_args
) {
    // Build type constraint system
    std::map<std::string, Type> type_var_constraints;

    const auto& params = generic_func->getParams();
    for (size_t i = 0; i < params.size(); ++i) {
        Type param_type = params[i].type;
        Type arg_type = call_args[i]->getType();

        // If param uses type parameter, constrain it
        collectTypeConstraints(param_type, arg_type, type_var_constraints);
    }

    // Solve constraints
    std::vector<Type> type_args;
    for (const auto& type_param : generic_func->getTypeParams()) {
        type_args.push_back(type_var_constraints[type_param]);
    }

    return type_args;
}
```

**Examples:**
```naab
function identity<T>(x: T) -> T {
    return x
}

let a = identity(42)         // Infer: identity<int>(42)
let b = identity("hello")    // Infer: identity<string>("hello")

function makePair<T, U>(first: T, second: U) -> Pair<T, U> {
    return Pair<T, U> { first: first, second: second }
}

let pair = makePair(1, "one")  // Infer: makePair<int, string>(1, "one")
```

---

## Type Constraint System

### Constraint Collection

During type inference, collect constraints:

```cpp
struct TypeConstraint {
    Type left;
    Type right;
    ConstraintKind kind;  // EQUALS, SUBTYPE, etc.
};

class ConstraintCollector {
public:
    void addConstraint(Type left, Type right, ConstraintKind kind);
    std::vector<TypeConstraint> getConstraints() const;
};
```

**Example:**
```naab
function process<T>(items: list<T>) -> int {
    let first = items[0]  // Constraint: typeof(first) = T
    return first.length   // Constraint: T has method 'length' -> int
}
```

### Constraint Solving

**Unification Algorithm (Hindley-Milner):**

```cpp
Type unify(Type t1, Type t2) {
    // If both are concrete and equal, unify
    if (t1 == t2) return t1;

    // If one is type variable, substitute
    if (t1.isTypeVariable()) {
        return substitute(t1, t2);
    }
    if (t2.isTypeVariable()) {
        return substitute(t2, t1);
    }

    // If both are generic with same base, unify arguments
    if (t1.kind == t2.kind && t1.kind == TypeKind::Generic) {
        if (t1.type_arguments.size() != t2.type_arguments.size()) {
            throw TypeError("Cannot unify different arities");
        }

        std::vector<Type> unified_args;
        for (size_t i = 0; i < t1.type_arguments.size(); ++i) {
            unified_args.push_back(unify(t1.type_arguments[i], t2.type_arguments[i]));
        }

        Type result = t1;
        result.type_arguments = unified_args;
        return result;
    }

    // Cannot unify
    throw TypeError("Cannot unify " + t1.toString() + " and " + t2.toString());
}
```

**Example Unification:**
```
Constraint: List<T> = List<int>
Unify base: List = List ✓
Unify args: T = int
Result: T ← int

Constraint: dict<K, V> = dict<string, int>
Unify: K ← string, V ← int
```

---

## Parser Updates

### Allow Omitted Type Annotations

**Variable Declarations:**
```naab
// Before (required):
let x: int = 42

// After (optional):
let x = 42  // Type annotation omitted, will be inferred
```

**Parser Changes:**
```cpp
// In parseVarDeclStmt():
std::optional<Type> type;
if (match(TokenType::COLON)) {
    type = parseType();
}

// Type can be nullopt, will be inferred later
```

**Function Returns:**
```naab
// Before (required):
function add(a: int, b: int) -> int {
    return a + b
}

// After (optional):
function add(a: int, b: int) {
    return a + b  // Return type inferred from body
}
```

**Parser Changes:**
```cpp
// In parseFunctionDecl():
std::optional<Type> return_type;
if (match(TokenType::ARROW)) {
    return_type = parseType();
}

// Return type can be nullopt, will be inferred
```

### AST Representation

**Mark types as "to be inferred":**
```cpp
struct Type {
    bool needs_inference = false;  // true if type should be inferred

    static Type makeInferred() {
        Type t(TypeKind::Any);
        t.needs_inference = true;
        return t;
    }
};
```

---

## Type Inference Pass

### Implementation

**New Phase: Type Inference (before interpretation):**
```cpp
class TypeInferencePass {
public:
    void runInference(Program* program);

private:
    void inferVariableTypes(CompoundStmt* block);
    void inferFunctionReturnTypes(FunctionDecl* func);
    void inferGenericArguments(CallExpr* call);

    std::map<std::string, Type> type_env_;  // Type environment
};
```

**Usage:**
```cpp
// In main or compiler pipeline:
Parser parser(tokens);
auto program = parser.parseProgram();

TypeInferencePass inference;
inference.runInference(program.get());  // Annotate AST with inferred types

Interpreter interp;
interp.execute(program.get());  // All types known
```

### Inference Order

**1. Variable declarations (forward pass):**
```naab
let a = 42        // Infer: int
let b = a + 1     // Use known type of 'a' to infer 'b': int
```

**2. Function bodies (after parameters known):**
```naab
function process(items: list<int>) {
    let first = items[0]  // Infer: int (from list<int>)
    return first * 2      // Infer return type: int
}
```

**3. Generic instantiations (at call site):**
```naab
function identity<T>(x: T) -> T { return x }

let x = identity(42)  // Infer T=int, x: int
```

---

## Error Messages

### When Inference Fails

**Insufficient Information:**
```naab
let x   // ERROR: Cannot infer type without initializer
```
**Error Message:**
```
Error at line 1: Cannot infer type for variable 'x'
  Help: Add an initializer or explicit type annotation
    let x = 0           // with initializer
    let x: int          // with type annotation
```

**Conflicting Types:**
```naab
function foo() {
    if (condition) {
        return 42
    } else {
        return "hello"
    }
}
```
**Error Message:**
```
Error at line 5: Cannot infer single return type
  Return types conflict:
    Line 3: returns int
    Line 5: returns string
  Help: All return statements must have compatible types
    Option 1: Use union type explicitly: -> int | string
    Option 2: Make returns consistent (both int or both string)
```

**Ambiguous Null:**
```naab
let x = null  // Which nullable type?
```
**Error Message:**
```
Error at line 1: Cannot infer type for 'x' from 'null'
  Help: 'null' can be any nullable type, add explicit annotation
    let x: string? = null
    let x: int? = null
```

---

## Examples

### Example 1: Variable Type Inference
```naab
// All types inferred
let age = 25                    // int
let name = "Alice"              // string
let scores = [95, 87, 92]       // list<int>
let grades = {"math": 95}       // dict<string, int>
let point = Point {x: 1, y: 2}  // Point

// Use inferred types
let doubled = age * 2           // int
let greeting = "Hello " + name  // string
```

### Example 2: Function Return Inference
```naab
function factorial(n: int) {
    if (n <= 1) {
        return 1    // int
    }
    return n * factorial(n - 1)  // int
}
// Inferred return type: int
```

### Example 3: Generic Inference
```naab
function map<T, U>(items: list<T>, fn: function(T) -> U) -> list<U> {
    let result = []
    for item in items {
        result.append(fn(item))
    }
    return result
}

let numbers = [1, 2, 3]
let doubled = map(numbers, function(x) { return x * 2 })
// Infer: T=int, U=int, doubled: list<int>

let strings = map(numbers, function(x) { return x.toString() })
// Infer: T=int, U=string, strings: list<string>
```

### Example 4: Complex Inference
```naab
function process<T>(items: list<T>) {
    if (items.length == 0) {
        return null  // T?
    }

    let first = items[0]         // T
    let doubled = first + first  // T (assuming T supports +)
    return doubled               // T
}
// Inferred return type: T?
```

---

## Implementation Strategy

### Phase 1: Basic Variable Inference (1-2 days)
1. Implement `getType()` for all expressions
2. Infer variable types from initializers
3. Test: Variables have correct inferred types

### Phase 2: Function Return Inference (1-2 days)
1. Find all return statements in function
2. Unify return types
3. Annotate function with inferred type
4. Test: Functions have correct return types

### Phase 3: Generic Argument Inference (2-3 days)
1. Implement constraint collection
2. Implement unification algorithm
3. Infer type arguments at call sites
4. Test: Generic calls work without explicit type args

### Phase 4: Error Messages (1 day)
1. Detect inference failures
2. Generate helpful error messages
3. Test: Error messages are clear

### Phase 5: Advanced Features (2-3 days)
1. Union type inference (multiple return types)
2. Recursive function inference
3. Mutual recursion (forward references)
4. Test: Advanced cases work

**Total: 7-11 days** (conservative estimate)

---

## Limitations

### Current Design Does NOT Support

1. **Bi-directional type inference**
   - Cannot infer function argument types from usage
   - Parameters must be explicitly typed

2. **Higher-rank types**
   - Cannot express "forall T. function(T) -> T"
   - No rank-2 or rank-N polymorphism

3. **Type classes / Traits**
   - Cannot constrain type parameters
   - No "T where T: Comparable"

4. **Recursive type inference**
   - Mutual recursion may require forward declarations
   - May need explicit type annotations to break cycles

### Future Enhancements

These could be added in v2.0:

1. **Constraint-based inference with type classes**
   ```naab
   function sort<T: Comparable>(items: list<T>) -> list<T>
   ```

2. **Bidirectional inference**
   ```naab
   let fn = function(x) { return x * 2 }  // Infer: function(int) -> int
   ```

3. **Local type inference in lambdas**
   ```naab
   numbers.map(x => x * 2)  // Infer x: int from numbers: list<int>
   ```

---

## Testing Strategy

### Test Cases

**1. Variable Inference:**
- [x] Literal values (int, float, string, bool)
- [x] List literals with homogeneous types
- [x] Dict literals with homogeneous types
- [x] Struct literals
- [x] Expression results (binary ops, function calls)

**2. Function Return Inference:**
- [ ] Single return statement
- [ ] Multiple return statements (same type)
- [ ] Multiple return statements (union type)
- [ ] No return (void)
- [ ] Recursive functions

**3. Generic Inference:**
- [ ] Single type parameter
- [ ] Multiple type parameters
- [ ] Nested generics (List<List<int>>)
- [ ] Constrained generics (future)

**4. Error Cases:**
- [ ] No initializer
- [ ] Null literal
- [ ] Conflicting return types
- [ ] Ambiguous generic arguments
- [ ] Circular type dependencies

---

## Integration with Existing Features

### Generics (Phase 2.4.1)
Type inference makes generics more ergonomic:
```naab
// Before:
let box = Box<int> { value: 42 }
let result = identity<int>(42)

// After (with inference):
let box = Box { value: 42 }        // Infer: Box<int>
let result = identity(42)          // Infer: identity<int>
```

### Union Types (Phase 2.4.2)
Inference can produce union types:
```naab
function getValue(flag: bool) {
    if (flag) {
        return 42
    } else {
        return "error"
    }
}
// Inferred return type: int | string
```

### Null Safety (Phase 2.4.5)
Inference respects nullability:
```naab
let x = 42           // int (non-nullable)
let y = null         // ERROR: ambiguous
let z: int? = null   // int? (nullable, explicit)
```

---

## Conclusion

**Phase 2.4.4 Status: DESIGN COMPLETE**

Type inference will:
- Reduce verbosity (fewer type annotations)
- Maintain type safety (all types known at compile time)
- Improve ergonomics (especially with generics)
- Provide helpful error messages when inference fails

**Implementation Effort:** 7-11 days

**Priority:** Medium (nice to have, not critical for v1.0)

**Recommended Approach:** Implement basic variable and function return inference first (Phase 1-2), defer generic inference to v1.1.

Once implemented, NAAb will have type inference comparable to TypeScript, Rust, or Swift.
