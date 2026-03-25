//
// NAAb Standard Library - String Module
// Full C++ implementation with 14 string functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace naab {
namespace stdlib {

bool StringModule::hasFunction(const std::string& name) const {
    static const std::vector<std::string> functions = {
        "length", "upper", "lower", "trim", "split", "join",
        "replace", "substring", "startswith", "endswith",
        "contains", "find", "repeat", "reverse"
    };

    return std::find(functions.begin(), functions.end(), name) != functions.end();
}

interpreter::NaabVal StringModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "length") return length(args);
    if (function_name == "upper") return upper(args);
    if (function_name == "lower") return lower(args);
    if (function_name == "trim") return trim(args);
    if (function_name == "split") return split(args);
    if (function_name == "join") return join(args);
    if (function_name == "replace") return replace(args);
    if (function_name == "substring") return substring(args);
    if (function_name == "startswith") return startswith(args);
    if (function_name == "endswith") return endswith(args);
    if (function_name == "contains") return contains(args);
    if (function_name == "find") return find(args);
    if (function_name == "repeat") return repeat(args);
    if (function_name == "reverse") return reverse(args);

    throw std::runtime_error("Unknown string function: " + function_name);
}

// ============================================================================
// String Operations
// ============================================================================

interpreter::NaabVal StringModule::length(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 1) {
        throw std::runtime_error("string.length() expects 1 argument");
    }

    auto str = args[0].asString();
    return interpreter::NaabVal::makeInt(static_cast<int>(str.size()));
}

interpreter::NaabVal StringModule::upper(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 1) {
        throw std::runtime_error("string.upper() expects 1 argument");
    }

    auto str = args[0].asString();
    std::transform(str.begin(), str.end(), str.begin(),
                  [](unsigned char c) { return std::toupper(c); });

    return interpreter::NaabVal::makeString(str);
}

interpreter::NaabVal StringModule::lower(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 1) {
        throw std::runtime_error("string.lower() expects 1 argument");
    }

    auto str = args[0].asString();
    std::transform(str.begin(), str.end(), str.begin(),
                  [](unsigned char c) { return std::tolower(c); });

    return interpreter::NaabVal::makeString(str);
}

interpreter::NaabVal StringModule::trim(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 1) {
        throw std::runtime_error("string.trim() expects 1 argument");
    }

    auto str = args[0].asString();

    // Trim leading whitespace
    auto start = std::find_if_not(str.begin(), str.end(),
                                  [](unsigned char c) { return std::isspace(c); });

    // Trim trailing whitespace
    auto end = std::find_if_not(str.rbegin(), str.rend(),
                               [](unsigned char c) { return std::isspace(c); }).base();

    if (start >= end) {
        return interpreter::NaabVal::makeString("");
    }

    return interpreter::NaabVal::makeString(std::string(start, end));
}

interpreter::NaabVal StringModule::split(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 2) {
        throw std::runtime_error("string.split() expects 2 arguments");
    }

    auto str = args[0].asString();
    auto delim = args[1].asString();

    std::vector<interpreter::NaabVal> result;
    size_t start = 0;
    size_t end = str.find(delim);

    while (end != std::string::npos) {
        result.push_back(interpreter::NaabVal::makeString(str.substr(start, end - start)));
        start = end + delim.length();
        end = str.find(delim, start);
    }

    result.push_back(interpreter::NaabVal::makeString(str.substr(start)));

    return interpreter::NaabVal::makeList(std::move(result));
}

interpreter::NaabVal StringModule::join(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 2) {
        throw std::runtime_error("string.join() expects 2 arguments");
    }

    auto arr = args[0].asList();
    auto delim = args[1].asString();

    std::ostringstream oss;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) oss << delim;
        oss << arr[i].toString();
    }

    return interpreter::NaabVal::makeString(oss.str());
}

interpreter::NaabVal StringModule::replace(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 3) {
        throw std::runtime_error("string.replace() expects 3 arguments");
    }

    auto str = args[0].asString();
    auto old_substr = args[1].asString();
    auto new_substr = args[2].asString();

    std::string result = str;
    size_t pos = 0;

    while ((pos = result.find(old_substr, pos)) != std::string::npos) {
        result.replace(pos, old_substr.length(), new_substr);
        pos += new_substr.length();
    }

    return interpreter::NaabVal::makeString(result);
}

interpreter::NaabVal StringModule::substring(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 2 || args.size() > 3) {
        throw std::runtime_error("string.substring() expects 2 or 3 arguments");
    }

    auto str = args[0].asString();
    auto start = static_cast<size_t>(args[1].asInt());

    if (args.size() == 3) {
        auto end = static_cast<size_t>(args[2].asInt());
        return interpreter::NaabVal::makeString(str.substr(start, end - start));
    }

    return interpreter::NaabVal::makeString(str.substr(start));
}

interpreter::NaabVal StringModule::startswith(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 2) {
        throw std::runtime_error("string.startswith() expects 2 arguments");
    }

    auto str = args[0].asString();
    auto prefix = args[1].asString();

    bool result = (str.size() >= prefix.size()) &&
                  (str.compare(0, prefix.size(), prefix) == 0);

    return interpreter::NaabVal::makeString(result);
}

interpreter::NaabVal StringModule::endswith(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 2) {
        throw std::runtime_error("string.endswith() expects 2 arguments");
    }

    auto str = args[0].asString();
    auto suffix = args[1].asString();

    bool result = (str.size() >= suffix.size()) &&
                  (str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);

    return interpreter::NaabVal::makeString(result);
}

interpreter::NaabVal StringModule::contains(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 2) {
        throw std::runtime_error("string.contains() expects 2 arguments");
    }

    auto str = args[0].asString();
    auto substr = args[1].asString();

    return interpreter::NaabVal::makeBool(str.find(substr) != std::string::npos);
}

interpreter::NaabVal StringModule::find(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 2) {
        throw std::runtime_error("string.find() expects 2 arguments");
    }

    auto str = args[0].asString();
    auto substr = args[1].asString();

    auto pos = str.find(substr);
    if (pos == std::string::npos) {
        return interpreter::NaabVal::makeInt(-1);
    }

    return interpreter::NaabVal::makeInt(static_cast<int>(pos));
}

interpreter::NaabVal StringModule::repeat(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 2) {
        throw std::runtime_error("string.repeat() expects 2 arguments");
    }

    auto str = args[0].asString();
    auto n = static_cast<size_t>(args[1].asInt());

    std::ostringstream oss;
    for (size_t i = 0; i < n; ++i) {
        oss << str;
    }

    return interpreter::NaabVal::makeString(oss.str());
}

interpreter::NaabVal StringModule::reverse(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() != 1) {
        throw std::runtime_error("string.reverse() expects 1 argument");
    }

    auto str = args[0].asString();
    std::reverse(str.begin(), str.end());

    return interpreter::NaabVal::makeString(str);
}

} // namespace stdlib
} // namespace naab
