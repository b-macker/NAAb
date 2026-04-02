//
// NAAb Standard Library - Validate Module
// Input validation helpers (email, URL, IP, ranges, types)
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace naab {
namespace stdlib {

static std::string getString(const interpreter::NaabVal& val) {
    if (!val.isString()) {
        throw std::runtime_error("validate: expected a string argument");
    }
    return val.asString();
}

static double toDouble(const interpreter::NaabVal& val) {
    if (val.isInt())    return static_cast<double>(val.asInt());
    if (val.isDouble()) return val.asDouble();
    throw std::runtime_error("validate: expected a numeric argument");
}

// Trim whitespace from both ends
static std::string trimStr(const std::string& s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(static_cast<unsigned char>(*start))) ++start;
    auto end = s.end();
    while (end != start && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(start, end);
}

bool ValidateModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "email", "url", "ip", "ipv6",
        "int_range", "not_empty", "length", "matches",
        "is_int", "is_float", "is_string"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal ValidateModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "email") {
        if (args.size() != 1) {
            throw std::runtime_error("validate.email() takes exactly 1 argument");
        }
        if (!args[0].isString()) {
            return interpreter::NaabVal::makeBool(false);
        }
        static const std::regex email_re(
            R"(^[a-zA-Z0-9._%+\-]+@[a-zA-Z0-9.\-]+\.[a-zA-Z]{2,}$)"
        );
        return interpreter::NaabVal::makeBool(
            std::regex_match(args[0].asString(), email_re));
    }

    if (function_name == "url") {
        if (args.size() != 1) {
            throw std::runtime_error("validate.url() takes exactly 1 argument");
        }
        if (!args[0].isString()) {
            return interpreter::NaabVal::makeBool(false);
        }
        static const std::regex url_re(
            R"(^(https?|ftp)://[^\s/$.?#][^\s]*$)"
        );
        return interpreter::NaabVal::makeBool(
            std::regex_match(args[0].asString(), url_re));
    }

    if (function_name == "ip") {
        if (args.size() != 1) {
            throw std::runtime_error("validate.ip() takes exactly 1 argument");
        }
        if (!args[0].isString()) {
            return interpreter::NaabVal::makeBool(false);
        }
        static const std::regex ipv4_re(
            R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)"
        );
        return interpreter::NaabVal::makeBool(
            std::regex_match(args[0].asString(), ipv4_re));
    }

    if (function_name == "ipv6") {
        if (args.size() != 1) {
            throw std::runtime_error("validate.ipv6() takes exactly 1 argument");
        }
        if (!args[0].isString()) {
            return interpreter::NaabVal::makeBool(false);
        }
        static const std::regex ipv6_re(
            R"(^([0-9a-fA-F]{0,4}:){2,7}[0-9a-fA-F]{0,4}$)"
        );
        return interpreter::NaabVal::makeBool(
            std::regex_match(args[0].asString(), ipv6_re));
    }

    if (function_name == "int_range") {
        if (args.size() != 3) {
            throw std::runtime_error(
                "validate.int_range() takes exactly 3 arguments: (value, min, max)\n\n"
                "  Example: validate.int_range(score, 0, 100)\n"
            );
        }
        double val = toDouble(args[0]);
        double mn  = toDouble(args[1]);
        double mx  = toDouble(args[2]);
        return interpreter::NaabVal::makeBool(val >= mn && val <= mx);
    }

    if (function_name == "not_empty") {
        if (args.size() != 1) {
            throw std::runtime_error("validate.not_empty() takes exactly 1 argument");
        }
        if (!args[0].isString()) {
            return interpreter::NaabVal::makeBool(false);
        }
        return interpreter::NaabVal::makeBool(!trimStr(args[0].asString()).empty());
    }

    if (function_name == "length") {
        if (args.size() != 3) {
            throw std::runtime_error(
                "validate.length() takes exactly 3 arguments: (string, min, max)\n\n"
                "  Example: validate.length(username, 3, 20)\n"
            );
        }
        std::string s = getString(args[0]);
        double mn = toDouble(args[1]);
        double mx = toDouble(args[2]);
        double len = static_cast<double>(s.size());
        return interpreter::NaabVal::makeBool(len >= mn && len <= mx);
    }

    if (function_name == "matches") {
        if (args.size() != 2) {
            throw std::runtime_error(
                "validate.matches() takes exactly 2 arguments: (string, pattern)\n\n"
                "  Example: validate.matches(code, \"^[A-Z]{3}[0-9]{4}$\")\n"
            );
        }
        std::string s       = getString(args[0]);
        std::string pattern = getString(args[1]);
        try {
            std::regex re(pattern);
            return interpreter::NaabVal::makeBool(std::regex_match(s, re));
        } catch (const std::regex_error& e) {
            throw std::runtime_error(
                std::string("validate.matches(): invalid regex pattern: ") + e.what());
        }
    }

    if (function_name == "is_int") {
        if (args.size() != 1) {
            throw std::runtime_error("validate.is_int() takes exactly 1 argument");
        }
        return interpreter::NaabVal::makeBool(args[0].isInt());
    }

    if (function_name == "is_float") {
        if (args.size() != 1) {
            throw std::runtime_error("validate.is_float() takes exactly 1 argument");
        }
        return interpreter::NaabVal::makeBool(args[0].isDouble());
    }

    if (function_name == "is_string") {
        if (args.size() != 1) {
            throw std::runtime_error("validate.is_string() takes exactly 1 argument");
        }
        return interpreter::NaabVal::makeBool(args[0].isString());
    }

    throw std::runtime_error(
        "Unknown validate function: " + function_name + "\n\n"
        "  Available: email, url, ip, ipv6, int_range, not_empty, length, matches,\n"
        "             is_int, is_float, is_string\n"
    );
}

} // namespace stdlib
} // namespace naab
