// NAAb Standard Library Implementation
// Built-in modules for common operations

#include "naab/stdlib.h"
#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace naab {
namespace stdlib {

// Global pipe mode flag: when true, io.write() redirects to stderr
// so stdout remains clean for machine-readable output (io.output())
static bool g_pipe_mode = false;

void setPipeMode(bool enabled) {
    g_pipe_mode = enabled;
}

bool getPipeMode() {
    return g_pipe_mode;
}

// V-DOS-002: thread-local output capture for REST API per-request isolation.
// When set, io.write/print/log/output write here instead of std::cout/cerr.
// Zero-cost (null check) on the common path; each REST request sets its own
// ostringstream, eliminating the need for a global rdbuf redirect + mutex.
static thread_local std::ostream* tl_capture_stream = nullptr;

void setIoCaptureStream(std::ostream* stream) {
    tl_capture_stream = stream;
}

// ============================================================================
// IO Module Implementation
// ============================================================================

bool IOModule::hasFunction(const std::string& name) const {
    return name == "read_file" || name == "write_file" ||
           name == "exists" || name == "list_dir" ||
           name == "write" || name == "write_error" || name == "read_line" ||
           name == "output" ||
           name == "print" || name == "println" || name == "log" ||
           name == "input";
}

interpreter::NaabVal IOModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    // File I/O functions
    if (function_name == "read_file") {
        return read_file(args);
    } else if (function_name == "write_file") {
        return write_file(args);
    } else if (function_name == "exists") {
        return exists(args);
    } else if (function_name == "list_dir") {
        return list_dir(args);
    }

    // Console I/O functions
    else if (function_name == "write") {
        // Write to stdout (or stderr in --pipe mode)
        if (args.empty()) {
            throw std::runtime_error("write() requires at least one argument");
        }
        // V-DOS-002: use per-request capture stream when set (REST API path)
        std::ostream& out = tl_capture_stream ? *tl_capture_stream
                                              : (g_pipe_mode ? std::cerr : std::cout);
        for (const auto& arg : args) {
            out << arg.toString();
        }
        out.flush();
        return interpreter::NaabVal::makeNull();
    }
    else if (function_name == "output") {
        // Write to stdout ALWAYS (even in --pipe mode)
        // Use this for machine-readable output (JSON results, etc.)
        if (args.empty()) {
            throw std::runtime_error("output() requires at least one argument");
        }
        // V-DOS-002: capture stream also covers io.output in REST context
        std::ostream& out = tl_capture_stream ? *tl_capture_stream : std::cout;
        for (const auto& arg : args) {
            out << arg.toString();
        }
        out.flush();
        return interpreter::NaabVal::makeNull();
    }
    else if (function_name == "write_error") {
        // Write to stderr
        if (args.empty()) {
            throw std::runtime_error("write_error() requires at least one argument");
        }
        for (const auto& arg : args) {
            std::cerr << arg.toString();
        }
        std::cerr.flush();
        return interpreter::NaabVal::makeNull();
    }
    else if (function_name == "read_line") {
        // Read line from stdin
        std::string line;
        if (std::getline(std::cin, line)) {
            return interpreter::NaabVal::makeString(line);
        }
        return interpreter::NaabVal::makeString("");  // Return empty string on EOF
    }

    else if (function_name == "input") {
        // Read line from stdin with optional prompt
        if (!args.empty()) {
            std::ostream& out = tl_capture_stream ? *tl_capture_stream
                                                  : (g_pipe_mode ? std::cerr : std::cout);
            out << args[0].toString();
            out.flush();
        }
        std::string line;
        if (std::getline(std::cin, line)) {
            return interpreter::NaabVal::makeString(line);
        }
        return interpreter::NaabVal::makeString("");
    }

    // Aliases: io.print(), io.println(), io.log() → same as io.write()
    if (function_name == "print" || function_name == "println" || function_name == "log") {
        // V-DOS-002: capture stream for REST API path
        std::ostream& out = tl_capture_stream ? *tl_capture_stream
                                              : (g_pipe_mode ? std::cerr : std::cout);
        if (args.empty()) {
            if (function_name == "println") {
                out << "\n";
                out.flush();
            }
            return interpreter::NaabVal::makeNull();
        }
        for (const auto& arg : args) {
            out << arg.toString();
        }
        if (function_name == "println") {
            out << "\n";
        }
        out.flush();
        return interpreter::NaabVal::makeNull();
    }
    throw std::runtime_error(
        "Unknown io function: " + function_name + "\n\n"
        "  Available: write, read, output\n"
    );
}

interpreter::NaabVal IOModule::read_file(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("read_file requires filename argument");
    }

    std::string filename = args[0].toString();

    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    return interpreter::NaabVal::makeString(buffer.str());
}

interpreter::NaabVal IOModule::write_file(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("write_file requires filename and content arguments");
    }

    std::string filename = args[0].toString();
    std::string content = args[1].toString();

    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }

    file << content;
    file.close();

    return interpreter::NaabVal::makeBool(true);
}

interpreter::NaabVal IOModule::exists(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("exists requires filename argument");
    }

    std::string filename = args[0].toString();
    bool file_exists = fs::exists(filename);

    return interpreter::NaabVal::makeBool(file_exists);
}

interpreter::NaabVal IOModule::list_dir(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("list_dir requires directory path argument");
    }

    std::string dir_path = args[0].toString();

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        throw std::runtime_error("Not a directory: " + dir_path);
    }

    std::vector<interpreter::NaabVal> entries;

    for (const auto& entry : fs::directory_iterator(dir_path)) {
        entries.push_back(interpreter::NaabVal::makeString(
            entry.path().filename().string()
        ));
    }

    return interpreter::NaabVal::makeList(std::move(entries));
}

// ============================================================================
// Collections Module Implementation
// ============================================================================

bool CollectionsModule::hasFunction(const std::string& name) const {
    return name == "Set" || name == "set_add" || name == "set_contains" ||
           name == "set_remove" || name == "set_size";
}

interpreter::NaabVal CollectionsModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "Set") {
        return set_create(args);
    } else if (function_name == "set_add") {
        return set_add(args);
    } else if (function_name == "set_contains") {
        return set_contains(args);
    } else if (function_name == "set_remove") {
        return set_remove(args);
    } else if (function_name == "set_size") {
        return set_size(args);
    }

    throw std::runtime_error("Unknown collections function: " + function_name);
}

interpreter::NaabVal CollectionsModule::set_create(
    std::vector<interpreter::NaabVal>& args) {
    (void)args; // Intentionally unused - creates empty set

    // Create empty set (represented as list for now)
    std::vector<interpreter::NaabVal> set_data;

    return interpreter::NaabVal::makeList(std::move(set_data));
}

interpreter::NaabVal CollectionsModule::set_add(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("set_add requires set and value arguments");
    }

    // Get the set (represented as vector)
    const auto& set_value = args[0];
    const auto& new_value = args[1];

    if (!set_value.isList()) {
        throw std::runtime_error("set_add: first argument must be a set");
    }
    const auto& vec = set_value.asListConst();

    // Check if value already exists (ensure uniqueness)
    for (const auto& item : vec) {
        if (item.toString() == new_value.toString()) {
            return set_value;
        }
    }

    // Create new set with the additional value
    std::vector<interpreter::NaabVal> new_set = vec;
    new_set.push_back(new_value);

    return interpreter::NaabVal::makeList(std::move(new_set));
}

interpreter::NaabVal CollectionsModule::set_contains(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("set_contains requires set and value arguments");
    }

    const auto& set_value = args[0];
    const auto& search_value = args[1];

    if (!set_value.isList()) {
        throw std::runtime_error("set_contains: first argument must be a set");
    }

    std::string search_str = search_value.toString();
    for (const auto& item : set_value.asListConst()) {
        if (item.toString() == search_str) {
            return interpreter::NaabVal::makeBool(true);
        }
    }

    return interpreter::NaabVal::makeBool(false);
}

interpreter::NaabVal CollectionsModule::set_remove(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("set_remove requires set and value arguments");
    }

    const auto& set_value = args[0];
    const auto& remove_value = args[1];

    if (!set_value.isList()) {
        throw std::runtime_error("set_remove: first argument must be a set");
    }

    std::vector<interpreter::NaabVal> new_set;
    std::string remove_str = remove_value.toString();

    for (const auto& item : set_value.asListConst()) {
        if (item.toString() != remove_str) {
            new_set.push_back(item);
        }
    }

    return interpreter::NaabVal::makeList(std::move(new_set));
}

interpreter::NaabVal CollectionsModule::set_size(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 1) {
        throw std::runtime_error("set_size requires set argument");
    }

    const auto& set_value = args[0];

    if (!set_value.isList()) {
        throw std::runtime_error("set_size: argument must be a set");
    }

    return interpreter::NaabVal::makeInt(static_cast<int>(set_value.asListConst().size()));
}

// ============================================================================
// Standard Library Manager
// ============================================================================

StdLib::StdLib() {
    registerModules();
}

void StdLib::registerModules() {
    // Existing modules
    modules_["io"] = std::make_shared<IOModule>();
    modules_["json"] = std::make_shared<JSONModule>();
    modules_["http"] = std::make_shared<HTTPModule>();
    modules_["collections"] = std::make_shared<CollectionsModule>();

    // New stdlib modules
    modules_["string"] = std::make_shared<StringModule>();
    modules_["array"] = std::make_shared<ArrayModule>();
    modules_["math"] = std::make_shared<MathModule>();
    modules_["time"] = std::make_shared<TimeModule>();
    modules_["env"] = std::make_shared<EnvModule>();
    modules_["csv"] = std::make_shared<CsvModule>();
    modules_["regex"] = std::make_shared<RegexModule>();
    modules_["crypto"] = std::make_shared<CryptoModule>();
    modules_["file"] = std::make_shared<FileModule>();
    modules_["debug"] = std::make_shared<DebugModule>();
    modules_["bolo"] = std::make_shared<BoloModule>();
    modules_["path"] = std::make_shared<PathModule>();
    modules_["dict"]     = std::make_shared<DictModule>();
    modules_["log"]      = std::make_shared<LogModule>();
    modules_["uuid"]     = std::make_shared<UuidModule>();
    modules_["validate"] = std::make_shared<ValidateModule>();
    modules_["process"]  = std::make_shared<ProcessModule>();
    modules_["agent"]    = std::make_shared<AgentModule>();
    modules_["governance"] = std::make_shared<GovernanceModule>();
}

std::shared_ptr<Module> StdLib::getModule(const std::string& name) const {
    auto it = modules_.find(name);
    if (it != modules_.end()) {
        return it->second;
    }
    return nullptr;
}

bool StdLib::hasModule(const std::string& name) const {
    return modules_.find(name) != modules_.end();
}

std::vector<std::string> StdLib::listModules() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : modules_) {
        names.push_back(name);
    }
    return names;
}

} // namespace stdlib
} // namespace naab
