// Cross-Language Type Marshalling Bridge Implementation
// Provides unified type conversion between Python, C++, and JavaScript

#include "naab/cross_language_bridge.h"
#include "naab/interpreter.h"
#include "naab/struct_registry.h"
#include <fmt/core.h>
#include <stdexcept>
#include <variant>
#include <unordered_map>

// QuickJS headers
extern "C" {
#include "quickjs.h"
}

namespace naab {
namespace runtime {

CrossLanguageBridge::CrossLanguageBridge()
    : conversions_count_(0), failed_conversions_(0) {
    fmt::print("[Bridge] Cross-language bridge initialized\n");
}

CrossLanguageBridge::~CrossLanguageBridge() {
    fmt::print("[Bridge] Conversions: {} total, {} failed\n",
               conversions_count_, failed_conversions_);
}

// ============================================================================
// Python ↔ C++ Conversions
// ============================================================================

#ifdef HAVE_PYBIND11

py::object CrossLanguageBridge::valueToPython(
    const std::shared_ptr<interpreter::Value>& val) {

    if (!val) {
        return py::none();
    }

    // FAST PATH: Primitives (80-90% of conversions)
    // Use holds_alternative + get instead of std::visit (5-10x faster)
    if (std::holds_alternative<int>(val->data)) {
        return py::int_(std::get<int>(val->data));
    }
    if (std::holds_alternative<double>(val->data)) {
        return py::float_(std::get<double>(val->data));
    }
    if (std::holds_alternative<bool>(val->data)) {
        return py::bool_(std::get<bool>(val->data));
    }
    if (std::holds_alternative<std::string>(val->data)) {
        return py::str(std::get<std::string>(val->data));
    }
    if (std::holds_alternative<std::monostate>(val->data)) {
        return py::none();
    }

    // SLOW PATH: Complex types (arrays, maps, structs)
    conversions_count_++;  // Only count slow path conversions

    return std::visit([this](auto&& arg) -> py::object {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            return arrayToPython(arg);
        }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
            return dictToPython(arg);
        }
        else if constexpr (std::is_same_v<T, std::shared_ptr<interpreter::StructValue>>) {
            return structToPython(arg);
        }
        else {
            // Should never reach here (fast path handles primitives)
            fmt::print("[WARN] Unsupported type for Python conversion\n");
            failed_conversions_++;
            return py::none();
        }
    }, val->data);
}

std::shared_ptr<interpreter::Value> CrossLanguageBridge::pythonToValue(
    const py::object& obj) {

    conversions_count_++;

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
        auto arr = pythonToArray(obj);
        return std::make_shared<interpreter::Value>(arr);
    }

    // Tuple → Array
    if (py::isinstance<py::tuple>(obj)) {
        auto arr = pythonToArray(obj);
        return std::make_shared<interpreter::Value>(arr);
    }

    // Dict → Dictionary
    if (py::isinstance<py::dict>(obj)) {
        auto dict = pythonToDict(obj.cast<py::dict>());
        return std::make_shared<interpreter::Value>(dict);
    }

    // Unknown type
    fmt::print("[WARN] Unknown Python type, converting to None\n");
    failed_conversions_++;
    return std::make_shared<interpreter::Value>();
}

py::list CrossLanguageBridge::arrayToPython(
    const std::vector<std::shared_ptr<interpreter::Value>>& arr) {

    py::list result;
    for (const auto& item : arr) {
        result.append(valueToPython(item));
    }
    return result;
}

std::vector<std::shared_ptr<interpreter::Value>> CrossLanguageBridge::pythonToArray(
    const py::object& obj) {

    std::vector<std::shared_ptr<interpreter::Value>> result;

    // Handle both list and tuple
    for (const auto& item : obj) {
        result.push_back(pythonToValue(py::cast<py::object>(item)));
    }

    return result;
}

py::dict CrossLanguageBridge::dictToPython(
    const std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>& dict) {

    py::dict result;
    for (const auto& [key, value] : dict) {
        result[py::str(key)] = valueToPython(value);
    }
    return result;
}

std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>
CrossLanguageBridge::pythonToDict(const py::dict& obj) {

    std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> result;

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

    // Import types module for dynamic class creation
    py::module types = py::module::import("types");

    // Create namespace dict with all field values
    py::dict namespace_dict;

    for (size_t i = 0; i < s->definition->fields.size(); ++i) {
        const auto& field = s->definition->fields[i];
        py::object field_val = valueToPython(s->field_values[i]);
        namespace_dict[py::str(field.name)] = field_val;
    }

    // Create dynamic class with struct type name
    // Lambda captures namespace_dict and populates class namespace
    auto populate_namespace = [namespace_dict](py::object ns) {
        ns.attr("update")(namespace_dict);
    };

    py::object struct_class = types.attr("new_class")(
        py::str(s->type_name),          // class name
        py::tuple(),                     // bases (no inheritance)
        py::dict(),                      // keywords
        py::cpp_function(populate_namespace)
    );

    // Instantiate and return the dynamic class
    return struct_class();
}

std::shared_ptr<interpreter::Value> CrossLanguageBridge::pythonToStruct(
    py::object obj, const std::string& expected_type_name) {

    // Get struct definition from registry
    auto struct_def = runtime::StructRegistry::instance().getStruct(expected_type_name);
    if (!struct_def) {
        throw std::runtime_error("Unknown struct type: " + expected_type_name);
    }

    // Create struct value
    auto struct_val = std::make_shared<interpreter::StructValue>(
        expected_type_name, struct_def);

    // Extract fields from Python object
    for (size_t i = 0; i < struct_def->fields.size(); ++i) {
        const auto& field = struct_def->fields[i];

        if (!py::hasattr(obj, field.name.c_str())) {
            throw std::runtime_error("Python object missing field: " + field.name);
        }

        py::object py_field = obj.attr(field.name.c_str());
        struct_val->field_values[i] = pythonToValue(py_field);
    }

    return std::make_shared<interpreter::Value>(struct_val);
}

#endif // HAVE_PYBIND11

// ============================================================================
// JavaScript ↔ C++ Conversions
// ============================================================================

JSValue CrossLanguageBridge::valueToJS(
    JSContext* ctx,
    const std::shared_ptr<interpreter::Value>& val) {

    conversions_count_++;

    if (!val) {
        return JS_UNDEFINED;
    }

    JSValue result;

    // Check type using std::holds_alternative
    if (std::holds_alternative<int>(val->data)) {
        result = JS_NewInt32(ctx, std::get<int>(val->data));
    }
    else if (std::holds_alternative<double>(val->data)) {
        result = JS_NewFloat64(ctx, std::get<double>(val->data));
    }
    else if (std::holds_alternative<bool>(val->data)) {
        result = JS_NewBool(ctx, std::get<bool>(val->data));
    }
    else if (std::holds_alternative<std::string>(val->data)) {
        const auto& str = std::get<std::string>(val->data);
        result = JS_NewString(ctx, str.c_str());
    }
    else if (std::holds_alternative<std::monostate>(val->data)) {
        result = JS_NULL;
    }
    else if (std::holds_alternative<std::vector<std::shared_ptr<interpreter::Value>>>(val->data)) {
        // Convert array to JavaScript array
        const auto& arr = std::get<std::vector<std::shared_ptr<interpreter::Value>>>(val->data);
        result = JS_NewArray(ctx);

        for (size_t i = 0; i < arr.size(); ++i) {
            JSValue elem = valueToJS(ctx, arr[i]);
            JS_SetPropertyUint32(ctx, result, i, elem);
        }
    }
    else if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(val->data)) {
        // Convert dict to JavaScript object
        const auto& dict = std::get<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(val->data);
        result = JS_NewObject(ctx);

        for (const auto& [key, value] : dict) {
            JSValue val_js = valueToJS(ctx, value);
            JS_SetPropertyStr(ctx, result, key.c_str(), val_js);
        }
    }
    else if (std::holds_alternative<std::shared_ptr<interpreter::StructValue>>(val->data)) {
        // Convert struct to JavaScript object
        const auto& s = std::get<std::shared_ptr<interpreter::StructValue>>(val->data);
        result = structToJS(ctx, s);
    }
    else {
        fmt::print("[WARN] Unsupported type for JavaScript conversion\n");
        failed_conversions_++;
        result = JS_UNDEFINED;
    }

    return result;
}

std::shared_ptr<interpreter::Value> CrossLanguageBridge::jsToValue(
    JSContext* ctx,
    JSValue val) {

    conversions_count_++;

    // Null or undefined
    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        return std::make_shared<interpreter::Value>();
    }

    // Boolean
    if (JS_IsBool(val)) {
        int32_t b = JS_ToBool(ctx, val);
        return std::make_shared<interpreter::Value>(b != 0);
    }

    // Number (check int first, then double)
    if (JS_IsNumber(val)) {
        int32_t i;
        if (JS_ToInt32(ctx, &i, val) == 0) {
            double d;
            JS_ToFloat64(ctx, &d, val);
            if (d == static_cast<double>(i)) {
                return std::make_shared<interpreter::Value>(i);
            } else {
                return std::make_shared<interpreter::Value>(d);
            }
        }
    }

    // String
    if (JS_IsString(val)) {
        const char* str = JS_ToCString(ctx, val);
        if (str) {
            auto result = std::make_shared<interpreter::Value>(std::string(str));
            JS_FreeCString(ctx, str);
            return result;
        }
    }

    // Array
    if (JS_IsArray(ctx, val)) {
        std::vector<std::shared_ptr<interpreter::Value>> arr;

        JSValue length_val = JS_GetPropertyStr(ctx, val, "length");
        int32_t length = 0;
        JS_ToInt32(ctx, &length, length_val);
        JS_FreeValue(ctx, length_val);

        for (int32_t i = 0; i < length; ++i) {
            JSValue elem = JS_GetPropertyUint32(ctx, val, i);
            arr.push_back(jsToValue(ctx, elem));
            JS_FreeValue(ctx, elem);
        }

        return std::make_shared<interpreter::Value>(arr);
    }

    // Object (dictionary)
    if (JS_IsObject(val) && !JS_IsFunction(ctx, val)) {
        std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> dict;

        // Get property names
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

        return std::make_shared<interpreter::Value>(dict);
    }

    // Unsupported type
    fmt::print("[WARN] Unsupported JavaScript type, returning null\n");
    failed_conversions_++;
    return std::make_shared<interpreter::Value>();
}

JSValue CrossLanguageBridge::structToJS(
    JSContext* ctx,
    const std::shared_ptr<interpreter::StructValue>& s) {

    if (!s) {
        return JS_NULL;
    }

    // Create JavaScript object
    JSValue obj = JS_NewObject(ctx);

    // Set constructor name for debugging
    JSValue proto = JS_GetPrototype(ctx, obj);
    JS_DefinePropertyValueStr(ctx, proto, "constructor",
        JS_NewString(ctx, s->type_name.c_str()),
        JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, proto);

    // Add __struct_type__ metadata
    JS_DefinePropertyValueStr(ctx, obj, "__struct_type__",
        JS_NewString(ctx, s->type_name.c_str()),
        JS_PROP_ENUMERABLE);

    // Set fields (recursively convert values)
    for (size_t i = 0; i < s->definition->fields.size(); ++i) {
        const auto& field = s->definition->fields[i];
        JSValue val = valueToJS(ctx, s->field_values[i]);
        JS_DefinePropertyValueStr(ctx, obj, field.name.c_str(), val,
            JS_PROP_C_W_E);  // Configurable, writable, enumerable
    }

    return obj;
}

std::shared_ptr<interpreter::Value> CrossLanguageBridge::jsToStruct(
    JSContext* ctx, JSValue obj, const std::string& expected_type_name) {

    // Get struct definition from registry
    auto struct_def = runtime::StructRegistry::instance().getStruct(expected_type_name);
    if (!struct_def) {
        throw std::runtime_error("Unknown struct type: " + expected_type_name);
    }

    // Create struct value
    auto struct_val = std::make_shared<interpreter::StructValue>(
        expected_type_name, struct_def);

    // Extract fields from JavaScript object
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

    return std::make_shared<interpreter::Value>(struct_val);
}

// ============================================================================
// Direct Cross-Language Conversions
// ============================================================================

#ifdef HAVE_PYBIND11

JSValue CrossLanguageBridge::pythonToJS(JSContext* ctx, const py::object& obj) {
    // Python → C++ → JavaScript
    auto cpp_val = pythonToValue(obj);
    return valueToJS(ctx, cpp_val);
}

py::object CrossLanguageBridge::jsToPython(JSContext* ctx, JSValue jsval) {
    // JavaScript → C++ → Python
    auto cpp_val = jsToValue(ctx, jsval);
    return valueToPython(cpp_val);
}

#endif // HAVE_PYBIND11

// ============================================================================
// Type Information
// ============================================================================

std::string CrossLanguageBridge::getTypeName(
    const std::shared_ptr<interpreter::Value>& val) {

    if (!val) {
        return "null";
    }

    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, std::monostate>) {
            return "null";
        }
        else if constexpr (std::is_same_v<T, int>) {
            return "int";
        }
        else if constexpr (std::is_same_v<T, double>) {
            return "double";
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return "bool";
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            return "string";
        }
        else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            return "array";
        }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
            return "object";
        }
        else {
            return "unknown";
        }
    }, val->data);
}

bool CrossLanguageBridge::isMarshallable(
    const std::shared_ptr<interpreter::Value>& val) {

    if (!val) {
        return true; // null is marshallable
    }

    // All current types are marshallable
    // In the future, we might have non-marshallable types (e.g., function pointers)
    std::string type = getTypeName(val);
    return type != "unknown";
}

} // namespace runtime
} // namespace naab
