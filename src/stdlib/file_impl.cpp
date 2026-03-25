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
static std::string getString(const interpreter::NaabVal& val);
static std::vector<std::string> getStringArray(const interpreter::NaabVal& val);
static bool getBool(const interpreter::NaabVal& val);

bool FileModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "read", "write", "append", "exists", "delete",
        "list_dir", "create_dir", "is_file", "is_dir",
        "read_lines", "write_lines",
        "copy", "move", "size", "basename", "dirname", "extension"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal FileModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

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
        return interpreter::NaabVal::makeString(buffer.str());
    }

    if (function_name == "write") {
        if (args.size() != 2) {
            throw std::runtime_error("write() takes exactly 2 arguments");
        }
        std::string path = getString(args[0]);
        std::string content = getString(args[1]);

        // FIX 26: Auto-create parent directories
        auto parent = fs::path(path).parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            std::error_code ec;
            fs::create_directories(parent, ec);
            if (ec) {
                throw std::runtime_error(
                    "Failed to create directory for file: " + path + "\n"
                    "  Directory: " + parent.string() + "\n"
                    "  Error: " + ec.message() + "\n\n"
                    "  Help: Ensure the parent directory path is valid and writable.\n"
                );
            }
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error(
                "Failed to open file for writing: " + path + "\n\n"
                "  Help:\n"
                "  - Check that the path is valid and writable\n"
                "  - Parent directory: " + parent.string() + "\n"
            );
        }
        file << content;
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "append") {
        if (args.size() != 2) {
            throw std::runtime_error("append() takes exactly 2 arguments");
        }
        std::string path = getString(args[0]);
        std::string content = getString(args[1]);

        // FIX 26: Auto-create parent directories (same as write)
        auto parent = fs::path(path).parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            std::error_code ec;
            fs::create_directories(parent, ec);
            if (ec) {
                throw std::runtime_error(
                    "Failed to create directory for file: " + path + "\n"
                    "  Directory: " + parent.string() + "\n"
                    "  Error: " + ec.message() + "\n\n"
                    "  Help: Ensure the parent directory path is valid and writable.\n"
                );
            }
        }

        std::ofstream file(path, std::ios::app);
        if (!file.is_open()) {
            throw std::runtime_error(
                "Failed to open file for appending: " + path + "\n\n"
                "  Help:\n"
                "  - Check that the path is valid and writable\n"
                "  - Parent directory: " + parent.string() + "\n"
            );
        }
        file << content;
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "exists") {
        if (args.size() != 1) {
            throw std::runtime_error("exists() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return interpreter::NaabVal::makeBool(fs::exists(path));
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
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "list_dir") {
        if (args.size() != 1) {
            throw std::runtime_error("list_dir() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        std::vector<interpreter::NaabVal> entries;
        if (fs::exists(path) && fs::is_directory(path)) {
            for (const auto& entry : fs::directory_iterator(path)) {
                entries.push_back(interpreter::NaabVal::makeString(
                    entry.path().filename().string()
                ));
            }
        }
        return interpreter::NaabVal::makeList(std::move(entries));
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
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "is_file") {
        if (args.size() != 1) {
            throw std::runtime_error("is_file() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return interpreter::NaabVal::makeBool(fs::is_regular_file(path));
    }

    if (function_name == "is_dir") {
        if (args.size() != 1) {
            throw std::runtime_error("is_dir() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return interpreter::NaabVal::makeBool(fs::is_directory(path));
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
        std::vector<interpreter::NaabVal> lines;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(interpreter::NaabVal::makeString(line));
        }
        return interpreter::NaabVal::makeList(std::move(lines));
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
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "copy") {
        if (args.size() != 2) {
            throw std::runtime_error("copy() takes exactly 2 arguments (source, destination)");
        }
        std::string src = getString(args[0]);
        std::string dst = getString(args[1]);
        std::error_code ec;
        fs::copy(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw std::runtime_error("Failed to copy file: " + src + " -> " + dst + "\n  Error: " + ec.message());
        }
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "move") {
        if (args.size() != 2) {
            throw std::runtime_error("move() takes exactly 2 arguments (source, destination)");
        }
        std::string src = getString(args[0]);
        std::string dst = getString(args[1]);
        std::error_code ec;
        fs::rename(src, dst, ec);
        if (ec) {
            throw std::runtime_error("Failed to move file: " + src + " -> " + dst + "\n  Error: " + ec.message());
        }
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "size") {
        if (args.size() != 1) {
            throw std::runtime_error("size() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (ec) {
            throw std::runtime_error("Failed to get file size: " + path + "\n  Error: " + ec.message());
        }
        return interpreter::NaabVal::makeInt(static_cast<int>(sz));
    }

    if (function_name == "basename") {
        if (args.size() != 1) {
            throw std::runtime_error("basename() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return interpreter::NaabVal::makeString(fs::path(path).filename().string());
    }

    if (function_name == "dirname") {
        if (args.size() != 1) {
            throw std::runtime_error("dirname() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return interpreter::NaabVal::makeString(fs::path(path).parent_path().string());
    }

    if (function_name == "extension") {
        if (args.size() != 1) {
            throw std::runtime_error("extension() takes exactly 1 argument");
        }
        std::string path = getString(args[0]);
        return interpreter::NaabVal::makeString(fs::path(path).extension().string());
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
    if (function_name == "open" || function_name == "fopen") {
        throw std::runtime_error(
            "Unknown file function: " + function_name + "\n\n"
            "  NAAb file operations are one-shot (no file handles).\n"
            "  Use file.read(path) or file.write(path, content) directly.\n\n"
            "  Example:\n"
            "    let content = file.read(\"/path/to/file\")\n"
            "    file.write(\"/path/to/file\", \"new content\")\n"
        );
    }
    if (function_name == "close" || function_name == "fclose") {
        throw std::runtime_error(
            "Unknown file function: " + function_name + "\n\n"
            "  NAAb file operations are one-shot — no file handles to close.\n"
            "  file.read() and file.write() handle opening/closing automatically.\n"
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
        "read_lines", "write_lines",
        "copy", "move", "size", "basename", "dirname", "extension"
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
static std::string getString(const interpreter::NaabVal& val) {
    if (!val.isString()) throw std::runtime_error("Expected string value");
    return val.asString();
}

static std::vector<std::string> getStringArray(const interpreter::NaabVal& val) {
    if (!val.isList()) throw std::runtime_error("Expected array value");
    std::vector<std::string> result;
    const auto& arr = val.asListConst();
    result.reserve(arr.size());
    for (const auto& item : arr) {
        result.push_back(item.toString());
    }
    return result;
}

static bool getBool(const interpreter::NaabVal& val) {
    if (!val.isBool()) throw std::runtime_error("Expected boolean value");
    return val.asBool();
}

} // namespace stdlib
} // namespace naab
