#pragma once

// Cross-Language Type Marshalling Bridge
// Provides unified type conversion between Python, C++, and JavaScript

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

// Forward declarations
struct JSRuntime;
struct JSContext;

// QuickJS JSValue type (defined in quickjs.h)
// Can be either pointer or uint64_t depending on configuration
#ifdef __cplusplus
extern "C" {
#endif
#include "quickjs.h"
#ifdef __cplusplus
}
#endif

#ifdef HAVE_PYBIND11
#include <pybind11/pybind11.h>
namespace py = pybind11;
#endif

namespace naab {
namespace interpreter {
    class Value;
    struct StructValue;
}

namespace runtime {

// Cross-Language Bridge: Unified type marshalling
class CrossLanguageBridge {
public:
    CrossLanguageBridge();
    ~CrossLanguageBridge();

    // ========================================================================
    // Python ↔ C++ Conversions
    // ========================================================================

#ifdef HAVE_PYBIND11
    // Convert NAAb Value → Python object
    py::object valueToPython(const std::shared_ptr<interpreter::Value>& val);

    // Convert Python object → NAAb Value
    std::shared_ptr<interpreter::Value> pythonToValue(const py::object& obj);
#endif

    // ========================================================================
    // JavaScript ↔ C++ Conversions
    // ========================================================================

    // Convert NAAb Value → JSValue (QuickJS)
    // Note: Caller must JS_FreeValue() when done
    JSValue valueToJS(JSContext* ctx, const std::shared_ptr<interpreter::Value>& val);

    // Convert JSValue → NAAb Value
    std::shared_ptr<interpreter::Value> jsToValue(JSContext* ctx, JSValue jsval);

    // Convert struct → JSValue (Week 6 - JavaScript marshalling)
    JSValue structToJS(JSContext* ctx, const std::shared_ptr<interpreter::StructValue>& s);
    std::shared_ptr<interpreter::Value> jsToStruct(
        JSContext* ctx, JSValue obj, const std::string& expected_type_name);

    // ========================================================================
    // Direct Cross-Language Conversions (via C++ Value)
    // ========================================================================

#ifdef HAVE_PYBIND11
    // Python → JavaScript (via C++ Value)
    JSValue pythonToJS(JSContext* ctx, const py::object& obj);

    // JavaScript → Python (via C++ Value)
    py::object jsToPython(JSContext* ctx, JSValue jsval);
#endif

    // ========================================================================
    // Type Information
    // ========================================================================

    // Get human-readable type name for debugging
    std::string getTypeName(const std::shared_ptr<interpreter::Value>& val);

    // Check if value can be marshalled between languages
    bool isMarshallable(const std::shared_ptr<interpreter::Value>& val);

#ifdef HAVE_PYBIND11
    // Convert structs (Week 5 - Python marshalling) - PUBLIC for PythonExecutor
    py::object structToPython(const std::shared_ptr<interpreter::StructValue>& s);
    std::shared_ptr<interpreter::Value> pythonToStruct(
        py::object obj, const std::string& expected_type_name);
#endif

private:
    // Internal helpers for complex types

#ifdef HAVE_PYBIND11
    // Convert arrays/vectors
    py::list arrayToPython(
        const std::vector<std::shared_ptr<interpreter::Value>>& arr);
    std::vector<std::shared_ptr<interpreter::Value>> pythonToArray(
        const py::object& obj);

    // Convert dictionaries/maps
    py::dict dictToPython(
        const std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>& dict);
    std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> pythonToDict(
        const py::dict& obj);
#endif

    // Statistics for debugging
    size_t conversions_count_;
    size_t failed_conversions_;
};

} // namespace runtime
} // namespace naab

