//
// NAAb Standard Library - CSV Module
// CSV file reading and writing
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/sandbox.h"
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
static std::string getString(const interpreter::NaabVal& val);
static std::vector<std::string> getStringArray(const interpreter::NaabVal& val);
static std::vector<std::vector<std::string>> getArrayOfArrays(const interpreter::NaabVal& val);
static std::vector<std::unordered_map<std::string, interpreter::NaabVal>> getArrayOfDicts(const interpreter::NaabVal& val);
static interpreter::NaabVal parseCSV(const std::string& content, const std::string& delimiter);
static interpreter::NaabVal parseCSVDict(const std::string& content, const std::string& delimiter);
static std::vector<std::string> parseCSVLine(const std::string& line, const std::string& delimiter);
static std::string formatCSVRow(const std::vector<std::string>& row, const std::string& delimiter);

// Security: Check sandbox permissions before CSV file operations
static void checkCsvSandbox(const std::string& path, const std::string& operation) {
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (!sandbox) return;

    bool allowed = false;
    std::string capability;

    if (operation == "read") {
        allowed = sandbox->canRead(path);
        capability = "FS_READ";
    } else if (operation == "write") {
        allowed = sandbox->canWrite(path);
        capability = "FS_WRITE";
    }

    if (!allowed) {
        sandbox->logViolation("csv." + operation, path, capability + " capability required");
        throw std::runtime_error(
            "Security: csv." + operation + "() denied by sandbox\n\n"
            "  Path: " + path + "\n\n"
            "  The current sandbox level does not permit this file operation.\n"
            "  The project owner can adjust the sandbox level in the project configuration.\n"
        );
    }
}

// Implementation of CsvModule public methods

bool CsvModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "read", "read_dict", "parse", "parse_dict",
        "write", "write_dict", "format_row", "format_rows",
        "stringify"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal CsvModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    // Function 1: read
    if (function_name == "read") {
        if (args.size() != 1) {
            throw std::runtime_error("read() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        checkCsvSandbox(path, "read");

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
        checkCsvSandbox(path, "read");

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
        checkCsvSandbox(path, "write");
        auto rows = getArrayOfArrays(args[1]);
        std::string delimiter = args.size() == 3 ? getString(args[2]) : ",";

        std::ofstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open CSV file for writing: " + path);
        }

        for (const auto& row : rows) {
            file << formatCSVRow(row, delimiter) << "\n";
        }

        return interpreter::NaabVal::makeNull();
    }

    // Function 6: write_dict
    if (function_name == "write_dict") {
        if (args.size() < 2 || args.size() > 3) {
            throw std::runtime_error("write_dict() takes 2 or 3 arguments");
        }
        std::string path = getString(args[0]);
        checkCsvSandbox(path, "write");
        auto rows = getArrayOfDicts(args[1]);
        std::string delimiter = args.size() == 3 ? getString(args[2]) : ",";

        if (rows.empty()) {
            return interpreter::NaabVal::makeNull();
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
                    row_values.push_back(it->second.toString());
                } else {
                    throw std::runtime_error("write_dict() row " + std::to_string(row_num) +
                                           " missing key '" + header + "'");
                }
            }
            file << formatCSVRow(row_values, delimiter) << "\n";
            row_num++;
        }

        return interpreter::NaabVal::makeNull();
    }

    // Function 7: format_row
    if (function_name == "format_row") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("format_row() takes 1 or 2 arguments");
        }
        auto row = getStringArray(args[0]);
        std::string delimiter = args.size() == 2 ? getString(args[1]) : ",";

        return interpreter::NaabVal::makeString(formatCSVRow(row, delimiter));
    }

    // Function 8: format_rows / stringify
    if (function_name == "format_rows" || function_name == "stringify") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("format_rows() takes 1 or 2 arguments");
        }
        auto rows = getArrayOfArrays(args[0]);
        std::string delimiter = args.size() == 2 ? getString(args[1]) : ",";

        std::string result;
        for (const auto& row : rows) {
            result += formatCSVRow(row, delimiter) + "\n";
        }
        return interpreter::NaabVal::makeString(result);
    }

    // Common LLM mistakes
    if (function_name == "load" || function_name == "read_file" || function_name == "open") {
        throw std::runtime_error(
            "Unknown csv function: " + function_name + "\n\n"
            "  Did you mean: csv.read(filepath) or csv.parse(string)?\n"
            "  Example: let data = csv.read(\"data.csv\")\n"
        );
    }

    // Fuzzy matching for typos
    static const std::vector<std::string> FUNCTIONS = {
        "read", "read_dict", "parse", "parse_dict",
        "write", "write_dict", "format_row", "format_rows",
        "stringify"
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
static interpreter::NaabVal parseCSV(const std::string& content, const std::string& delimiter) {
        std::vector<interpreter::NaabVal> rows;
        std::istringstream iss(content);
        std::string line;

        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            std::vector<std::string> fields = parseCSVLine(line, delimiter);
            std::vector<interpreter::NaabVal> row;
            for (const auto& field : fields) {
                row.push_back(interpreter::NaabVal::makeString(field));
            }
            rows.push_back(interpreter::NaabVal::makeList(std::move(row)));
        }

        return interpreter::NaabVal::makeList(std::move(rows));
    }

static interpreter::NaabVal parseCSVDict(const std::string& content, const std::string& delimiter) {
        std::vector<interpreter::NaabVal> rows;
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

            std::unordered_map<std::string, interpreter::NaabVal> row_dict;
            for (size_t i = 0; i < headers.size(); ++i) {
                row_dict[headers[i]] = interpreter::NaabVal::makeString(values[i]);
            }
            rows.push_back(interpreter::NaabVal::makeDict(std::move(row_dict)));
            row_num++;
        }

        return interpreter::NaabVal::makeList(std::move(rows));
    }

static std::vector<std::string> parseCSVLine(const std::string& line, const std::string& delimiter) {
        std::vector<std::string> fields;
        std::string field;
        bool in_quotes = false;

        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];

            if (c == '"') {
                if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                    // RFC 4180: escaped quote ("") → literal "
                    field += '"';
                    ++i;  // skip the second quote
                } else {
                    in_quotes = !in_quotes;
                }
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

            // Quote field if it contains delimiter, quotes, or newlines (RFC 4180)
            if (row[i].find(delimiter) != std::string::npos ||
                row[i].find('"') != std::string::npos ||
                row[i].find('\n') != std::string::npos ||
                row[i].find('\r') != std::string::npos) {
                // RFC 4180: double internal quotes before wrapping
                std::string escaped = row[i];
                size_t pos = 0;
                while ((pos = escaped.find('"', pos)) != std::string::npos) {
                    escaped.insert(pos, 1, '"');
                    pos += 2;
                }
                result += '"' + escaped + '"';
            } else {
                result += row[i];
            }
        }
        return result;
    }

// Helper functions
static std::string getString(const interpreter::NaabVal& val) {
    if (!val.isString()) throw std::runtime_error("Expected string value");
    return val.asString();
}

static std::vector<std::string> getStringArray(const interpreter::NaabVal& val) {
    if (!val.isList()) throw std::runtime_error("Expected array value");
    std::vector<std::string> result;
    for (const auto& item : val.asListConst()) {
        result.push_back(item.toString());
    }
    return result;
}

static std::vector<std::vector<std::string>> getArrayOfArrays(const interpreter::NaabVal& val) {
    if (!val.isList()) throw std::runtime_error("Expected array of arrays");
    std::vector<std::vector<std::string>> result;
    for (const auto& item : val.asListConst()) {
        result.push_back(getStringArray(item));
    }
    return result;
}

static std::vector<std::unordered_map<std::string, interpreter::NaabVal>> getArrayOfDicts(
        const interpreter::NaabVal& val) {
    if (!val.isList()) throw std::runtime_error("Expected array of dictionaries");
    std::vector<std::unordered_map<std::string, interpreter::NaabVal>> result;
    for (const auto& item : val.asListConst()) {
        if (!item.isDict()) throw std::runtime_error("Expected dictionary");
        result.push_back(item.asDictConst());
    }
    return result;
}

} // namespace stdlib
} // namespace naab
