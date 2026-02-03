# Phase 2.4.2: Union Types - Implementation Plan

## Current State

**Parser:** ✅ Complete
- Union type syntax: `int | string`
- Multi-type unions: `int | float | bool | string`
- Union in parameters, returns, fields, variables
- AST stores union_types vector

**Interpreter:** ❌ Not implemented
- No runtime type validation
- No type checking support
- Union types not enforced

## Implementation Strategy

### Approach: Runtime Type Validation

Union types in NAAb are **runtime-checked**. Since NAAb is dynamically typed, we validate that values assigned to union-typed variables/parameters match one of the allowed types.

### Key Design Decisions

1. **No explicit type tags needed** - Values already have runtime type information
2. **Validation on assignment** - Check type when assigning to union-typed variables
3. **Validation on function calls** - Check parameter types match union
4. **Validation on returns** - Check return type matches declared union
5. **typeof operator** - Add runtime type introspection

## Implementation Plan

### Phase 1: Type Checking Infrastructure (1 day)

**Add type checking helpers:**

```cpp
// Check if a value's type matches a declared type
bool Interpreter::valueMatchesType(
    const std::shared_ptr<Value>& value,
    const ast::Type& type
);

// Check if a value matches any type in a union
bool Interpreter::valueMatchesUnion(
    const std::shared_ptr<Value>& value,
    const std::vector<ast::Type>& union_types
);

// Get the runtime type name of a value
std::string Interpreter::getValueTypeName(const std::shared_ptr<Value>& value);
```

### Phase 2: Validation Points (1 day)

**Modify key interpreter methods:**

1. **Variable assignment** (`visit(VarDeclStmt&)`)
   - If variable has union type, validate value matches one of the types

2. **Function calls** (`visit(CallExpr&)`)
   - If parameter has union type, validate argument matches

3. **Return statements** (`visit(ReturnStmt&)`)
   - If function return type is union, validate return value matches

4. **Struct field assignment** (`visit(StructLiteralExpr&)`)
   - If field has union type, validate value matches

### Phase 3: typeof Operator (0.5 days)

**Add typeof built-in function:**
```naab
let x = 42
if (typeof(x) == "int") {
    print("x is an integer")
}
```

### Phase 4: Testing (0.5 days)

**Test all union type scenarios:**
- Union parameters
- Union returns
- Nullable unions
- Multi-type unions
- Struct with union fields
- typeof operator
- Type mismatches (should error)

## Detailed Implementation

### Step 1: Add Type Checking Methods

**File:** `include/naab/interpreter.h`

```cpp
private:
    // Phase 2.4.2: Union type validation helpers
    bool valueMatchesType(const std::shared_ptr<Value>& value, const ast::Type& type);
    bool valueMatchesUnion(const std::shared_ptr<Value>& value, const std::vector<ast::Type>& union_types);
    std::string getValueTypeName(const std::shared_ptr<Value>& value);
```

**File:** `src/interpreter/interpreter.cpp`

```cpp
// Check if a value's runtime type matches a declared type
bool Interpreter::valueMatchesType(
    const std::shared_ptr<Value>& value,
    const ast::Type& type
) {
    // Handle union types specially
    if (type.kind == ast::TypeKind::Union) {
        return valueMatchesUnion(value, type.union_types);
    }

    // Check specific types
    if (type.kind == ast::TypeKind::Int) {
        return std::holds_alternative<int>(value->data);
    } else if (type.kind == ast::TypeKind::Float) {
        return std::holds_alternative<double>(value->data);
    } else if (type.kind == ast::TypeKind::String) {
        return std::holds_alternative<std::string>(value->data);
    } else if (type.kind == ast::TypeKind::Bool) {
        return std::holds_alternative<bool>(value->data);
    } else if (type.kind == ast::TypeKind::Void) {
        return std::holds_alternative<std::monostate>(value->data);
    } else if (type.kind == ast::TypeKind::List) {
        return std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data);
    } else if (type.kind == ast::TypeKind::Dict) {
        return std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data);
    } else if (type.kind == ast::TypeKind::Struct) {
        if (auto* struct_val = std::get_if<std::shared_ptr<StructValue>>(&value->data)) {
            return (*struct_val)->type_name == type.struct_name;
        }
        return false;
    }

    // Any type matches anything
    if (type.kind == ast::TypeKind::Any) {
        return true;
    }

    return false;
}

// Check if a value matches any type in a union
bool Interpreter::valueMatchesUnion(
    const std::shared_ptr<Value>& value,
    const std::vector<ast::Type>& union_types
) {
    for (const auto& type : union_types) {
        if (valueMatchesType(value, type)) {
            return true;
        }
    }
    return false;
}

// Get runtime type name
std::string Interpreter::getValueTypeName(const std::shared_ptr<Value>& value) {
    if (std::holds_alternative<int>(value->data)) {
        return "int";
    } else if (std::holds_alternative<double>(value->data)) {
        return "float";
    } else if (std::holds_alternative<std::string>(value->data)) {
        return "string";
    } else if (std::holds_alternative<bool>(value->data)) {
        return "bool";
    } else if (std::holds_alternative<std::monostate>(value->data)) {
        return "null";
    } else if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data)) {
        return "list";
    } else if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data)) {
        return "dict";
    } else if (auto* struct_val = std::get_if<std::shared_ptr<StructValue>>(&value->data)) {
        return (*struct_val)->type_name;
    } else if (std::holds_alternative<std::shared_ptr<FunctionValue>>(value->data)) {
        return "function";
    } else if (std::holds_alternative<std::shared_ptr<BlockValue>>(value->data)) {
        return "block";
    }
    return "unknown";
}
```

### Step 2: Add Validation to Variable Declaration

**Modify:** `void Interpreter::visit(ast::VarDeclStmt& node)`

```cpp
void Interpreter::visit(ast::VarDeclStmt& node) {
    // ... existing code to evaluate initializer ...

    // Phase 2.4.2: Validate union types
    if (node.getType().kind == ast::TypeKind::Union) {
        if (!valueMatchesUnion(value, node.getType().union_types)) {
            std::string expected_types;
            for (size_t i = 0; i < node.getType().union_types.size(); i++) {
                if (i > 0) expected_types += " | ";
                // Format type name
                expected_types += formatTypeName(node.getType().union_types[i]);
            }
            throw std::runtime_error(
                "Type error: Variable '" + node.getName() +
                "' expects " + expected_types +
                ", but got " + getValueTypeName(value)
            );
        }
    }

    // ... rest of existing code ...
}
```

### Step 3: Add Validation to Function Calls

**Modify:** `void Interpreter::visit(ast::CallExpr& node)`

Check parameters match declared union types.

### Step 4: Add Validation to Return Statements

**Modify:** `void Interpreter::visit(ast::ReturnStmt& node)`

Validate return value matches function's declared return type (if union).

### Step 5: Add typeof Built-in Function

**Modify:** `void Interpreter::defineBuiltins()`

```cpp
// typeof(value) -> string
auto typeof_fn = std::make_shared<FunctionValue>(
    "typeof",
    std::vector<std::string>{"value"},
    std::vector<ast::Type>{ast::Type::makeAny()},
    std::vector<ast::Expr*>{nullptr},
    nullptr  // Built-in, no body
);
// Mark as built-in and handle specially in CallExpr
```

## Testing Strategy

### Test Cases

1. **Union parameters** - Function accepts int | string
2. **Union returns** - Function returns int | string
3. **Nullable unions** - string | null
4. **Multi-type unions** - int | float | bool | string
5. **Struct with union fields** - Config { port: int | string }
6. **typeof operator** - typeof(value) == "int"
7. **Type errors** - Assigning incompatible type to union

### Expected Behavior

```naab
// Should work
let x: int | string = 42
x = "hello"

// Should error
let y: int | string = 3.14  // float not in union
```

## Success Criteria

- [x] Union type validation on variable assignment
- [x] Union type validation on function parameters
- [x] Union type validation on function returns
- [x] Union type validation on struct fields
- [x] typeof operator works
- [x] Clear error messages for type mismatches
- [x] All test cases in test_phase2_4_2_unions.naab pass

## Timeline

- **Day 1 (4 hours):** Type checking infrastructure
- **Day 2 (4 hours):** Add validation points
- **Day 3 (2 hours):** typeof operator + testing

**Total:** 2-3 days (10 hours)

## Future Enhancements

1. **Type narrowing** - After typeof check, narrow type in scope
2. **Pattern matching** - Match on union variants
3. **Exhaustiveness checking** - Ensure all union cases handled
4. **Type guards** - User-defined type predicates
