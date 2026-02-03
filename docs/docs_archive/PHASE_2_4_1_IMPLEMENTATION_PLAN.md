# Phase 2.4.1: Generics Monomorphization - Implementation Plan

## Current State

**Parser:** ✅ Complete
- Generic declarations: `struct Box<T>`, `function identity<T>(x: T) -> T`
- Type parameters in function/struct definitions
- Type arguments in type annotations: `let x: Box<int>`

**Interpreter:** ❌ Not implemented
- No monomorphization
- No type substitution
- Generic functions/structs cannot be instantiated

## Implementation Strategy

### Phase 1: Type Inference for Monomorphization (3-4 days)

**Approach:** Instead of requiring explicit type arguments (`Box<int> { ... }`), infer types from usage.

**Example:**
```naab
// Declaration
struct Box<T> { value: T }

// Usage - infer T = int from value type
let box = Box { value: 42 }  // Creates Box<int>
```

**Implementation:**
1. When creating struct instance, check if struct has type parameters
2. Infer type arguments from field initializer types
3. Create specialized instance with substituted types

### Phase 2: Monomorphization Engine (2-3 days)

**Core Components:**

1. **Type Substitution**
   - Map: `{T -> int, U -> string}`
   - Substitute throughout function body or struct fields

2. **Instance Cache**
   - Track specialized versions: `Box<int>`, `Box<string>`
   - Reuse existing specializations

3. **Specialized Code Generation**
   - Create new FunctionValue/StructDef with concrete types
   - Store in global registry with mangled names

### Phase 3: Runtime Integration (1-2 days)

**Modifications:**

1. **Struct Creation** (`visit(StructLiteralExpr&)`)
   - Check if struct is generic
   - Infer type arguments
   - Monomorphize and create instance

2. **Function Calls** (`visit(CallExpr&)`)
   - Check if function is generic
   - Infer type arguments from parameter types
   - Monomorphize and execute

## Detailed Implementation

### Step 1: Add Monomorphization Infrastructure

**File:** `include/naab/interpreter.h`

```cpp
// Monomorphization cache
struct MonomorphizedInstance {
    std::string mangled_name;
    std::map<std::string, ast::Type> type_bindings;
};

class Interpreter {
    // ...

    // Monomorphization support
    std::map<std::string, std::vector<MonomorphizedInstance>> monomorphized_funcs_;
    std::map<std::string, std::vector<MonomorphizedInstance>> monomorphized_structs_;

    // Type inference and substitution
    std::map<std::string, ast::Type> inferTypeArguments(
        const std::vector<std::string>& type_params,
        const std::vector<ast::Type>& expected_types,
        const std::vector<std::shared_ptr<Value>>& actual_values
    );

    ast::Type substituteType(
        const ast::Type& type,
        const std::map<std::string, ast::Type>& bindings
    );

    std::string mangleName(
        const std::string& base_name,
        const std::vector<ast::Type>& type_args
    );
};
```

### Step 2: Implement Type Substitution

**File:** `src/interpreter/interpreter.cpp`

```cpp
ast::Type Interpreter::substituteType(
    const ast::Type& type,
    const std::map<std::string, ast::Type>& bindings
) {
    // If this is a type parameter, substitute it
    if (type.kind == ast::TypeKind::TypeParameter) {
        auto it = bindings.find(type.type_parameter_name);
        if (it != bindings.end()) {
            return it->second;
        }
        return type;  // Unbound parameter
    }

    // Recursively substitute in composite types
    if (type.kind == ast::TypeKind::List && type.element_type) {
        ast::Type result = type;
        result.element_type = std::make_shared<ast::Type>(
            substituteType(*type.element_type, bindings)
        );
        return result;
    }

    // For struct types with type arguments, substitute each argument
    if (type.kind == ast::TypeKind::Struct && !type.type_arguments.empty()) {
        ast::Type result = type;
        result.type_arguments.clear();
        for (const auto& arg : type.type_arguments) {
            result.type_arguments.push_back(substituteType(arg, bindings));
        }
        return result;
    }

    return type;
}
```

### Step 3: Infer Type Arguments from Values

```cpp
std::map<std::string, ast::Type> Interpreter::inferTypeArguments(
    const std::vector<std::string>& type_params,
    const std::vector<ast::Type>& expected_types,
    const std::vector<std::shared_ptr<Value>>& actual_values
) {
    std::map<std::string, ast::Type> bindings;

    // Match expected types (with type parameters) against actual value types
    for (size_t i = 0; i < expected_types.size() && i < actual_values.size(); i++) {
        const auto& expected = expected_types[i];
        const auto& actual_val = actual_values[i];

        // If expected type is a type parameter, bind it
        if (expected.kind == ast::TypeKind::TypeParameter) {
            ast::Type inferred = inferValueType(actual_val);
            bindings[expected.type_parameter_name] = inferred;
        }
    }

    return bindings;
}

ast::Type Interpreter::inferValueType(const std::shared_ptr<Value>& val) {
    if (std::holds_alternative<int>(val->data)) {
        return ast::Type::makeInt();
    } else if (std::holds_alternative<double>(val->data)) {
        return ast::Type::makeFloat();
    } else if (std::holds_alternative<std::string>(val->data)) {
        return ast::Type::makeString();
    } else if (std::holds_alternative<bool>(val->data)) {
        return ast::Type::makeBool();
    }
    // ... handle other types
    return ast::Type::makeAny();
}
```

### Step 4: Monomorphize Struct Instances

**Modify:** `void Interpreter::visit(ast::StructLiteralExpr& node)`

```cpp
void Interpreter::visit(ast::StructLiteralExpr& node) {
    auto struct_def = runtime::StructRegistry::instance().getStruct(node.getStructName());
    if (!struct_def) {
        throw std::runtime_error("Undefined struct: " + node.getStructName());
    }

    // Check if struct is generic
    if (!struct_def->type_parameters.empty()) {
        // Infer type arguments from field values
        std::map<std::string, ast::Type> type_bindings;

        for (const auto& [field_name, init_expr] : node.getFieldInits()) {
            auto field_value = eval(*init_expr);

            // Find corresponding field in struct definition
            for (const auto& field : struct_def->fields) {
                if (field.name == field_name) {
                    // If field type is a type parameter, infer it
                    if (field.type.kind == ast::TypeKind::TypeParameter) {
                        ast::Type inferred = inferValueType(field_value);
                        type_bindings[field.type.type_parameter_name] = inferred;
                    }
                    break;
                }
            }
        }

        // Create specialized version
        std::string mangled = mangleName(node.getStructName(), type_bindings);

        // Check cache
        // If not cached, create specialized struct definition
        // Use the specialized definition to create instance
    }

    // Rest of existing implementation...
}
```

### Step 5: Monomorphize Function Calls

**Modify:** `void Interpreter::visit(ast::CallExpr& node)`

Similar approach for generic functions.

## Testing Strategy

### Test 1: Simple Generic Struct
```naab
struct Box<T> { value: T }
let int_box = Box { value: 42 }
print(int_box.value)  // Should print 42
```

### Test 2: Generic Function
```naab
function identity<T>(x: T) -> T { return x }
let result = identity(42)
print(result)  // Should print 42
```

### Test 3: Multiple Type Parameters
```naab
struct Pair<T, U> {
    first: T
    second: U
}
let pair = Pair { first: 1, second: "hello" }
print(pair.first)  // 1
print(pair.second)  // "hello"
```

## Success Criteria

- [x] Type inference from values works
- [x] Monomorphization creates specialized instances
- [x] Cache prevents duplicate specializations
- [x] Simple generics work (Box<T>, identity<T>)
- [x] Multiple type parameters work (Pair<T,U>)
- [x] Nested generics work (Container<List<int>>)

## Timeline

- **Day 1-2:** Type substitution and inference infrastructure
- **Day 3:** Struct monomorphization
- **Day 4:** Function monomorphization
- **Day 5:** Testing and edge cases

**Total:** 4-5 days

## Future Enhancements

After basic monomorphization works:
1. Explicit type arguments: `Box<int> { ... }` (requires parser changes)
2. Type constraints: `function sort<T: Comparable>(items: List<T>)`
3. Default type parameters: `struct Box<T = int>`
4. Better error messages for type mismatches
