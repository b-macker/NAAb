// NAAb Module System Implementation
// Phase 4.0: Build System

#include "naab/module_system.h"
#include "naab/parser.h"
#include "naab/lexer.h"
#include "naab/logger.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <fmt/core.h>

namespace naab {
namespace modules {

// Helper: Check if a module is a stdlib module
// ISS-022 Fix: Stdlib modules are built-in and don't need to be loaded from files
static bool isStdlibModule(const std::string& module_path) {
    static const std::unordered_set<std::string> stdlib_modules = {
        "io", "json", "string", "array", "math", "file", "http",
        "time", "regex", "crypto", "csv", "env", "collections", "core", "console", "process"
    };
    return stdlib_modules.count(module_path) > 0;
}

ModuleRegistry::ModuleRegistry() {
    // Add default search paths
    // 1. Current directory (implicit in resolveModulePath)
    // 2. Standard library location (future)
    // 3. User-defined paths via NAAB_PATH environment variable (future)
}

// Convert module path to file path
// "math_utils" -> "math_utils.naab"
// "data.processor" -> "data/processor.naab"
std::string ModuleRegistry::modulePathToFilePath(const std::string& module_path) const {
    std::string file_path = module_path;

    // Replace dots with directory separators
    for (char& c : file_path) {
        if (c == '.') {
            c = std::filesystem::path::preferred_separator;
        }
    }

    // Add .naab extension
    file_path += ".naab";

    return file_path;
}

// Resolve module path to absolute file path
std::optional<std::string> ModuleRegistry::resolveModulePath(
    const std::string& module_path,
    const std::filesystem::path& current_dir
) {
    std::string file_path = modulePathToFilePath(module_path);

    // 1. Check relative to current directory
    auto full_path = current_dir / file_path;
    if (std::filesystem::exists(full_path)) {
        return std::filesystem::absolute(full_path).string();
    }

    // 2. Check each search path
    for (const auto& search_path : search_paths_) {
        auto search_full_path = std::filesystem::path(search_path) / file_path;
        if (std::filesystem::exists(search_full_path)) {
            return std::filesystem::absolute(search_full_path).string();
        }
    }

    // 3. Check standard library location (future)
    // For now, just return nullopt if not found

    return std::nullopt;
}

// Parse a module file
std::unique_ptr<ast::Program> ModuleRegistry::parseModuleFile(const std::string& file_path) {
    // Read source code
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    if (source.empty()) {
        throw std::runtime_error("Empty module file: " + file_path);
    }

    // Tokenize
    lexer::Lexer lexer(source);
    auto tokens = lexer.tokenize();

    // Parse
    parser::Parser parser(tokens);
    parser.setSource(source, file_path);
    auto ast = parser.parseProgram();

    if (!ast) {
        throw std::runtime_error("Failed to parse module: " + file_path);
    }

    return ast;
}

// Extract dependencies from parsed AST
std::vector<std::string> ModuleRegistry::extractDependencies(const ast::Program* program) const {
    std::vector<std::string> dependencies;

    if (!program) {
        return dependencies;
    }

    // Extract from ModuleUseStmt nodes
    for (const auto& module_use : program->getModuleUses()) {
        dependencies.push_back(module_use->getModulePath());
    }

    // Note: We don't need to extract from ImportStmt nodes for now,
    // as they use a different system (ES6-style imports)

    return dependencies;
}

// Load a module (parse if not already loaded)
NaabModule* ModuleRegistry::loadModule(
    const std::string& module_path,
    const std::filesystem::path& current_dir
) {
    // Check if already loaded
    if (isLoaded(module_path)) {
        return getModule(module_path);
    }

    LOG_DEBUG("[MODULE] Loading module: {}\n", module_path);

    // Resolve file path
    auto resolved_path_opt = resolveModulePath(module_path, current_dir);
    if (!resolved_path_opt) {
        fmt::print("[ERROR] Module not found: {}\n", module_path);
        fmt::print("        Searched in:\n");
        fmt::print("          - {}\n", (current_dir / modulePathToFilePath(module_path)).string());
        for (const auto& search_path : search_paths_) {
            fmt::print("          - {}\n",
                      (std::filesystem::path(search_path) / modulePathToFilePath(module_path)).string());
        }
        // Check for common double-path mistake (e.g., modules/modules/foo.naab)
        std::string searched_path = (current_dir / modulePathToFilePath(module_path)).string();
        std::string dir_name = current_dir.filename().string();
        // Check if the module path starts with the directory name the script is in
        bool is_double_path = (module_path.find(dir_name + ".") == 0);

        if (is_double_path) {
            // Script is in modules/ and uses "use modules.X" → double path
            std::string suggested = module_path.substr(dir_name.size() + 1);
            fmt::print("\n  Hint: Double path detected! Your script is inside the '{}/' directory\n"
                       "  and uses 'use {}', which resolves to '{}'.\n\n"
                       "  Since the script is already in '{}/', use the shorter form:\n"
                       "    use {}  (not 'use {}')\n\n"
                       "  Or move the script to the parent directory of '{}/'\n"
                       "  so 'use {}' resolves correctly.\n\n",
                       dir_name, module_path, searched_path,
                       dir_name, suggested, module_path,
                       dir_name, module_path);
        } else {
            fmt::print("\n  Hint: NAAb resolves 'use' modules relative to the SCRIPT FILE's directory.\n"
                       "  If your script is at /tmp/script.naab and uses 'use modules.foo',\n"
                       "  NAAb looks for /tmp/modules/foo.naab — NOT relative to the working directory.\n\n"
                       "  Fix: Place the script in the same directory as the modules/ folder.\n"
                       "  Example: if modules/ is at /project/modules/foo.naab,\n"
                       "  put your script at /project/script.naab (not /project/output/script.naab).\n\n"
                       "  There is no --path flag. Module resolution is always relative to the script.\n\n");
        }
        return nullptr;
    }

    std::string resolved_path = *resolved_path_opt;
    LOG_DEBUG("[MODULE] Resolved to: {}\n", resolved_path);

    // Parse module
    try {
        auto ast = parseModuleFile(resolved_path);

        // Create module
        auto module = std::make_unique<NaabModule>(module_path, resolved_path);
        module->setAST(std::move(ast));

        // Extract dependencies
        auto dependencies = extractDependencies(module->getAST());
        for (const auto& dep : dependencies) {
            module->addDependency(dep);
            LOG_DEBUG("[MODULE]   Dependency: {}\n", dep);
        }

        // Store module
        NaabModule* module_ptr = module.get();
        modules_[module_path] = std::move(module);

        LOG_DEBUG("[MODULE] Successfully loaded: {}\n", module_path);
        return module_ptr;

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to load module {}: {}\n", module_path, e.what());
        return nullptr;
    }
}

// Check if module is loaded
bool ModuleRegistry::isLoaded(const std::string& module_path) const {
    return modules_.find(module_path) != modules_.end();
}

// Get a loaded module
NaabModule* ModuleRegistry::getModule(const std::string& module_path) {
    auto it = modules_.find(module_path);
    return it != modules_.end() ? it->second.get() : nullptr;
}

const NaabModule* ModuleRegistry::getModule(const std::string& module_path) const {
    auto it = modules_.find(module_path);
    return it != modules_.end() ? it->second.get() : nullptr;
}

// Recursive helper for dependency graph construction
void ModuleRegistry::buildDependencyGraphRecursive(
    NaabModule* module,
    std::vector<NaabModule*>& result,
    std::unordered_set<NaabModule*>& visited,
    std::unordered_set<NaabModule*>& in_progress,
    std::vector<std::string>& cycle_path
) {
    if (visited.count(module)) {
        return;  // Already processed
    }

    // Detect circular dependencies
    if (in_progress.count(module)) {
        // Circular dependency detected!
        std::string cycle_str = "\n  Dependency cycle:\n    " + cycle_path[0];
        for (size_t i = 1; i < cycle_path.size(); i++) {
            cycle_str += "\n      -> " + cycle_path[i];
        }
        cycle_str += "\n      -> " + module->getName() + " (cycle!)";

        throw std::runtime_error(
            "Circular dependency detected: " + module->getName() + cycle_str +
            "\n\n  Help: Remove one of these imports to break the cycle"
        );
    }

    in_progress.insert(module);
    cycle_path.push_back(module->getName());

    // Visit dependencies first (depth-first)
    for (const auto& dep_path : module->getDependencies()) {
        // ISS-022 Fix: Skip stdlib modules - they're built-in and don't need file loading
        if (isStdlibModule(dep_path)) {
            LOG_DEBUG("[MODULE]   Skipping stdlib module: {}\n", dep_path);
            continue;
        }

        NaabModule* dep = getModule(dep_path);
        if (!dep) {
            // Dependency not loaded yet - try to load it
            dep = loadModule(dep_path, std::filesystem::path(module->getFilePath()).parent_path());
            if (!dep) {
                throw std::runtime_error(
                    "Failed to load dependency: " + dep_path +
                    "\n  Required by: " + module->getName()
                );
            }
        }

        buildDependencyGraphRecursive(dep, result, visited, in_progress, cycle_path);
    }

    in_progress.erase(module);
    cycle_path.pop_back();
    visited.insert(module);
    result.push_back(module);  // Add after dependencies (topological order)
}

// Build dependency graph with topological sort
std::vector<NaabModule*> ModuleRegistry::buildDependencyGraph(NaabModule* entry_module) {
    if (!entry_module) {
        return {};
    }

    LOG_DEBUG("[MODULE] Building dependency graph for: {}\n", entry_module->getName());

    std::vector<NaabModule*> result;
    std::unordered_set<NaabModule*> visited;
    std::unordered_set<NaabModule*> in_progress;
    std::vector<std::string> cycle_path;

    try {
        buildDependencyGraphRecursive(entry_module, result, visited, in_progress, cycle_path);

        LOG_DEBUG("[MODULE] Execution order:\n");
        for (size_t i = 0; i < result.size(); i++) {
            LOG_DEBUG("[MODULE]   {}. {}\n", i + 1, result[i]->getName());
        }

        return result;

    } catch (const std::exception& e) {
        fmt::print("[ERROR] {}\n", e.what());
        throw;
    }
}

} // namespace modules
} // namespace naab
