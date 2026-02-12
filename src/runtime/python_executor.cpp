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
#include "naab/python_interpreter_manager.h"  // For main interpreter management
#include <fmt/core.h>
#include <stdexcept>
#include <unordered_map>

// NOTE: PYBIND11_EMBEDDED_MODULE disabled - we now use pure C API (PythonCExecutor)
// This pybind11 module was causing Android CFI crashes at startup
// Keeping PythonExecutor class only for setBlockDangerousImports() static method
/*
PYBIND11_EMBEDDED_MODULE(naab_internal, m) {
    py::class_<naab::runtime::PythonExecutor::PythonOutputRedirector>(m, "OutputRedirector")
        .def(py::init<naab::runtime::OutputBuffer&>())
        .def("write", &naab::runtime::PythonExecutor::PythonOutputRedirector::write)
        .def("flush", &naab::runtime::PythonExecutor::PythonOutputRedirector::flush);
}
*/

namespace naab {
namespace runtime {

PythonExecutor::PythonExecutor(bool redirect_output) {
    // CRITICAL: Main interpreter must be initialized first
    if (!PythonInterpreterManager::isInitialized()) {
        throw std::runtime_error("Main Python interpreter must be initialized before creating sub-interpreters");
    }

    LOG_DEBUG("[Python] Creating sub-interpreter for TRUE isolation\n");

    // Create isolated Python sub-interpreter using C API
    // NOTE: Each sub-interpreter has:
    // - Own GIL (independent of other sub-interpreters)
    // - Own globals (__main__ module with own dict)
    // - Own sys.modules, sys.path, builtins, etc.

    sub_interpreter_ = Py_NewInterpreter();
    if (!sub_interpreter_) {
        throw std::runtime_error("Failed to create Python sub-interpreter");
    }

    LOG_DEBUG("[Python] Sub-interpreter created successfully (thread state: {})\n",
              static_cast<void*>(sub_interpreter_));

    // Py_NewInterpreter() automatically:
    // 1. Creates new thread state
    // 2. Acquires that sub-interpreter's GIL
    // 3. Makes it the current thread state
    // We are now running in the sub-interpreter's context!

    // Import standard modules into THIS sub-interpreter
    PyRun_SimpleString("import sys");
    PyRun_SimpleString("import os");

    LOG_DEBUG("[Python] Standard modules imported into sub-interpreter\n");

    // TODO: Implement output redirection using C API
    // For now, output redirection is disabled in sub-interpreters
    // (Cannot use py::object - need PyObject* and manual ref counting)
    if (redirect_output) {
        LOG_DEBUG("[Python] Output redirection not yet implemented for sub-interpreters\n");
    }

    LOG_DEBUG("[Python] Sub-interpreter initialized successfully\n");
}

PythonExecutor::~PythonExecutor() {
    if (sub_interpreter_) {
        LOG_DEBUG("[Python] Destroying sub-interpreter (thread state: {})\n",
                  static_cast<void*>(sub_interpreter_));

        // Switch to this sub-interpreter's context
        // This is necessary because the current thread state might not be our sub-interpreter
        PyThreadState* old_state = PyThreadState_Swap(sub_interpreter_);

        // Clean up and destroy the sub-interpreter
        // NOTE: This also releases the sub-interpreter's GIL
        Py_EndInterpreter(sub_interpreter_);
        sub_interpreter_ = nullptr;

        // Restore previous thread state (likely main interpreter or another sub-interpreter)
        PyThreadState_Swap(old_state);

        LOG_DEBUG("[Python] Sub-interpreter destroyed successfully\n");
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

    // NOTE: Output redirection not yet implemented for sub-interpreters
    // TODO: Implement C API-based output redirection if needed
    // For now, Python output goes to standard stdout/stderr

    // Enterprise Security: Block dangerous imports when enabled
    // NOTE: Due to threading in parallel execution, we can't rely on ScopedSandbox
    // thread-local storage. Instead, use static flag set by main.cpp.
    std::string final_code = code;  // Make mutable copy

    // Check if import blocking is enabled (controlled via setBlockDangerousImports())
    if (PythonExecutor::shouldBlockDangerousImports()) {
        std::string security_prefix =
            "import sys\n"
            "# Security: Block dangerous modules\n"
            "_blocked = ['os', 'subprocess', 'commands', 'pty', 'fcntl', 'multiprocessing', 'threading', 'ctypes']\n"
            "for _m in _blocked:\n"
            "    if _m in sys.modules: del sys.modules[_m]\n"
            "    sys.modules[_m] = None\n"
            "del _blocked, _m\n\n";
        final_code = security_prefix + code;
    }

    try {
        // Execute with configurable timeout (default 30 seconds)
        security::ScopedTimeout timeout(static_cast<unsigned int>(timeout_seconds_));

        // SUBINTERPRETER: Switch to this sub-interpreter's context
        PyThreadState* old_state = PyThreadState_Swap(sub_interpreter_);

        // Execute in THIS sub-interpreter's isolated globals
        PyRun_SimpleString(final_code.c_str());

        // Restore previous thread state
        PyThreadState_Swap(old_state);

        LOG_DEBUG("[Python] Executed code successfully in sub-interpreter\n");
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

    // Enterprise Security: Install signal handlers for resource limits (once)
    if (!security::ResourceLimiter::isInitialized()) {
        security::ResourceLimiter::installSignalHandlers();
    }

    // Enterprise Security: Get limits from sandbox config (if active)
    unsigned int timeout = 30;  // Default: 30 seconds
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox) {
        timeout = sandbox->getConfig().max_cpu_seconds;
    }

    // Enterprise Security: Apply CPU timeout
    // NOTE: Memory limits are NOT applied to Python due to RLIMIT_AS being
    // incompatible with Python's memory allocator architecture. Python's
    // allocator pre-allocates virtual address space for memory pools, which
    // counts against RLIMIT_AS even when not used, causing MemoryError on
    // basic operations like creating empty lists.
    //
    // SECURITY TRADE-OFF: Python blocks can allocate large amounts of memory.
    // Mitigation: CPU timeout (30s default) still prevents infinite loops and
    // limits total work. For stricter control, use subprocess-based executors.
    security::ScopedTimeout scoped_timeout(timeout);

    // Enterprise Security: Block dangerous imports when enabled
    // NOTE: Due to threading in parallel execution, we can't rely on ScopedSandbox
    // thread-local storage. Instead, use static flag set by main.cpp.
    std::string final_code = code;  // Make mutable copy

    // Check if import blocking is enabled (controlled via setBlockDangerousImports())
    if (PythonExecutor::shouldBlockDangerousImports()) {
        std::string security_prefix =
            "import sys\n"
            "# Security: Block dangerous modules\n"
            "_blocked = ['os', 'subprocess', 'commands', 'pty', 'fcntl', 'multiprocessing', 'threading', 'ctypes']\n"
            "for _m in _blocked:\n"
            "    if _m in sys.modules: del sys.modules[_m]\n"
            "    sys.modules[_m] = None\n"
            "del _blocked, _m\n\n";
        final_code = security_prefix + code;
    }

    // SUBINTERPRETER: Declare old_state here so catch blocks can access it
    PyThreadState* old_state = nullptr;

    try {
        // Issue #3/#5/#7 Fix: Wrap code in function if it contains 'return' statement
        // This allows return statements, multi-line dicts, and with open(...) with return to work
        if (final_code.find("return ") != std::string::npos || final_code.find("return\n") != std::string::npos) {
            LOG_DEBUG("[Python] Code contains 'return', wrapping in function\n");

            // Wrap code in a function and immediately call it
            std::string wrapped = "def __naab_wrapper():\n";

            // Indent all lines of the original code
            std::vector<std::string> code_lines;
            std::istringstream stream(final_code);
            std::string line;
            while (std::getline(stream, line)) {
                code_lines.push_back(line);
            }

            // Find the last non-empty, non-comment line to add 'return' if it's an expression
            int last_expr_idx = -1;
            for (int i = static_cast<int>(code_lines.size()) - 1; i >= 0; i--) {
                std::string trimmed = code_lines[static_cast<size_t>(i)];
                // Remove leading/trailing whitespace
                size_t start = trimmed.find_first_not_of(" \t");
                if (start == std::string::npos) continue;  // Empty line
                trimmed = trimmed.substr(start);

                // Skip comments
                if (trimmed[0] == '#') continue;

                // Check if it's a statement (not an expression)
                if (trimmed.find("return ") == 0 || trimmed.find("if ") == 0 ||
                    trimmed.find("for ") == 0 || trimmed.find("while ") == 0 ||
                    trimmed.find("def ") == 0 || trimmed.find("class ") == 0 ||
                    trimmed.find("import ") == 0 || trimmed.find("from ") == 0 ||
                    trimmed.find("break") == 0 || trimmed.find("continue") == 0 ||
                    trimmed.find("pass") == 0) {
                    // It's a statement, don't add return
                    last_expr_idx = -2;  // Signal: don't add return
                    break;
                }

                // Found a potential expression line
                last_expr_idx = i;
                break;
            }

            // Add all lines with proper indentation
            for (size_t i = 0; i < code_lines.size(); i++) {
                if (static_cast<int>(i) == last_expr_idx && last_expr_idx >= 0) {
                    // Add 'return' before the last expression
                    wrapped += "    return " + code_lines[i] + "\n";
                } else {
                    wrapped += "    " + code_lines[i] + "\n";
                }
            }

            // Call the function and store result
            wrapped += "_ = __naab_wrapper()\n";

            try {
                // SUBINTERPRETER: Switch to this sub-interpreter's context
                old_state = PyThreadState_Swap(sub_interpreter_);

                // Get __main__ module's dict (sub-interpreter's globals)
                PyObject* main_module = PyImport_AddModule("__main__");
                PyObject* globals = PyModule_GetDict(main_module);

                // Execute wrapped code in sub-interpreter
                PyRun_SimpleString(wrapped.c_str());

                // Get the result from _ variable
                PyObject* result_key = PyUnicode_FromString("_");
                PyObject* result = PyDict_GetItem(globals, result_key);
                Py_DECREF(result_key);

                std::shared_ptr<interpreter::Value> naab_value;
                if (result && result != Py_None) {
                    naab_value = pyObjectToValue(result);
                } else {
                    naab_value = std::make_shared<interpreter::Value>();
                }

                // Restore previous thread state
                PyThreadState_Swap(old_state);

                return naab_value;
            } catch (const std::exception& e) {
                // If wrapping failed, restore state and rethrow
                PyThreadState_Swap(old_state);
                throw;
            }
        }

        // Note: Timeout is now handled by AsyncCallbackWrapper for async execution
        // For blocking execution, the wrapper also provides timeout control

        // SUBINTERPRETER: Switch to this sub-interpreter's context
        old_state = PyThreadState_Swap(sub_interpreter_);

        // Get __main__ module's dict (sub-interpreter's globals)
        PyObject* main_module = PyImport_AddModule("__main__");
        if (!main_module) {
            PyThreadState_Swap(old_state);
            throw std::runtime_error("Python C API: Failed to get __main__ module");
        }
        PyObject* globals = PyModule_GetDict(main_module);  // Borrowed reference
        if (!globals) {
            PyThreadState_Swap(old_state);
            throw std::runtime_error("Python C API: Failed to get globals dict");
        }

        // Try eval() first for simple expressions (backwards compatible)
        PyObject* result = PyRun_String(final_code.c_str(), Py_eval_input, globals, globals);

        if (result) {
            // Eval succeeded - convert and return
            auto naab_value = pyObjectToValue(result);
            Py_DECREF(result);
            PyThreadState_Swap(old_state);
            return naab_value;
        }

        // Eval failed - check if it's a SyntaxError (likely multi-line statements)
        if (PyErr_Occurred()) {
            PyObject *ptype, *pvalue, *ptraceback;
            PyErr_Fetch(&ptype, &pvalue, &ptraceback);

            // Check if it's a SyntaxError
            bool is_syntax_error = false;
            if (ptype) {
                PyObject* type_str = PyObject_Str(ptype);
                if (type_str) {
                    const char* type_cstr = PyUnicode_AsUTF8(type_str);
                    if (type_cstr && std::string(type_cstr).find("SyntaxError") != std::string::npos) {
                        is_syntax_error = true;
                    }
                    Py_DECREF(type_str);
                }
            }

            if (is_syntax_error) {
                // Fall back to exec() for multi-line statements
                LOG_DEBUG("[Python] eval() failed, trying exec() for multi-line code\n");

                // Clear the error
                Py_XDECREF(ptype);
                Py_XDECREF(pvalue);
                Py_XDECREF(ptraceback);

                // Phase 2.3: Auto-capture last expression value
                // Strategy: Find the last non-indented statement and prepend `_ = ` to capture its value
                std::string modified_code = final_code;
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

                // Use exec() for multi-line statements (C API)
                PyObject* exec_result = PyRun_String(modified_code.c_str(), Py_file_input, globals, globals);
                if (!exec_result) {
                    // Exec failed - restore error and throw
                    PyThreadState_Swap(old_state);

                    // Get error message
                    PyObject *exec_ptype, *exec_pvalue, *exec_ptraceback;
                    PyErr_Fetch(&exec_ptype, &exec_pvalue, &exec_ptraceback);
                    std::string error_msg = "Unknown Python error";
                    if (exec_pvalue) {
                        PyObject* str_obj = PyObject_Str(exec_pvalue);
                        if (str_obj) {
                            const char* error_cstr = PyUnicode_AsUTF8(str_obj);
                            if (error_cstr) {
                                error_msg = error_cstr;
                            }
                            Py_DECREF(str_obj);
                        }
                    }
                    Py_XDECREF(exec_ptype);
                    Py_XDECREF(exec_pvalue);
                    Py_XDECREF(exec_ptraceback);

                    throw std::runtime_error("Python exec() failed: " + error_msg);
                }
                Py_DECREF(exec_result);

                // Try to get the result from the `_` variable
                try {
                    PyObject* underscore_key = PyUnicode_FromString("_");
                    if (!underscore_key) {
                        PyThreadState_Swap(old_state);
                        throw std::runtime_error("Python C API: Failed to create '_' key");
                    }

                    // Check if '_' exists in globals
                    if (PyDict_Contains(globals, underscore_key) == 1) {
                        PyObject* underscore_val = PyDict_GetItem(globals, underscore_key);  // Borrowed reference
                        Py_DECREF(underscore_key);

                        if (underscore_val && underscore_val != Py_None) {
                            auto naab_value = pyObjectToValue(underscore_val);
                            PyThreadState_Swap(old_state);
                            return naab_value;
                        } else {
                            // Python block returned None explicitly
                            Py_DECREF(underscore_key);
                            PyThreadState_Swap(old_state);
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
                        Py_DECREF(underscore_key);
                        PyThreadState_Swap(old_state);
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
                    PyThreadState_Swap(old_state);
                    return std::make_shared<interpreter::Value>();
                }
            } else {
                // Not a SyntaxError - restore error and throw
                PyThreadState_Swap(old_state);

                // Get error message from pvalue
                std::string error_msg = "Unknown Python error";
                if (pvalue) {
                    PyObject* str_obj = PyObject_Str(pvalue);
                    if (str_obj) {
                        const char* error_cstr = PyUnicode_AsUTF8(str_obj);
                        if (error_cstr) {
                            error_msg = error_cstr;
                        }
                        Py_DECREF(str_obj);
                    }
                }

                // Clean up error objects
                Py_XDECREF(ptype);
                Py_XDECREF(pvalue);
                Py_XDECREF(ptraceback);

                // Enhanced error with code preview
                std::ostringstream oss;
                oss << "Error in Python polyglot block:\n"
                    << "  Python error: " << error_msg << "\n";

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
    } catch (const std::runtime_error& e) {
        // Re-throw runtime errors (including our helpful messages)
        throw;
    } catch (const std::exception& e) {
        // Catch any other standard exceptions
        PyThreadState_Swap(old_state);

        std::ostringstream oss;
        oss << "Error in Python polyglot block:\n"
            << "  C++ error: " << e.what() << "\n";

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
    } catch (...) {
        // Catch all other exceptions
        PyThreadState_Swap(old_state);
        throw std::runtime_error("Unknown error in Python polyglot block");
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
        try {
            // Try to cast to int32 first
            int int_val = obj.cast<int>();
            return std::make_shared<interpreter::Value>(int_val);
        } catch (const py::cast_error&) {
            // Integer too large for int32, convert to double instead
            // This handles Python's arbitrary precision integers
            try {
                double double_val = obj.cast<double>();
                return std::make_shared<interpreter::Value>(double_val);
            } catch (const py::cast_error&) {
                // If even double conversion fails, convert to string representation
                std::string str_repr = py::str(obj).cast<std::string>();
                throw std::runtime_error(
                    "Python integer too large to convert\n\n"
                    "  Value: " + str_repr + "\n\n"
                    "  Help:\n"
                    "  - NAAb uses 32-bit integers (range: -2,147,483,648 to 2,147,483,647)\n"
                    "  - Values outside this range are converted to float (may lose precision)\n"
                    "  - For very large integers, consider using strings or breaking into smaller values\n"
                );
            }
        }
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

void PythonExecutor::extractPythonTraceback() {
    // Extract traceback from Python's error state
    // Exception info fetched from Python's global error state

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

// ============================================================================
// C API Type Conversion Helpers (for sub-interpreters)
// ============================================================================
// These functions replace pybind11 conversions when using Python C API
// Manual reference counting required (Py_INCREF/Py_DECREF/Py_XDECREF)

std::shared_ptr<interpreter::Value> PythonExecutor::pyObjectToValue(PyObject* obj) {
    if (!obj) {
        throw std::runtime_error("Python C API: Cannot convert NULL PyObject to Value");
    }

    // None -> monostate (null)
    if (obj == Py_None) {
        return std::make_shared<interpreter::Value>();
    }

    // Bool -> bool
    if (PyBool_Check(obj)) {
        bool val = (obj == Py_True);
        return std::make_shared<interpreter::Value>(val);
    }

    // Int -> int (with overflow handling to double)
    if (PyLong_Check(obj)) {
        // Try int32 first
        long long_val = PyLong_AsLong(obj);
        if (long_val == -1 && PyErr_Occurred()) {
            // Overflow or other error
            PyErr_Clear();
            // Try converting to double instead
            double double_val = PyLong_AsDouble(obj);
            if (double_val == -1.0 && PyErr_Occurred()) {
                PyErr_Clear();
                throw std::runtime_error("Python C API: Integer too large to convert");
            }
            return std::make_shared<interpreter::Value>(double_val);
        }
        // Check if fits in int32 range
        if (long_val > INT_MAX || long_val < INT_MIN) {
            // Convert to double for large integers
            double double_val = static_cast<double>(long_val);
            return std::make_shared<interpreter::Value>(double_val);
        }
        int int_val = static_cast<int>(long_val);
        return std::make_shared<interpreter::Value>(int_val);
    }

    // Float -> double
    if (PyFloat_Check(obj)) {
        double val = PyFloat_AsDouble(obj);
        if (val == -1.0 && PyErr_Occurred()) {
            PyErr_Clear();
            throw std::runtime_error("Python C API: Failed to convert float");
        }
        return std::make_shared<interpreter::Value>(val);
    }

    // String -> string
    if (PyUnicode_Check(obj)) {
        Py_ssize_t size;
        const char* data = PyUnicode_AsUTF8AndSize(obj, &size);
        if (!data) {
            PyErr_Clear();
            throw std::runtime_error("Python C API: Failed to convert unicode string");
        }
        std::string val(data, size);
        return std::make_shared<interpreter::Value>(val);
    }

    // List -> vector (recursive)
    if (PyList_Check(obj)) {
        Py_ssize_t size = PyList_Size(obj);
        std::vector<std::shared_ptr<interpreter::Value>> vec;
        vec.reserve(size);
        for (Py_ssize_t i = 0; i < size; ++i) {
            PyObject* item = PyList_GetItem(obj, i);  // Borrowed reference
            if (!item) {
                PyErr_Clear();
                throw std::runtime_error("Python C API: Failed to get list item");
            }
            vec.push_back(pyObjectToValue(item));  // Recursive conversion
        }
        return std::make_shared<interpreter::Value>(vec);
    }

    // Tuple -> vector (recursive, treat as list in NAAb)
    if (PyTuple_Check(obj)) {
        Py_ssize_t size = PyTuple_Size(obj);
        std::vector<std::shared_ptr<interpreter::Value>> vec;
        vec.reserve(size);
        for (Py_ssize_t i = 0; i < size; ++i) {
            PyObject* item = PyTuple_GetItem(obj, i);  // Borrowed reference
            if (!item) {
                PyErr_Clear();
                throw std::runtime_error("Python C API: Failed to get tuple item");
            }
            vec.push_back(pyObjectToValue(item));  // Recursive conversion
        }
        return std::make_shared<interpreter::Value>(vec);
    }

    // Dict -> unordered_map (recursive)
    if (PyDict_Check(obj)) {
        std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> map;
        PyObject* key;
        PyObject* value;
        Py_ssize_t pos = 0;

        while (PyDict_Next(obj, &pos, &key, &value)) {  // Borrowed references
            // Key must be string
            if (!PyUnicode_Check(key)) {
                throw std::runtime_error("Python C API: Dictionary keys must be strings");
            }

            Py_ssize_t key_size;
            const char* key_data = PyUnicode_AsUTF8AndSize(key, &key_size);
            if (!key_data) {
                PyErr_Clear();
                throw std::runtime_error("Python C API: Failed to convert dict key");
            }
            std::string key_str(key_data, key_size);

            map[key_str] = pyObjectToValue(value);  // Recursive conversion
        }

        return std::make_shared<interpreter::Value>(map);
    }

    // Unknown type -> try converting to string representation
    PyObject* str_obj = PyObject_Str(obj);
    if (str_obj) {
        Py_ssize_t size;
        const char* data = PyUnicode_AsUTF8AndSize(str_obj, &size);
        if (data) {
            std::string val(data, size);
            Py_DECREF(str_obj);
            return std::make_shared<interpreter::Value>(val);
        }
        Py_DECREF(str_obj);
    }
    PyErr_Clear();

    throw std::runtime_error("Python C API: Unsupported Python type for conversion to NAAb Value");
}

PyObject* PythonExecutor::valueToPyObject(const std::shared_ptr<interpreter::Value>& val) {
    if (!val) {
        Py_RETURN_NONE;
    }

    return std::visit([this](auto&& arg) -> PyObject* {
        using T = std::decay_t<decltype(arg)>;

        // monostate (null) -> None
        if constexpr (std::is_same_v<T, std::monostate>) {
            Py_RETURN_NONE;
        }
        // int -> PyLong
        else if constexpr (std::is_same_v<T, int>) {
            return PyLong_FromLong(static_cast<long>(arg));
        }
        // double -> PyFloat
        else if constexpr (std::is_same_v<T, double>) {
            return PyFloat_FromDouble(arg);
        }
        // bool -> PyBool
        else if constexpr (std::is_same_v<T, bool>) {
            if (arg) {
                Py_RETURN_TRUE;
            } else {
                Py_RETURN_FALSE;
            }
        }
        // string -> PyUnicode
        else if constexpr (std::is_same_v<T, std::string>) {
            return PyUnicode_FromStringAndSize(arg.c_str(), arg.size());
        }
        // vector -> PyList (recursive)
        else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            PyObject* list = PyList_New(arg.size());
            if (!list) {
                return nullptr;
            }
            for (size_t i = 0; i < arg.size(); ++i) {
                PyObject* item = this->valueToPyObject(arg[i]);  // Recursive conversion
                if (!item) {
                    Py_DECREF(list);
                    return nullptr;
                }
                PyList_SET_ITEM(list, i, item);  // Steals reference to item
            }
            return list;
        }
        // unordered_map -> PyDict (recursive)
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
            PyObject* dict = PyDict_New();
            if (!dict) {
                return nullptr;
            }
            for (const auto& [key, value] : arg) {
                PyObject* py_key = PyUnicode_FromStringAndSize(key.c_str(), key.size());
                if (!py_key) {
                    Py_DECREF(dict);
                    return nullptr;
                }
                PyObject* py_value = this->valueToPyObject(value);  // Recursive conversion
                if (!py_value) {
                    Py_DECREF(py_key);
                    Py_DECREF(dict);
                    return nullptr;
                }
                int result = PyDict_SetItem(dict, py_key, py_value);
                Py_DECREF(py_key);
                Py_DECREF(py_value);
                if (result != 0) {
                    Py_DECREF(dict);
                    return nullptr;
                }
            }
            return dict;
        }
        // StructValue -> Not directly supported in C API, return None
        // (Could use CrossLanguageBridge if needed, but for now keep it simple)
        else if constexpr (std::is_same_v<T, interpreter::StructValue>) {
            // TODO: If struct support is needed, implement via CrossLanguageBridge
            Py_RETURN_NONE;
        }
        // FunctionValue -> Not supported, return None
        else if constexpr (std::is_same_v<T, interpreter::FunctionValue>) {
            Py_RETURN_NONE;
        }
        // BlockValue (shared_ptr) -> Not supported, return None
        else if constexpr (std::is_same_v<T, std::shared_ptr<interpreter::BlockValue>>) {
            Py_RETURN_NONE;
        }
        // PythonObjectValue (shared_ptr) -> Not supported, return None
        else if constexpr (std::is_same_v<T, std::shared_ptr<interpreter::PythonObjectValue>>) {
            Py_RETURN_NONE;
        }
        // Unknown type -> None
        else {
            Py_RETURN_NONE;
        }
    }, val->data);
}

} // namespace runtime
} // namespace naab
