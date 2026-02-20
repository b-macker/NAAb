/**
 * C++ Adapter for Python C API Wrapper - Implementation
 *
 * Uses python_c_gil_acquire/release for safe GIL management.
 * On worker threads, this uses pre-created PyThreadState + PyEval_RestoreThread
 * instead of PyGILState_Ensure (which crashes on Android with bionic CFI).
 */

#include "naab/python_c_executor.h"
#include "naab/interpreter.h"
#include <stdexcept>
#include <sstream>
#include <string>
#include <algorithm>

namespace naab {
namespace runtime {

// NOTE: Don't use 'using namespace interpreter' - causes type conflicts
// Use fully qualified names instead

/**
 * Execute Python code (statement mode)
 */
void PythonCExecutor::execute(const std::string& code) {
    PythonCResult result = python_c_execute(code.c_str());

    if (!result.success) {
        std::string error_msg = result.error_message ? result.error_message : "Unknown error";
        python_c_free_result(&result);
        throw std::runtime_error("Python execution error: " + error_msg);
    }

    python_c_free_result(&result);
}

/**
 * Helper: trim trailing whitespace from a string
 */
static std::string trimRight(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

/**
 * Helper: split code into "all lines except last expression" and "last expression"
 * Returns true if code was successfully split, false if it's a single expression
 */
static bool splitStatementsAndLastExpr(const std::string& code,
                                        std::string& statements_out,
                                        std::string& last_expr_out) {
    // Split code into lines
    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Remove trailing empty lines
    while (!lines.empty() && trimRight(lines.back()).empty()) {
        lines.pop_back();
    }

    if (lines.empty()) return false;
    if (lines.size() == 1) return false;  // Single line = single expression

    // Last non-empty line is the expression to return
    last_expr_out = trimRight(lines.back());

    // Everything before is statements to exec
    statements_out.clear();
    for (size_t i = 0; i + 1 < lines.size(); i++) {
        statements_out += lines[i] + "\n";
    }

    return true;
}

/**
 * Execute Python expression and return result
 *
 * Handles both single expressions (e.g., "10 + 20") and multi-line code
 * (e.g., "x = 10\ny = 20\nx + y") by:
 * 1. First trying Py_eval_input (single expression mode)
 * 2. If SyntaxError, splitting into statements + last expression:
 *    - Execute all statements with Py_file_input (exec mode)
 *    - Evaluate last line with Py_eval_input (eval mode) to get return value
 *
 * Uses python_c_gil_acquire/release for safe GIL management.
 * On worker threads: uses pre-created PyThreadState + PyEval_RestoreThread
 * On main thread: falls back to PyGILState_Ensure
 */
std::shared_ptr<interpreter::Value> PythonCExecutor::executeWithReturn(const std::string& code) {
    // Acquire GIL safely (pre-created state on workers, PyGILState on main)
    int gil_handle = python_c_gil_acquire();

    // Get __main__ module and globals
    PyObject* main_module = PyImport_AddModule("__main__");
    if (!main_module) {
        python_c_gil_release(gil_handle);
        throw std::runtime_error("Python execution error: Failed to get __main__ module");
    }

    PyObject* globals = PyModule_GetDict(main_module);
    if (!globals) {
        python_c_gil_release(gil_handle);
        throw std::runtime_error("Python execution error: Failed to get globals dict");
    }

    // Step 1: Try evaluating as a single expression (Py_eval_input)
    PyObject* py_result = PyRun_String(code.c_str(), Py_eval_input, globals, globals);

    if (!py_result) {
        // Check if it's a SyntaxError (meaning multi-line code, not a real error)
        PyObject *ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);

        bool is_syntax_error = ptype && PyErr_GivenExceptionMatches(ptype, PyExc_SyntaxError);

        Py_XDECREF(ptype);
        Py_XDECREF(pvalue);
        Py_XDECREF(ptraceback);

        if (!is_syntax_error) {
            // Not a SyntaxError - re-raise as runtime error
            // Re-evaluate to get the error message cleanly
            py_result = PyRun_String(code.c_str(), Py_eval_input, globals, globals);
            if (!py_result) {
                PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                std::string error_msg = "Unknown Python error";
                if (pvalue) {
                    PyObject* str_exc = PyObject_Str(pvalue);
                    if (str_exc) {
                        const char* err = PyUnicode_AsUTF8(str_exc);
                        if (err) error_msg = err;
                        Py_DECREF(str_exc);
                    }
                }
                Py_XDECREF(ptype);
                Py_XDECREF(pvalue);
                Py_XDECREF(ptraceback);
                python_c_gil_release(gil_handle);
                throw std::runtime_error("Python execution error: " + error_msg);
            }
        } else {
            // SyntaxError from eval = multi-line code
            // Step 2: Split into statements + last expression
            std::string statements, last_expr;
            if (splitStatementsAndLastExpr(code, statements, last_expr)) {
                // Execute all statements (assignments, imports, etc.)
                PyObject* exec_result = PyRun_String(statements.c_str(), Py_file_input, globals, globals);
                if (!exec_result) {
                    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                    std::string error_msg = "Unknown Python error";
                    if (pvalue) {
                        PyObject* str_exc = PyObject_Str(pvalue);
                        if (str_exc) {
                            const char* err = PyUnicode_AsUTF8(str_exc);
                            if (err) error_msg = err;
                            Py_DECREF(str_exc);
                        }
                    }
                    Py_XDECREF(ptype);
                    Py_XDECREF(pvalue);
                    Py_XDECREF(ptraceback);
                    python_c_gil_release(gil_handle);
                    throw std::runtime_error("Python execution error: " + error_msg);
                }
                Py_DECREF(exec_result);

                // Evaluate last expression to get return value
                py_result = PyRun_String(last_expr.c_str(), Py_eval_input, globals, globals);
                if (!py_result) {
                    // Last line might also be a statement (not an expression)
                    // Try executing it and return None
                    PyErr_Clear();
                    exec_result = PyRun_String(last_expr.c_str(), Py_file_input, globals, globals);
                    if (!exec_result) {
                        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                        std::string error_msg = "Unknown Python error";
                        if (pvalue) {
                            PyObject* str_exc = PyObject_Str(pvalue);
                            if (str_exc) {
                                const char* err = PyUnicode_AsUTF8(str_exc);
                                if (err) error_msg = err;
                                Py_DECREF(str_exc);
                            }
                        }
                        Py_XDECREF(ptype);
                        Py_XDECREF(pvalue);
                        Py_XDECREF(ptraceback);
                        python_c_gil_release(gil_handle);
                        throw std::runtime_error("Python execution error: " + error_msg);
                    }
                    Py_DECREF(exec_result);

                    // Return None for statement-only blocks
                    python_c_gil_release(gil_handle);
                    return std::make_shared<interpreter::Value>();
                }
            } else {
                // Can't split (single line that's not an expression)
                // Try exec mode and return None
                PyObject* exec_result = PyRun_String(code.c_str(), Py_file_input, globals, globals);
                if (!exec_result) {
                    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                    std::string error_msg = "Unknown Python error";
                    if (pvalue) {
                        PyObject* str_exc = PyObject_Str(pvalue);
                        if (str_exc) {
                            const char* err = PyUnicode_AsUTF8(str_exc);
                            if (err) error_msg = err;
                            Py_DECREF(str_exc);
                        }
                    }
                    Py_XDECREF(ptype);
                    Py_XDECREF(pvalue);
                    Py_XDECREF(ptraceback);
                    python_c_gil_release(gil_handle);
                    throw std::runtime_error("Python execution error: " + error_msg);
                }
                Py_DECREF(exec_result);

                python_c_gil_release(gil_handle);
                return std::make_shared<interpreter::Value>();
            }
        }
    }

    // Convert PyObject to NAAb Value (still holding GIL)
    std::shared_ptr<interpreter::Value> value = pyObjectToValue(py_result);

    // Release the Python object
    Py_DECREF(py_result);

    // Release GIL
    python_c_gil_release(gil_handle);

    return value;
}

/**
 * Convert PyObject* to NAAb Value
 */
std::shared_ptr<interpreter::Value> PythonCExecutor::pyObjectToValue(PyObject* obj) {
    if (!obj || obj == Py_None) {
        return std::make_shared<interpreter::Value>();  // monostate (null)
    }

    // Bool (check before int, since bool is a subclass of int in Python)
    if (PyBool_Check(obj)) {
        return std::make_shared<interpreter::Value>(obj == Py_True);
    }

    // Int
    if (PyLong_Check(obj)) {
        // Try int first, fall back to double if overflow
        long long val = PyLong_AsLongLong(obj);
        if (val == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            // Overflow - convert to double
            double dval = PyLong_AsDouble(obj);
            return std::make_shared<interpreter::Value>(dval);
        }
        // Check if it fits in int32
        if (val >= INT32_MIN && val <= INT32_MAX) {
            return std::make_shared<interpreter::Value>(static_cast<int>(val));
        } else {
            // Too large for int32, use double
            return std::make_shared<interpreter::Value>(static_cast<double>(val));
        }
    }

    // Float
    if (PyFloat_Check(obj)) {
        return std::make_shared<interpreter::Value>(PyFloat_AsDouble(obj));
    }

    // String
    if (PyUnicode_Check(obj)) {
        const char* str = PyUnicode_AsUTF8(obj);
        return std::make_shared<interpreter::Value>(std::string(str ? str : ""));
    }

    // List
    if (PyList_Check(obj)) {
        std::vector<std::shared_ptr<interpreter::Value>> vec;
        Py_ssize_t size = PyList_Size(obj);
        vec.reserve(size);

        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject* item = PyList_GetItem(obj, i);  // Borrowed reference
            vec.push_back(pyObjectToValue(item));
        }

        return std::make_shared<interpreter::Value>(std::move(vec));
    }

    // Tuple (convert to list)
    if (PyTuple_Check(obj)) {
        std::vector<std::shared_ptr<interpreter::Value>> vec;
        Py_ssize_t size = PyTuple_Size(obj);
        vec.reserve(size);

        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject* item = PyTuple_GetItem(obj, i);  // Borrowed reference
            vec.push_back(pyObjectToValue(item));
        }

        return std::make_shared<interpreter::Value>(std::move(vec));
    }

    // Dict
    if (PyDict_Check(obj)) {
        std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> map;

        PyObject *key, *value;
        Py_ssize_t pos = 0;

        while (PyDict_Next(obj, &pos, &key, &value)) {  // Borrowed references
            // Convert key to string
            if (!PyUnicode_Check(key)) {
                throw std::runtime_error("Dictionary keys must be strings");
            }

            const char* key_str = PyUnicode_AsUTF8(key);
            if (!key_str) {
                throw std::runtime_error("Failed to convert dictionary key to string");
            }

            map[key_str] = pyObjectToValue(value);
        }

        return std::make_shared<interpreter::Value>(std::move(map));
    }

    // Unsupported type - wrap in PythonObjectValue
    Py_INCREF(obj);  // PythonObjectValue will own this reference
    return std::make_shared<interpreter::Value>(std::make_shared<naab::interpreter::PythonObjectValue>(obj));
}

/**
 * Convert NAAb Value to PyObject*
 */
PyObject* PythonCExecutor::valueToPyObject(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([this](auto&& arg) -> PyObject* {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, std::monostate>) {
            // Null
            Py_RETURN_NONE;
        }
        else if constexpr (std::is_same_v<T, int>) {
            return PyLong_FromLong(arg);
        }
        else if constexpr (std::is_same_v<T, double>) {
            return PyFloat_FromDouble(arg);
        }
        else if constexpr (std::is_same_v<T, bool>) {
            if (arg) { Py_RETURN_TRUE; }
            else { Py_RETURN_FALSE; }
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            return PyUnicode_FromString(arg.c_str());
        }
        else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            PyObject* list = PyList_New(static_cast<Py_ssize_t>(arg.size()));
            for (size_t i = 0; i < arg.size(); i++) {
                PyObject* item = valueToPyObject(arg[i]);
                PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);  // Steals reference
            }
            return list;
        }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
            PyObject* dict = PyDict_New();
            for (const auto& [key, value] : arg) {
                PyObject* py_key = PyUnicode_FromString(key.c_str());
                PyObject* py_val = valueToPyObject(value);
                PyDict_SetItem(dict, py_key, py_val);
                Py_DECREF(py_key);
                Py_DECREF(py_val);
            }
            return dict;
        }
        else {
            // Unsupported type
            Py_RETURN_NONE;
        }
    }, val->data);
}

/**
 * Call a Python function by name (STUB)
 */
std::shared_ptr<interpreter::Value> PythonCExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args
) {
    throw std::runtime_error("PythonCExecutor::callFunction() not yet implemented for C API");
}

/**
 * Get captured output (STUB)
 */
std::string PythonCExecutor::getCapturedOutput() {
    return "";
}

} // namespace runtime
} // namespace naab
