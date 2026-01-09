#ifndef NAAB_PYTHON_EXECUTOR_H
#define NAAB_PYTHON_EXECUTOR_H

// NAAb Python Block Executor
// Embeds CPython interpreter using pybind11

#include <memory>
#include <string>
#include <vector>
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace naab {
namespace interpreter {
    class Value;  // Forward declaration
}

namespace runtime {

// Python Block Executor - embeds CPython for executing Python blocks
class PythonExecutor {
public:
    PythonExecutor();
    ~PythonExecutor();

    // Execute Python code in the global namespace
    void execute(const std::string& code);

    // Execute Python code and return result
    std::shared_ptr<interpreter::Value> executeWithResult(const std::string& code);

    // Call a Python function by name
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    );

    // Load a Python module/block
    bool loadModule(const std::string& module_name, const std::string& code);

    // Check if a function exists in the global namespace
    bool hasFunction(const std::string& function_name) const;

    // Get the global namespace (for debugging)
    py::dict getGlobalNamespace() const { return global_namespace_; }

private:
    py::scoped_interpreter guard_;  // Python interpreter lifecycle
    py::dict global_namespace_;     // Shared global namespace

    // Type conversion helpers
    py::object valueToPython(const std::shared_ptr<interpreter::Value>& val);
    std::shared_ptr<interpreter::Value> pythonToValue(const py::object& obj);
};

} // namespace runtime
} // namespace naab

#endif // NAAB_PYTHON_EXECUTOR_H
