#include "naab/json_result_parser.h"
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <regex>
#include <algorithm>
#include <string>
#include <unordered_map>

namespace naab {
namespace runtime {

std::shared_ptr<interpreter::Value> JsonResultParser::parse(const std::string& json_output) {
    try {
        // Trim whitespace and newlines
        std::string trimmed = json_output;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
        trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);

        if (trimmed.empty()) {
            return std::make_shared<interpreter::Value>();  // null
        }

        // Try to parse as JSON
        auto j = nlohmann::json::parse(trimmed);
        return parseValue(j);

    } catch (const nlohmann::json::parse_error& e) {
        // Not valid JSON - try simple parsing
        return parseSimple(json_output);
    }
}

std::shared_ptr<interpreter::Value> JsonResultParser::parseSimple(const std::string& output) {
    std::string trimmed = output;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);

    if (trimmed.empty()) {
        return std::make_shared<interpreter::Value>();  // null
    }

    // Try to parse as integer
    try {
        // Check if it looks like an integer (optional minus, digits only)
        if (std::regex_match(trimmed, std::regex("^-?\\d+$"))) {
            int64_t val = std::stoll(trimmed);
            return std::make_shared<interpreter::Value>(static_cast<int>(val));
        }
    } catch (...) {
        // Not an integer
    }

    // Try to parse as double
    try {
        if (std::regex_match(trimmed, std::regex("^-?\\d+\\.\\d+$"))) {
            double val = std::stod(trimmed);
            return std::make_shared<interpreter::Value>(val);
        }
    } catch (...) {
        // Not a double
    }

    // Try to parse as boolean
    if (trimmed == "true" || trimmed == "True" || trimmed == "TRUE") {
        return std::make_shared<interpreter::Value>(true);
    }
    if (trimmed == "false" || trimmed == "False" || trimmed == "FALSE") {
        return std::make_shared<interpreter::Value>(false);
    }

    // Try to parse as null
    if (trimmed == "null" || trimmed == "nil" || trimmed == "None") {
        return std::make_shared<interpreter::Value>();
    }

    // Otherwise, return as string
    return std::make_shared<interpreter::Value>(trimmed);
}

std::shared_ptr<interpreter::Value> JsonResultParser::parseValue(const nlohmann::json& j) {
    if (j.is_null()) {
        return std::make_shared<interpreter::Value>();
    }

    if (j.is_boolean()) {
        return std::make_shared<interpreter::Value>(j.get<bool>());
    }

    if (j.is_number_integer()) {
        return std::make_shared<interpreter::Value>(static_cast<int>(j.get<int64_t>()));
    }

    if (j.is_number_float()) {
        return std::make_shared<interpreter::Value>(j.get<double>());
    }

    if (j.is_string()) {
        return std::make_shared<interpreter::Value>(j.get<std::string>());
    }

    if (j.is_array()) {
        std::vector<std::shared_ptr<interpreter::Value>> arr;
        for (const auto& item : j) {
            arr.push_back(parseValue(item));
        }
        return std::make_shared<interpreter::Value>(arr);
    }

    if (j.is_object()) {
        std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> obj;
        for (auto it = j.begin(); it != j.end(); ++it) {
            obj[it.key()] = parseValue(it.value());
        }
        return std::make_shared<interpreter::Value>(obj);
    }

    // Unknown type - return as string
    return std::make_shared<interpreter::Value>(j.dump());
}

} // namespace runtime
} // namespace naab
