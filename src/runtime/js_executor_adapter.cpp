// NAAb JavaScript Executor Adapter Implementation
// Adapts JsExecutor to the Executor interface

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/js_executor_adapter.h"
#include <fmt/core.h>

namespace naab {
namespace runtime {

JsExecutorAdapter::JsExecutorAdapter() {
    fmt::print("[JS ADAPTER] JavaScript executor adapter initialized\n");
}

bool JsExecutorAdapter::execute(const std::string& code) {
    fmt::print("[JS ADAPTER] Executing JavaScript code\n");
    return executor_.execute(code);
}

// Phase 2.3: Execute code and return result value
std::shared_ptr<interpreter::Value> JsExecutorAdapter::executeWithReturn(
    const std::string& code) {
    fmt::print("[JS ADAPTER] Executing JavaScript code with return\n");
    try {
        return executor_.evaluate(code);
    } catch (const std::exception& e) {
        fmt::print("[JS ADAPTER ERROR] {}\n", e.what());
        return std::make_shared<interpreter::Value>();  // Return null on error
    }
}

std::shared_ptr<interpreter::Value> JsExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    fmt::print("[JS ADAPTER] Calling function: {}\n", function_name);
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
