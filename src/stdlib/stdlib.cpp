// NAAb Standard Library Implementation
// Built-in modules for common operations

#include "naab/stdlib.h"
#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
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
           name == "exists" || name == "list_dir";
}

std::shared_ptr<interpreter::Value> IOModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "read_file") {
        return read_file(args);
    } else if (function_name == "write_file") {
        return write_file(args);
    } else if (function_name == "exists") {
        return exists(args);
    } else if (function_name == "list_dir") {
        return list_dir(args);
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
    return name == "Set" || name == "set_add" || name == "set_contains";
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
    }

    throw std::runtime_error("Unknown collections function: " + function_name);
}

std::shared_ptr<interpreter::Value> CollectionsModule::set_create(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Create empty set (represented as list for now)
    std::vector<std::shared_ptr<interpreter::Value>> set_data;

    return std::make_shared<interpreter::Value>(set_data);
}

std::shared_ptr<interpreter::Value> CollectionsModule::set_add(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("set_add requires set and value arguments");
    }

    // Simplified implementation - production needs proper set type
    fmt::print("[COLLECTIONS] set_add called (placeholder)\n");

    return std::make_shared<interpreter::Value>(true);
}

std::shared_ptr<interpreter::Value> CollectionsModule::set_contains(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("set_contains requires set and value arguments");
    }

    // Simplified implementation
    fmt::print("[COLLECTIONS] set_contains called (placeholder)\n");

    return std::make_shared<interpreter::Value>(false);
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
