#include "naab/json_result_parser.h"
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <regex>
#include <algorithm>
#include <string>
#include <sstream>
#include <unordered_map>

namespace naab {
namespace runtime {

// File-local helper for JSON -> Value conversion (must be before parse() which calls it)
static std::shared_ptr<interpreter::Value> parseValue(const nlohmann::json& j) {
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
        if (std::regex_match(trimmed, std::regex("^-?\\d+$"))) {
            int64_t val = std::stoll(trimmed);
            return std::make_shared<interpreter::Value>(static_cast<int>(val));
        }
    } catch (...) {}

    // Try to parse as double
    try {
        if (std::regex_match(trimmed, std::regex("^-?\\d+\\.\\d+$"))) {
            double val = std::stod(trimmed);
            return std::make_shared<interpreter::Value>(val);
        }
    } catch (...) {}

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

// Phase 12: Parse polyglot stdout with sentinel detection and JSON scanning
PolyglotOutput parsePolyglotOutput(const std::string& stdout_output, const std::string& return_type) {
    PolyglotOutput result;
    const std::string sentinel = "__NAAB_RETURN__:";

    // Split stdout into lines
    std::vector<std::string> lines;
    std::istringstream stream(stdout_output);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Pass 1: Check for explicit sentinel (highest priority)
    int sentinel_idx = -1;
    for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
        if (lines[i].find(sentinel) == 0) {
            std::string json_data = lines[i].substr(sentinel.size());
            while (!json_data.empty() && (json_data.back() == '\n' || json_data.back() == '\r' || json_data.back() == ' ')) {
                json_data.pop_back();
            }
            result.return_value = JsonResultParser::parse(json_data);
            sentinel_idx = i;
            break;
        }
    }

    // Pass 2: If no sentinel and return_type specified (e.g., "JSON"), find last valid JSON line
    if (!result.return_value && !return_type.empty()) {
        for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
            if (i == sentinel_idx) continue;
            std::string trimmed = lines[i];
            size_t start = trimmed.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            trimmed = trimmed.substr(start);

            if (!trimmed.empty() && (trimmed[0] == '{' || trimmed[0] == '[' || trimmed[0] == '"' ||
                std::isdigit(trimmed[0]) || trimmed[0] == '-' ||
                trimmed.substr(0, 4) == "true" || trimmed.substr(0, 5) == "false" ||
                trimmed.substr(0, 4) == "null")) {
                try {
                    nlohmann::json::parse(trimmed);  // Validate JSON
                    result.return_value = JsonResultParser::parse(trimmed);
                    sentinel_idx = i;
                    break;
                } catch (...) {}
            }
        }
    }

    // Remaining lines = log output
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (i != sentinel_idx) {
            if (!result.log_output.empty()) result.log_output += "\n";
            result.log_output += lines[i];
        }
    }

    return result;
}

} // namespace runtime
} // namespace naab
