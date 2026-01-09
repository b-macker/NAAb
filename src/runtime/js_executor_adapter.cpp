// NAAb JavaScript Executor Adapter Implementation
// Adapts JsExecutor to the Executor interface

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

std::shared_ptr<interpreter::Value> JsExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    fmt::print("[JS ADAPTER] Calling function: {}\n", function_name);
    return executor_.callFunction(function_name, args);
}

bool JsExecutorAdapter::isInitialized() const {
    return executor_.isInitialized();
}

} // namespace runtime
} // namespace naab
