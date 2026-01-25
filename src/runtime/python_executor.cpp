// NAAb Python Block Executor Implementation
// Embeds CPython interpreter using pybind11

#include "naab/python_executor.h"
#include "naab/interpreter.h"
#include "naab/resource_limits.h"
#include "naab/audit_logger.h"
#include "naab/sandbox.h"
#include "naab/stack_tracer.h"  // Phase 4.2.2: Cross-language stack traces
#include <fmt/core.h>
#include <stdexcept>
#include <unordered_map>

// This block defines how Python can interact with our C++ class
PYBIND11_EMBEDDED_MODULE(naab_internal, m) {
    py::class_<naab::runtime::PythonExecutor::PythonOutputRedirector>(m, "OutputRedirector")
        .def(py::init<naab::runtime::OutputBuffer&>())
        .def("write", &naab::runtime::PythonExecutor::PythonOutputRedirector::write)
        .def("flush", &naab::runtime::PythonExecutor::PythonOutputRedirector::flush);
}

namespace naab {
namespace runtime {

PythonExecutor::PythonExecutor()
    : guard_(), global_namespace_(py::globals()) {

    fmt::print("[Python] Initialized CPython interpreter\n");

    // Import commonly used modules
    try {
        py::module_::import("sys");
        py::module_::import("os");
        // Ensure our internal module is loaded so OutputRedirector type is registered
        py::module_::import("naab_internal");
        fmt::print("[Python] Imported standard modules (sys, os) and naab_internal\n");
    } catch (const py::error_already_set& e) {
        fmt::print("[WARN] Failed to import standard modules: {}\n", e.what());
    }

    // --- Redirect Python stdout/stderr ---
    // Use low-level C-API PySys_SetObject to bypass potential pybind11 proxy issues
    // Pybind11 casts automatically handle ref-counting for the new C++ objects
    stdout_redirector_ = py::cast(new PythonOutputRedirector(stdout_buffer_), py::return_value_policy::take_ownership);
    stderr_redirector_ = py::cast(new PythonOutputRedirector(stderr_buffer_), py::return_value_policy::take_ownership);

    if (PySys_SetObject("stdout", stdout_redirector_.ptr()) != 0) {
        fmt::print("[WARN] Failed to set sys.stdout via PySys_SetObject\n");
    }
    if (PySys_SetObject("stderr", stderr_redirector_.ptr()) != 0) {
        fmt::print("[WARN] Failed to set sys.stderr via PySys_SetObject\n");
    }

    fmt::print("[Python] Redirected stdout/stderr to internal buffers\n");
}

PythonExecutor::~PythonExecutor() {
    fmt::print("[Python] Shutting down CPython interpreter\n");
}

void PythonExecutor::execute(const std::string& code) {
    // Check sandbox permissions for code execution
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Python execution denied\n");
        sandbox->logViolation("executePython", "<code>", "BLOCK_CALL capability required");
        throw std::runtime_error("Python execution denied by sandbox");
    }

    // Restore sys.stdout/stderr if missing (using robust C-API)
    // We don't need to import sys module object to use PySys_SetObject
    if (PySys_GetObject("stdout") == NULL) {
        if (PySys_SetObject("stdout", stdout_redirector_.ptr()) != 0) {
             fmt::print("[WARN] Failed to restore sys.stdout\n");
        }
    }
    if (PySys_GetObject("stderr") == NULL) {
        if (PySys_SetObject("stderr", stderr_redirector_.ptr()) != 0) {
             fmt::print("[WARN] Failed to restore sys.stderr\n");
        }
    }

    try {
        // Execute with 30-second timeout
        security::ScopedTimeout timeout(30);
        py::exec(code, global_namespace_);
        fmt::print("[Python] Executed code successfully\n");
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Python execution timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("Python exec()", 30);
        throw std::runtime_error("Python execution timed out");
    } catch (const py::error_already_set& e) {
        throw std::runtime_error(fmt::format("Python execution error: {}", e.what()));
    }
}

std::shared_ptr<interpreter::Value> PythonExecutor::executeWithResult(const std::string& code) {
    try {
        // Execute with 30-second timeout
        security::ScopedTimeout timeout(30);

        // Try eval() first for simple expressions (backwards compatible)
        try {
            py::object result = py::eval(code, global_namespace_);
            return pythonToValue(result);
        } catch (const py::error_already_set& e) {
            // Check if it's a SyntaxError (likely multi-line statements)
            std::string error_msg = e.what();
            if (error_msg.find("SyntaxError") != std::string::npos) {
                // Fall back to exec() for multi-line statements
                fmt::print("[Python] eval() failed, trying exec() for multi-line code\n");

                // Clear the error state before trying exec
                PyErr_Clear();

                // Phase 2.3: Auto-capture last expression value
                // Strategy: Find the last non-indented statement and prepend `_ = ` to capture its value
                std::string modified_code = code;
                std::vector<std::string> lines;
                std::istringstream stream(modified_code);
                std::string line;
                while (std::getline(stream, line)) {
                    lines.push_back(line);
                }

                // Find the last non-empty, non-comment, non-indented line (top-level statement)
                int last_line_idx = -1;
                for (int i = lines.size() - 1; i >= 0; i--) {
                    std::string trimmed = lines[i];
                    // Check if line starts without indentation (top-level)
                    if (!trimmed.empty() && trimmed[0] != ' ' && trimmed[0] != '\t') {
                        // Skip comment lines
                        if (trimmed[0] != '#') {
                            last_line_idx = i;
                            break;
                        }
                    }
                }

                // Build modified code with `_ = ` before the last top-level statement
                if (last_line_idx >= 0) {
                    std::string wrapped_code;
                    for (size_t i = 0; i < lines.size(); i++) {
                        if (static_cast<int>(i) == last_line_idx) {
                            // Check if last line is already an assignment or control structure
                            std::string trimmed = lines[i];

                            // Skip control structures (if, for, while, def, class, etc.)
                            // Also skip imports and statements that don't produce a value
                            if (trimmed.find("if ") == 0 || trimmed.find("if(") == 0 ||
                                trimmed.find("for ") == 0 || trimmed.find("while ") == 0 ||
                                trimmed.find("def ") == 0 || trimmed.find("class ") == 0 ||
                                trimmed.find("with ") == 0 || trimmed.find("try:") == 0 ||
                                trimmed.find("except") == 0 || trimmed.find("finally:") == 0 ||
                                trimmed.find("else:") == 0 || trimmed.find("elif ") == 0 ||
                                trimmed.find("import ") == 0 || trimmed.find("from ") == 0 ||
                                trimmed.find("_ =") == 0 || trimmed.find("_=") == 0) {
                                // Don't modify control structures, imports, or existing _ assignments
                                wrapped_code += lines[i] + "\n";
                            } else {
                                // Prepend `_ = ` to capture the expression value
                                wrapped_code += "_ = " + lines[i] + "\n";
                            }
                        } else {
                            wrapped_code += lines[i] + "\n";
                        }
                    }
                    modified_code = wrapped_code;
                }

                // Use exec() for multi-line statements
                py::exec(modified_code, global_namespace_);

                // Try to get the result from the `_` variable
                try {
                    if (global_namespace_.contains("_")) {
                        py::object result = global_namespace_["_"];
                        if (!result.is_none()) {
                            return pythonToValue(result);
                        }
                    }
                } catch (...) {
                    // Ignore if '_' doesn't exist or can't be converted
                }

                // If no '_' variable, return None/void
                return std::make_shared<interpreter::Value>();
            } else {
                // Not a SyntaxError, re-throw the original error
                throw;
            }
        }
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Python execution timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("Python exec()", 30);
        throw std::runtime_error("Python execution timed out");
    } catch (const py::error_already_set& e) {
        throw std::runtime_error(fmt::format("Python execution error: {}", e.what()));
    }
}

std::shared_ptr<interpreter::Value> PythonExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    fmt::print("[Python] Calling function: {}\n", function_name);

    // Phase 4.2.2: Push stack frame for cross-language tracing
    error::ScopedStackFrame stack_frame("python", function_name, "<python>", 0);

    // Check if function exists
    if (!global_namespace_.contains(function_name.c_str())) {
        throw std::runtime_error(fmt::format("Python function not found: {}", function_name));
    }

    try {
        // Get the function object
        py::object func = global_namespace_[function_name.c_str()];

        // Convert NAAb arguments to Python objects
        py::list py_args;
        for (const auto& arg : args) {
            py_args.append(valueToPython(arg));
        }

        // Call the function with 30-second timeout
        py::object result;
        {
            security::ScopedTimeout timeout(30);
            result = func(*py_args);
        }

        // Convert result back to NAAb Value
        return pythonToValue(result);
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Python function timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("Python function: " + function_name, 30);
        throw std::runtime_error("Python function call timed out");
    } catch (const py::error_already_set& e) {
        // Phase 4.2.2: Extract Python traceback and add to stack
        extractPythonTraceback(e);

        // Re-throw with enriched stack trace
        throw std::runtime_error(fmt::format("Python call error in {}: {}\n{}",
            function_name, e.what(), error::StackTracer::formatTrace()));
    }
}

bool PythonExecutor::loadModule(const std::string& module_name, const std::string& code) {
    fmt::print("[Python] Loading module: {}\n", module_name);

    try {
        // Execute the module code in the global namespace
        py::exec(code, global_namespace_);
        fmt::print("[Python] Module {} loaded successfully\n", module_name);
        return true;
    } catch (const py::error_already_set& e) {
        fmt::print("[ERROR] Failed to load module {}: {}\n", module_name, e.what());
        return false;
    }
}

bool PythonExecutor::hasFunction(const std::string& function_name) const {
    return global_namespace_.contains(function_name.c_str());
}

// ============================================================================
// Type Conversion: NAAb Value → Python Object
// ============================================================================

py::object PythonExecutor::valueToPython(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> py::object {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, std::monostate>) {
            // null/void → None
            return py::none();
        }
        else if constexpr (std::is_same_v<T, int>) {
            return py::int_(arg);
        }
        else if constexpr (std::is_same_v<T, double>) {
            return py::float_(arg);
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return py::bool_(arg);
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            return py::str(arg);
        }
        else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            // Array → Python list
            py::list result;
            for (const auto& item : arg) {
                // Recursive call - need to access PythonExecutor instance
                // For now, create a simple conversion
                std::visit([&result](auto&& inner_arg) {
                    using InnerT = std::decay_t<decltype(inner_arg)>;
                    if constexpr (std::is_same_v<InnerT, int>) {
                        result.append(py::int_(inner_arg));
                    } else if constexpr (std::is_same_v<InnerT, double>) {
                        result.append(py::float_(inner_arg));
                    } else if constexpr (std::is_same_v<InnerT, bool>) {
                        result.append(py::bool_(inner_arg));
                    } else if constexpr (std::is_same_v<InnerT, std::string>) {
                        result.append(py::str(inner_arg));
                    } else if constexpr (std::is_same_v<InnerT, std::monostate>) {
                        result.append(py::none());
                    } else {
                        result.append(py::str("<complex>"));
                    }
                }, item->data);
            }
            return result;
        }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
            // Dictionary → Python dict
            py::dict result;
            for (const auto& pair : arg) {
                const auto& key = pair.first;
                const auto& value = pair.second;
                // Simple conversion for nested values
                std::visit([&result, &key](auto&& inner_arg) {
                    using InnerT = std::decay_t<decltype(inner_arg)>;
                    if constexpr (std::is_same_v<InnerT, int>) {
                        result[py::str(key)] = py::int_(inner_arg);
                    } else if constexpr (std::is_same_v<InnerT, double>) {
                        result[py::str(key)] = py::float_(inner_arg);
                    } else if constexpr (std::is_same_v<InnerT, bool>) {
                        result[py::str(key)] = py::bool_(inner_arg);
                    } else if constexpr (std::is_same_v<InnerT, std::string>) {
                        result[py::str(key)] = py::str(inner_arg);
                    } else if constexpr (std::is_same_v<InnerT, std::monostate>) {
                        result[py::str(key)] = py::none();
                    } else {
                        result[py::str(key)] = py::str("<complex>");
                    }
                }, value->data);
            }
            return result;
        }
        else {
            return py::str("<unknown>");
        }
    }, val->data);
}

// ============================================================================
// Type Conversion: Python Object → NAAb Value
// ============================================================================

std::shared_ptr<interpreter::Value> PythonExecutor::pythonToValue(const py::object& obj) {
    // None → null
    if (obj.is_none()) {
        return std::make_shared<interpreter::Value>();
    }

    // Boolean (check before int, since bool is subclass of int in Python)
    if (py::isinstance<py::bool_>(obj)) {
        return std::make_shared<interpreter::Value>(obj.cast<bool>());
    }

    // Integer
    if (py::isinstance<py::int_>(obj)) {
        return std::make_shared<interpreter::Value>(obj.cast<int>());
    }

    // Float
    if (py::isinstance<py::float_>(obj)) {
        return std::make_shared<interpreter::Value>(obj.cast<double>());
    }

    // String
    if (py::isinstance<py::str>(obj)) {
        return std::make_shared<interpreter::Value>(obj.cast<std::string>());
    }

    // List → Array
    if (py::isinstance<py::list>(obj)) {
        py::list py_list = obj.cast<py::list>();
        std::vector<std::shared_ptr<interpreter::Value>> result;

        for (const auto& item : py_list) {
            result.push_back(pythonToValue(py::cast<py::object>(item)));
        }

        return std::make_shared<interpreter::Value>(result);
    }

    // Tuple → Array
    if (py::isinstance<py::tuple>(obj)) {
        py::tuple py_tuple = obj.cast<py::tuple>();
        std::vector<std::shared_ptr<interpreter::Value>> result;

        for (const auto& item : py_tuple) {
            result.push_back(pythonToValue(py::cast<py::object>(item)));
        }

        return std::make_shared<interpreter::Value>(result);
    }

    // Dict → Dictionary
    if (py::isinstance<py::dict>(obj)) {
        py::dict py_dict = obj.cast<py::dict>();
        std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> result;

        for (const auto& item : py_dict) {
            std::string key = py::str(item.first).cast<std::string>();
            result[key] = pythonToValue(py::cast<py::object>(item.second));
        }

        return std::make_shared<interpreter::Value>(result);
    }

    // Unknown type - convert to string representation
    fmt::print("[WARN] Unknown Python type, converting to string\n");
    std::string str_repr = py::str(obj).cast<std::string>();
    return std::make_shared<interpreter::Value>(str_repr);
}

// ============================================================================
// Phase 4.2.2: Python Traceback Extraction
// ============================================================================

void PythonExecutor::extractPythonTraceback(const py::error_already_set& e) {
    (void)e; // Exception info fetched from Python's global error state instead

    try {
        // Get Python exception info
        PyObject* ptype = nullptr;
        PyObject* pvalue = nullptr;
        PyObject* ptraceback = nullptr;

        // Fetch the exception from Python's error indicator
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
        PyErr_NormalizeException(&ptype, &pvalue, &ptraceback);

        if (ptraceback) {
            // Extract traceback frames using Python's traceback module
            py::object traceback_module = py::module_::import("traceback");
            py::object extract_tb = traceback_module.attr("extract_tb");

            // Convert ptraceback to py::object
            py::handle tb_handle(ptraceback);
            py::object tb_obj = py::reinterpret_borrow<py::object>(tb_handle);

            // Extract frames
            py::object frames = extract_tb(tb_obj);

            // Iterate through frames and add to stack trace
            for (const auto& frame_obj : frames) {
                py::tuple frame = frame_obj.cast<py::tuple>();

                // Frame format: (filename, line_number, function_name, text)
                std::string filename = py::str(frame[0]).cast<std::string>();
                size_t line_number = frame[1].cast<size_t>();
                std::string function_name = py::str(frame[2]).cast<std::string>();

                // Add Python frame to stack trace
                error::StackFrame python_frame("python", function_name, filename, line_number);
                error::StackTracer::pushFrame(python_frame);

                fmt::print("[TRACE] Python frame: {} ({}:{})\n",
                    function_name, filename, line_number);
            }
        }

        // Restore exception for pybind11 to handle
        PyErr_Restore(ptype, pvalue, ptraceback);

    } catch (const std::exception& ex) {
        fmt::print("[WARN] Failed to extract Python traceback: {}\n", ex.what());
    }
}

std::string PythonExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string error_output = stderr_buffer_.getAndClear();
    if (!error_output.empty()) {
        output += " (stderr: " + error_output + ")";
    }
    return output;
}

} // namespace runtime
} // namespace naab
