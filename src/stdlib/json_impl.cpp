// NAAb JSON Module - Real Implementation using nlohmann/json
// Provides JSON parsing and stringification

#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include "naab/limits.h"
#include "naab/utils/string_utils.h"
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <stdexcept>
#include <sstream>

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

interpreter::NaabVal JSONModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

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

    // Common LLM mistakes - Python/JS naming conventions
    if (function_name == "dumps" || function_name == "encode") {
        throw std::runtime_error(
            "Unknown JSON function: " + function_name + "\n\n"
            "  Did you mean: json.stringify()?\n"
            "  NAAb uses 'stringify' (like JavaScript), not 'dumps' (Python) or 'encode'.\n\n"
            "  Example:\n"
            "    let text = json.stringify(my_dict)\n"
        );
    }
    if (function_name == "loads" || function_name == "decode") {
        throw std::runtime_error(
            "Unknown JSON function: " + function_name + "\n\n"
            "  Did you mean: json.parse()?\n"
            "  NAAb uses 'parse' (like JavaScript), not 'loads' (Python) or 'decode'.\n\n"
            "  Example:\n"
            "    let data = json.parse(json_string)\n"
        );
    }
    if (function_name == "to_string" || function_name == "serialize" || function_name == "toString") {
        throw std::runtime_error(
            "Unknown JSON function: " + function_name + "\n\n"
            "  Did you mean: json.stringify()?\n"
            "  Example: json.stringify({\"key\": \"value\"})\n"
        );
    }
    if (function_name == "from_string" || function_name == "deserialize" || function_name == "fromString") {
        throw std::runtime_error(
            "Unknown JSON function: " + function_name + "\n\n"
            "  Did you mean: json.parse()?\n"
            "  Example: json.parse(json_string)\n"
        );
    }
    if (function_name == "format" || function_name == "prettify" || function_name == "indent") {
        throw std::runtime_error(
            "Unknown JSON function: " + function_name + "\n\n"
            "  Did you mean: json.pretty()?\n"
            "  Example: json.pretty(data, 2)  // 2-space indent\n"
        );
    }

    // Fuzzy matching for typos
    static const std::vector<std::string> FUNCTIONS = {
        "parse", "stringify", "parse_object", "parse_array", "is_valid", "pretty"
    };
    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown JSON function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }
    throw std::runtime_error(oss.str());
}

// Helper: Convert nlohmann::json to NaabVal
interpreter::NaabVal jsonToValue(const json& j) {
    if (j.is_null()) {
        return interpreter::NaabVal::makeNull();
    } else if (j.is_boolean()) {
        return interpreter::NaabVal::makeBool(j.get<bool>());
    } else if (j.is_number_integer()) {
        return interpreter::NaabVal::makeInt(j.get<int>());
    } else if (j.is_number_float()) {
        return interpreter::NaabVal::makeDouble(j.get<double>());
    } else if (j.is_string()) {
        return interpreter::NaabVal::makeString(j.get<std::string>());
    } else if (j.is_array()) {
        std::vector<interpreter::NaabVal> vec;
        for (const auto& item : j) {
            vec.push_back(jsonToValue(item));
        }
        return interpreter::NaabVal::makeList(std::move(vec));
    } else if (j.is_object()) {
        std::unordered_map<std::string, interpreter::NaabVal> map;
        for (auto it = j.begin(); it != j.end(); ++it) {
            map[it.key()] = jsonToValue(it.value());
        }
        return interpreter::NaabVal::makeDict(std::move(map));
    }

    // Unknown type - return null
    return interpreter::NaabVal::makeNull();
}

// V-DOS-009: Maximum JSON serialization depth
// Default depth 64, overridden by governance max_json_depth via limits::setMaxJsonDepth()
static constexpr int DEFAULT_JSON_DEPTH = 64;

// Helper: Convert NaabVal to nlohmann::json with depth + cycle guards
json valueToJson(const interpreter::NaabVal& val, int depth = 0,
                 std::unordered_set<const void*>* visited = nullptr) {
    int max_depth = naab::limits::getMaxJsonDepth();
    if (depth > max_depth) {
        throw std::runtime_error(fmt::format(
            "json.stringify(): maximum nesting depth exceeded ({})", max_depth));
    }
    if (val.isNull()) {
        return nullptr;
    } else if (val.isInt()) {
        return val.asInt();
    } else if (val.isDouble()) {
        return val.asDouble();
    } else if (val.isBool()) {
        return val.asBool();
    } else if (val.isString()) {
        return val.asString();
    } else if (val.isList()) {
        // Cycle detection for containers
        const void* ptr = val.toLegacy().get();
        std::unordered_set<const void*> local_visited;
        if (!visited) visited = &local_visited;
        if (ptr && !visited->insert(ptr).second) {
            throw std::runtime_error("json.stringify(): circular reference detected");
        }
        json arr = json::array();
        for (const auto& item : val.asListConst()) {
            arr.push_back(valueToJson(item, depth + 1, visited));
        }
        if (ptr) visited->erase(ptr);
        return arr;
    } else if (val.isDict()) {
        const void* ptr = val.toLegacy().get();
        std::unordered_set<const void*> local_visited;
        if (!visited) visited = &local_visited;
        if (ptr && !visited->insert(ptr).second) {
            throw std::runtime_error("json.stringify(): circular reference detected");
        }
        json obj = json::object();
        for (const auto& [key, value] : val.asDictConst()) {
            obj[key] = valueToJson(value, depth + 1, visited);
        }
        if (ptr) visited->erase(ptr);
        return obj;
    } else if (val.isStructVal()) {
        // V-DOS-013: Cycle detection for structs (mirrors List/Dict pattern)
        const void* ptr = val.toLegacy().get();
        std::unordered_set<const void*> local_visited;
        if (!visited) visited = &local_visited;
        if (ptr && !visited->insert(ptr).second) {
            throw std::runtime_error("json.stringify(): circular reference detected");
        }
        json obj = json::object();
        auto& sv = val.asStructConst();
        if (sv && sv->definition) {
            const auto& fields = sv->definition->fields;
            const auto& values = sv->field_values;
            for (size_t i = 0; i < fields.size() && i < values.size(); ++i) {
                const std::string& field_name = fields[i].name;
                if (!values[i].isNull()) {
                    obj[field_name] = valueToJson(values[i], depth + 1, visited);
                } else {
                    obj[field_name] = nullptr;
                }
            }
        }
        if (ptr) visited->erase(ptr);
        return obj;
    }
    return "<unsupported>";
}

interpreter::NaabVal JSONModule::parse(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("json.parse() requires JSON string argument");
    }

    std::string json_str = args[0].toString();

    try {
        // V-DOS-009: depth guard before parse to prevent stack overflow DoS
        {
            int depth = 0;
            bool in_string = false, escape_next = false;
            for (unsigned char c : json_str) {
                if (escape_next) { escape_next = false; continue; }
                if (c == '\\' && in_string) { escape_next = true; continue; }
                if (c == '"') { in_string = !in_string; continue; }
                if (!in_string) {
                    if (c == '{' || c == '[') { int md = naab::limits::getMaxJsonDepth(); if (++depth > md * 2) throw std::runtime_error(fmt::format("json.parse(): maximum nesting depth exceeded ({})", md * 2)); }
                    else if (c == '}' || c == ']') { if (depth > 0) --depth; }
                }
            }
        }
        // Parse JSON using nlohmann/json
        json j = json::parse(json_str);

        // Convert to NAAb Value
        return jsonToValue(j);

    } catch (const json::parse_error& e) {
        std::string error_msg = e.what();
        std::string hint;

        // Detect common issues and add helpful hints
        if (error_msg.find("control character") != std::string::npos ||
            error_msg.find("U+000A") != std::string::npos ||
            error_msg.find("U+000D") != std::string::npos) {
            hint = "\n\n  Help:\n"
                   "  - The input string contains unescaped newlines or control characters.\n"
                   "  - Common cause: C++ polyglot output includes trailing newlines (std::endl).\n"
                   "  - Fix: Use std::cout << result (without << std::endl) in your C++ code.\n"
                   "  - Or: Use string.trim() on the value before calling json.parse().\n";
        } else if (error_msg.find("unexpected end of input") != std::string::npos) {
            hint = "\n\n  Help:\n"
                   "  - The JSON string is incomplete or empty.\n"
                   "  - Check that the polyglot block actually outputs a value.\n"
                   "  - Common cause: C++ code uses 'return' in void main (use std::cout instead).\n";
        } else if (json_str.empty()) {
            hint = "\n\n  Help:\n"
                   "  - The input to json.parse() is an empty string.\n"
                   "  - Ensure the polyglot block produces output via std::cout or print().\n";
        }

        // Show a preview of the problematic input
        std::string preview = json_str.substr(0, 100);
        // Escape control chars for display
        std::string escaped_preview;
        for (char c : preview) {
            if (c == '\n') escaped_preview += "\\n";
            else if (c == '\r') escaped_preview += "\\r";
            else if (c == '\t') escaped_preview += "\\t";
            else escaped_preview += c;
        }

        throw std::runtime_error(
            fmt::format("JSON parse error at byte {}: {}{}\n\n  Input preview: \"{}\"",
                        e.byte, e.what(), hint, escaped_preview)
        );
    } catch (const std::exception& e) {
        throw std::runtime_error(
            fmt::format("JSON parse error: {}", e.what())
        );
    }
}

interpreter::NaabVal JSONModule::stringify(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("json.stringify() requires value argument");
    }

    try {
        // Convert NAAb Value to JSON
        json j = valueToJson(args[0]);

        // Optional: indentation (pretty print)
        int indent = -1;  // Compact by default
        if (args.size() >= 2) {
            indent = args[1].toInt();
        }

        // Stringify
        std::string result;
        if (indent >= 0) {
            result = j.dump(indent);  // Pretty print
        } else {
            result = j.dump();  // Compact
        }

        return interpreter::NaabVal::makeString(result);

    } catch (const std::exception& e) {
        throw std::runtime_error(
            fmt::format("JSON stringify error: {}", e.what())
        );
    }
}

interpreter::NaabVal JSONModule::parse_object(
    std::vector<interpreter::NaabVal>& args) {

    // Parse the JSON using the main parse function
    auto result = parse(args);

    // Validate that the result is a dictionary (object)
    if (result.isDict()) {
        return result;
    }
    throw std::runtime_error("JSON parse_object: Expected JSON object, got non-object type");
}

interpreter::NaabVal JSONModule::parse_array(
    std::vector<interpreter::NaabVal>& args) {

    // Parse the JSON using the main parse function
    auto result = parse(args);

    // Validate that the result is an array
    if (result.isList()) {
        return result;
    }
    throw std::runtime_error("JSON parse_array: Expected JSON array, got non-array type");
}

interpreter::NaabVal JSONModule::is_valid(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        return interpreter::NaabVal::makeBool(false);
    }

    try {
        // Try to parse the JSON
        parse(args);
        // If successful, return true
        return interpreter::NaabVal::makeBool(true);
    } catch (...) {
        // If parse throws, JSON is invalid
        return interpreter::NaabVal::makeBool(false);
    }
}

interpreter::NaabVal JSONModule::pretty(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("json.pretty() requires value argument");
    }

    // Default indent is 2 spaces for pretty printing
    int indent = 2;
    if (args.size() >= 2) {
        indent = args[1].toInt();
    }

    // Call stringify with indent
    std::vector<interpreter::NaabVal> stringify_args = {
        args[0],
        interpreter::NaabVal::makeInt(indent)
    };

    return stringify(stringify_args);
}

} // namespace stdlib
} // namespace naab
