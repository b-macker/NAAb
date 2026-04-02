// NAAb Interpreter — Type System
// Type checking, inference, generics, monomorphization
// Split from interpreter.cpp for maintainability

#include "naab/interpreter.h"
#include "naab/logger.h"
#include <fmt/core.h>
#include <map>
#include <sstream>

namespace naab {
namespace interpreter {

// ============================================================================
// Phase 2.4.1: Generics/Monomorphization Implementation
// ============================================================================

// Infer the type of a runtime value
ast::Type Interpreter::inferValueType(NaabVal nval) {
    // NaabVal fast path for primitives (no heap allocation)
    if (nval.isInt()) return ast::Type::makeInt();
    if (nval.isDouble()) return ast::Type::makeFloat();
    if (nval.isBool()) return ast::Type::makeBool();
    if (nval.isNull()) return ast::Type::makeVoid();
    if (nval.isString()) return ast::Type::makeString();

    // Complex types: use NaabVal type checks directly
    if (nval.isList()) {
        const auto& list = nval.asListConst();
        if (!list.empty()) {
            ast::Type list_type(ast::TypeKind::List);
            list_type.element_type = std::make_shared<ast::Type>(inferValueType(list[0]));
            return list_type;
        }
        return ast::Type(ast::TypeKind::List);
    } else if (nval.isStructVal()) {
        const auto& struct_val = nval.asStructConst();
        return ast::Type::makeStruct(struct_val->type_name);
    }

    return ast::Type::makeAny();
}

// Infer type bindings for generic struct from field initializers
std::map<std::string, ast::Type> Interpreter::inferTypeBindings(
    const std::vector<std::string>& type_params,
    const std::vector<ast::StructField>& fields,
    const std::vector<std::pair<std::string, std::unique_ptr<ast::Expr>>>& field_inits
) {
    (void)type_params;  // Reserved for future generic type inference
    std::map<std::string, ast::Type> bindings;

    // For each field initializer, match it against the field definition
    for (const auto& [field_name, init_expr] : field_inits) {
        // Find corresponding field definition
        for (const auto& field : fields) {
            if (field.name == field_name) {
                // Check if this field's type is a type parameter
                if (field.type.kind == ast::TypeKind::TypeParameter) {
                    // Infer the type from the initializer value
                    auto init_value = eval(*init_expr);
                    ast::Type inferred = inferValueType(init_value);
                    // Use insert instead of operator[] to avoid default construction
                    bindings.insert({field.type.type_parameter_name, inferred});
                }
                break;
            }
        }
    }

    return bindings;
}

// Substitute type parameters with concrete types
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
        // Unbound parameter - return as-is
        return type;
    }

    // For list types, recursively substitute element type
    if (type.kind == ast::TypeKind::List && type.element_type) {
        ast::Type result = type;
        result.element_type = std::make_shared<ast::Type>(
            substituteType(*type.element_type, bindings)
        );
        return result;
    }

    // For dict types, recursively substitute key and value types
    if (type.kind == ast::TypeKind::Dict && type.key_value_types) {
        ast::Type result = type;
        result.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(
            substituteType(type.key_value_types->first, bindings),
            substituteType(type.key_value_types->second, bindings)
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

    // No substitution needed
    return type;
}

// Create a specialized (monomorphized) version of a generic struct
std::shared_ptr<StructDef> Interpreter::monomorphizeStruct(
    const std::shared_ptr<StructDef>& generic_def,
    const std::map<std::string, ast::Type>& type_bindings
) {
    // Create specialized field list with substituted types
    std::vector<ast::StructField> specialized_fields;
    for (const auto& field : generic_def->fields) {
        // Construct new StructField explicitly (can't copy due to unique_ptr)
        ast::StructField specialized_field{
            field.name,
            substituteType(field.type, type_bindings),
            std::nullopt  // Default value not needed at runtime
        };
        specialized_fields.push_back(std::move(specialized_field));
    }

    // Create mangled name for specialized struct (e.g., "Box_int")
    std::string mangled_name = generic_def->name;
    for (const auto& param : generic_def->type_parameters) {
        auto it = type_bindings.find(param);
        if (it != type_bindings.end()) {
            mangled_name += "_";
            if (it->second.kind == ast::TypeKind::Int) {
                mangled_name += "int";
            } else if (it->second.kind == ast::TypeKind::Float) {
                mangled_name += "float";
            } else if (it->second.kind == ast::TypeKind::String) {
                mangled_name += "string";
            } else if (it->second.kind == ast::TypeKind::Bool) {
                mangled_name += "bool";
            } else if (it->second.kind == ast::TypeKind::Struct) {
                mangled_name += it->second.struct_name;
            } else {
                mangled_name += "any";
            }
        }
    }

    // Create specialized struct definition (no type parameters)
    auto specialized_def = std::make_shared<StructDef>(
        mangled_name,
        std::move(specialized_fields),  // Move to avoid copying
        std::vector<std::string>{}  // No type parameters in specialized version
    );

    return specialized_def;
}

// ============================================================================
// Phase 2.4.2: Union Type Validation Implementation
// ============================================================================

// Check if a value's runtime type matches a declared type
bool Interpreter::valueMatchesType(
    NaabVal nval,
    const ast::Type& type
) {
    // Phase 2.4.5: If type is nullable and value is null, that's valid
    if (type.is_nullable && nval.isNull()) {
        return true;
    }

    // Handle union types specially
    if (type.kind == ast::TypeKind::Union) {
        return valueMatchesUnion(nval, type.union_types);
    }

    // NaabVal fast path for primitive types (no toLegacy needed)
    if (type.kind == ast::TypeKind::Int) {
        return nval.isInt();
    } else if (type.kind == ast::TypeKind::Float) {
        return nval.isDouble();
    } else if (type.kind == ast::TypeKind::String) {
        return nval.isString();
    } else if (type.kind == ast::TypeKind::Bool) {
        return nval.isBool();
    } else if (type.kind == ast::TypeKind::Void) {
        return nval.isNull();
    }

    // Complex types: use NaabVal type checks directly
    if (type.kind == ast::TypeKind::List) {
        return nval.isList();
    } else if (type.kind == ast::TypeKind::Dict) {
        return nval.isDict();
    } else if (type.kind == ast::TypeKind::Struct) {
        if (nval.isStructVal()) {
            const auto& sv = nval.asStructConst();
            const std::string& actual_name = sv->type_name;
            std::string expected_name = type.struct_name;

            // ISS-024 Fix: If type has module prefix, compare without it
            // e.g., if expected is "types.Config", just compare "Config"
            if (!type.module_prefix.empty()) {
                // Module-qualified type - use only the struct name part
                expected_name = type.struct_name;  // This already has module prefix stripped by parser
            }

            // Exact match
            if (actual_name == expected_name) {
                return true;
            }

            // Check if actual is a specialization of expected (e.g., Box_int is a Box)
            std::string prefix = expected_name + "_";
            if (actual_name.size() >= prefix.size() &&
                actual_name.substr(0, prefix.size()) == prefix) {
                return true;
            }

            return false;
        }
        return false;
    } else if (type.kind == ast::TypeKind::Function) {
        return nval.isFunction();
    } else if (type.kind == ast::TypeKind::Enum) {
        return nval.isInt();
    }

    // Any type matches anything
    if (type.kind == ast::TypeKind::Any) {
        return true;
    }

    return false;
}

// Check if a value matches any type in a union
bool Interpreter::valueMatchesUnion(
    NaabVal nval,
    const std::vector<ast::Type>& union_types
) {
    for (const auto& type : union_types) {
        if (valueMatchesType(nval, type)) {
            return true;
        }
    }
    return false;
}

// Get runtime type name of a value
std::string Interpreter::getValueTypeName(NaabVal nval) {
    return nval.getTypeName();
}

// Format a type name for error messages
std::string Interpreter::formatTypeName(const ast::Type& type) {
    std::string base_name;

    if (type.kind == ast::TypeKind::Int) base_name = "int";
    else if (type.kind == ast::TypeKind::Float) base_name = "float";
    else if (type.kind == ast::TypeKind::String) base_name = "string";
    else if (type.kind == ast::TypeKind::Bool) base_name = "bool";
    else if (type.kind == ast::TypeKind::Void) base_name = "null";
    else if (type.kind == ast::TypeKind::List) base_name = "array";
    else if (type.kind == ast::TypeKind::Dict) base_name = "dict";
    else if (type.kind == ast::TypeKind::Any) base_name = "any";
    else if (type.kind == ast::TypeKind::Function) base_name = "function";  // ISS-002
    else if (type.kind == ast::TypeKind::Struct) base_name = type.struct_name;
    else if (type.kind == ast::TypeKind::Enum) base_name = type.enum_name;  // Phase 4.1
    else if (type.kind == ast::TypeKind::Union) {
        std::string result;
        for (size_t i = 0; i < type.union_types.size(); i++) {
            if (i > 0) result += " | ";
            result += formatTypeName(type.union_types[i]);
        }
        base_name = result;
    }
    else {
        base_name = "unknown";
    }

    // Phase 2.4.5: Add nullable marker
    if (type.is_nullable) {
        base_name += "?";
    }

    return base_name;
}

// Phase 2.4.5: Null safety helper - check if value is null
bool Interpreter::isNull(NaabVal value) {
    return value.isNull();
}

// Phase 2.4.4: Type inference - infer ast::Type from runtime Value
ast::Type Interpreter::inferTypeFromValue(NaabVal nval) {
    // NaabVal fast path for primitives (no toLegacy needed)
    if (nval.isNull()) {
        // Null is ambiguous - could be any nullable type
        ast::Type t = ast::Type::makeAny();
        t.is_nullable = true;
        return t;
    }
    if (nval.isInt()) return ast::Type::makeInt();
    if (nval.isDouble()) return ast::Type::makeFloat();
    if (nval.isBool()) return ast::Type::makeBool();
    if (nval.isString()) return ast::Type::makeString();

    // Complex types: use NaabVal type checks directly
    if (nval.isList()) {
        const auto& list = nval.asListConst();
        ast::Type list_type(ast::TypeKind::List);
        if (!list.empty()) {
            list_type.element_type = std::make_shared<ast::Type>(inferTypeFromValue(list[0]));
        } else {
            list_type.element_type = std::make_shared<ast::Type>(ast::Type::makeAny());
        }
        return list_type;
    }
    else if (nval.isDict()) {
        const auto& dict = nval.asDictConst();
        ast::Type dict_type(ast::TypeKind::Dict);
        if (!dict.empty()) {
            auto first_entry = dict.begin();
            auto key_type = ast::Type::makeString();
            auto value_type = inferTypeFromValue(first_entry->second);
            dict_type.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(key_type, value_type);
        } else {
            auto key_type = ast::Type::makeString();
            auto value_type = ast::Type::makeAny();
            dict_type.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(key_type, value_type);
        }
        return dict_type;
    }
    else if (nval.isStructVal()) {
        return ast::Type(ast::TypeKind::Struct, nval.asStructConst()->type_name);
    }
    else if (nval.isFunction()) {
        return ast::Type::makeFunction();
    }
    else if (nval.isBlock()) {
        return ast::Type(ast::TypeKind::Block);
    }
    else if (nval.isPythonObject()) {
        return ast::Type::makeAny();
    }

    // Unknown type - fallback to any
    return ast::Type::makeAny();
}

// Phase 2.4.4 Phase 2: Collect all return types from a statement (recursively)
void Interpreter::collectReturnTypes(ast::Stmt* stmt, std::vector<ast::Type>& return_types) {
    if (!stmt) return;

    // Check if this is a return statement
    if (auto* return_stmt = dynamic_cast<ast::ReturnStmt*>(stmt)) {
        if (return_stmt->getExpr()) {
            // Has a return value - try to evaluate it to get its type
            // BUGFIX: Catch errors during type inference - don't crash on undefined vars
            try {
                auto return_value = eval(*return_stmt->getExpr());
                ast::Type inferred_type = inferTypeFromValue(return_value);
                return_types.push_back(inferred_type);
            } catch (...) {
                // Can't infer type (e.g., undefined variable) - use unknown type
                return_types.push_back(ast::Type(ast::TypeKind::Any));
            }
        } else {
            // Return with no value -> void
            return_types.push_back(ast::Type::makeVoid());
        }
        return;
    }

    // Recursively search compound statements
    if (auto* compound = dynamic_cast<ast::CompoundStmt*>(stmt)) {
        for (auto& child : compound->getStatements()) {
            collectReturnTypes(child.get(), return_types);
        }
        return;
    }

    // Recursively search if statements
    if (auto* if_stmt = dynamic_cast<ast::IfStmt*>(stmt)) {
        collectReturnTypes(if_stmt->getThenBranch(), return_types);
        if (if_stmt->getElseBranch()) {
            collectReturnTypes(if_stmt->getElseBranch(), return_types);
        }
        return;
    }

    // Recursively search while loops
    if (auto* while_stmt = dynamic_cast<ast::WhileStmt*>(stmt)) {
        collectReturnTypes(while_stmt->getBody(), return_types);
        return;
    }

    // Recursively search for loops
    if (auto* for_stmt = dynamic_cast<ast::ForStmt*>(stmt)) {
        collectReturnTypes(for_stmt->getBody(), return_types);
        return;
    }

    // Other statement types don't contain returns
}

// Phase 2.4.4 Phase 2: Infer function return type from body
ast::Type Interpreter::inferReturnType(ast::Stmt* body) {
    std::vector<ast::Type> return_types;

    // Collect all return statement types
    collectReturnTypes(body, return_types);

    // If no return statements found, type is void
    if (return_types.empty()) {
        return ast::Type::makeVoid();
    }

    // If single return statement, use that type
    if (return_types.size() == 1) {
        return return_types[0];
    }

    // Multiple return statements - check if they're all the same type
    ast::Type first_type = return_types[0];
    bool all_same = true;
    for (size_t i = 1; i < return_types.size(); i++) {
        // Simple type equality check
        if (return_types[i].kind != first_type.kind) {
            all_same = false;
            break;
        }
    }

    if (all_same) {
        // All returns have the same type
        return first_type;
    }

    // Different return types - create union type
    // Phase 2.4.2: Use union types for multiple incompatible returns
    ast::Type union_type(ast::TypeKind::Union);
    union_type.union_types = return_types;
    return union_type;
}

// Phase 2.4.4 Phase 3: Collect type constraints from parameter-argument matching
void Interpreter::collectTypeConstraints(
    const ast::Type& param_type,
    const ast::Type& arg_type,
    std::map<std::string, ast::Type>& constraints
) {
    // If parameter is a type parameter (e.g., "T"), record the constraint
    if (param_type.kind == ast::TypeKind::TypeParameter) {
        const std::string& type_param_name = param_type.type_parameter_name;

        // If we already have a constraint for this type parameter, check compatibility
        auto it = constraints.find(type_param_name);
        if (it != constraints.end()) {
            // Known limitation: full Hindley-Milner type unification not yet implemented.
            // When a type parameter has conflicting constraints, a warning is emitted.
            // Full unification is deferred — see ENTERPRISE_REMEDIATION_PLAN.md §4.2.
            if (it->second.kind != arg_type.kind) {
                fmt::print("[WARN] Type parameter {} has conflicting constraints\n", type_param_name);
            }
        } else {
            // Record new constraint
            constraints.insert({type_param_name, arg_type});
        }
        return;
    }

    // If parameter is a generic container (e.g., list<T>), recurse
    if (param_type.kind == ast::TypeKind::List && arg_type.kind == ast::TypeKind::List) {
        if (param_type.element_type && arg_type.element_type) {
            collectTypeConstraints(*param_type.element_type, *arg_type.element_type, constraints);
        }
        return;
    }

    if (param_type.kind == ast::TypeKind::Dict && arg_type.kind == ast::TypeKind::Dict) {
        if (param_type.key_value_types && arg_type.key_value_types) {
            collectTypeConstraints(param_type.key_value_types->first, arg_type.key_value_types->first, constraints);
            collectTypeConstraints(param_type.key_value_types->second, arg_type.key_value_types->second, constraints);
        }
        return;
    }

    // For concrete types, no constraints to collect
}

// Phase 2.4.4 Phase 3: Substitute type parameters with concrete types
ast::Type Interpreter::substituteTypeParams(
    const ast::Type& type,
    const std::map<std::string, ast::Type>& substitutions
) {
    // If this is a type parameter, substitute it
    if (type.kind == ast::TypeKind::TypeParameter) {
        auto it = substitutions.find(type.type_parameter_name);
        if (it != substitutions.end()) {
            return it->second;
        }
        // If not found in substitutions, return as-is
        return type;
    }

    // If this is a container type, recursively substitute element types
    if (type.kind == ast::TypeKind::List && type.element_type) {
        ast::Type substituted_type(ast::TypeKind::List);
        substituted_type.element_type = std::make_shared<ast::Type>(
            substituteTypeParams(*type.element_type, substitutions)
        );
        substituted_type.is_nullable = type.is_nullable;
        return substituted_type;
    }

    if (type.kind == ast::TypeKind::Dict && type.key_value_types) {
        ast::Type substituted_type(ast::TypeKind::Dict);
        substituted_type.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(
            substituteTypeParams(type.key_value_types->first, substitutions),
            substituteTypeParams(type.key_value_types->second, substitutions)
        );
        substituted_type.is_nullable = type.is_nullable;
        return substituted_type;
    }

    // For concrete types, return as-is
    return type;
}

// Phase 2.4.4 Phase 3: Infer generic type arguments from call site
std::vector<ast::Type> Interpreter::inferGenericArgs(
    const std::shared_ptr<FunctionValue>& func,
    const std::vector<NaabVal>& args
) {
    // Build type constraint system
    std::map<std::string, ast::Type> constraints;

    // Match each argument to its parameter type
    for (size_t i = 0; i < args.size() && i < func->param_types.size(); i++) {
        const ast::Type& param_type = func->param_types[i];
        ast::Type arg_type = inferTypeFromValue(args[i]);

        // Collect constraints from this parameter-argument pair
        collectTypeConstraints(param_type, arg_type, constraints);
    }

    // Solve constraints: for each type parameter, look up its inferred type
    std::vector<ast::Type> type_args;
    for (const std::string& type_param : func->type_parameters) {
        auto it = constraints.find(type_param);
        if (it != constraints.end()) {
            type_args.push_back(it->second);
            LOG_DEBUG("[INFO] Inferred type argument {}: {}\n",
                       type_param, formatTypeName(it->second));
        } else {
            // Could not infer this type parameter
            fmt::print("[WARN] Could not infer type parameter {}, defaulting to Any\n", type_param);
            type_args.push_back(ast::Type::makeAny());
        }
    }

    return type_args;
}

} // namespace interpreter
} // namespace naab
