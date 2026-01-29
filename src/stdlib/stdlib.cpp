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

// ============================================================================
// IO Module Implementation
// ============================================================================

bool IOModule::hasFunction(const std::string& name) const {
    return name == "read_file" || name == "write_file" ||
           name == "exists" || name == "list_dir" ||
           name == "write" || name == "write_error" || name == "read_line";
}

std::shared_ptr<interpreter::Value> IOModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

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
        // Write to stdout
        if (args.empty()) {
            throw std::runtime_error("write() requires at least one argument");
        }
        for (const auto& arg : args) {
            std::cout << arg->toString();
        }
        std::cout.flush();
        return std::make_shared<interpreter::Value>();  // Return null/void
    }
    else if (function_name == "write_error") {
        // Write to stderr
        if (args.empty()) {
            throw std::runtime_error("write_error() requires at least one argument");
        }
        for (const auto& arg : args) {
            std::cerr << arg->toString();
        }
        std::cerr.flush();
        return std::make_shared<interpreter::Value>();  // Return null/void
    }
    else if (function_name == "read_line") {
        // Read line from stdin
        std::string line;
        if (std::getline(std::cin, line)) {
            return std::make_shared<interpreter::Value>(line);
        }
        return std::make_shared<interpreter::Value>("");  // Return empty string on EOF
    }

    throw std::runtime_error("Unknown io function: " + function_name);
}

std::shared_ptr<interpreter::Value> IOModule::read_file(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        throw std::runtime_error("read_file requires filename argument");
    }

    std::string filename = args[0]->toString();

    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    return std::make_shared<interpreter::Value>(buffer.str());
}

std::shared_ptr<interpreter::Value> IOModule::write_file(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("write_file requires filename and content arguments");
    }

    std::string filename = args[0]->toString();
    std::string content = args[1]->toString();

    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }

    file << content;
    file.close();

    return std::make_shared<interpreter::Value>(true);
}

std::shared_ptr<interpreter::Value> IOModule::exists(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        throw std::runtime_error("exists requires filename argument");
    }

    std::string filename = args[0]->toString();
    bool file_exists = fs::exists(filename);

    return std::make_shared<interpreter::Value>(file_exists);
}

std::shared_ptr<interpreter::Value> IOModule::list_dir(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        throw std::runtime_error("list_dir requires directory path argument");
    }

    std::string dir_path = args[0]->toString();

    if (!fs::exists(dir_path) || !fs::is_directory(dir_path)) {
        throw std::runtime_error("Not a directory: " + dir_path);
    }

    std::vector<std::shared_ptr<interpreter::Value>> entries;

    for (const auto& entry : fs::directory_iterator(dir_path)) {
        entries.push_back(std::make_shared<interpreter::Value>(
            entry.path().filename().string()
        ));
    }

    return std::make_shared<interpreter::Value>(entries);
}

// ============================================================================
// Collections Module Implementation
// ============================================================================

bool CollectionsModule::hasFunction(const std::string& name) const {
    return name == "Set" || name == "set_add" || name == "set_contains" ||
           name == "set_remove" || name == "set_size";
}

std::shared_ptr<interpreter::Value> CollectionsModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

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

std::shared_ptr<interpreter::Value> CollectionsModule::set_create(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {
    (void)args; // Intentionally unused - creates empty set

    // Create empty set (represented as list for now)
    std::vector<std::shared_ptr<interpreter::Value>> set_data;

    return std::make_shared<interpreter::Value>(set_data);
}

std::shared_ptr<interpreter::Value> CollectionsModule::set_add(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("set_add requires set and value arguments");
    }

    // Get the set (represented as vector)
    auto& set_value = args[0];
    auto& new_value = args[1];

    // Extract vector from set
    auto vec_ptr = std::get_if<std::vector<std::shared_ptr<interpreter::Value>>>(&set_value->data);
    if (!vec_ptr) {
        throw std::runtime_error("set_add: first argument must be a set");
    }

    // Check if value already exists (ensure uniqueness)
    for (const auto& item : *vec_ptr) {
        if (item->toString() == new_value->toString()) {
            // Value already exists, return the same set
            return set_value;
        }
    }

    // Create new set with the additional value
    std::vector<std::shared_ptr<interpreter::Value>> new_set = *vec_ptr;
    new_set.push_back(new_value);

    return std::make_shared<interpreter::Value>(new_set);
}

std::shared_ptr<interpreter::Value> CollectionsModule::set_contains(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("set_contains requires set and value arguments");
    }

    auto& set_value = args[0];
    auto& search_value = args[1];

    // Extract vector from set
    auto vec_ptr = std::get_if<std::vector<std::shared_ptr<interpreter::Value>>>(&set_value->data);
    if (!vec_ptr) {
        throw std::runtime_error("set_contains: first argument must be a set");
    }

    // Search for value in set
    std::string search_str = search_value->toString();
    for (const auto& item : *vec_ptr) {
        if (item->toString() == search_str) {
            return std::make_shared<interpreter::Value>(true);
        }
    }

    return std::make_shared<interpreter::Value>(false);
}

std::shared_ptr<interpreter::Value> CollectionsModule::set_remove(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("set_remove requires set and value arguments");
    }

    auto& set_value = args[0];
    auto& remove_value = args[1];

    // Extract vector from set
    auto vec_ptr = std::get_if<std::vector<std::shared_ptr<interpreter::Value>>>(&set_value->data);
    if (!vec_ptr) {
        throw std::runtime_error("set_remove: first argument must be a set");
    }

    // Create new set without the specified value
    std::vector<std::shared_ptr<interpreter::Value>> new_set;
    std::string remove_str = remove_value->toString();

    for (const auto& item : *vec_ptr) {
        if (item->toString() != remove_str) {
            new_set.push_back(item);
        }
    }

    return std::make_shared<interpreter::Value>(new_set);
}

std::shared_ptr<interpreter::Value> CollectionsModule::set_size(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 1) {
        throw std::runtime_error("set_size requires set argument");
    }

    auto& set_value = args[0];

    // Extract vector from set
    auto vec_ptr = std::get_if<std::vector<std::shared_ptr<interpreter::Value>>>(&set_value->data);
    if (!vec_ptr) {
        throw std::runtime_error("set_size: argument must be a set");
    }

    return std::make_shared<interpreter::Value>(static_cast<int>(vec_ptr->size()));
}

// ============================================================================
// Standard Library Manager
// ============================================================================

// GEMINI FIX: Constructor now takes Interpreter pointer
StdLib::StdLib(interpreter::Interpreter* interpreter)
    : interpreter_(interpreter) // GEMINI FIX: Store interpreter pointer
{
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
    // GEMINI FIX: Pass interpreter pointer to EnvModule constructor
    modules_["env"] = std::make_shared<EnvModule>(interpreter_);
    modules_["csv"] = std::make_shared<CsvModule>();
    modules_["regex"] = std::make_shared<RegexModule>();
    modules_["crypto"] = std::make_shared<CryptoModule>();
    modules_["file"] = std::make_shared<FileModule>();
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
