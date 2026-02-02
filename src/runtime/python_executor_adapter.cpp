// NAAb Python Executor Adapter Implementation
// Adapts PythonExecutor to the Executor interface

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/python_executor_adapter.h"
#include <fmt/core.h>

namespace naab {
namespace runtime {

PyExecutorAdapter::PyExecutorAdapter() {
    fmt::print("[PY ADAPTER] Python executor adapter initialized\n");
    executor_ = std::make_unique<PythonExecutor>();
}

bool PyExecutorAdapter::execute(const std::string& code) {
    fmt::print("[PY ADAPTER] Executing Python code\n");
    try {
        executor_->execute(code);
        return true;
    } catch (const std::exception& e) {
        fmt::print("[PY ADAPTER ERROR] {}\n", e.what());
        return false;
    }
}

// Phase 2.3: Execute code and return result value
std::shared_ptr<interpreter::Value> PyExecutorAdapter::executeWithReturn(
    const std::string& code) {
    fmt::print("[PY ADAPTER] Executing Python code with return\n");
    try {
        return executor_->executeWithResult(code);
    } catch (const std::exception& e) {
        fmt::print("[PY ADAPTER ERROR] {}\n", e.what());
        return std::make_shared<interpreter::Value>();  // Return null on error
    }
}

std::shared_ptr<interpreter::Value> PyExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    fmt::print("[PY ADAPTER] Calling function: {}\n", function_name);
    return executor_->callFunction(function_name, args);
}

bool PyExecutorAdapter::isInitialized() const {
    return executor_ != nullptr;
}

std::string PyExecutorAdapter::getCapturedOutput() {
    if (executor_) {
        return executor_->getCapturedOutput();
    }
    return "";
}

} // namespace runtime
} // namespace naab
