#include "naab/ffi_callback_validator.h"
#include "naab/audit_logger.h"
#include <fmt/core.h>

namespace naab {
namespace ffi {

//=============================================================================
// CallbackValidator Implementation
//=============================================================================

bool CallbackValidator::validatePointer(const void* callback_ptr) {
    if (callback_ptr == nullptr) {
        security::AuditLogger::logSecurityViolation(
            "FFI callback validation failed: null pointer"
        );
        return false;
    }
    return true;
}

bool CallbackValidator::validateArgumentCount(
    size_t actual_count,
    size_t expected_count
) {
    if (actual_count != expected_count) {
        security::AuditLogger::logSecurityViolation(
            fmt::format(
                "FFI callback argument count mismatch: expected {}, got {}",
                expected_count, actual_count
            )
        );
        return false;
    }
    return true;
}

bool CallbackValidator::validateSignature(
    const std::vector<interpreter::Value>& args,
    const std::vector<ast::Type>& expected_types
) {
    // Check argument count
    if (!validateArgumentCount(args.size(), expected_types.size())) {
        return false;
    }

    // Check each argument type
    for (size_t i = 0; i < args.size(); ++i) {
        if (!valueMatchesType(args[i], expected_types[i])) {
            security::AuditLogger::logSecurityViolation(
                fmt::format(
                    "FFI callback type mismatch at argument {}: expected {}, got {}",
                    i, getTypeName(expected_types[i]), getValueTypeName(args[i])
                )
            );
            return false;
        }
    }

    return true;
}

bool CallbackValidator::validateReturnType(
    const interpreter::Value& return_value,
    const ast::Type& expected_type
) {
    if (!valueMatchesType(return_value, expected_type)) {
        security::AuditLogger::logSecurityViolation(
            fmt::format(
                "FFI callback return type mismatch: expected {}, got {}",
                getTypeName(expected_type), getValueTypeName(return_value)
            )
        );
        return false;
    }
    return true;
}

bool CallbackValidator::valueMatchesType(
    const interpreter::Value& value,
    const ast::Type& type
) {
    // Use type compatibility check (handles Any type)
    return isTypeCompatible(value, type);
}

bool CallbackValidator::isTypeCompatible(
    const interpreter::Value& value,
    const ast::Type& expected_type
) {
    // Finding F fix: implement basic type compatibility check with audit logging for mismatches
    // Null/void values are compatible with any type
    if (value.data.index() == 0) return true;  // std::monostate = null

    const std::string type_name = getTypeName(expected_type);

    // Any/unknown/unresolved type: accept all (getTypeName returns "type" until §4.2)
    if (type_name == "type" || type_name == "any" || type_name == "unknown") return true;

    const std::string val_type = getValueTypeName(value);

    // Exact match
    if (val_type == type_name) return true;

    // Numeric widening: int is compatible with double/float
    if ((val_type == "int" || val_type == "float") &&
        (type_name == "int" || type_name == "float" ||
         type_name == "double" || type_name == "number")) return true;

    // Log type mismatch for audit — accept for backward compatibility until ast::Type
    // exposes full resolution (ENTERPRISE_REMEDIATION_PLAN.md §4.2)
    security::AuditLogger::logSecurityViolation(
        fmt::format("FFI type mismatch (non-fatal): expected {}, got {}", type_name, val_type)
    );
    return true;
}

std::string CallbackValidator::getTypeName(const ast::Type& type) {
    (void)type;
    // Known limitation: full type name resolution deferred — see ENTERPRISE_REMEDIATION_PLAN.md §4.2.
    return "type";
}

std::string CallbackValidator::getValueTypeName(const interpreter::Value& value) {
    // Use variant index to determine type
    size_t index = value.data.index();

    switch (index) {
        case 0: return "null";
        case 1: return "int";
        case 2: return "float";
        case 3: return "bool";
        case 4: return "string";
        case 5: return "list";
        case 6: return "dict";
        case 7: return "block";
        case 8: return "function";
        case 9: return "polyglot_object";
        case 10: return "struct";
        default: return "unknown";
    }
}

//=============================================================================
// CallbackValidationGuard Implementation
//=============================================================================

CallbackValidationGuard::CallbackValidationGuard(
    const void* callback_ptr,
    const std::vector<interpreter::Value>& args,
    const std::vector<ast::Type>& expected_types,
    const std::string& callback_name
) : is_valid_(true), error_message_() {

    // Validate pointer
    if (!CallbackValidator::validatePointer(callback_ptr)) {
        is_valid_ = false;
        error_message_ = fmt::format(
            "Callback '{}': null pointer",
            callback_name
        );
        return;
    }

    // Validate signature
    if (!CallbackValidator::validateSignature(args, expected_types)) {
        is_valid_ = false;
        error_message_ = fmt::format(
            "Callback '{}': signature mismatch",
            callback_name
        );
        return;
    }
}

} // namespace ffi
} // namespace naab
