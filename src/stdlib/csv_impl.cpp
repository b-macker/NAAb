//
// NAAb Standard Library - CSV Module
// CSV file reading and writing
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <memory>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Forward declarations of helper functions
static std::string getString(const std::shared_ptr<interpreter::Value>& val);
static std::vector<std::string> getStringArray(const std::shared_ptr<interpreter::Value>& val);
static std::vector<std::vector<std::string>> getArrayOfArrays(const std::shared_ptr<interpreter::Value>& val);
static std::vector<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>> getArrayOfDicts(const std::shared_ptr<interpreter::Value>& val);
static std::shared_ptr<interpreter::Value> parseCSV(const std::string& content, const std::string& delimiter);
static std::shared_ptr<interpreter::Value> parseCSVDict(const std::string& content, const std::string& delimiter);
static std::vector<std::string> parseCSVLine(const std::string& line, const std::string& delimiter);
static std::string formatCSVRow(const std::vector<std::string>& row, const std::string& delimiter);

// Implementation of CsvModule public methods

bool CsvModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "read", "read_dict", "parse", "parse_dict",
        "write", "write_dict", "format_row", "format_rows"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> CsvModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Function 1: read
    if (function_name == "read") {
        if (args.size() != 1) {
            throw std::runtime_error("read() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);

        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open CSV file: " + path);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        return parseCSV(content, ",");
    }

    // Function 2: read_dict
    if (function_name == "read_dict") {
        if (args.size() != 1) {
            throw std::runtime_error("read_dict() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);

        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open CSV file: " + path);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        return parseCSVDict(content, ",");
    }

    // Function 3: parse
    if (function_name == "parse") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("parse() takes 1 or 2 arguments");
        }
        std::string content = getString(args[0]);
        std::string delimiter = args.size() == 2 ? getString(args[1]) : ",";

        return parseCSV(content, delimiter);
    }

    // Function 4: parse_dict
    if (function_name == "parse_dict") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("parse_dict() takes 1 or 2 arguments");
        }
        std::string content = getString(args[0]);
        std::string delimiter = args.size() == 2 ? getString(args[1]) : ",";

        return parseCSVDict(content, delimiter);
    }

    // Function 5: write
    if (function_name == "write") {
        if (args.size() < 2 || args.size() > 3) {
            throw std::runtime_error("write() takes 2 or 3 arguments");
        }
        std::string path = getString(args[0]);
        auto rows = getArrayOfArrays(args[1]);
        std::string delimiter = args.size() == 3 ? getString(args[2]) : ",";

        std::ofstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open CSV file for writing: " + path);
        }

        for (const auto& row : rows) {
            file << formatCSVRow(row, delimiter) << "\n";
        }

        return std::make_shared<interpreter::Value>();
    }

    // Function 6: write_dict
    if (function_name == "write_dict") {
        if (args.size() < 2 || args.size() > 3) {
            throw std::runtime_error("write_dict() takes 2 or 3 arguments");
        }
        std::string path = getString(args[0]);
        auto rows = getArrayOfDicts(args[1]);
        std::string delimiter = args.size() == 3 ? getString(args[2]) : ",";

        if (rows.empty()) {
            return std::make_shared<interpreter::Value>();
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open CSV file for writing: " + path);
        }

        // Get headers from first row
        std::vector<std::string> headers;
        for (const auto& [key, _] : rows[0]) {
            headers.push_back(key);
        }
        size_t expected_keys = headers.size();

        // Write header row
        file << formatCSVRow(headers, delimiter) << "\n";

        // Write data rows
        int row_num = 0;
        for (const auto& row_dict : rows) {
            // Validate that all rows have same number of keys
            if (row_dict.size() != expected_keys) {
                throw std::runtime_error("write_dict() row " + std::to_string(row_num) +
                                       " has " + std::to_string(row_dict.size()) +
                                       " keys, expected " + std::to_string(expected_keys));
            }

            std::vector<std::string> row_values;
            for (const auto& header : headers) {
                auto it = row_dict.find(header);
                if (it != row_dict.end()) {
                    row_values.push_back(getString(it->second));
                } else {
                    throw std::runtime_error("write_dict() row " + std::to_string(row_num) +
                                           " missing key '" + header + "'");
                }
            }
            file << formatCSVRow(row_values, delimiter) << "\n";
            row_num++;
        }

        return std::make_shared<interpreter::Value>();
    }

    // Function 7: format_row
    if (function_name == "format_row") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("format_row() takes 1 or 2 arguments");
        }
        auto row = getStringArray(args[0]);
        std::string delimiter = args.size() == 2 ? getString(args[1]) : ",";

        return std::make_shared<interpreter::Value>(formatCSVRow(row, delimiter));
    }

    // Function 8: format_rows
    if (function_name == "format_rows") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("format_rows() takes 1 or 2 arguments");
        }
        auto rows = getArrayOfArrays(args[0]);
        std::string delimiter = args.size() == 2 ? getString(args[1]) : ",";

        std::string result;
        for (const auto& row : rows) {
            result += formatCSVRow(row, delimiter) + "\n";
        }
        return std::make_shared<interpreter::Value>(result);
    }

    // Fuzzy matching for typos
    static const std::vector<std::string> FUNCTIONS = {
        "read", "read_dict", "parse", "parse_dict",
        "write", "write_dict", "format_row", "format_rows"
    };
    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown csv function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }
    throw std::runtime_error(oss.str());
}

// CSV parsing helper functions
static std::shared_ptr<interpreter::Value> parseCSV(const std::string& content, const std::string& delimiter) {
        std::vector<std::shared_ptr<interpreter::Value>> rows;
        std::istringstream iss(content);
        std::string line;

        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            // Convert vector<string> to vector<shared_ptr<Value>>
            std::vector<std::string> fields = parseCSVLine(line, delimiter);
            std::vector<std::shared_ptr<interpreter::Value>> row;
            for (const auto& field : fields) {
                row.push_back(std::make_shared<interpreter::Value>(field));
            }
            rows.push_back(std::make_shared<interpreter::Value>(row));
        }

        return std::make_shared<interpreter::Value>(rows);
    }

static std::shared_ptr<interpreter::Value> parseCSVDict(const std::string& content, const std::string& delimiter) {
        std::vector<std::shared_ptr<interpreter::Value>> rows;
        std::istringstream iss(content);
        std::string line;

        // Read header
        if (!std::getline(iss, line)) {
            throw std::runtime_error("parseCSVDict() requires at least a header row");
        }
        std::vector<std::string> headers = parseCSVLine(line, delimiter);
        size_t expected_cols = headers.size();

        // Read data rows
        int row_num = 1;  // Start at 1 (0 is header)
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::vector<std::string> values = parseCSVLine(line, delimiter);

            // Validate column count
            if (values.size() != expected_cols) {
                throw std::runtime_error("parseCSVDict() row " + std::to_string(row_num) +
                                       " has " + std::to_string(values.size()) +
                                       " columns, expected " + std::to_string(expected_cols));
            }

            std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> row_dict;
            for (size_t i = 0; i < headers.size(); ++i) {
                row_dict[headers[i]] = std::make_shared<interpreter::Value>(values[i]);
            }
            rows.push_back(std::make_shared<interpreter::Value>(row_dict));
            row_num++;
        }

        return std::make_shared<interpreter::Value>(rows);
    }

static std::vector<std::string> parseCSVLine(const std::string& line, const std::string& delimiter) {
        std::vector<std::string> fields;
        std::string field;
        bool in_quotes = false;

        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];

            if (c == '"') {
                in_quotes = !in_quotes;
            } else if (!in_quotes && line.substr(i, delimiter.size()) == delimiter) {
                fields.push_back(field);
                field.clear();
                i += delimiter.size() - 1;
            } else {
                field += c;
            }
        }
        fields.push_back(field);

        return fields;
    }

static std::string formatCSVRow(const std::vector<std::string>& row, const std::string& delimiter) {
        std::string result;
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) result += delimiter;

            // Quote field if it contains delimiter or quotes
            if (row[i].find(delimiter) != std::string::npos || row[i].find('"') != std::string::npos) {
                result += '"' + row[i] + '"';
            } else {
                result += row[i];
            }
        }
        return result;
    }

// Helper functions
static std::string getString(const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return arg;
            } else {
                throw std::runtime_error("Expected string value");
            }
        }, val->data);
    }

static std::vector<std::string> getStringArray(const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> std::vector<std::string> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
                std::vector<std::string> result;
                for (const auto& item : arg) {
                    result.push_back(getString(item));
                }
                return result;
            } else {
                throw std::runtime_error("Expected array value");
            }
        }, val->data);
    }

static std::vector<std::vector<std::string>> getArrayOfArrays(const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> std::vector<std::vector<std::string>> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
                std::vector<std::vector<std::string>> result;
                for (const auto& item : arg) {
                    result.push_back(getStringArray(item));
                }
                return result;
            } else {
                throw std::runtime_error("Expected array of arrays");
            }
        }, val->data);
    }

static std::vector<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>> getArrayOfDicts(
        const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> std::vector<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
                std::vector<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>> result;
                for (const auto& item : arg) {
                    auto dict = std::visit([](auto&& dict_arg) -> std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> {
                        using DT = std::decay_t<decltype(dict_arg)>;
                        if constexpr (std::is_same_v<DT, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
                            return dict_arg;
                        } else {
                            throw std::runtime_error("Expected dictionary");
                        }
                    }, item->data);
                    result.push_back(dict);
                }
                return result;
            } else {
                throw std::runtime_error("Expected array of dictionaries");
            }
        }, val->data);
    }

} // namespace stdlib
} // namespace naab
