// NAAb Module Resolver Implementation - Phase 3.1 & 3.2
// Resolves and loads NAAb modules from filesystem

#include "naab/module_resolver.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace naab {
namespace modules {

// ============================================================================
// ModuleCache Implementation
// ============================================================================

bool ModuleCache::has(const std::string& canonical_path) const {
    return cache_.find(canonical_path) != cache_.end();
}

std::shared_ptr<Module> ModuleCache::get(const std::string& canonical_path) {
    auto it = cache_.find(canonical_path);
    if (it != cache_.end()) {
        return it->second;
    }
    return nullptr;
}

void ModuleCache::put(const std::string& canonical_path, std::shared_ptr<Module> module) {
    cache_[canonical_path] = std::move(module);
}

void ModuleCache::clear() {
    cache_.clear();
}

std::vector<std::string> ModuleCache::getPaths() const {
    std::vector<std::string> paths;
    paths.reserve(cache_.size());
    for (const auto& [path, _] : cache_) {
        paths.push_back(path);
    }
    return paths;
}

// ============================================================================
// CircularDependencyError Implementation
// ============================================================================

std::string CircularDependencyError::formatMessage(
    const std::string& path,
    const std::vector<std::string>& chain) {

    std::stringstream ss;
    ss << "Circular dependency detected:\n";

    for (size_t i = 0; i < chain.size(); ++i) {
        ss << "  " << (i + 1) << ". " << chain[i] << "\n";
    }

    ss << "  " << (chain.size() + 1) << ". " << path << " (circular!)";

    return ss.str();
}

// ============================================================================
// ModuleResolver Implementation
// ============================================================================

ModuleResolver::ModuleResolver() {
    initializeSearchPaths();
}

void ModuleResolver::initializeSearchPaths() {
    // Phase 3.2: Initialize default search paths

    // 1. Current directory naab_modules/ (will be added per-file)
    // 2. Global modules: ~/.naab/modules/
    auto home = std::getenv("HOME");
    if (home) {
        fs::path global_modules = fs::path(home) / ".naab" / "modules";
        if (fs::exists(global_modules)) {
            search_paths_.push_back(global_modules);
        }
    }

    // 3. System modules: /usr/local/naab/modules/
    fs::path system_modules = "/usr/local/naab/modules";
    if (fs::exists(system_modules)) {
        search_paths_.push_back(system_modules);
    }
}

std::string ModuleResolver::canonicalizePath(const fs::path& path) {
    try {
        return fs::canonical(path).string();
    } catch (const fs::filesystem_error&) {
        // If canonical fails, use absolute path
        return fs::absolute(path).string();
    }
}

std::optional<fs::path> ModuleResolver::resolve(
    const std::string& module_spec,
    const fs::path& current_file_dir) {

    // 1. Try relative path resolution
    auto relative = resolveRelative(module_spec, current_file_dir);
    if (relative) return relative;

    // 2. Try naab_modules/ resolution
    auto from_modules = resolveFromModules(module_spec, current_file_dir);
    if (from_modules) return from_modules;

    // 3. Try global modules
    auto from_global = resolveFromGlobal(module_spec);
    if (from_global) return from_global;

    // 4. Try system modules
    auto from_system = resolveFromSystem(module_spec);
    if (from_system) return from_system;

    // 5. Try custom search paths
    for (const auto& search_path : search_paths_) {
        fs::path candidate = search_path / module_spec;
        if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
            return candidate;
        }

        // Try with .naab extension
        fs::path with_ext = search_path / (module_spec + ".naab");
        if (fs::exists(with_ext) && fs::is_regular_file(with_ext)) {
            return with_ext;
        }
    }

    return std::nullopt;
}

std::optional<fs::path> ModuleResolver::resolveRelative(
    const std::string& spec,
    const fs::path& current_dir) {

    // First try paths starting with ./ or ../
    if (spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0) {
        fs::path candidate = current_dir / spec;

        // Try exact path
        if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
            return candidate;
        }

        // Try with .naab extension
        fs::path with_ext = fs::path(candidate.string() + ".naab");
        if (fs::exists(with_ext) && fs::is_regular_file(with_ext)) {
            return with_ext;
        }

        return std::nullopt;
    }

    // Also try bare relative paths (e.g., "modules/logger.naab")
    // These are resolved relative to the current file's directory
    fs::path candidate = current_dir / spec;
    if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
        return candidate;
    }

    // Try with .naab extension
    fs::path with_ext = fs::path(candidate.string() + ".naab");
    if (fs::exists(with_ext) && fs::is_regular_file(with_ext)) {
        return with_ext;
    }

    return std::nullopt;
}

std::optional<fs::path> ModuleResolver::resolveFromModules(
    const std::string& spec,
    const fs::path& current_dir) {

    // Don't try naab_modules for relative paths
    if (spec.rfind("./", 0) == 0 || spec.rfind("../", 0) == 0) {
        return std::nullopt;
    }

    // Search current directory and parents
    fs::path dir = current_dir;

    while (true) {
        fs::path naab_modules = dir / "naab_modules";

        if (fs::exists(naab_modules) && fs::is_directory(naab_modules)) {
            fs::path candidate = naab_modules / spec;

            if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
                return candidate;
            }

            // Try with .naab extension
            fs::path with_ext = naab_modules / (spec + ".naab");
            if (fs::exists(with_ext) && fs::is_regular_file(with_ext)) {
                return with_ext;
            }
        }

        // Move to parent directory
        if (dir == dir.parent_path()) {
            break;  // Reached root
        }
        dir = dir.parent_path();
    }

    return std::nullopt;
}

std::optional<fs::path> ModuleResolver::resolveFromGlobal(const std::string& spec) {
    auto home = std::getenv("HOME");
    if (!home) {
        return std::nullopt;
    }

    fs::path global_modules = fs::path(home) / ".naab" / "modules";

    if (!fs::exists(global_modules)) {
        return std::nullopt;
    }

    fs::path candidate = global_modules / spec;
    if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
        return candidate;
    }

    fs::path with_ext = global_modules / (spec + ".naab");
    if (fs::exists(with_ext) && fs::is_regular_file(with_ext)) {
        return with_ext;
    }

    return std::nullopt;
}

std::optional<fs::path> ModuleResolver::resolveFromSystem(const std::string& spec) {
    fs::path system_modules = "/usr/local/naab/modules";

    if (!fs::exists(system_modules)) {
        return std::nullopt;
    }

    fs::path candidate = system_modules / spec;
    if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
        return candidate;
    }

    fs::path with_ext = system_modules / (spec + ".naab");
    if (fs::exists(with_ext) && fs::is_regular_file(with_ext)) {
        return with_ext;
    }

    return std::nullopt;
}

std::unique_ptr<ast::Program> ModuleResolver::parseModuleFile(const fs::path& path) {
    // Read file
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open module file: " + path.string());
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    // Lex and parse
    lexer::Lexer lexer(source);
    auto tokens = lexer.tokenize();

    parser::Parser parser(tokens);
    parser.setSource(source, path.string());

    return parser.parseProgram();
}

std::shared_ptr<Module> ModuleResolver::loadModule(const fs::path& module_path) {
    std::string canonical = canonicalizePath(module_path);

    // Check if already loaded
    if (cache_.has(canonical)) {
        return cache_.get(canonical);
    }

    // Check for circular dependency
    if (isInImportStack(canonical)) {
        throw CircularDependencyError(canonical, import_stack_);
    }

    // Push to import stack
    pushImportStack(canonical);

    try {
        // Create module
        auto module = std::make_shared<Module>(canonical);

        // Parse module
        module->ast = parseModuleFile(module_path);
        module->is_loaded = true;

        // Cache it
        cache_.put(canonical, module);

        // Pop from stack
        popImportStack();

        return module;

    } catch (...) {
        popImportStack();
        throw;
    }
}

std::shared_ptr<Module> ModuleResolver::getModule(const std::string& canonical_path) {
    return cache_.get(canonical_path);
}

void ModuleResolver::addSearchPath(const fs::path& path) {
    if (fs::exists(path) && fs::is_directory(path)) {
        search_paths_.push_back(path);
    }
}

void ModuleResolver::pushImportStack(const std::string& module_path) {
    import_stack_.push_back(module_path);
}

void ModuleResolver::popImportStack() {
    if (!import_stack_.empty()) {
        import_stack_.pop_back();
    }
}

bool ModuleResolver::isInImportStack(const std::string& module_path) const {
    return std::find(import_stack_.begin(), import_stack_.end(), module_path)
           != import_stack_.end();
}

// ============================================================================
// ModuleConfig Implementation
// ============================================================================

std::optional<ModuleConfig> ModuleConfig::loadFrom(const fs::path& config_file) {
    if (!fs::exists(config_file)) {
        return std::nullopt;
    }

    std::ifstream file(config_file);
    if (!file.is_open()) {
        return std::nullopt;
    }

    ModuleConfig config;
    std::string line;

    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Parse key=value format
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        if (key == "module_path") {
            config.custom_paths.push_back(value);
        } else if (key.find("alias.") == 0) {
            std::string alias_name = key.substr(6);
            config.path_aliases[alias_name] = value;
        }
    }

    return config;
}

std::optional<ModuleConfig> ModuleConfig::findAndLoad(const fs::path& start_dir) {
    fs::path dir = start_dir;

    while (true) {
        fs::path config_file = dir / ".naabrc";

        if (fs::exists(config_file)) {
            return loadFrom(config_file);
        }

        // Move to parent
        if (dir == dir.parent_path()) {
            break;  // Reached root
        }
        dir = dir.parent_path();
    }

    return std::nullopt;
}

} // namespace modules
} // namespace naab
