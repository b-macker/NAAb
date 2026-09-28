//
// NAAb Standard Library - String Module
// Complete implementation with all 12 functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include "naab/string_ops.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

// V-DOS-011: Maximum string result size (64 MB)
static constexpr size_t MAX_STRING_RESULT_BYTES = 64 * 1024 * 1024;

namespace naab {
namespace stdlib {

// Forward declarations
static std::string getString(const interpreter::NaabVal& val);
static std::vector<std::string> getStringArray(const interpreter::NaabVal& val);
static int getInt(const interpreter::NaabVal& val);
static interpreter::NaabVal makeString(const std::string& s);
static interpreter::NaabVal makeInt(int i);
static interpreter::NaabVal makeBool(bool b);
static interpreter::NaabVal makeStringArray(const std::vector<std::string>& arr);

bool StringModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "length", "substring", "slice", "concat", "split", "join",
        "trim", "upper", "lower", "replace", "contains",
        "starts_with", "ends_with", "index_of", "repeat",
        "char_at", "reverse", "format", "fmt",
        "pad_left", "pad_right"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal StringModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    // Function 1: length (returns byte count, not Unicode codepoint count)
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
        return makeString(strops::substring(getString(args[0]), getInt(args[1]), true, getInt(args[2])));
    }

    // slice(s, start[, end]): JavaScript semantics, the same as the method
    // form s.slice(...) -- negative indices count from the end. It existed only
    // as a method, so string.slice(s, ...) was an unknown function while
    // s.slice(...) worked (repo-sentinel F-007).
    if (function_name == "slice") {
        if (args.size() != 2 && args.size() != 3) {
            throw std::runtime_error(
                "slice() takes 2 or 3 arguments\n\n"
                "  Expected: string.slice(s, start[, end])\n"
                "  Example: string.slice(\"hello\", -3)  // \"llo\"\n");
        }
        bool has_end = args.size() == 3;
        return makeString(strops::slice(getString(args[0]), getInt(args[1]), has_end,
                                        has_end ? getInt(args[2]) : 0));
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

        return makeString(strops::replaceAll(s, old_str, new_str));
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

        // V-DOS-011: Bound allocation size
        size_t result_size = s.length() * static_cast<size_t>(count);
        if (result_size > MAX_STRING_RESULT_BYTES) {
            throw std::runtime_error("repeat() result exceeds maximum string size (64 MB)");
        }

        std::string result;
        result.reserve(result_size);
        for (int i = 0; i < count; ++i) {
            result += s;
        }
        return makeString(result);
    }

    // Function 15: pad_right
    if (function_name == "pad_right") {
        if (args.size() < 2 || args.size() > 3) {
            throw std::runtime_error("pad_right() takes 2-3 arguments (string, width[, fill_char])");
        }
        std::string s = getString(args[0]);
        int width = getInt(args[1]);
        std::string fill = " ";
        if (args.size() == 3) {
            fill = getString(args[2]);
            if (fill.length() != 1) {
                throw std::runtime_error("pad_right() fill_char must be exactly 1 character");
            }
        }
        if (static_cast<int>(s.length()) >= width) return makeString(s);
        // V-DOS-011: Bound allocation size
        if (static_cast<size_t>(width) > MAX_STRING_RESULT_BYTES) {
            throw std::runtime_error("pad_right() width exceeds maximum string size (64 MB)");
        }
        s.append(width - s.length(), fill[0]);
        return makeString(s);
    }

    // Function 16: pad_left
    if (function_name == "pad_left") {
        if (args.size() < 2 || args.size() > 3) {
            throw std::runtime_error("pad_left() takes 2-3 arguments (string, width[, fill_char])");
        }
        std::string s = getString(args[0]);
        int width = getInt(args[1]);
        std::string fill = " ";
        if (args.size() == 3) {
            fill = getString(args[2]);
            if (fill.length() != 1) {
                throw std::runtime_error("pad_left() fill_char must be exactly 1 character");
            }
        }
        if (static_cast<int>(s.length()) >= width) return makeString(s);
        // V-DOS-011: Bound allocation size
        if (static_cast<size_t>(width) > MAX_STRING_RESULT_BYTES) {
            throw std::runtime_error("pad_left() width exceeds maximum string size (64 MB)");
        }
        return makeString(std::string(width - s.length(), fill[0]) + s);
    }

    // Function 17: char_at
    if (function_name == "char_at") {
        if (args.size() != 2) {
            throw std::runtime_error("char_at() takes exactly 2 arguments (string, index)");
        }
        std::string s = getString(args[0]);
        int index = getInt(args[1]);
        if (index < 0 || index >= static_cast<int>(s.length())) {
            throw std::runtime_error(
                "Index error: char_at() index " + std::to_string(index) +
                " out of range for string of length " + std::to_string(s.length())
            );
        }
        return makeString(std::string(1, s[index]));
    }

    // Function 16: reverse
    if (function_name == "reverse") {
        if (args.size() != 1) {
            throw std::runtime_error("reverse() takes exactly 1 argument");
        }
        std::string s = getString(args[0]);
        std::reverse(s.begin(), s.end());
        return makeString(s);
    }

    // Common LLM mistakes with specific guidance
    if (function_name == "from_int" || function_name == "to_string" || function_name == "str" || function_name == "toString") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Help: NAAb uses the + operator for string conversion:\n"
            "    \"\" + 42       // \"42\"\n"
            "    \"score: \" + x // \"score: 5\"\n\n"
            "  There is no string.from_int() or string.to_string() function.\n"
            "  The + operator auto-converts int/float/bool to string.\n"
        );
    }

    // Python/Ruby naming conventions
    if (function_name == "to_upper" || function_name == "upcase") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Did you mean: string.upper()?\n"
            "  Example: string.upper(\"hello\")  // \"HELLO\"\n"
            "  Or use dot notation: \"hello\".upper()\n"
        );
    }
    if (function_name == "to_lower" || function_name == "downcase") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Did you mean: string.lower()?\n"
            "  Example: string.lower(\"HELLO\")  // \"hello\"\n"
            "  Or use dot notation: \"HELLO\".lower()\n"
        );
    }
    if (function_name == "capitalize" || function_name == "title") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  NAAb doesn't have capitalize/title. Use string.upper() for uppercase,\n"
            "  or use a polyglot block:\n"
            "    let result = <<python\n\"hello world\".title()\n    >>\n"
        );
    }
    if (function_name == "to_int" || function_name == "to_i" || function_name == "parseInt") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Use the built-in int() function:\n"
            "    let n = int(\"42\")  // 42\n"
        );
    }
    if (function_name == "to_float" || function_name == "to_f" || function_name == "parseFloat") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Use the built-in float() function:\n"
            "    let f = float(\"3.14\")  // 3.14\n"
        );
    }
    if (function_name == "len" || function_name == "size" || function_name == "count") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Did you mean: string.length()?\n"
            "  Example: string.length(\"hello\")  // 5\n"
            "  Or use dot notation: \"hello\".length()\n"
        );
    }
    if (function_name == "strip" || function_name == "lstrip" || function_name == "rstrip") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Did you mean: string.trim()?\n"
            "  NAAb uses 'trim' (like JavaScript), not 'strip' (Python).\n"
            "  Example: string.trim(\"  hello  \")  // \"hello\"\n"
        );
    }
    if (function_name == "match" || function_name == "matches" || function_name == "regex" || function_name == "test") {
        throw std::runtime_error(
            "string." + function_name + "() does not exist in NAAb\n\n"
            "  For pattern matching, use the regex module:\n"
            "    use regex\n\n"
            "    regex.search(text, \"[0-9]+\")     // partial match — returns match or null\n"
            "    regex.matches(text, \"^\\\\d+$\")    // full string match (true/false)\n"
            "    regex.find_all(text, \"[0-9]+\")   // all matches as array\n\n"
            "  For simple substring checking:\n"
            "    string.contains(text, \"hello\")   // returns true/false\n"
            "    text.contains(\"hello\")           // dot-notation also works\n"
        );
    }
    if (function_name == "find" || function_name == "search") {
        throw std::runtime_error(
            "string." + function_name + "() does not exist in NAAb\n\n"
            "  Use string.index_of() instead:\n"
            "    string.index_of(text, \"world\")   // returns index or -1\n"
            "    text.index_of(\"world\")           // dot-notation also works\n\n"
            "  For checking if a substring exists:\n"
            "    string.contains(text, \"world\")   // returns true/false\n"
        );
    }

    // camelCase → snake_case helpers
    if (function_name == "charAt") {
        throw std::runtime_error(
            "Unknown string function: charAt\n\n"
            "  Did you mean: string.char_at()? NAAb uses snake_case.\n"
            "  Example: string.char_at(\"hello\", 0)  // \"h\"\n"
        );
    }
    if (function_name == "toUpper" || function_name == "toUpperCase") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Did you mean: string.upper()?\n"
            "  Example: string.upper(\"hello\")  // \"HELLO\"\n"
        );
    }
    if (function_name == "toLower" || function_name == "toLowerCase") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Did you mean: string.lower()?\n"
            "  Example: string.lower(\"HELLO\")  // \"hello\"\n"
        );
    }
    if (function_name == "indexOf") {
        throw std::runtime_error(
            "Unknown string function: indexOf\n\n"
            "  Did you mean: string.index_of()? NAAb uses snake_case.\n"
            "  Example: string.index_of(\"hello\", \"ll\")  // 2\n"
        );
    }
    // Names from JavaScript and Python that an LLM (or a person) reaches for.
    // The generic "did you mean" picks by edit distance, which sent `slice`
    // to split(); these say the equivalent directly.
    if (function_name == "substr") {
        throw std::runtime_error(
            "Unknown string function: substr\n\n"
            "  Did you mean: string.substring(s, start, end)? It takes an END index, not a length.\n"
            "  Example: string.substring(\"hello\", 1, 3)  // \"el\"\n"
        );
    }
    if (function_name == "includes") {
        throw std::runtime_error(
            "Unknown string function: includes\n\n"
            "  Did you mean: string.contains()?\n"
            "  Example: string.contains(\"hello\", \"ell\")  // true\n"
        );
    }
    if (function_name == "padStart" || function_name == "padEnd" ||
        function_name == "rjust" || function_name == "ljust") {
        bool left = function_name == "padStart" || function_name == "rjust";
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Did you mean: string." + std::string(left ? "pad_left" : "pad_right") +
            "(s, width, fill)?\n"
            "  Example: string." + std::string(left ? "pad_left" : "pad_right") +
            "(\"7\", 3, \"0\")  // \"" + std::string(left ? "007" : "700") + "\"\n"
        );
    }
    if (function_name == "trimStart" || function_name == "trimEnd" ||
        function_name == "trimLeft" || function_name == "trimRight") {
        throw std::runtime_error(
            "Unknown string function: " + function_name + "\n\n"
            "  Did you mean: string.trim()? It trims both ends.\n"
            "  Example: string.trim(\"  hi  \")  // \"hi\"\n"
        );
    }
    if (function_name == "replaceAll") {
        throw std::runtime_error(
            "Unknown string function: replaceAll\n\n"
            "  Did you mean: string.replace()? It already replaces every occurrence.\n"
            "  Example: string.replace(\"a-b-c\", \"-\", \"+\")  // \"a+b+c\"\n"
        );
    }
    if (function_name == "startsWith") {
        throw std::runtime_error(
            "Unknown string function: startsWith\n\n"
            "  Did you mean: string.starts_with()? NAAb uses snake_case.\n"
            "  Example: string.starts_with(\"hello\", \"hel\")  // true\n"
        );
    }
    if (function_name == "endsWith") {
        throw std::runtime_error(
            "Unknown string function: endsWith\n\n"
            "  Did you mean: string.ends_with()? NAAb uses snake_case.\n"
            "  Example: string.ends_with(\"hello\", \"llo\")  // true\n"
        );
    }

    if (function_name == "format" || function_name == "fmt") {
        if (args.empty()) {
            throw std::runtime_error(
                "Argument error: string.format() requires at least 1 argument\n\n"
                "  Expected: string.format(template, args...)\n\n"
                "  Example:\n"
                "    string.format(\"Hello {}, score: {}\", name, score)\n"
            );
        }

        if (!args[0].isString()) {
            throw std::runtime_error(
                "Type error: string.format() first argument must be a string template\n\n"
                "  Got: " + args[0].toString() + "\n"
                "  Expected: string with {} placeholders\n"
            );
        }

        // Replace {} placeholders with arguments
        std::string result = args[0].asString();
        size_t arg_idx = 1;
        size_t pos = 0;
        while ((pos = result.find("{}", pos)) != std::string::npos) {
            if (arg_idx < args.size()) {
                std::string replacement;
                // Use raw string value without quotes
                if (args[arg_idx].isString()) {
                    replacement = args[arg_idx].asString();
                } else {
                    replacement = args[arg_idx].toString();
                }
                result.replace(pos, 2, replacement);
                pos += replacement.length();
                arg_idx++;
            } else {
                pos += 2;  // Skip unfilled placeholder
            }
        }

        return interpreter::NaabVal::makeString(result);
    }

    // Generic unknown function with suggestions
    static const std::vector<std::string> FUNCTIONS = {
        "length", "substring", "slice", "upper", "lower", "trim", "split",
        "contains", "starts_with", "ends_with", "replace", "index_of",
        "char_at", "repeat", "reverse", "format", "fmt",
        "pad_left", "pad_right"
    };

    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown string function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }

    throw std::runtime_error(oss.str());
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

static int getInt(const interpreter::NaabVal& val) {
    if (!val.isInt()) throw std::runtime_error("Expected integer value");
    return val.asInt();
}

static interpreter::NaabVal makeString(const std::string& s) {
    return interpreter::NaabVal::makeString(s);
}

static interpreter::NaabVal makeInt(int i) {
    return interpreter::NaabVal::makeInt(i);
}

static interpreter::NaabVal makeBool(bool b) {
    return interpreter::NaabVal::makeBool(b);
}

static interpreter::NaabVal makeStringArray(const std::vector<std::string>& arr) {
    std::vector<interpreter::NaabVal> elements;
    for (const auto& s : arr) {
        elements.push_back(interpreter::NaabVal::makeString(s));
    }
    return interpreter::NaabVal::makeList(std::move(elements));
}

} // namespace stdlib
} // namespace naab
