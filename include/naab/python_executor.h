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
#include "naab/output_buffer.h" // Include the OutputBuffer

namespace py = pybind11;

namespace naab {
namespace interpreter {
    class Value;  // Forward declaration
}

namespace runtime {

// Python Block Executor - embeds CPython for executing Python blocks
class PythonExecutor {
public:
    // Nested class to redirect Python's stdout/stderr to our C++ OutputBuffer
    class PythonOutputRedirector {
    public:
        PythonOutputRedirector(OutputBuffer& buffer) : buffer_(buffer) {}

        void write(const std::string& text) {
            buffer_.append(text);
        }

        // Python's print function often calls flush. We need to provide it.
        void flush() {
            // No-op for our buffer, as we append immediately
        }

    private:
        OutputBuffer& buffer_;
    };

public:
    // Constructor with optional output redirection
    // For async contexts, set redirect_output=false to avoid conflicts
    explicit PythonExecutor(bool redirect_output = true);
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
    py::dict getGlobalNamespace() const { return py::globals(); }

    // Get all captured output and clear buffers
    std::string getCapturedOutput();

    // Timeout configuration
    void setTimeout(int seconds) { timeout_seconds_ = seconds; }
    int getTimeout() const { return timeout_seconds_; }

    // Import security configuration
    static void setBlockDangerousImports(bool block) { block_dangerous_imports_ = block; }
    static bool shouldBlockDangerousImports() { return block_dangerous_imports_; }

private:
    // NOTE: Interpreter is now managed globally by PythonInterpreterManager
    // No more py::scoped_interpreter guard_ here!
    // No more global_namespace_ member - call py::globals() when needed

    OutputBuffer stdout_buffer_; // Buffer for Python stdout
    OutputBuffer stderr_buffer_; // Buffer for Python stderr

    // Optional redirectors - only used when redirect_output=true
    std::unique_ptr<py::object> stdout_redirector_; // Python-side stdout redirector instance
    std::unique_ptr<py::object> stderr_redirector_; // Python-side stderr redirector instance

    int timeout_seconds_ = 30;  // Timeout for Python execution (default: 30)

    // Security: Control whether to block dangerous imports (static for thread safety)
    static inline bool block_dangerous_imports_ = true;  // Default: block for safety

    // Type conversion helpers
    py::object valueToPython(const std::shared_ptr<interpreter::Value>& val);
    std::shared_ptr<interpreter::Value> pythonToValue(const py::object& obj);

    // Phase 4.2.2: Python traceback extraction
    void extractPythonTraceback(const py::error_already_set& e);
};

} // namespace runtime
} // namespace naab

#endif // NAAB_PYTHON_EXECUTOR_H
