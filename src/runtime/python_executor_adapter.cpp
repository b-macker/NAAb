// NAAb Python Executor Adapter Implementation
// Adapts PythonExecutor to the Executor interface

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/python_executor_adapter.h"
#include <pybind11/pybind11.h>
#include <fmt/core.h>

namespace py = pybind11;

namespace naab {
namespace runtime {

PyExecutorAdapter::PyExecutorAdapter() {
    // CRITICAL: Acquire GIL before creating PythonExecutor
    // The global Python interpreter releases GIL for multi-threading,
    // but PythonExecutor constructor requires the caller to hold the GIL
    py::gil_scoped_acquire gil;
    executor_ = std::make_unique<PythonExecutor>();
}

bool PyExecutorAdapter::execute(const std::string& code) {
    // Executing Python code (silent)
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
    // Executing Python code with return (silent)
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

    // Calling function (silent)
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
