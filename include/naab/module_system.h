#pragma once

// NAAb Module System - Multi-file project support
// Phase 4.0: Build System implementation

#include "naab/ast.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <filesystem>

namespace naab {

// Forward declarations
namespace parser { class Parser; }
namespace interpreter { class Environment; }

namespace modules {

// A loaded module with parsed AST and execution state
// Renamed from Module to NaabModule to avoid conflict with module_resolver.h
class NaabModule {
public:
    NaabModule(const std::string& name, const std::string& file_path)
        : name_(name), file_path_(file_path),
          is_parsed_(false), is_executed_(false) {}

    // Module identification
    const std::string& getName() const { return name_; }
    const std::string& getFilePath() const { return file_path_; }

    // Parsing state
    void setAST(std::unique_ptr<ast::Program> ast) {
        ast_ = std::move(ast);
        is_parsed_ = true;
    }
    ast::Program* getAST() const { return ast_.get(); }
    bool isParsed() const { return is_parsed_; }

    // Execution state
    void setEnvironment(std::shared_ptr<interpreter::Environment> env) {
        module_env_ = env;
    }
    std::shared_ptr<interpreter::Environment> getEnvironment() const { return module_env_; }
    bool isExecuted() const { return is_executed_; }
    void markExecuted() { is_executed_ = true; }

    // Exported items (functions, structs, enums)
    void addExport(const std::string& name, std::shared_ptr<class Value> value) {
        exports_[name] = value;
    }
    const std::unordered_map<std::string, std::shared_ptr<class Value>>& getExports() const {
        return exports_;
    }
    bool hasExport(const std::string& name) const {
        return exports_.find(name) != exports_.end();
    }
    std::shared_ptr<class Value> getExport(const std::string& name) const {
        auto it = exports_.find(name);
        return it != exports_.end() ? it->second : nullptr;
    }

    // Dependencies (other modules this module imports)
    void addDependency(const std::string& module_path) {
        dependencies_.push_back(module_path);
    }
    const std::vector<std::string>& getDependencies() const {
        return dependencies_;
    }

private:
    std::string name_;                  // Module name (math_utils, data.processor)
    std::string file_path_;             // Absolute file path
    std::unique_ptr<ast::Program> ast_; // Parsed AST
    std::shared_ptr<interpreter::Environment> module_env_; // Module's environment
    bool is_parsed_;
    bool is_executed_;

    // Exported items
    std::unordered_map<std::string, std::shared_ptr<class Value>> exports_;

    // Dependencies
    std::vector<std::string> dependencies_;
};

// Registry for managing loaded modules
class ModuleRegistry {
public:
    ModuleRegistry();

    // Module resolution: convert module path to file path
    // "math_utils" -> "./math_utils.naab"
    // "data.processor" -> "./data/processor.naab"
    std::optional<std::string> resolveModulePath(
        const std::string& module_path,
        const std::filesystem::path& current_dir = std::filesystem::current_path()
    );

    // Load a module (parse if not already loaded)
    // Returns nullptr if module cannot be found or parsed
    NaabModule* loadModule(
        const std::string& module_path,
        const std::filesystem::path& current_dir = std::filesystem::current_path()
    );

    // Check if module is already loaded
    bool isLoaded(const std::string& module_path) const;

    // Get a loaded module (returns nullptr if not loaded)
    NaabModule* getModule(const std::string& module_path);
    const NaabModule* getModule(const std::string& module_path) const;

    // Build dependency graph for a module (topological sort)
    // Returns modules in execution order (dependencies first)
    // Throws exception if circular dependency detected
    std::vector<NaabModule*> buildDependencyGraph(NaabModule* entry_module);

    // Add search path for modules
    void addSearchPath(const std::string& path) {
        search_paths_.push_back(path);
    }

    // Get all search paths
    const std::vector<std::string>& getSearchPaths() const {
        return search_paths_;
    }

    // Get all loaded modules
    const std::unordered_map<std::string, std::unique_ptr<NaabModule>>& getModules() const {
        return modules_;
    }

    // Statistics
    size_t moduleCount() const { return modules_.size(); }

private:
    // Loaded modules (key: module path like "math_utils" or "data.processor")
    std::unordered_map<std::string, std::unique_ptr<NaabModule>> modules_;

    // Module search paths
    std::vector<std::string> search_paths_;

    // Helper: Convert module path to file path
    // "data.processor" -> "data/processor.naab"
    std::string modulePathToFilePath(const std::string& module_path) const;

    // Helper: Parse a module file
    std::unique_ptr<ast::Program> parseModuleFile(const std::string& file_path);

    // Helper: Extract dependencies from a parsed module
    std::vector<std::string> extractDependencies(const ast::Program* program) const;

    // Helper: Topological sort with cycle detection
    void buildDependencyGraphRecursive(
        NaabModule* module,
        std::vector<NaabModule*>& result,
        std::unordered_set<NaabModule*>& visited,
        std::unordered_set<NaabModule*>& in_progress,
        std::vector<std::string>& cycle_path
    );
};

} // namespace modules
} // namespace naab

