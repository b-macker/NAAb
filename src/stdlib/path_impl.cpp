//
// NAAb Standard Library - Path Module
// Path manipulation utilities
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace naab {
namespace stdlib {

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

bool PathModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "join", "dirname", "basename", "extension", "resolve",
        "is_absolute", "normalize", "exists"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> PathModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "join") {
        if (args.empty()) {
            throw std::runtime_error("path.join() requires at least 1 argument");
        }
        fs::path result(getString(args[0]));
        for (size_t i = 1; i < args.size(); ++i) {
            result /= getString(args[i]);
        }
        return std::make_shared<interpreter::Value>(result.string());
    }

    if (function_name == "dirname") {
        if (args.size() != 1) {
            throw std::runtime_error("path.dirname() takes exactly 1 argument");
        }
        return std::make_shared<interpreter::Value>(
            fs::path(getString(args[0])).parent_path().string());
    }

    if (function_name == "basename") {
        if (args.size() != 1) {
            throw std::runtime_error("path.basename() takes exactly 1 argument");
        }
        return std::make_shared<interpreter::Value>(
            fs::path(getString(args[0])).filename().string());
    }

    if (function_name == "extension") {
        if (args.size() != 1) {
            throw std::runtime_error("path.extension() takes exactly 1 argument");
        }
        return std::make_shared<interpreter::Value>(
            fs::path(getString(args[0])).extension().string());
    }

    if (function_name == "resolve") {
        if (args.size() != 1) {
            throw std::runtime_error("path.resolve() takes exactly 1 argument");
        }
        std::error_code ec;
        auto resolved = fs::canonical(getString(args[0]), ec);
        if (ec) {
            // Fall back to absolute if canonical fails (file doesn't exist)
            return std::make_shared<interpreter::Value>(
                fs::absolute(getString(args[0])).string());
        }
        return std::make_shared<interpreter::Value>(resolved.string());
    }

    if (function_name == "is_absolute") {
        if (args.size() != 1) {
            throw std::runtime_error("path.is_absolute() takes exactly 1 argument");
        }
        return std::make_shared<interpreter::Value>(
            fs::path(getString(args[0])).is_absolute());
    }

    if (function_name == "normalize") {
        if (args.size() != 1) {
            throw std::runtime_error("path.normalize() takes exactly 1 argument");
        }
        return std::make_shared<interpreter::Value>(
            fs::path(getString(args[0])).lexically_normal().string());
    }

    if (function_name == "exists") {
        if (args.size() != 1) {
            throw std::runtime_error("path.exists() takes exactly 1 argument");
        }
        return std::make_shared<interpreter::Value>(
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
