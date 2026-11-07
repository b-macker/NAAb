//
// NAAb Standard Library - Path Module
// Path manipulation utilities
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace fs = std::filesystem;

namespace naab {
namespace stdlib {

// Every path this module returns uses '/' as its separator, on every host
// (the owner's decision, 2026-11-07). std::filesystem's native form is
// mixed on Windows: operator/= appends a backslash after what the caller
// wrote, so join("/usr", "local") was "/usr" + backslash + "local", while
// lexically_normal() turns every separator into a backslash, so one program
// built paths that read differently on every host. generic_string() is the
// native string on POSIX, so nothing changes there; Windows opens a path
// written with '/'. What a path MEANS stays the host's: is_absolute() still
// needs a drive on Windows.
static interpreter::NaabVal pathString(const fs::path& p) {
    return interpreter::NaabVal::makeString(p.generic_string());
}

static std::string getString(const interpreter::NaabVal& val) {
    if (!val.isString()) {
        throw std::runtime_error("Expected string value");
    }
    return val.asString();
}

bool PathModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "join", "dirname", "basename", "extension", "resolve",
        "is_absolute", "normalize", "exists"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal PathModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "join") {
        if (args.empty()) {
            throw std::runtime_error("path.join() requires at least 1 argument");
        }
        fs::path result(getString(args[0]));
        for (size_t i = 1; i < args.size(); ++i) {
            result /= getString(args[i]);
        }
        return pathString(result);
    }

    if (function_name == "dirname") {
        if (args.size() != 1) {
            throw std::runtime_error("path.dirname() takes exactly 1 argument");
        }
        return pathString(fs::path(getString(args[0])).parent_path());
    }

    if (function_name == "basename") {
        if (args.size() != 1) {
            throw std::runtime_error("path.basename() takes exactly 1 argument");
        }
        return pathString(fs::path(getString(args[0])).filename());
    }

    if (function_name == "extension") {
        if (args.size() != 1) {
            throw std::runtime_error("path.extension() takes exactly 1 argument");
        }
        return pathString(fs::path(getString(args[0])).extension());
    }

    if (function_name == "resolve") {
        if (args.size() != 1) {
            throw std::runtime_error("path.resolve() takes exactly 1 argument");
        }
        std::error_code ec;
        auto resolved = fs::canonical(getString(args[0]), ec);
        if (ec) {
            // Fall back to absolute if canonical fails (file doesn't exist)
            return pathString(fs::absolute(getString(args[0])));
        }
        return pathString(resolved);
    }

    if (function_name == "is_absolute") {
        if (args.size() != 1) {
            throw std::runtime_error("path.is_absolute() takes exactly 1 argument");
        }
        return interpreter::NaabVal::makeBool(
            fs::path(getString(args[0])).is_absolute());
    }

    if (function_name == "normalize") {
        if (args.size() != 1) {
            throw std::runtime_error("path.normalize() takes exactly 1 argument");
        }
        return pathString(fs::path(getString(args[0])).lexically_normal());
    }

    if (function_name == "exists") {
        if (args.size() != 1) {
            throw std::runtime_error("path.exists() takes exactly 1 argument");
        }
        return interpreter::NaabVal::makeBool(
            fs::exists(getString(args[0])));
    }

    // Fuzzy matching
    static const std::vector<std::string> FUNCTIONS = {
        "join", "dirname", "basename", "extension", "resolve",
        "is_absolute", "normalize", "exists"
    };
    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown path function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }
    throw std::runtime_error(oss.str());
}

} // namespace stdlib
} // namespace naab
