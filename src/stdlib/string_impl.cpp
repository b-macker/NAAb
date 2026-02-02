//
// NAAb Standard Library - String Module
// Complete implementation with all 12 functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Forward declarations
static std::string getString(const std::shared_ptr<interpreter::Value>& val);
static std::vector<std::string> getStringArray(const std::shared_ptr<interpreter::Value>& val);
static int getInt(const std::shared_ptr<interpreter::Value>& val);
static std::shared_ptr<interpreter::Value> makeString(const std::string& s);
static std::shared_ptr<interpreter::Value> makeInt(int i);
static std::shared_ptr<interpreter::Value> makeBool(bool b);
static std::shared_ptr<interpreter::Value> makeStringArray(const std::vector<std::string>& arr);

bool StringModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "length", "substring", "concat", "split", "join",
        "trim", "upper", "lower", "replace", "contains",
        "starts_with", "ends_with", "index_of", "repeat"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> StringModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Function 1: length
    if (function_name == "length") {
        if (args.size() != 1) {
            throw std::runtime_error("length() takes exactly 1 argument");
        }
        std::string s = getString(args[0]);
        return makeInt(static_cast<int>(s.length()));
    }

    // Function 2: substring
    if (function_name == "substring") {
        if (args.size() != 3) {
            throw std::runtime_error("substring() takes exactly 3 arguments");
        }
        std::string s = getString(args[0]);
        int start = getInt(args[1]);
        int end = getInt(args[2]);

        // Bounds checking
        if (start < 0) start = 0;
        if (end > static_cast<int>(s.length())) end = s.length();
        if (start >= end) return makeString("");

        return makeString(s.substr(start, end - start));
    }

    // Function 3: concat
    if (function_name == "concat") {
        if (args.size() != 2) {
            throw std::runtime_error("concat() takes exactly 2 arguments");
        }
        std::string s1 = getString(args[0]);
        std::string s2 = getString(args[1]);
        return makeString(s1 + s2);
    }

    // Function 4: split
    if (function_name == "split") {
        if (args.size() != 2) {
            throw std::runtime_error("split() takes exactly 2 arguments");
        }
        std::string s = getString(args[0]);
        std::string delimiter = getString(args[1]);

        std::vector<std::string> parts;
        if (delimiter.empty()) {
            // Split into individual characters
            for (char c : s) {
                parts.push_back(std::string(1, c));
            }
        } else {
            size_t pos = 0;
            size_t found;
            while ((found = s.find(delimiter, pos)) != std::string::npos) {
                parts.push_back(s.substr(pos, found - pos));
                pos = found + delimiter.length();
            }
            parts.push_back(s.substr(pos));
        }
        return makeStringArray(parts);
    }

    // Function 5: join
    if (function_name == "join") {
        if (args.size() != 2) {
            throw std::runtime_error("join() takes exactly 2 arguments");
        }
        std::vector<std::string> arr = getStringArray(args[0]);
        std::string delimiter = getString(args[1]);

        if (arr.empty()) return makeString("");

        std::string result = arr[0];
        for (size_t i = 1; i < arr.size(); ++i) {
            result += delimiter + arr[i];
        }
        return makeString(result);
    }

    // Function 6: trim
    if (function_name == "trim") {
        if (args.size() != 1) {
            throw std::runtime_error("trim() takes exactly 1 argument");
        }
        std::string s = getString(args[0]);

        // Trim leading whitespace
        size_t start = s.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return makeString("");

        // Trim trailing whitespace
        size_t end = s.find_last_not_of(" \t\n\r");
        return makeString(s.substr(start, end - start + 1));
    }

    // Function 7: upper
    if (function_name == "upper") {
        if (args.size() != 1) {
            throw std::runtime_error("upper() takes exactly 1 argument");
        }
        std::string s = getString(args[0]);
        std::transform(s.begin(), s.end(), s.begin(), ::toupper);
        return makeString(s);
    }

    // Function 8: lower
    if (function_name == "lower") {
        if (args.size() != 1) {
            throw std::runtime_error("lower() takes exactly 1 argument");
        }
        std::string s = getString(args[0]);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return makeString(s);
    }

    // Function 9: replace
    if (function_name == "replace") {
        if (args.size() != 3) {
            throw std::runtime_error("replace() takes exactly 3 arguments");
        }
        std::string s = getString(args[0]);
        std::string old_str = getString(args[1]);
        std::string new_str = getString(args[2]);

        if (old_str.empty()) return makeString(s);

        size_t pos = 0;
        while ((pos = s.find(old_str, pos)) != std::string::npos) {
            s.replace(pos, old_str.length(), new_str);
            pos += new_str.length();
        }
        return makeString(s);
    }

    // Function 10: contains
    if (function_name == "contains") {
        if (args.size() != 2) {
            throw std::runtime_error("contains() takes exactly 2 arguments");
        }
        std::string s = getString(args[0]);
        std::string substr = getString(args[1]);
        return makeBool(s.find(substr) != std::string::npos);
    }

    // Function 11: starts_with
    if (function_name == "starts_with") {
        if (args.size() != 2) {
            throw std::runtime_error("starts_with() takes exactly 2 arguments");
        }
        std::string s = getString(args[0]);
        std::string prefix = getString(args[1]);
        return makeBool(s.find(prefix) == 0);
    }

    // Function 12: ends_with
    if (function_name == "ends_with") {
        if (args.size() != 2) {
            throw std::runtime_error("ends_with() takes exactly 2 arguments");
        }
        std::string s = getString(args[0]);
        std::string suffix = getString(args[1]);
        if (suffix.length() > s.length()) return makeBool(false);
        return makeBool(s.compare(s.length() - suffix.length(),
                                   suffix.length(), suffix) == 0);
    }

    // Function 13: index_of
    if (function_name == "index_of") {
        if (args.size() != 2) {
            throw std::runtime_error("index_of() takes exactly 2 arguments");
        }
        std::string s = getString(args[0]);
        std::string substr = getString(args[1]);
        size_t pos = s.find(substr);
        if (pos == std::string::npos) {
            return makeInt(-1);
        }
        return makeInt(static_cast<int>(pos));
    }

    // Function 14: repeat
    if (function_name == "repeat") {
        if (args.size() != 2) {
            throw std::runtime_error("repeat() takes exactly 2 arguments");
        }
        std::string s = getString(args[0]);
        int count = getInt(args[1]);
        if (count < 0) {
            throw std::runtime_error("repeat() count must be non-negative");
        }
        if (count == 0) return makeString("");

        std::string result;
        result.reserve(s.length() * count);
        for (int i = 0; i < count; ++i) {
            result += s;
        }
        return makeString(result);
    }

    throw std::runtime_error("Unknown function: " + function_name);
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

static int getInt(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> int {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            return arg;
        } else {
            throw std::runtime_error("Expected integer value");
        }
    }, val->data);
}

static std::shared_ptr<interpreter::Value> makeString(const std::string& s) {
    return std::make_shared<interpreter::Value>(s);
}

static std::shared_ptr<interpreter::Value> makeInt(int i) {
    return std::make_shared<interpreter::Value>(i);
}

static std::shared_ptr<interpreter::Value> makeBool(bool b) {
    return std::make_shared<interpreter::Value>(b);
}

static std::shared_ptr<interpreter::Value> makeStringArray(const std::vector<std::string>& arr) {
    std::vector<std::shared_ptr<interpreter::Value>> elements;
    for (const auto& s : arr) {
        elements.push_back(makeString(s));
    }
    return std::make_shared<interpreter::Value>(elements);
}

} // namespace stdlib
} // namespace naab
