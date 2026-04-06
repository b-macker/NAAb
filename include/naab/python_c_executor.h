#pragma once

/**
 * C++ Adapter for Python C API Wrapper
 *
 * Provides a C++ interface that wraps python_c_wrapper.c
 * Converts between PyObject* and NAAb Value types
 * 5x faster than pybind11, no Android CFI crashes
 */

#include "naab/naab_val.h"
#include <memory>
#include <string>
#include <vector>

#ifdef HAVE_PYBIND11
#include "naab/python_c_wrapper.h"
#endif

namespace naab {
namespace runtime {

/**
 * C++ wrapper around pure C Python API
 *
 * This class provides a C++ interface to the thread-safe Python C wrapper
 * It handles:
 * - Conversion between PyObject* and NAAb Value types
 * - Error handling and exception translation
 * - Memory management (RAII for PyObject references)
 */
class PythonCExecutor {
public:
    PythonCExecutor() = default;
    ~PythonCExecutor() = default;

    // Non-copyable (Python objects can't be easily copied)
    PythonCExecutor(const PythonCExecutor&) = delete;
    PythonCExecutor& operator=(const PythonCExecutor&) = delete;

    /**
     * Execute Python code (statement mode)
     *
     * @param code Python code to execute
     * @throws std::runtime_error if execution fails
     */
    void execute(const std::string& code);

    /**
     * Execute Python expression and return result
     *
     * @param code Python expression to evaluate
     * @return NAAb Value containing the result
     * @throws std::runtime_error if execution fails
     */
    interpreter::NaabVal executeWithReturn(const std::string& code);

    /**
     * Call a Python function by name (STUB - not yet implemented for C API)
     *
     * @param function_name Name of function to call
     * @param args Function arguments as NAAb Values
     * @return Result of function call
     * @throws std::runtime_error always (not implemented)
     */
    interpreter::NaabVal callFunction(
        const std::string& function_name,
        const std::vector<interpreter::NaabVal>& args
    );

    /**
     * Get captured output (STUB - not yet implemented for C API)
     *
     * @return Empty string (output capture not implemented)
     */
    std::string getCapturedOutput();

private:
#ifdef HAVE_PYBIND11
    interpreter::NaabVal pyObjectToValue(PyObject* obj);
    // V-RT-005: depth parameter prevents stack overflow on deeply-nested structures.
    // Same guard as V-VM-002 (serializeForLanguage): throws at depth > 64.
    PyObject* valueToPyObject(const interpreter::NaabVal& val, int depth = 0);
#endif
};

} // namespace runtime
} // namespace naab

