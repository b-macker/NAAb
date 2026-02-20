//
// NAAb Standard Library - Regex Module
// Complete implementation with 12 regex functions
// Now with ReDoS protection via SafeRegex
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include "naab/safe_regex.h"
#include <regex>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Forward declarations
static std::string getString(const std::shared_ptr<interpreter::Value>& val);
static std::shared_ptr<interpreter::Value> makeString(const std::string& s);
static std::shared_ptr<interpreter::Value> makeBool(bool b);
static std::shared_ptr<interpreter::Value> makeArray(const std::vector<std::shared_ptr<interpreter::Value>>& arr);
static std::shared_ptr<interpreter::Value> makeStringArray(const std::vector<std::string>& arr);
static std::shared_ptr<interpreter::Value> makeNull();

bool RegexModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "matches", "search", "find", "find_all", "replace", "replace_first",
        "split", "groups", "find_groups", "escape", "is_valid", "compile_pattern"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> RegexModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Function 1: matches - Full string match
    // Renamed from "match" to avoid keyword conflict
    // Now with ReDoS protection
    if (function_name == "matches") {
        if (args.size() != 2) {
            throw std::runtime_error("matches() takes exactly 2 arguments (text, pattern)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);

        auto& safe_regex = regex_safety::getGlobalSafeRegex();
        bool result = safe_regex.safeMatch(text, pattern);
        return makeBool(result);
    }

    // Function 2: search - Partial match
    // Now with ReDoS protection
    if (function_name == "search") {
        if (args.size() != 2) {
            throw std::runtime_error("search() takes exactly 2 arguments (text, pattern)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);

        auto& safe_regex = regex_safety::getGlobalSafeRegex();
        bool result = safe_regex.safeSearch(text, pattern);
        return makeBool(result);
    }

    // Function 3: find - Find first match
    // Now with ReDoS protection
    if (function_name == "find") {
        if (args.size() != 2) {
            throw std::runtime_error("find() takes exactly 2 arguments (text, pattern)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);

        auto& safe_regex = regex_safety::getGlobalSafeRegex();
        std::smatch match;
        bool found = safe_regex.safeSearch(text, pattern, match);
        if (found) {
            return makeString(match.str(0));
        }
        return makeNull();
    }

    // Function 4: find_all - Find all matches
    // Now with ReDoS protection and match limit
    if (function_name == "find_all") {
        if (args.size() != 2) {
            throw std::runtime_error("find_all() takes exactly 2 arguments (text, pattern)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);

        auto& safe_regex = regex_safety::getGlobalSafeRegex();
        std::vector<std::string> matches = safe_regex.safeFindAll(text, pattern);
        return makeStringArray(matches);
    }

    // Function 5: replace - Replace all matches
    // Now with ReDoS protection
    if (function_name == "replace") {
        if (args.size() != 3) {
            throw std::runtime_error("replace() takes exactly 3 arguments (text, pattern, replacement)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);
        std::string replacement = getString(args[2]);

        auto& safe_regex = regex_safety::getGlobalSafeRegex();
        std::string result = safe_regex.safeReplace(text, pattern, replacement,
                                                     std::chrono::milliseconds(0), true);
        return makeString(result);
    }

    // Function 6: replace_first - Replace first match
    // Now with ReDoS protection
    if (function_name == "replace_first") {
        if (args.size() != 3) {
            throw std::runtime_error("replace_first() takes exactly 3 arguments (text, pattern, replacement)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);
        std::string replacement = getString(args[2]);

        auto& safe_regex = regex_safety::getGlobalSafeRegex();
        std::string result = safe_regex.safeReplace(text, pattern, replacement,
                                                     std::chrono::milliseconds(0), false);
        return makeString(result);
    }

    // Function 7: split - Split by regex
    if (function_name == "split") {
        if (args.size() != 2) {
            throw std::runtime_error("split() takes exactly 2 arguments (text, pattern)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);

        try {
            std::regex re(pattern);
            std::vector<std::string> parts;
            std::sregex_token_iterator begin(text.begin(), text.end(), re, -1);
            std::sregex_token_iterator end;

            for (auto it = begin; it != end; ++it) {
                parts.push_back(it->str());
            }

            return makeStringArray(parts);
        } catch (const std::regex_error& e) {
            throw std::runtime_error("Invalid regex pattern: " + std::string(e.what()));
        }
    }

    // Function 8: groups - Get capture groups from first match
    if (function_name == "groups") {
        if (args.size() != 2) {
            throw std::runtime_error("groups() takes exactly 2 arguments (text, pattern)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);

        try {
            std::regex re(pattern);
            std::smatch match;
            if (std::regex_search(text, match, re)) {
                std::vector<std::string> groups;
                for (size_t i = 0; i < match.size(); ++i) {
                    groups.push_back(match[i].str());
                }
                return makeStringArray(groups);
            }
            return makeStringArray({});
        } catch (const std::regex_error& e) {
            throw std::runtime_error("Invalid regex pattern: " + std::string(e.what()));
        }
    }

    // Function 9: find_groups - Get capture groups from all matches
    if (function_name == "find_groups") {
        if (args.size() != 2) {
            throw std::runtime_error("find_groups() takes exactly 2 arguments (text, pattern)");
        }
        std::string text = getString(args[0]);
        std::string pattern = getString(args[1]);

        try {
            std::regex re(pattern);
            std::vector<std::shared_ptr<interpreter::Value>> all_groups;
            auto begin = std::sregex_iterator(text.begin(), text.end(), re);
            auto end = std::sregex_iterator();

            for (std::sregex_iterator i = begin; i != end; ++i) {
                std::vector<std::string> groups;
                for (size_t j = 0; j < i->size(); ++j) {
                    groups.push_back((*i)[j].str());
                }
                all_groups.push_back(makeStringArray(groups));
            }

            return makeArray(all_groups);
        } catch (const std::regex_error& e) {
            throw std::runtime_error("Invalid regex pattern: " + std::string(e.what()));
        }
    }

    // Function 10: escape - Escape regex special characters
    if (function_name == "escape") {
        if (args.size() != 1) {
            throw std::runtime_error("escape() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        std::string escaped;

        static const std::string special_chars = R"(\.^$*+?{}[]()|\)";
        for (char c : text) {
            if (special_chars.find(c) != std::string::npos) {
                escaped += '\\';
            }
            escaped += c;
        }

        return makeString(escaped);
    }

    // Function 11: is_valid - Check if regex pattern is valid
    if (function_name == "is_valid") {
        if (args.size() != 1) {
            throw std::runtime_error("is_valid() takes exactly 1 argument");
        }
        std::string pattern = getString(args[0]);

        try {
            std::regex re(pattern);
            return makeBool(true);
        } catch (const std::regex_error&) {
            return makeBool(false);
        }
    }

    // Function 12: compile_pattern - Validate and return pattern (no-op in C++)
    if (function_name == "compile_pattern") {
        if (args.size() != 1) {
            throw std::runtime_error("compile_pattern() takes exactly 1 argument");
        }
        std::string pattern = getString(args[0]);

        try {
            std::regex re(pattern);
            // In C++, we can't return a compiled regex object, so return the pattern string
            return makeString(pattern);
        } catch (const std::regex_error& e) {
            throw std::runtime_error("Invalid regex pattern: " + std::string(e.what()));
        }
    }

    // Common LLM mistakes
    if (function_name == "match") {
        throw std::runtime_error(
            "Unknown regex function: " + function_name + "\n\n"
            "  Did you mean: regex.matches() (full match) or regex.search() (partial match)?\n\n"
            "  regex.matches(str, pattern) — true only if ENTIRE string matches\n"
            "  regex.search(str, pattern)  — true if pattern found anywhere\n"
        );
    }
    if (function_name == "test") {
        throw std::runtime_error(
            "Unknown regex function: " + function_name + "\n\n"
            "  Did you mean: regex.matches() or regex.search()?\n"
            "  NAAb uses 'matches' (full) or 'search' (partial), not 'test'.\n"
        );
    }
    if (function_name == "findAll" || function_name == "find_all_matches") {
        throw std::runtime_error(
            "Unknown regex function: " + function_name + "\n\n"
            "  Did you mean: regex.find_all()? NAAb uses snake_case.\n"
        );
    }
    if (function_name == "replaceAll" || function_name == "replace_all") {
        throw std::runtime_error(
            "Unknown regex function: " + function_name + "\n\n"
            "  Did you mean: regex.replace()? It replaces all matches by default.\n"
        );
    }

    // Fuzzy matching for typos
    static const std::vector<std::string> FUNCTIONS = {
        "matches", "search", "find", "find_all", "replace", "replace_first",
        "split", "groups", "find_groups", "escape", "is_valid", "compile_pattern"
    };
    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown regex function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }
    throw std::runtime_error(oss.str());
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

static std::shared_ptr<interpreter::Value> makeString(const std::string& s) {
    return std::make_shared<interpreter::Value>(s);
}

static std::shared_ptr<interpreter::Value> makeBool(bool b) {
    return std::make_shared<interpreter::Value>(b);
}

static std::shared_ptr<interpreter::Value> makeArray(const std::vector<std::shared_ptr<interpreter::Value>>& arr) {
    return std::make_shared<interpreter::Value>(arr);
}

static std::shared_ptr<interpreter::Value> makeStringArray(const std::vector<std::string>& arr) {
    std::vector<std::shared_ptr<interpreter::Value>> elements;
    for (const auto& s : arr) {
        elements.push_back(makeString(s));
    }
    return std::make_shared<interpreter::Value>(elements);
}

static std::shared_ptr<interpreter::Value> makeNull() {
    return std::make_shared<interpreter::Value>();
}

} // namespace stdlib
} // namespace naab
