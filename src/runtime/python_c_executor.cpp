/**
 * C++ Adapter for Python C API Wrapper - Implementation
 *
 * Uses python_c_gil_acquire/release for safe GIL management.
 * On worker threads, this uses pre-created PyThreadState + PyEval_RestoreThread
 * instead of PyGILState_Ensure (which crashes on Android with bionic CFI).
 */

#include "naab/python_c_executor.h"
#include "naab/interpreter.h"
#include "naab/sandbox.h"
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

    // If the last line has leading whitespace, it's inside a block (for/if/while/etc.)
    // and can't be executed standalone — don't split
    if (!last_expr_out.empty() && (last_expr_out[0] == ' ' || last_expr_out[0] == '\t')) {
        return false;
    }

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
interpreter::NaabVal PythonCExecutor::executeWithReturn(const std::string& code) {
    // Sandbox gate: Python polyglot blocks require BLOCK_CALL capability
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(
            security::Capability::BLOCK_CALL)) {
        sandbox->logViolation("executePython", code.substr(0, 100),
            "BLOCK_CALL capability required");
        throw std::runtime_error(
            "Security: Python execution denied by sandbox\n\n"
            "  Python polyglot blocks require BLOCK_CALL capability.\n"
        );
    }
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

    // Redirect sys.stdout to capture print() output
    PyRun_SimpleString(
        "import sys as _naab_sys, io as _naab_io\n"
        "_naab_stdout = _naab_io.StringIO()\n"
        "_naab_old_stdout = _naab_sys.stdout\n"
        "_naab_sys.stdout = _naab_stdout\n"
    );

    // Helper lambda: restore stdout and get captured output
    auto captureAndRestoreStdout = [&globals]() -> std::string {
        PyRun_SimpleString("_naab_sys.stdout = _naab_old_stdout");
        std::string captured;
        PyObject* captured_obj = PyRun_String("_naab_stdout.getvalue()", Py_eval_input, globals, globals);
        if (captured_obj && PyUnicode_Check(captured_obj)) {
            const char* s = PyUnicode_AsUTF8(captured_obj);
            if (s) captured = s;
            // Trim trailing newline
            while (!captured.empty() && (captured.back() == '\n' || captured.back() == '\r')) {
                captured.pop_back();
            }
        }
        Py_XDECREF(captured_obj);
        PyRun_SimpleString("del _naab_stdout, _naab_old_stdout, _naab_io, _naab_sys");
        return captured;
    };

    // Helper lambda: restore stdout on error (no capture needed)
    auto restoreStdoutOnly = []() {
        PyRun_SimpleString(
            "import sys as _naab_sys\n"
            "if hasattr(_naab_sys, '_naab_old_stdout') and _naab_sys._naab_old_stdout is not None:\n"
            "    _naab_sys.stdout = _naab_sys._naab_old_stdout\n"
            "for _n in ['_naab_stdout', '_naab_old_stdout', '_naab_io', '_naab_sys']:\n"
            "    try:\n"
            "        exec(f'del {_n}', globals())\n"
            "    except: pass\n"
        );
    };

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
                if (ptype && pvalue) {
                    PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                    PyObject* str_exc = PyObject_Str(pvalue);
                    if (type_name && str_exc) {
                        error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                    } else if (str_exc) {
                        const char* err = PyUnicode_AsUTF8(str_exc);
                        if (err) error_msg = err;
                    }
                    Py_XDECREF(type_name);
                    Py_XDECREF(str_exc);
                } else if (pvalue) {
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
                restoreStdoutOnly();
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
                    // The naive split may have broken a multi-line expression
                    // (e.g., print(json.dumps({\n...\n})) split at the closing line).
                    // Check if it's a SyntaxError — if so, fall back to executing
                    // the ENTIRE code as Py_file_input instead of split exec+eval.
                    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                    bool split_syntax_error = ptype && PyErr_GivenExceptionMatches(ptype, PyExc_SyntaxError);
                    Py_XDECREF(ptype);
                    Py_XDECREF(pvalue);
                    Py_XDECREF(ptraceback);

                    if (split_syntax_error) {
                        // Naive split broke the code — execute entire code as file
                        exec_result = PyRun_String(code.c_str(), Py_file_input, globals, globals);
                        if (!exec_result) {
                            PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                            std::string error_msg = "Unknown Python error";
                            if (ptype && pvalue) {
                                PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                                PyObject* str_exc = PyObject_Str(pvalue);
                                if (type_name && str_exc) {
                                    error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                                } else if (str_exc) {
                                    const char* err = PyUnicode_AsUTF8(str_exc);
                                    if (err) error_msg = err;
                                }
                                Py_XDECREF(type_name);
                                Py_XDECREF(str_exc);
                            } else if (pvalue) {
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
                            restoreStdoutOnly();
                            python_c_gil_release(gil_handle);
                            throw std::runtime_error("Python execution error: " + error_msg);
                        }
                        Py_DECREF(exec_result);

                        // Whole-code exec succeeded — return captured stdout
                        std::string captured = captureAndRestoreStdout();
                        python_c_gil_release(gil_handle);
                        if (!captured.empty()) {
                            return interpreter::NaabVal::makeString(captured);
                        }
                        return interpreter::NaabVal::makeNull();
                    }

                    // Not a SyntaxError — real error in the statements
                    // Re-run to get clean error message
                    exec_result = PyRun_String(statements.c_str(), Py_file_input, globals, globals);
                    if (!exec_result) {
                        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                        std::string error_msg = "Unknown Python error";
                        if (ptype && pvalue) {
                            PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                            PyObject* str_exc = PyObject_Str(pvalue);
                            if (type_name && str_exc) {
                                error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                            } else if (str_exc) {
                                const char* err = PyUnicode_AsUTF8(str_exc);
                                if (err) error_msg = err;
                            }
                            Py_XDECREF(type_name);
                            Py_XDECREF(str_exc);
                        } else if (pvalue) {
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
                        restoreStdoutOnly();
                        python_c_gil_release(gil_handle);
                        throw std::runtime_error("Python execution error: " + error_msg);
                    }
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
                        if (ptype && pvalue) {
                            PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                            PyObject* str_exc = PyObject_Str(pvalue);
                            if (type_name && str_exc) {
                                error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                            } else if (str_exc) {
                                const char* err = PyUnicode_AsUTF8(str_exc);
                                if (err) error_msg = err;
                            }
                            Py_XDECREF(type_name);
                            Py_XDECREF(str_exc);
                        } else if (pvalue) {
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
                        restoreStdoutOnly();
                        python_c_gil_release(gil_handle);
                        throw std::runtime_error("Python execution error: " + error_msg);
                    }
                    Py_DECREF(exec_result);

                    // Return captured stdout as value
                    std::string captured = captureAndRestoreStdout();
                    python_c_gil_release(gil_handle);
                    if (!captured.empty()) {
                        return interpreter::NaabVal::makeString(captured);
                    }
                    return interpreter::NaabVal::makeNull();
                }
            } else {
                // Can't split (last line is indented or single statement)
                // Execute entire code as file input
                PyObject* exec_result = PyRun_String(code.c_str(), Py_file_input, globals, globals);
                if (!exec_result) {
                    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                    std::string error_msg = "Unknown Python error";
                    if (ptype && pvalue) {
                        PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                        PyObject* str_exc = PyObject_Str(pvalue);
                        if (type_name && str_exc) {
                            error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                        } else if (str_exc) {
                            const char* err = PyUnicode_AsUTF8(str_exc);
                            if (err) error_msg = err;
                        }
                        Py_XDECREF(type_name);
                        Py_XDECREF(str_exc);
                    } else if (pvalue) {
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
                    restoreStdoutOnly();
                    python_c_gil_release(gil_handle);
                    throw std::runtime_error("Python execution error: " + error_msg);
                }
                Py_DECREF(exec_result);

                // Return captured stdout as value
                std::string captured = captureAndRestoreStdout();
                python_c_gil_release(gil_handle);
                if (!captured.empty()) {
                    return interpreter::NaabVal::makeString(captured);
                }
                return interpreter::NaabVal::makeNull();
            }
        }
    }

    // Convert PyObject to NAAb Value (still holding GIL)
    interpreter::NaabVal value = pyObjectToValue(py_result);

    // Release the Python object
    Py_DECREF(py_result);

    // Check: if result is None/null, return captured stdout as value
    bool is_null = value.isNull();
    std::string captured = captureAndRestoreStdout();
    python_c_gil_release(gil_handle);

    if (is_null && !captured.empty()) {
        return interpreter::NaabVal::makeString(captured);
    }

    return value;
}

/**
 * Convert PyObject* to NAAb Value
 */
interpreter::NaabVal PythonCExecutor::pyObjectToValue(PyObject* obj) {
    if (!obj || obj == Py_None) {
        return interpreter::NaabVal::makeNull();
    }

    // Bool (check before int, since bool is a subclass of int in Python)
    if (PyBool_Check(obj)) {
        return interpreter::NaabVal::makeBool(obj == Py_True);
    }

    // Int
    if (PyLong_Check(obj)) {
        long long val = PyLong_AsLongLong(obj);
        if (val == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            double dval = PyLong_AsDouble(obj);
            return interpreter::NaabVal::makeDouble(dval);
        }
        if (val >= INT32_MIN && val <= INT32_MAX) {
            return interpreter::NaabVal::makeInt(static_cast<int>(val));
        } else {
            return interpreter::NaabVal::makeDouble(static_cast<double>(val));
        }
    }

    // Float
    if (PyFloat_Check(obj)) {
        return interpreter::NaabVal::makeDouble(PyFloat_AsDouble(obj));
    }

    // String
    if (PyUnicode_Check(obj)) {
        const char* str = PyUnicode_AsUTF8(obj);
        return interpreter::NaabVal::makeString(std::string(str ? str : ""));
    }

    // List
    if (PyList_Check(obj)) {
        std::vector<interpreter::NaabVal> vec;
        Py_ssize_t size = PyList_Size(obj);
        vec.reserve(size);

        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject* item = PyList_GetItem(obj, i);
            vec.push_back(pyObjectToValue(item));
        }

        return interpreter::NaabVal::makeList(std::move(vec));
    }

    // Tuple (convert to list)
    if (PyTuple_Check(obj)) {
        std::vector<interpreter::NaabVal> vec;
        Py_ssize_t size = PyTuple_Size(obj);
        vec.reserve(size);

        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject* item = PyTuple_GetItem(obj, i);
            vec.push_back(pyObjectToValue(item));
        }

        return interpreter::NaabVal::makeList(std::move(vec));
    }

    // Dict
    if (PyDict_Check(obj)) {
        std::unordered_map<std::string, interpreter::NaabVal> map;

        PyObject *key, *value;
        Py_ssize_t pos = 0;

        while (PyDict_Next(obj, &pos, &key, &value)) {
            if (!PyUnicode_Check(key)) {
                throw std::runtime_error("Dictionary keys must be strings");
            }

            const char* key_str = PyUnicode_AsUTF8(key);
            if (!key_str) {
                throw std::runtime_error("Failed to convert dictionary key to string");
            }

            map[key_str] = pyObjectToValue(value);
        }

        return interpreter::NaabVal::makeDict(std::move(map));
    }

    // Unsupported type - wrap in PythonObjectValue
    Py_INCREF(obj);
    return interpreter::NaabVal::makePythonObject(
        std::make_shared<naab::interpreter::PythonObjectValue>(obj));
}

/**
 * Convert NAAb Value to PyObject*
 */
PyObject* PythonCExecutor::valueToPyObject(const interpreter::NaabVal& val, int depth) {
    // V-RT-005: prevent stack overflow on deeply-nested structures.
    // Same depth limit as V-VM-002 (serializeForLanguage): 64 levels maximum.
    if (depth > 64) {
        PyErr_SetString(PyExc_ValueError,
            "NAAb marshalling error: nested structure exceeds maximum depth (64). "
            "Flatten the structure before passing it to a Python block.");
        return nullptr;
    }
    if (val.isNull()) { Py_RETURN_NONE; }
    if (val.isBool()) {
        if (val.asBool()) { Py_RETURN_TRUE; }
        else { Py_RETURN_FALSE; }
    }
    if (val.isInt()) { return PyLong_FromLong(val.asInt()); }
    if (val.isDouble()) { return PyFloat_FromDouble(val.asDouble()); }
    if (val.isString()) { return PyUnicode_FromString(val.asString().c_str()); }
    if (val.isList()) {
        const auto& arr = val.asListConst();
        PyObject* list = PyList_New(static_cast<Py_ssize_t>(arr.size()));
        for (size_t i = 0; i < arr.size(); i++) {
            PyObject* item = valueToPyObject(arr[i], depth + 1);
            if (!item) { Py_DECREF(list); return nullptr; }  // propagate depth error
            PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
        }
        return list;
    }
    if (val.isDict()) {
        const auto& dict = val.asDictConst();
        PyObject* py_dict = PyDict_New();
        for (const auto& [key, value] : dict) {
            PyObject* py_key = PyUnicode_FromString(key.c_str());
            PyObject* py_val = valueToPyObject(value, depth + 1);
            if (!py_val) { Py_DECREF(py_key); Py_DECREF(py_dict); return nullptr; }
            PyDict_SetItem(py_dict, py_key, py_val);
            Py_DECREF(py_key);
            Py_DECREF(py_val);
        }
        return py_dict;
    }
    Py_RETURN_NONE;
}

/**
 * Call a Python function by name (STUB)
 */
interpreter::NaabVal PythonCExecutor::callFunction(
    const std::string& function_name,
    const std::vector<interpreter::NaabVal>& args
) {
    throw std::runtime_error("PythonCExecutor::callFunction() not yet implemented for C API");
}

/**
 * Get captured output (STUB - Python uses StringIO capture internally)
 */
std::string PythonCExecutor::getCapturedOutput() {
    return "";
}

} // namespace runtime
} // namespace naab
