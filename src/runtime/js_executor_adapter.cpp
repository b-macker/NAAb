// NAAb JavaScript Executor Adapter Implementation
// Adapts JsExecutor to the Executor interface

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/js_executor_adapter.h"
#include <fmt/core.h>
#include <iostream>

namespace naab {
namespace runtime {

JsExecutorAdapter::JsExecutorAdapter() {
    // JavaScript executor adapter initialized (silent)
}

bool JsExecutorAdapter::execute(const std::string& code) {
    // Executing JavaScript code (silent)
    return executor_.execute(code);
}

bool JsExecutorAdapter::execute(const std::string& code, JsExecutionMode mode) {
    // Executing JavaScript code with mode (silent)
    return executor_.execute(code, mode);
}

// Phase 2.3: Execute code and return result value
std::shared_ptr<interpreter::Value> JsExecutorAdapter::executeWithReturn(
    const std::string& code) {
    // Executing JavaScript code with return (silent)
    try {
        auto result = executor_.evaluate(code);
        return result;
    } catch (const std::exception& e) {
        std::string error_msg = e.what();
        fmt::print("[JS Error] {}\n", error_msg);

        // Helper hints for common JS-in-polyglot errors
        if (error_msg.find("is not defined") != std::string::npos) {
            fmt::print("\n  Hint: JavaScript variable not defined.\n"
                       "  - Bound variables from NAAb use <<javascript[var1, var2] syntax\n"
                       "  - Check spelling and that the variable is listed in the binding list\n"
                       "  - Built-in objects like JSON, Math, Array are always available\n\n");
        } else if (error_msg.find("SyntaxError") != std::string::npos) {
            fmt::print("\n  Hint: JavaScript syntax error in polyglot block.\n"
                       "  - NAAb wraps multi-line JS in an IIFE for return value capture\n"
                       "  - Avoid trailing comments (// ...) on the last expression line\n"
                       "  - For complex JS, consider using an external .js file via <<sh: node script.js\n\n");
        } else if (error_msg.find("is not a function") != std::string::npos) {
            fmt::print("\n  Hint: Value is not callable.\n"
                       "  - Check that bound variables have the expected type\n"
                       "  - NAAb arrays become JS arrays, dicts become objects\n"
                       "  - NAAb strings are passed as JS strings\n\n");
        }

        return std::make_shared<interpreter::Value>();  // Return null on error
    }
}

std::shared_ptr<interpreter::Value> JsExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Calling function (silent)
    return executor_.callFunction(function_name, args);
}

bool JsExecutorAdapter::isInitialized() const {
    return executor_.isInitialized();
}

std::string JsExecutorAdapter::getCapturedOutput() {
    // For now, JS executor doesn't capture output
    // This will be improved in Phase 4
    return "";
}

} // namespace runtime
} // namespace naab
