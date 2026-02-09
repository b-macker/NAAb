// NAAb Python Block Executor Implementation
#include "naab/logger.h"
// Embeds CPython interpreter using pybind11

#include "naab/python_executor.h"
#include "naab/interpreter.h"
#include "naab/resource_limits.h"
#include "naab/audit_logger.h"
#include "naab/sandbox.h"
#include "naab/stack_tracer.h"  // Phase 4.2.2: Cross-language stack traces
#include "naab/limits.h"  // Week 1, Task 1.2: Input size caps
#include "naab/ffi_callback_validator.h"  // Phase 1 Item 9: FFI callback safety
#include "naab/cross_language_bridge.h"  // For struct serialization
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

PythonExecutor::PythonExecutor(bool redirect_output) {
    // NOTE: Caller must hold GIL before creating PythonExecutor
    // In async contexts, the callback already acquired GIL
    // In sync contexts, acquire GIL before calling this constructor

    LOG_DEBUG("[DEBUG] PythonExecutor constructor started (redirect_output={})\n", redirect_output);

    // Note: We don't store py::globals() as a member to avoid cleanup issues
    // Instead, we call py::globals() each time we need it
    LOG_DEBUG("[DEBUG] Verifying access to py::globals()...\n");
    auto globals = py::globals();  // Test access
    (void)globals;  // Unused
    LOG_DEBUG("[DEBUG] Global namespace accessible\n");

    LOG_DEBUG("[Python] PythonExecutor initialized (using global interpreter)\n");

    // Import commonly used modules
    try {
        LOG_DEBUG("[DEBUG] Importing sys module...\n");
        py::module_::import("sys");
        LOG_DEBUG("[DEBUG] Importing os module...\n");
        py::module_::import("os");
        // Ensure our internal module is loaded so OutputRedirector type is registered
        LOG_DEBUG("[DEBUG] Importing naab_internal module...\n");
        py::module_::import("naab_internal");
        LOG_DEBUG("[Python] Imported standard modules (sys, os) and naab_internal\n");
    } catch (const py::error_already_set& e) {
        fmt::print("[WARN] Failed to import standard modules: {}\n", e.what());
    }

    // --- Optionally redirect Python stdout/stderr ---
    if (redirect_output) {
        // Use low-level C-API PySys_SetObject to bypass potential pybind11 proxy issues
        // Pybind11 casts automatically handle ref-counting for the new C++ objects
        stdout_redirector_ = std::make_unique<py::object>(
            py::cast(new PythonOutputRedirector(stdout_buffer_), py::return_value_policy::take_ownership)
        );
        stderr_redirector_ = std::make_unique<py::object>(
            py::cast(new PythonOutputRedirector(stderr_buffer_), py::return_value_policy::take_ownership)
        );

        if (PySys_SetObject("stdout", (*stdout_redirector_).ptr()) != 0) {
            fmt::print("[WARN] Failed to set sys.stdout via PySys_SetObject\n");
        }
        if (PySys_SetObject("stderr", (*stderr_redirector_).ptr()) != 0) {
            fmt::print("[WARN] Failed to set sys.stderr via PySys_SetObject\n");
        }

        LOG_DEBUG("[Python] Redirected stdout/stderr to internal buffers\n");
    } else {
        // Don't create redirector objects at all in async mode
        stdout_redirector_ = nullptr;
        stderr_redirector_ = nullptr;
        LOG_DEBUG("[Python] Skipped stdout/stderr redirection (async mode)\n");
    }
}

PythonExecutor::~PythonExecutor() {
    // Only need GIL if we have redirectors to clean up
    if (stdout_redirector_ || stderr_redirector_) {
        py::gil_scoped_acquire gil;

        LOG_DEBUG("[Python] Cleaning up PythonExecutor with redirectors (GIL held)\n");

        // CRITICAL: Explicitly destroy Python objects WHILE holding GIL
        // unique_ptr::reset() destroys the object it points to
        stdout_redirector_.reset();  // Destroys py::object if it exists
        stderr_redirector_.reset();  // Destroys py::object if it exists

        LOG_DEBUG("[Python] Python redirector objects cleaned up successfully\n");

        // GIL is automatically released when gil goes out of scope
    } else {
        LOG_DEBUG("[Python] Cleaning up PythonExecutor (no redirectors, no GIL needed)\n");
    }
}

void PythonExecutor::execute(const std::string& code) {
    // Week 1, Task 1.2: Check polyglot block size
    limits::checkPolyglotBlockSize(code.size(), "Python");

    // Check sandbox permissions for code execution
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Python execution denied\n");
        sandbox->logViolation("executePython", "<code>", "BLOCK_CALL capability required");
        std::ostringstream oss;
        oss << "Security error: Python execution denied by sandbox\n\n";
        oss << "  Required capability: BLOCK_CALL\n\n";
        oss << "  Help:\n";
        oss << "  - Sandbox restricts Python code execution for security\n";
        oss << "  - Grant BLOCK_CALL capability if needed\n";
        oss << "  - Or disable sandbox mode (unsafe)\n\n";
        oss << "  Example:\n";
        oss << "    // In sandbox config:\n";
        oss << "    capabilities: [\"BLOCK_CALL\"]\n";
        throw std::runtime_error(oss.str());
    }

    // Restore sys.stdout/stderr if missing (using robust C-API)
    // We don't need to import sys module object to use PySys_SetObject
    // Only if redirectors were created
    if (stdout_redirector_ && PySys_GetObject("stdout") == NULL) {
        if (PySys_SetObject("stdout", (*stdout_redirector_).ptr()) != 0) {
             fmt::print("[WARN] Failed to restore sys.stdout\n");
        }
    }
    if (stderr_redirector_ && PySys_GetObject("stderr") == NULL) {
        if (PySys_SetObject("stderr", (*stderr_redirector_).ptr()) != 0) {
             fmt::print("[WARN] Failed to restore sys.stderr\n");
        }
    }

    try {
        // Execute with configurable timeout (default 30 seconds)
        security::ScopedTimeout timeout(static_cast<unsigned int>(timeout_seconds_));
        py::exec(code, py::globals());
        LOG_DEBUG("[Python] Executed code successfully\n");
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Python execution timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("Python exec()", static_cast<unsigned int>(timeout_seconds_));
        std::ostringstream oss;
        oss << "Python execution error: Code execution timed out\n\n";
        oss << "  Timeout limit: " << timeout_seconds_ << " seconds\n\n";
        oss << "  Help:\n";
        oss << "  - Python code took too long to execute\n";
        oss << "  - Check for infinite loops or blocking operations\n";
        oss << "  - Optimize algorithm complexity\n";
        oss << "  - Consider async execution for long operations\n\n";
        oss << "  Common causes:\n";
        oss << "  - Infinite while loop\n";
        oss << "  - Blocking I/O without timeout\n";
        oss << "  - CPU-intensive computation\n";
        throw std::runtime_error(oss.str());
    } catch (const py::error_already_set& e) {
        // Enhanced error with code preview
        std::ostringstream oss;
        oss << "Error in Python polyglot block:\n"
            << "  Python error: " << e.what() << "\n";

        // Add code preview (first 200 chars)
        if (!code.empty()) {
            std::string preview = code.substr(0, std::min(code.size(), size_t(200)));
            oss << "  Block preview:\n";
            std::istringstream code_stream(preview);
            std::string line;
            while (std::getline(code_stream, line)) {
                oss << "    " << line << "\n";
            }
            if (code.size() > 200) {
                oss << "    ...\n";
            }
        }

        oss << "\n  Hint: Check Python syntax and indentation";

        throw std::runtime_error(oss.str());
    }
}

std::shared_ptr<interpreter::Value> PythonExecutor::executeWithResult(const std::string& code) {
    // Week 1, Task 1.2: Check polyglot block size
    limits::checkPolyglotBlockSize(code.size(), "Python");

    try {
        // Issue #3/#5/#7 Fix: Wrap code in function if it contains 'return' statement
        // This allows return statements, multi-line dicts, and with open(...) with return to work
        if (code.find("return ") != std::string::npos || code.find("return\n") != std::string::npos) {
            LOG_DEBUG("[Python] Code contains 'return', wrapping in function\n");

            // Wrap code in a function and immediately call it
            std::string wrapped = "def __naab_wrapper():\n";

            // Indent all lines of the original code
            std::istringstream stream(code);
            std::string line;
            while (std::getline(stream, line)) {
                wrapped += "    " + line + "\n";
            }

            // Call the function and store result
            wrapped += "_ = __naab_wrapper()\n";

            try {
                py::exec(wrapped, py::globals());

                // Get the result from _ variable
                auto globals = py::globals();
                if (globals.contains("_")) {
                    py::object result = globals["_"];
                    if (!result.is_none()) {
                        return pythonToValue(result);
                    }
                }
                // Return empty dict if function returned None
                return std::make_shared<interpreter::Value>();
            } catch (const py::error_already_set& wrap_error) {
                // If wrapping failed, provide helpful error message
                PyErr_Clear();
                LOG_DEBUG("[Python] Function wrapping failed, trying original method\n");

                std::string wrap_err = wrap_error.what();
                if (wrap_err.find("IndentationError") != std::string::npos) {
                    throw std::runtime_error(
                        "Python code with 'return' has indentation issues\n\n"
                        "When using 'return', ensure all code is properly indented:\n"
                        "  ✓ Correct:\n"
                        "    if condition:\n"
                        "        return value\n\n"
                        "  ✗ Wrong:\n"
                        "    if condition:\n"
                        "    return value  # Indentation error\n\n"
                        "Original error: " + wrap_err
                    );
                }
                // Fall through to original logic for other errors
            }
        }

        // Note: Timeout is now handled by AsyncCallbackWrapper for async execution
        // For blocking execution, the wrapper also provides timeout control

        // Try eval() first for simple expressions (backwards compatible)
        try {
            py::object result = py::eval(code, py::globals());
            return pythonToValue(result);
        } catch (const py::error_already_set& e) {
            // Check if it's a SyntaxError (likely multi-line statements)
            std::string error_msg = e.what();
            if (error_msg.find("SyntaxError") != std::string::npos) {
                // Fall back to exec() for multi-line statements
                LOG_DEBUG("[Python] eval() failed, trying exec() for multi-line code\n");

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
                for (int i = static_cast<int>(lines.size()) - 1; i >= 0; i--) {
                    std::string trimmed = lines[static_cast<size_t>(i)];
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
                                trimmed.find("raise ") == 0 || trimmed.find("raise(") == 0 ||
                                trimmed.find("return ") == 0 || trimmed.find("break") == 0 ||
                                trimmed.find("continue") == 0 || trimmed.find("pass") == 0 ||
                                trimmed.find("assert ") == 0 || trimmed.find("del ") == 0 ||
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
                py::exec(modified_code, py::globals());

                // Try to get the result from the `_` variable
                try {
                    auto globals = py::globals();
                    if (globals.contains("_")) {
                        py::object result = globals["_"];
                        if (!result.is_none()) {
                            return pythonToValue(result);
                        } else {
                            // Python block returned None explicitly
                            fmt::print("[WARN] Python block returned None\n");
                            throw std::runtime_error(
                                "Python block returned None/null\n\n"
                                "Help: NAAb polyglot blocks must return a value (cannot be None/null).\n"
                                "Even standalone blocks executed for side-effects need a return value.\n\n"
                                "  ✗ Wrong - returns None:\n"
                                "    <<python\n"
                                "    print(\"Hello\")\n"
                                "    for i in range(3):\n"
                                "        print(f\"Count: {i}\")\n"
                                "    None  # ← Cannot return None!\n"
                                "    >>\n\n"
                                "  ✓ Correct - return a simple value:\n"
                                "    <<python\n"
                                "    print(\"Hello\")\n"
                                "    for i in range(3):\n"
                                "        print(f\"Count: {i}\")\n"
                                "    True  # ← or \"ok\", 1, etc.\n"
                                "    >>\n\n"
                                "  ✓ Or capture and return data:\n"
                                "    let count = <<python\n"
                                "    sum([1, 2, 3, 4, 5])  # ← Returns 15\n"
                                "    >>\n\n"
                                "  Common issues:\n"
                                "  - Last line is None (use True, \"ok\", 1, etc. instead)\n"
                                "  - Last line is inside an if/else/for/while block\n"
                                "  - Last line is an assignment (use variable name on next line)\n"
                                "  - Function returns None instead of a value\n"
                            );
                        }
                    } else {
                        // No '_' variable captured - last line wasn't an expression
                        fmt::print("[WARN] Python block has no return value (no '_' variable)\n");
                        throw std::runtime_error(
                            "Python block has no return value\n\n"
                            "Help: The last line of your Python block must be an EXPRESSION (not a statement).\n"
                            "NAAb captures the last expression's value and returns it.\n\n"
                            "  ✗ Wrong - last line is a statement:\n"
                            "    <<python\n"
                            "    import json\n"
                            "    data = {\"key\": \"value\"}  # ← Assignment (statement)\n"
                            "    # No return value!\n"
                            "    >>\n\n"
                            "  ✓ Correct - add expression on last line:\n"
                            "    <<python\n"
                            "    import json\n"
                            "    data = {\"key\": \"value\"}\n"
                            "    json.dumps(data)  # ← Expression (returns value)\n"
                            "    >>\n\n"
                            "  ✓ Or use the variable name directly:\n"
                            "    <<python\n"
                            "    import json\n"
                            "    result = json.dumps({\"key\": \"value\"})\n"
                            "    result  # ← Variable name is an expression\n"
                            "    >>\n\n"
                            "  For standalone blocks (side-effects only):\n"
                            "    <<python\n"
                            "    print(\"Hello, world!\")\n"
                            "    True  # ← Simple return value\n"
                            "    >>\n"
                        );
                    }
                } catch (const std::runtime_error& e) {
                    // Re-throw our helpful error messages
                    throw;
                } catch (...) {
                    // Ignore other errors (type conversion failures, etc.)
                    fmt::print("[WARN] Failed to retrieve Python block result\n");
                    return std::make_shared<interpreter::Value>();
                }
            } else {
                // Not a SyntaxError, re-throw the original error
                throw;
            }
        }
    } catch (const py::error_already_set& e) {
        // Enhanced error with code preview
        std::ostringstream oss;
        oss << "Error in Python polyglot block:\n"
            << "  Python error: " << e.what() << "\n";

        // Add code preview (first 200 chars)
        if (!code.empty()) {
            std::string preview = code.substr(0, std::min(code.size(), size_t(200)));
            oss << "  Block preview:\n";
            std::istringstream code_stream(preview);
            std::string line;
            while (std::getline(code_stream, line)) {
                oss << "    " << line << "\n";
            }
            if (code.size() > 200) {
                oss << "    ...\n";
            }
        }

        oss << "\n  Hint: Check Python syntax and indentation";

        throw std::runtime_error(oss.str());
    }
}

std::shared_ptr<interpreter::Value> PythonExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    LOG_DEBUG("[Python] Calling function: {}\n", function_name);

    // Phase 4.2.2: Push stack frame for cross-language tracing
    error::ScopedStackFrame stack_frame("python", function_name, "<python>", 0);

    // Check if function exists
    auto globals = py::globals();
    if (!globals.contains(function_name.c_str())) {
        std::ostringstream oss;
        oss << "Python execution error: Function not found\n\n";
        oss << "  Function: " << function_name << "\n\n";
        oss << "  Help:\n";
        oss << "  - The function must be defined in the Python block\n";
        oss << "  - Check function name spelling (case-sensitive)\n";
        oss << "  - Ensure function is at module level (not nested)\n";
        oss << "  - Verify the block executed successfully\n\n";
        oss << "  Example:\n";
        oss << "    ✗ Wrong: def myFunc(): ...  // called as myFunction\n";
        oss << "    ✓ Right: def myFunction(): ...  // exact match\n\n";
        oss << "  Common causes:\n";
        oss << "  - Typo in function name\n";
        oss << "  - Function defined inside another function\n";
        oss << "  - Python block failed to execute\n";
        throw std::runtime_error(oss.str());
    }

    // Phase 1 Item 9: FFI callback safety - wrap with exception boundary
    // Temporarily simplified for compilation
    auto execute_call = [this, &function_name, &args]() -> interpreter::Value {
            // Get the function object
            py::object func = py::globals()[function_name.c_str()];

            // Phase 1 Item 9: Validate function pointer
            if (!ffi::CallbackValidator::validatePointer(func.ptr())) {
                throw ffi::CallbackValidationException(
                    fmt::format("Invalid Python function pointer: {}", function_name)
                );
            }

            // Convert NAAb arguments to Python objects
            py::list py_args;
            for (const auto& arg : args) {
                py_args.append(valueToPython(arg));
            }

            // Call the function with 30-second timeout
            py::object result;
            try {
                security::ScopedTimeout timeout(30);
                result = func(*py_args);
            } catch (const security::ResourceLimitException& e) {
                fmt::print("[ERROR] Python function timeout: {}\n", e.what());
                security::AuditLogger::logTimeout("Python function: " + function_name, 30);

                std::ostringstream oss;
                oss << "Python execution error: Function call timed out\n\n";
                oss << "  Function: " << function_name << "\n";
                oss << "  Timeout limit: 30 seconds\n\n";
                oss << "  Help:\n";
                oss << "  - Python function took too long to execute\n";
                oss << "  - Check for infinite loops or blocking operations\n";
                oss << "  - Optimize algorithm complexity\n";
                oss << "  - Consider async execution for long operations\n\n";
                oss << "  Common causes:\n";
                oss << "  - Infinite while loop\n";
                oss << "  - Blocking I/O without timeout\n";
                oss << "  - CPU-intensive computation\n";
                oss << "  - Network request without timeout\n\n";
                oss << "  Example fixes:\n";
                oss << "    ✗ Wrong: while True: compute()  // never exits\n";
                oss << "    ✓ Right: for i in range(1000): compute()  // bounded\n";
                throw std::runtime_error(oss.str());
            }

            // Convert result back to NAAb Value
            auto naab_value = pythonToValue(result);
            if (naab_value) {
                return *naab_value;
            } else {
                return interpreter::Value();  // Return null if conversion fails
            }
    };

    // Execute the call directly (simplified for now)
    try {
        auto result = execute_call();
        return std::make_shared<interpreter::Value>(result);
    } catch (const std::exception& e) {
        security::AuditLogger::logSecurityViolation(
            fmt::format("Python FFI error in {}: {}", function_name, e.what())
        );
        throw;
    }
}

bool PythonExecutor::loadModule(const std::string& module_name, const std::string& code) {
    LOG_DEBUG("[Python] Loading module: {}\n", module_name);

    try {
        // Execute the module code in the global namespace
        py::exec(code, py::globals());
        LOG_DEBUG("[Python] Module {} loaded successfully\n", module_name);
        return true;
    } catch (const py::error_already_set& e) {
        fmt::print("[ERROR] Failed to load module {}: {}\n", module_name, e.what());
        return false;
    }
}

bool PythonExecutor::hasFunction(const std::string& function_name) const {
    py::gil_scoped_acquire gil;  // Acquire GIL for const method
    return py::globals().contains(function_name.c_str());
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
        else if constexpr (std::is_same_v<T, std::shared_ptr<interpreter::StructValue>>) {
            // Struct → Python object (use CrossLanguageBridge)
            CrossLanguageBridge bridge;
            return bridge.structToPython(arg);
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
