#pragma once

#include <memory>
#include <string>

// Python embedding support
#ifdef __has_include
#  if __has_include(<Python.h>)
#    define NAAB_HAS_PYTHON 1
#    include <Python.h> // For PyObject*
#    include <pybind11/embed.h> // For pybind11::object
#  endif
#endif

// GEMINI FIX: Forward declaration of interpreter::Value
namespace naab {
namespace interpreter {
    class Value;
}
}

namespace naab {
namespace interpreter {

// Represents a generic Python object returned from a Python block (Phase 2.2)
// This allows calling methods on Python objects from NAAb
class PythonObjectValue {
public:
#ifdef NAAB_HAS_PYTHON
    pybind11::object obj; // The actual Python object
    std::string repr;     // String representation for display

    PythonObjectValue(pybind11::object obj) : obj(std::move(obj)) {
        try {
            repr = pybind11::repr(this->obj).cast<std::string>();
        } catch (...) {
            repr = "<Python object>";
        }
    }
#else
    std::string repr;
    PythonObjectValue(const std::string& r = "<Python object (no Python support)>") : repr(r) {}
#endif

    ~PythonObjectValue(); // Implemented in interpreter.cpp to avoid Py_DECREF in header

    // Delete copy constructor/assignment to prevent double-free
    PythonObjectValue(const PythonObjectValue&) = delete;
    PythonObjectValue& operator=(const PythonObjectValue&) = delete;

    // Allow move
    PythonObjectValue(PythonObjectValue&& other) noexcept;
    PythonObjectValue& operator=(PythonObjectValue&& other) noexcept;
};

} // namespace interpreter
} // namespace naab

