// BLOCK-JSON: JSON parsing and stringification using nlohmann/json
// Dynamically loadable C++ block for NAAb
// Version: 1.0.0

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <stdexcept>
#include <cstring>

using json = nlohmann::json;

// Forward declarations - matching naab::interpreter::Value structure
using ValueData = std::variant<
    std::monostate,  // void/null
    int,
    double,
    bool,
    std::string,
    std::vector<void*>,  // Actually std::vector<std::shared_ptr<Value>>
    std::unordered_map<std::string, void*>,  // Actually std::unordered_map<std::string, std::shared_ptr<Value>>
    void*,  // BlockValue
    void*,  // FunctionValue
    void*   // PythonObjectValue
>;

// Simplified Value structure for block interface
struct Value {
    ValueData data;

    Value() : data(std::monostate{}) {}
    explicit Value(int v) : data(v) {}
    explicit Value(double v) : data(v) {}
    explicit Value(bool v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
};

// Helper: Convert nlohmann::json to simplified Value
Value* jsonToValue(const json& j) {
    if (j.is_null()) {
        return new Value();
    } else if (j.is_boolean()) {
        return new Value(j.get<bool>());
    } else if (j.is_number_integer()) {
        return new Value(j.get<int>());
    } else if (j.is_number_float()) {
        return new Value(j.get<double>());
    } else if (j.is_string()) {
        return new Value(j.get<std::string>());
    }
    // TODO: Handle arrays and objects when full Value API is available
    return new Value();
}

// Helper: Convert Value to nlohmann::json
json valueToJson(const Value& val) {
    return std::visit([](auto&& arg) -> json {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, std::monostate>) {
            return nullptr;
        } else if constexpr (std::is_same_v<T, int>) {
            return arg;
        } else if constexpr (std::is_same_v<T, double>) {
            return arg;
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return arg;
        } else {
            return "<unsupported>";
        }
    }, val.data);
}

// Exported C functions for block loading
extern "C" {

// Parse JSON string
void* json_parse(const char* json_str) {
    try {
        json j = json::parse(json_str);
        return jsonToValue(j);
    } catch (const json::parse_error& e) {
        // TODO: Better error handling
        return new Value();  // Return null on error
    }
}

// Stringify value to JSON
const char* json_stringify(void* value_ptr, int indent) {
    try {
        Value* val = static_cast<Value*>(value_ptr);
        json j = valueToJson(*val);

        std::string* result = new std::string();
        if (indent >= 0) {
            *result = j.dump(indent);
        } else {
            *result = j.dump();
        }

        return result->c_str();
    } catch (const std::exception& e) {
        return "";
    }
}

// Block metadata
const char* block_id() {
    return "BLOCK-JSON";
}

const char* block_version() {
    return "1.0.0";
}

const char* block_functions() {
    return "parse,stringify";
}

} // extern "C"
