// NAAb JSON Module - Real Implementation using nlohmann/json
// Provides JSON parsing and stringification

#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <stdexcept>

using json = nlohmann::json;

namespace naab {
namespace stdlib {

// ============================================================================
// JSON Module Implementation
// ============================================================================

bool JSONModule::hasFunction(const std::string& name) const {
    return name == "parse" || name == "stringify" ||
           name == "parse_object" || name == "parse_array" ||
           name == "is_valid" || name == "pretty";
}

std::shared_ptr<interpreter::Value> JSONModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "parse") {
        return parse(args);
    } else if (function_name == "stringify") {
        return stringify(args);
    } else if (function_name == "parse_object") {
        return parse_object(args);
    } else if (function_name == "parse_array") {
        return parse_array(args);
    } else if (function_name == "is_valid") {
        return is_valid(args);
    } else if (function_name == "pretty") {
        return pretty(args);
    }

    throw std::runtime_error("Unknown JSON function: " + function_name);
}

// Helper: Convert nlohmann::json to NAAb Value
std::shared_ptr<interpreter::Value> jsonToValue(const json& j) {
    if (j.is_null()) {
        return std::make_shared<interpreter::Value>();
    } else if (j.is_boolean()) {
        return std::make_shared<interpreter::Value>(j.get<bool>());
    } else if (j.is_number_integer()) {
        return std::make_shared<interpreter::Value>(j.get<int>());
    } else if (j.is_number_float()) {
        return std::make_shared<interpreter::Value>(j.get<double>());
    } else if (j.is_string()) {
        return std::make_shared<interpreter::Value>(j.get<std::string>());
    } else if (j.is_array()) {
        std::vector<std::shared_ptr<interpreter::Value>> vec;
        for (const auto& item : j) {
            vec.push_back(jsonToValue(item));
        }
        return std::make_shared<interpreter::Value>(vec);
    } else if (j.is_object()) {
        std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> map;
        for (auto it = j.begin(); it != j.end(); ++it) {
            map[it.key()] = jsonToValue(it.value());
        }
        return std::make_shared<interpreter::Value>(map);
    }

    // Unknown type - return null
    return std::make_shared<interpreter::Value>();
}

// Helper: Convert NAAb Value to nlohmann::json
json valueToJson(const interpreter::Value& val) {
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
        } else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            json arr = json::array();
            for (const auto& item : arg) {
                arr.push_back(valueToJson(*item));
            }
            return arr;
        } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
            json obj = json::object();
            for (const auto& [key, value] : arg) {
                obj[key] = valueToJson(*value);
            }
            return obj;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<interpreter::StructValue>>) {
            // Struct serialization - convert to JSON object
            json obj = json::object();
            if (arg && arg->definition) {
                const auto& fields = arg->definition->fields;
                const auto& values = arg->field_values;

                for (size_t i = 0; i < fields.size() && i < values.size(); ++i) {
                    const std::string& field_name = fields[i].name;
                    if (values[i]) {
                        obj[field_name] = valueToJson(*values[i]);
                    } else {
                        obj[field_name] = nullptr;  // null field
                    }
                }
            }
            return obj;
        } else {
            // Unsupported type - convert to string
            return "<unsupported>";
        }
    }, val.data);
}

std::shared_ptr<interpreter::Value> JSONModule::parse(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        throw std::runtime_error("json.parse() requires JSON string argument");
    }

    std::string json_str = args[0]->toString();

    try {
        // Parse JSON using nlohmann/json
        json j = json::parse(json_str);

        // Convert to NAAb Value
        return jsonToValue(j);

    } catch (const json::parse_error& e) {
        throw std::runtime_error(
            fmt::format("JSON parse error at byte {}: {}", e.byte, e.what())
        );
    } catch (const std::exception& e) {
        throw std::runtime_error(
            fmt::format("JSON parse error: {}", e.what())
        );
    }
}

std::shared_ptr<interpreter::Value> JSONModule::stringify(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        throw std::runtime_error("json.stringify() requires value argument");
    }

    try {
        // Convert NAAb Value to JSON
        json j = valueToJson(*args[0]);

        // Optional: indentation (pretty print)
        int indent = -1;  // Compact by default
        if (args.size() >= 2) {
            indent = args[1]->toInt();
        }

        // Stringify
        std::string result;
        if (indent >= 0) {
            result = j.dump(indent);  // Pretty print
        } else {
            result = j.dump();  // Compact
        }

        return std::make_shared<interpreter::Value>(result);

    } catch (const std::exception& e) {
        throw std::runtime_error(
            fmt::format("JSON stringify error: {}", e.what())
        );
    }
}

std::shared_ptr<interpreter::Value> JSONModule::parse_object(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Parse the JSON using the main parse function
    auto result = parse(args);

    // Validate that the result is a dictionary (object)
    return std::visit([](auto&& arg) -> std::shared_ptr<interpreter::Value> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
            // Return the parsed dict
            return std::make_shared<interpreter::Value>(arg);
        } else {
            throw std::runtime_error("JSON parse_object: Expected JSON object, got non-object type");
        }
    }, result->data);
}

std::shared_ptr<interpreter::Value> JSONModule::parse_array(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Parse the JSON using the main parse function
    auto result = parse(args);

    // Validate that the result is an array
    return std::visit([](auto&& arg) -> std::shared_ptr<interpreter::Value> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            // Return the parsed array
            return std::make_shared<interpreter::Value>(arg);
        } else {
            throw std::runtime_error("JSON parse_array: Expected JSON array, got non-array type");
        }
    }, result->data);
}

std::shared_ptr<interpreter::Value> JSONModule::is_valid(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        return std::make_shared<interpreter::Value>(false);
    }

    try {
        // Try to parse the JSON
        parse(args);
        // If successful, return true
        return std::make_shared<interpreter::Value>(true);
    } catch (...) {
        // If parse throws, JSON is invalid
        return std::make_shared<interpreter::Value>(false);
    }
}

std::shared_ptr<interpreter::Value> JSONModule::pretty(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        throw std::runtime_error("json.pretty() requires value argument");
    }

    // Default indent is 2 spaces for pretty printing
    int indent = 2;
    if (args.size() >= 2) {
        indent = args[1]->toInt();
    }

    // Call stringify with indent
    std::vector<std::shared_ptr<interpreter::Value>> stringify_args = {
        args[0],
        std::make_shared<interpreter::Value>(indent)
    };

    return stringify(stringify_args);
}

} // namespace stdlib
} // namespace naab
