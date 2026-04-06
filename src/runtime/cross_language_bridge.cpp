// Cross-Language Type Marshalling Bridge Implementation
// Provides unified type conversion between Python, C++, and JavaScript

#include "naab/cross_language_bridge.h"
#include "naab/interpreter.h"
#include "naab/struct_registry.h"
#include <fmt/core.h>
#include <stdexcept>
#include <unordered_map>

// QuickJS headers
extern "C" {
#include "quickjs.h"
}

namespace naab {
namespace runtime {

CrossLanguageBridge::CrossLanguageBridge()
    : conversions_count_(0), failed_conversions_(0) {
}

CrossLanguageBridge::~CrossLanguageBridge() {
}

// ============================================================================
// Python ↔ C++ Conversions
// ============================================================================

#ifdef HAVE_PYBIND11

py::object CrossLanguageBridge::valueToPython(
    const interpreter::NaabVal& val) {

    if (val.isNull()) {
        return py::none();
    }

    // FAST PATH: Primitives
    if (val.isInt()) {
        return py::int_(val.asInt());
    }
    if (val.isDouble()) {
        return py::float_(val.asDouble());
    }
    if (val.isBool()) {
        return py::bool_(val.asBool());
    }
    if (val.isString()) {
        return py::str(val.asString());
    }

    // SLOW PATH: Complex types
    conversions_count_++;

    if (val.isList()) {
        return arrayToPython(val.asListConst());
    }
    if (val.isDict()) {
        return dictToPython(val.asDictConst());
    }
    if (val.isStructVal()) {
        return structToPython(val.asStructConst());
    }

    fmt::print("[WARN] Unsupported type for Python conversion\n");
    failed_conversions_++;
    return py::none();
}

interpreter::NaabVal CrossLanguageBridge::pythonToValue(
    const py::object& obj) {

    conversions_count_++;

    if (obj.is_none()) {
        return interpreter::NaabVal::makeNull();
    }

    // Boolean (check before int, since bool is subclass of int in Python)
    if (py::isinstance<py::bool_>(obj)) {
        return interpreter::NaabVal::makeBool(obj.cast<bool>());
    }
    if (py::isinstance<py::int_>(obj)) {
        return interpreter::NaabVal::makeInt(obj.cast<int>());
    }
    if (py::isinstance<py::float_>(obj)) {
        return interpreter::NaabVal::makeDouble(obj.cast<double>());
    }
    if (py::isinstance<py::str>(obj)) {
        return interpreter::NaabVal::makeString(obj.cast<std::string>());
    }

    // List → Array
    if (py::isinstance<py::list>(obj)) {
        return interpreter::NaabVal::makeList(pythonToArray(obj));
    }
    // Tuple → Array
    if (py::isinstance<py::tuple>(obj)) {
        return interpreter::NaabVal::makeList(pythonToArray(obj));
    }
    // Dict → Dictionary
    if (py::isinstance<py::dict>(obj)) {
        return interpreter::NaabVal::makeDict(pythonToDict(obj.cast<py::dict>()));
    }

    fmt::print("[WARN] Unknown Python type, converting to None\n");
    failed_conversions_++;
    return interpreter::NaabVal::makeNull();
}

py::list CrossLanguageBridge::arrayToPython(
    const std::vector<interpreter::NaabVal>& arr) {

    py::list result;
    for (const auto& item : arr) {
        result.append(valueToPython(item));
    }
    return result;
}

std::vector<interpreter::NaabVal> CrossLanguageBridge::pythonToArray(
    const py::object& obj) {

    std::vector<interpreter::NaabVal> result;
    for (const auto& item : obj) {
        result.push_back(pythonToValue(py::cast<py::object>(item)));
    }
    return result;
}

py::dict CrossLanguageBridge::dictToPython(
    const std::unordered_map<std::string, interpreter::NaabVal>& dict) {

    py::dict result;
    for (const auto& [key, value] : dict) {
        result[py::str(key)] = valueToPython(value);
    }
    return result;
}

std::unordered_map<std::string, interpreter::NaabVal>
CrossLanguageBridge::pythonToDict(const py::dict& obj) {

    std::unordered_map<std::string, interpreter::NaabVal> result;
    for (const auto& item : obj) {
        std::string key = py::str(item.first).cast<std::string>();
        result[key] = pythonToValue(py::cast<py::object>(item.second));
    }
    return result;
}

py::object CrossLanguageBridge::structToPython(
    const std::shared_ptr<interpreter::StructValue>& s) {

    if (!s) {
        return py::none();
    }

    py::module types = py::module::import("types");
    py::dict namespace_dict;

    for (size_t i = 0; i < s->definition->fields.size(); ++i) {
        const auto& field = s->definition->fields[i];
        py::object field_val = valueToPython(s->field_values[i]);
        namespace_dict[py::str(field.name)] = field_val;
    }

    auto populate_namespace = [namespace_dict](py::object ns) {
        ns.attr("update")(namespace_dict);
    };

    py::object struct_class = types.attr("new_class")(
        py::str(s->type_name),
        py::tuple(),
        py::dict(),
        py::cpp_function(populate_namespace)
    );

    return struct_class();
}

interpreter::NaabVal CrossLanguageBridge::pythonToStruct(
    py::object obj, const std::string& expected_type_name) {

    auto struct_def = runtime::StructRegistry::instance().getStruct(expected_type_name);
    if (!struct_def) {
        throw std::runtime_error("Unknown struct type: " + expected_type_name);
    }

    auto struct_val = std::make_shared<interpreter::StructValue>(
        expected_type_name, struct_def);

    for (size_t i = 0; i < struct_def->fields.size(); ++i) {
        const auto& field = struct_def->fields[i];
        if (!py::hasattr(obj, field.name.c_str())) {
            throw std::runtime_error("Python object missing field: " + field.name);
        }
        py::object py_field = obj.attr(field.name.c_str());
        struct_val->field_values[i] = pythonToValue(py_field);
    }

    return interpreter::NaabVal::makeStruct(struct_val);
}

#endif // HAVE_PYBIND11

// ============================================================================
// JavaScript ↔ C++ Conversions
// ============================================================================

JSValue CrossLanguageBridge::valueToJS(
    JSContext* ctx,
    const interpreter::NaabVal& val,
    int depth) {

    // V-RT-006: prevent stack overflow on deeply-nested structures.
    // Parity with PythonCExecutor::valueToPyObject (V-RT-005) and serializeForLanguage (V-VM-002).
    if (depth > 64) {
        JS_ThrowRangeError(ctx,
            "NAAb marshalling error: nested structure exceeds maximum depth (64). "
            "Flatten the structure before passing it to a JS block.");
        return JS_EXCEPTION;
    }

    conversions_count_++;

    if (val.isNull()) {
        return JS_NULL;
    }

    if (val.isInt()) {
        return JS_NewInt32(ctx, val.asInt());
    }
    if (val.isDouble()) {
        return JS_NewFloat64(ctx, val.asDouble());
    }
    if (val.isBool()) {
        return JS_NewBool(ctx, val.asBool());
    }
    if (val.isString()) {
        return JS_NewString(ctx, val.asString().c_str());
    }
    if (val.isList()) {
        const auto& arr = val.asListConst();
        JSValue result = JS_NewArray(ctx);
        for (size_t i = 0; i < arr.size(); ++i) {
            JSValue elem = valueToJS(ctx, arr[i], depth + 1);
            if (JS_IsException(elem)) {
                JS_FreeValue(ctx, result);
                return JS_EXCEPTION;
            }
            JS_SetPropertyUint32(ctx, result, static_cast<uint32_t>(i), elem);
        }
        return result;
    }
    if (val.isDict()) {
        const auto& dict = val.asDictConst();
        JSValue result = JS_NewObject(ctx);
        for (const auto& [key, value] : dict) {
            JSValue val_js = valueToJS(ctx, value, depth + 1);
            if (JS_IsException(val_js)) {
                JS_FreeValue(ctx, result);
                return JS_EXCEPTION;
            }
            JS_SetPropertyStr(ctx, result, key.c_str(), val_js);
        }
        return result;
    }
    if (val.isStructVal()) {
        return structToJS(ctx, val.asStructConst());
    }

    fmt::print("[WARN] Unsupported type for JavaScript conversion\n");
    failed_conversions_++;
    return JS_UNDEFINED;
}

interpreter::NaabVal CrossLanguageBridge::jsToValue(
    JSContext* ctx,
    JSValue val) {

    conversions_count_++;

    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        return interpreter::NaabVal::makeNull();
    }

    if (JS_IsBool(val)) {
        int32_t b = JS_ToBool(ctx, val);
        return interpreter::NaabVal::makeBool(b != 0);
    }

    if (JS_IsNumber(val)) {
        int32_t i;
        if (JS_ToInt32(ctx, &i, val) == 0) {
            double d;
            JS_ToFloat64(ctx, &d, val);
            if (d == static_cast<double>(i)) {
                return interpreter::NaabVal::makeInt(i);
            } else {
                return interpreter::NaabVal::makeDouble(d);
            }
        }
    }

    if (JS_IsString(val)) {
        const char* str = JS_ToCString(ctx, val);
        if (str) {
            auto result = interpreter::NaabVal::makeString(std::string(str));
            JS_FreeCString(ctx, str);
            return result;
        }
    }

    if (JS_IsArray(ctx, val)) {
        std::vector<interpreter::NaabVal> arr;

        JSValue length_val = JS_GetPropertyStr(ctx, val, "length");
        int32_t length = 0;
        JS_ToInt32(ctx, &length, length_val);
        JS_FreeValue(ctx, length_val);

        for (int32_t i = 0; i < length; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, val, static_cast<uint32_t>(i));
            arr.push_back(jsToValue(ctx, elem));
            JS_FreeValue(ctx, elem);
        }

        return interpreter::NaabVal::makeList(std::move(arr));
    }

    if (JS_IsObject(val) && !JS_IsFunction(ctx, val)) {
        std::unordered_map<std::string, interpreter::NaabVal> dict;

        JSPropertyEnum* tab;
        uint32_t tab_len;
        if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, val, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < tab_len; ++i) {
                JSValue prop_name = JS_AtomToString(ctx, tab[i].atom);
                const char* key_str = JS_ToCString(ctx, prop_name);

                if (key_str) {
                    JSValue prop_val = JS_GetProperty(ctx, val, tab[i].atom);
                    dict[key_str] = jsToValue(ctx, prop_val);
                    JS_FreeValue(ctx, prop_val);
                    JS_FreeCString(ctx, key_str);
                }

                JS_FreeValue(ctx, prop_name);
            }

            js_free(ctx, tab);
        }

        return interpreter::NaabVal::makeDict(std::move(dict));
    }

    fmt::print("[WARN] Unsupported JavaScript type, returning null\n");
    failed_conversions_++;
    return interpreter::NaabVal::makeNull();
}

JSValue CrossLanguageBridge::structToJS(
    JSContext* ctx,
    const std::shared_ptr<interpreter::StructValue>& s) {

    if (!s) {
        return JS_NULL;
    }

    JSValue obj = JS_NewObject(ctx);

    JSValue proto = JS_GetPrototype(ctx, obj);
    JS_DefinePropertyValueStr(ctx, proto, "constructor",
        JS_NewString(ctx, s->type_name.c_str()),
        JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, proto);

    JS_DefinePropertyValueStr(ctx, obj, "__struct_type__",
        JS_NewString(ctx, s->type_name.c_str()),
        JS_PROP_ENUMERABLE);

    for (size_t i = 0; i < s->definition->fields.size(); ++i) {
        const auto& field = s->definition->fields[i];
        JSValue val = valueToJS(ctx, s->field_values[i]);
        JS_DefinePropertyValueStr(ctx, obj, field.name.c_str(), val,
            JS_PROP_C_W_E);
    }

    return obj;
}

interpreter::NaabVal CrossLanguageBridge::jsToStruct(
    JSContext* ctx, JSValue obj, const std::string& expected_type_name) {

    auto struct_def = runtime::StructRegistry::instance().getStruct(expected_type_name);
    if (!struct_def) {
        throw std::runtime_error("Unknown struct type: " + expected_type_name);
    }

    auto struct_val = std::make_shared<interpreter::StructValue>(
        expected_type_name, struct_def);

    for (size_t i = 0; i < struct_def->fields.size(); ++i) {
        const auto& field = struct_def->fields[i];
        JSValue js_field = JS_GetPropertyStr(ctx, obj, field.name.c_str());
        if (JS_IsUndefined(js_field)) {
            JS_FreeValue(ctx, js_field);
            throw std::runtime_error("JS object missing field: " + field.name);
        }
        struct_val->field_values[i] = jsToValue(ctx, js_field);
        JS_FreeValue(ctx, js_field);
    }

    return interpreter::NaabVal::makeStruct(struct_val);
}

// ============================================================================
// Direct Cross-Language Conversions
// ============================================================================

#ifdef HAVE_PYBIND11

JSValue CrossLanguageBridge::pythonToJS(JSContext* ctx, const py::object& obj) {
    auto cpp_val = pythonToValue(obj);
    return valueToJS(ctx, cpp_val);
}

py::object CrossLanguageBridge::jsToPython(JSContext* ctx, JSValue jsval) {
    auto cpp_val = jsToValue(ctx, jsval);
    return valueToPython(cpp_val);
}

#endif // HAVE_PYBIND11

// ============================================================================
// Type Information
// ============================================================================

std::string CrossLanguageBridge::getTypeName(
    const interpreter::NaabVal& val) {
    return val.getTypeName();
}

bool CrossLanguageBridge::isMarshallable(
    const interpreter::NaabVal& val) {
    std::string type = getTypeName(val);
    return type != "unknown";
}

} // namespace runtime
} // namespace naab
