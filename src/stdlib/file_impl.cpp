//
// NAAb Standard Library - File Module
// File I/O operations
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <memory>
#include <unordered_set>

namespace fs = std::filesystem;

namespace naab {
namespace stdlib {

// Forward declarations of helper functions
static std::string getString(const std::shared_ptr<interpreter::Value>& val);
static std::vector<std::string> getStringArray(const std::shared_ptr<interpreter::Value>& val);
static bool getBool(const std::shared_ptr<interpreter::Value>& val);

bool FileModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "read", "write", "append", "exists", "delete",
        "list_dir", "create_dir", "is_file", "is_dir",
        "read_lines", "write_lines"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> FileModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "read") {
        // Inline implementation
        if (args.size() != 1) {
            throw std::runtime_error("read() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + path);
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return std::make_shared<interpreter::Value>(buffer.str());
    }

    if (function_name == "write") {
        if (args.size() != 2) {
            throw std::runtime_error("write() takes exactly 2 arguments");
        }
        std::string path = getString(args[0]);
        std::string content = getString(args[1]);
        std::ofstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for writing: " + path);
        }
        file << content;
        return std::make_shared<interpreter::Value>();
    }

    if (function_name == "append") {
        if (args.size() != 2) {
            throw std::runtime_error("append() takes exactly 2 arguments");
        }
        std::string path = getString(args[0]);
        std::string content = getString(args[1]);
        std::ofstream file(path, std::ios::app);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for appending: " + path);
        }
        file << content;
        return std::make_shared<interpreter::Value>();
    }

    if (function_name == "exists") {
        if (args.size() != 1) {
            throw std::runtime_error("exists() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return std::make_shared<interpreter::Value>(fs::exists(path));
    }

    if (function_name == "delete") {
        if (args.size() != 1) {
            throw std::runtime_error("delete() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        if (fs::exists(path)) {
            // Check if path is a directory (should not delete directories)
            if (fs::is_directory(path)) {
                throw std::runtime_error("delete() cannot delete directory: " + path +
                                       " (use a dedicated directory removal function)");
            }
            fs::remove(path);
        }
        return std::make_shared<interpreter::Value>();
    }

    if (function_name == "list_dir") {
        if (args.size() != 1) {
            throw std::runtime_error("list_dir() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        std::vector<std::shared_ptr<interpreter::Value>> entries;
        if (fs::exists(path) && fs::is_directory(path)) {
            for (const auto& entry : fs::directory_iterator(path)) {
                entries.push_back(std::make_shared<interpreter::Value>(
                    entry.path().filename().string()
                ));
            }
        }
        return std::make_shared<interpreter::Value>(entries);
    }

    if (function_name == "create_dir") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("create_dir() takes 1 or 2 arguments (path, recursive?)");
        }
        std::string path = getString(args[0]);
        bool recursive = true;  // Default to recursive for convenience

        if (args.size() == 2) {
            recursive = getBool(args[1]);
        }

        if (recursive) {
            fs::create_directories(path);  // Creates parent dirs as needed
        } else {
            fs::create_directory(path);    // Fails if parent missing
        }
        return std::make_shared<interpreter::Value>();
    }

    if (function_name == "is_file") {
        if (args.size() != 1) {
            throw std::runtime_error("is_file() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return std::make_shared<interpreter::Value>(fs::is_regular_file(path));
    }

    if (function_name == "is_dir") {
        if (args.size() != 1) {
            throw std::runtime_error("is_dir() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return std::make_shared<interpreter::Value>(fs::is_directory(path));
    }

    if (function_name == "read_lines") {
        if (args.size() != 1) {
            throw std::runtime_error("read_lines() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + path);
        }
        std::vector<std::shared_ptr<interpreter::Value>> lines;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(std::make_shared<interpreter::Value>(line));
        }
        return std::make_shared<interpreter::Value>(lines);
    }

    if (function_name == "write_lines") {
        if (args.size() != 2) {
            throw std::runtime_error("write_lines() takes exactly 2 arguments");
        }
        std::string path = getString(args[0]);
        auto lines = getStringArray(args[1]);
        std::ofstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for writing: " + path);
        }
        for (const auto& line : lines) {
            file << line << '\n';
        }
        return std::make_shared<interpreter::Value>();
    }

    // Common LLM mistakes - Node.js/Python naming conventions
    if (function_name == "readFile" || function_name == "readFileSync" || function_name == "read_file") {
        throw std::runtime_error(
            "Unknown file function: " + function_name + "\n\n"
            "  Did you mean: file.read()?\n"
            "  NAAb file operations are synchronous. Just use file.read(path).\n"
        );
    }
    if (function_name == "writeFile" || function_name == "writeFileSync" || function_name == "write_file") {
        throw std::runtime_error(
            "Unknown file function: " + function_name + "\n\n"
            "  Did you mean: file.write()?\n"
            "  Example: file.write(path, content)\n"
        );
    }
    if (function_name == "mkdir" || function_name == "mkdirs" || function_name == "makedirs") {
        throw std::runtime_error(
            "Unknown file function: " + function_name + "\n\n"
            "  Did you mean: file.create_dir()?\n"
            "  Example: file.create_dir(\"/path/to/dir\")\n"
        );
    }
    if (function_name == "remove" || function_name == "unlink" || function_name == "rm") {
        throw std::runtime_error(
            "Unknown file function: " + function_name + "\n\n"
            "  Did you mean: file.delete()?\n"
            "  Example: file.delete(\"/path/to/file\")\n"
        );
    }
    if (function_name == "readdir" || function_name == "listdir" || function_name == "ls") {
        throw std::runtime_error(
            "Unknown file function: " + function_name + "\n\n"
            "  Did you mean: file.list_dir()?\n"
            "  Example: file.list_dir(\"/path/to/dir\")\n"
        );
    }

    // Fuzzy matching for typos
    static const std::vector<std::string> FUNCTIONS = {
        "read", "write", "append", "exists", "delete",
        "list_dir", "create_dir", "is_file", "is_dir",
        "read_lines", "write_lines"
    };
    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown file function: " << function_name << suggestion
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

static std::vector<std::string> getStringArray(
    const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> std::vector<std::string> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            std::vector<std::string> result;
            result.reserve(arg.size());
            for (const auto& item : arg) {
                result.push_back(getString(item));
            }
            return result;
        } else {
            throw std::runtime_error("Expected array value");
        }
    }, val->data);
}

static bool getBool(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, bool>) {
            return arg;
        } else {
            throw std::runtime_error("Expected boolean value");
        }
    }, val->data);
}

} // namespace stdlib
} // namespace naab
