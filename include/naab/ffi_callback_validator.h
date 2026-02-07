#pragma once

// FFI Callback Safety - Phase 1 Item 9
// Validates callbacks from foreign code before invocation

#include "naab/value.h"
#include "naab/ast.h"
#include <functional>
#include <vector>
#include <string>
#include <stdexcept>

namespace naab {
namespace ffi {

// Exception thrown when callback validation fails
class CallbackValidationException : public std::runtime_error {
public:
    explicit CallbackValidationException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// Exception boundary result (captures exceptions at FFI boundary)
struct ExceptionBoundaryResult {
    bool success;                    // True if callback succeeded
    interpreter::Value value;        // Return value (if success)
    std::string error_message;       // Error message (if failed)
    std::string error_type;          // Error type (if failed)

    // Create success result
    static ExceptionBoundaryResult makeSuccess(interpreter::Value val) {
        return {true, val, "", ""};
    }

    // Create error result
    static ExceptionBoundaryResult makeError(
        const std::string& type,
        const std::string& message
    ) {
        return {false, interpreter::Value(), message, type};
    }
};

// Callback signature validator
class CallbackValidator {
public:
    // Validate callback pointer is not null
    static bool validatePointer(const void* callback_ptr);

    // Validate argument types match expected signature
    static bool validateSignature(
        const std::vector<interpreter::Value>& args,
        const std::vector<ast::Type>& expected_types
    );

    // Validate argument count matches expected
    static bool validateArgumentCount(
        size_t actual_count,
        size_t expected_count
    );

    // Validate return value type
    static bool validateReturnType(
        const interpreter::Value& return_value,
        const ast::Type& expected_type
    );

    // Check if value matches type
    static bool valueMatchesType(
        const interpreter::Value& value,
        const ast::Type& type
    );

    // Wrap callback with exception boundary (catches all exceptions)
    template<typename Func>
    static std::function<ExceptionBoundaryResult()> wrapCallback(
        Func callback,
        const std::string& callback_name = "foreign_callback"
    ) {
        return [callback, callback_name]() -> ExceptionBoundaryResult {
            try {
                auto result = callback();
                return ExceptionBoundaryResult::makeSuccess(result);
            } catch (const CallbackValidationException& e) {
                // Catch derived exception first
                return ExceptionBoundaryResult::makeError(
                    "CallbackValidationException",
                    std::string(e.what())
                );
            } catch (const std::exception& e) {
                // Catch base exception second
                return ExceptionBoundaryResult::makeError(
                    "std::exception",
                    std::string(e.what())
                );
            } catch (...) {
                return ExceptionBoundaryResult::makeError(
                    "unknown_exception",
                    "Unknown exception caught at FFI boundary in " + callback_name
                );
            }
        };
    }

    // Wrap callback with exception boundary (with arguments)
    template<typename Func, typename... Args>
    static std::function<ExceptionBoundaryResult()> wrapCallbackWithArgs(
        Func callback,
        const std::string& callback_name,
        Args&&... args
    ) {
        return [callback, callback_name, args...]() -> ExceptionBoundaryResult {
            try {
                auto result = callback(std::forward<Args>(args)...);
                return ExceptionBoundaryResult::makeSuccess(result);
            } catch (const CallbackValidationException& e) {
                // Catch derived exception first
                return ExceptionBoundaryResult::makeError(
                    "CallbackValidationException",
                    std::string(e.what())
                );
            } catch (const std::exception& e) {
                // Catch base exception second
                return ExceptionBoundaryResult::makeError(
                    "std::exception",
                    std::string(e.what())
                );
            } catch (...) {
                return ExceptionBoundaryResult::makeError(
                    "unknown_exception",
                    "Unknown exception in " + callback_name
                );
            }
        };
    }

    // Get type name for error messages
    static std::string getTypeName(const ast::Type& type);
    static std::string getValueTypeName(const interpreter::Value& value);

private:
    // Helper to check if type is compatible (considering Any type)
    static bool isTypeCompatible(
        const interpreter::Value& value,
        const ast::Type& expected_type
    );
};

// RAII guard for callback validation (validates on construction)
class CallbackValidationGuard {
public:
    CallbackValidationGuard(
        const void* callback_ptr,
        const std::vector<interpreter::Value>& args,
        const std::vector<ast::Type>& expected_types,
        const std::string& callback_name = "callback"
    );

    ~CallbackValidationGuard() = default;

    // Check if validation passed
    bool isValid() const { return is_valid_; }

    // Get validation error (if any)
    const std::string& getError() const { return error_message_; }

private:
    bool is_valid_;
    std::string error_message_;
};

} // namespace ffi
} // namespace naab
