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
