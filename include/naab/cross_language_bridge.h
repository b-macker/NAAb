#pragma once

// Cross-Language Type Marshalling Bridge
// Provides unified type conversion between Python, C++, and JavaScript

#include <string>
#include <vector>
#include <unordered_map>
#include "naab/naab_val.h"

// Forward declarations
struct JSRuntime;
struct JSContext;

// QuickJS JSValue type (defined in quickjs.h)
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
    // Convert NaabVal → Python object
    py::object valueToPython(const interpreter::NaabVal& val);

    // Convert Python object → NaabVal
    interpreter::NaabVal pythonToValue(const py::object& obj);
#endif

    // ========================================================================
    // JavaScript ↔ C++ Conversions
    // ========================================================================

    // Convert NaabVal → JSValue (QuickJS)
    JSValue valueToJS(JSContext* ctx, const interpreter::NaabVal& val);

    // Convert JSValue → NaabVal
    interpreter::NaabVal jsToValue(JSContext* ctx, JSValue jsval);

    // Convert struct → JSValue
    JSValue structToJS(JSContext* ctx, const std::shared_ptr<interpreter::StructValue>& s);
    interpreter::NaabVal jsToStruct(
        JSContext* ctx, JSValue obj, const std::string& expected_type_name);

    // ========================================================================
    // Direct Cross-Language Conversions (via NaabVal)
    // ========================================================================

#ifdef HAVE_PYBIND11
    JSValue pythonToJS(JSContext* ctx, const py::object& obj);
    py::object jsToPython(JSContext* ctx, JSValue jsval);
#endif

    // ========================================================================
    // Type Information
    // ========================================================================

    std::string getTypeName(const interpreter::NaabVal& val);
    bool isMarshallable(const interpreter::NaabVal& val);

#ifdef HAVE_PYBIND11
    // Struct conversions - PUBLIC for PythonCExecutor
    py::object structToPython(const std::shared_ptr<interpreter::StructValue>& s);
    interpreter::NaabVal pythonToStruct(
        py::object obj, const std::string& expected_type_name);
#endif

private:
#ifdef HAVE_PYBIND11
    py::list arrayToPython(const std::vector<interpreter::NaabVal>& arr);
    std::vector<interpreter::NaabVal> pythonToArray(const py::object& obj);
    py::dict dictToPython(const std::unordered_map<std::string, interpreter::NaabVal>& dict);
    std::unordered_map<std::string, interpreter::NaabVal> pythonToDict(const py::dict& obj);
#endif

    size_t conversions_count_;
    size_t failed_conversions_;
};

} // namespace runtime
} // namespace naab
