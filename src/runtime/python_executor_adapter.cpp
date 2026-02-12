// NAAb Python Executor Adapter Implementation
// Adapts PythonCExecutor to the Executor interface

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/python_executor_adapter.h"
#include <fmt/core.h>

namespace naab {
namespace runtime {

PyExecutorAdapter::PyExecutorAdapter() {
    // PythonCExecutor uses pure C API - no pybind11, no CFI crashes
    // No need to acquire GIL here - PythonCExecutor acquires it when needed
    executor_ = std::make_unique<PythonCExecutor>();
}

bool PyExecutorAdapter::execute(const std::string& code) {
    // PythonCExecutor handles GIL acquisition internally
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
    // PythonCExecutor handles GIL acquisition internally
    try {
        auto result = executor_->executeWithReturn(code);
        return result;
    } catch (const std::exception& e) {
        fmt::print("[PY ADAPTER ERROR] {}\n", e.what());
        return std::make_shared<interpreter::Value>();  // Return null on error
    }
}

std::shared_ptr<interpreter::Value> PyExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // PythonCExecutor handles GIL acquisition internally
    auto result = executor_->callFunction(function_name, args);
    return result;
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
