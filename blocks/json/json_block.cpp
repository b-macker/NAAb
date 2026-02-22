// BLOCK-JSON: JSON parsing and stringification using nlohmann/json
// Dynamically loadable C++ block for NAAb
// Version: 1.0.0

#include <nlohmann/json.hpp>
#include "naab/interpreter.h"
#include <cstring>

using json = nlohmann::json;
using naab::Value;
using naab::ValueData;

// Helper: Convert nlohmann::json to Value
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
    } else if (j.is_array()) {
        std::vector<std::shared_ptr<Value>> arr;
        for (const auto& elem : j) {
            arr.push_back(std::shared_ptr<Value>(jsonToValue(elem)));
        }
        return new Value(std::move(arr));
    } else if (j.is_object()) {
        std::unordered_map<std::string, std::shared_ptr<Value>> dict;
        for (auto& [key, val] : j.items()) {
            dict[key] = std::shared_ptr<Value>(jsonToValue(val));
        }
        return new Value(std::move(dict));
    }
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
        } else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) {
            json arr = json::array();
            for (const auto& elem : arg) {
                arr.push_back(valueToJson(*elem));
            }
            return arr;
        } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) {
            json obj = json::object();
            for (const auto& [key, val] : arg) {
                obj[key] = valueToJson(*val);
            }
            return obj;
        } else {
            return "<unsupported>";
        }
    }, val.data);
}

// Exported C functions for block loading
extern "C" {

// Parse JSON string — caller owns the returned Value*
void* json_parse(const char* json_str) {
    try {
        json j = json::parse(json_str);
        return jsonToValue(j);
    } catch (const json::parse_error& e) {
        return new Value();  // Return null on error
    }
}

// Stringify value to JSON — returns statically managed string
// Note: Uses thread_local buffer to avoid memory leaks
const char* json_stringify(void* value_ptr, int indent) {
    try {
        Value* val = static_cast<Value*>(value_ptr);
        json j = valueToJson(*val);

        thread_local std::string result_buf;
        if (indent >= 0) {
            result_buf = j.dump(indent);
        } else {
            result_buf = j.dump();
        }

        return result_buf.c_str();
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
