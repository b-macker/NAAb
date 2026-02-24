#pragma once

// NAAb Module Resolver - Phase 3.1 & 3.2
// Resolves module paths and manages module loading

#include "naab/ast.h"
#include <string>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <filesystem>

namespace fs = std::filesystem;

namespace naab {
namespace modules {

// ============================================================================
// Module - Represents a loaded NAAb module
// ============================================================================
struct Module {
    std::string path;                           // Full path to module file
    std::unique_ptr<ast::Program> ast;          // Parsed AST
    std::unordered_map<std::string, void*> exports;  // Exported symbols
    bool is_loaded;                             // Loading complete?

    explicit Module(std::string p)
        : path(std::move(p)), is_loaded(false) {}
};

// ============================================================================
// ModuleCache - Prevents reloading modules
// ============================================================================
class ModuleCache {
public:
    // Check if module is cached
    bool has(const std::string& canonical_path) const;

    // Get cached module
    std::shared_ptr<Module> get(const std::string& canonical_path);

    // Add module to cache
    void put(const std::string& canonical_path, std::shared_ptr<Module> module);

    // Clear cache
    void clear();

    // Get all cached paths
    std::vector<std::string> getPaths() const;

private:
    std::unordered_map<std::string, std::shared_ptr<Module>> cache_;
};

// ============================================================================
// CircularDependencyError - Detected circular import
// ============================================================================
class CircularDependencyError : public std::runtime_error {
public:
    CircularDependencyError(const std::string& module_path,
                             const std::vector<std::string>& import_chain)
        : std::runtime_error(formatMessage(module_path, import_chain)),
          module_path_(module_path), import_chain_(import_chain) {}

    const std::string& getModulePath() const { return module_path_; }
    const std::vector<std::string>& getImportChain() const { return import_chain_; }

private:
    std::string module_path_;
    std::vector<std::string> import_chain_;

    static std::string formatMessage(const std::string& path,
                                       const std::vector<std::string>& chain);
};

// ============================================================================
// ModuleResolver - Resolves and loads modules
// ============================================================================
class ModuleResolver {
public:
    ModuleResolver();

    // Resolve module path from import statement
    std::optional<fs::path> resolve(const std::string& module_spec,
                                     const fs::path& current_file_dir);

    // Load module from filesystem
    std::shared_ptr<Module> loadModule(const fs::path& module_path);

    // Get cached module
    std::shared_ptr<Module> getModule(const std::string& canonical_path);

    // Add custom search path
    void addSearchPath(const fs::path& path);

    // Get all search paths
    std::vector<fs::path> getSearchPaths() const { return search_paths_; }

    // Clear module cache
    void clearCache() { cache_.clear(); }

    // Check for circular dependencies
    void pushImportStack(const std::string& module_path);
    void popImportStack();
    bool isInImportStack(const std::string& module_path) const;

private:
    ModuleCache cache_;
    std::vector<fs::path> search_paths_;
    std::vector<std::string> import_stack_;  // For circular detection

    // Initialize default search paths (Phase 3.2)
    void initializeSearchPaths();

    // Resolve relative path (./module.naab or ../module.naab)
    std::optional<fs::path> resolveRelative(const std::string& spec,
                                             const fs::path& current_dir);

    // Resolve from naab_modules/ directories
    std::optional<fs::path> resolveFromModules(const std::string& spec,
                                                 const fs::path& current_dir);

    // Resolve from global modules (~/.naab/modules/)
    std::optional<fs::path> resolveFromGlobal(const std::string& spec);

    // Resolve from system modules (/usr/local/naab/modules/)
    std::optional<fs::path> resolveFromSystem(const std::string& spec);

    // Read and parse module file
    std::unique_ptr<ast::Program> parseModuleFile(const fs::path& path);

public:
    // Canonicalize path for caching (public static)
    static std::string canonicalizePath(const fs::path& path);
};

// ============================================================================
// ModuleConfig - Configuration from .naabrc
// ============================================================================
struct ModuleConfig {
    std::vector<std::string> custom_paths;
    std::unordered_map<std::string, std::string> path_aliases;

    // Load from .naabrc file
    static std::optional<ModuleConfig> loadFrom(const fs::path& config_file);

    // Load from current directory and parents
    static std::optional<ModuleConfig> findAndLoad(const fs::path& start_dir);
};

} // namespace modules
} // namespace naab

