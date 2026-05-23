// NAAb Interpreter — Module System
// Split from interpreter.cpp for maintainability
//
// Contains: visit(UseStatement), visit(ModuleUseStmt), visit(ImportStmt),
//           visit(ExportStmt), loadAndExecuteModule

#include "naab/interpreter.h"
#include "naab/logger.h"
#include "naab/language_registry.h"
#include "naab/block_registry.h"
#ifndef _WIN32
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#endif
#include "naab/stdlib_new_modules.h"
#include "naab/struct_registry.h"
#include <fmt/core.h>
#include <unordered_map>
#include <filesystem>

namespace naab {
namespace interpreter {

void Interpreter::visit(ast::UseStatement& node) {
    std::string module_name = node.getBlockId();
    std::string alias = node.getAlias().empty() ? module_name : node.getAlias();

    // Check if it's a stdlib module first
    if (stdlib_->hasModule(module_name)) {
        auto module = stdlib_->getModule(module_name);
        imported_modules_[alias] = module;

        LOG_DEBUG("[INFO] Imported stdlib module: {} as {}\n", module_name, alias);

        // Store module reference in environment (we'll handle calls specially)
        // For now, create a special marker value
        current_env_->define(alias, NaabVal::makeString("__stdlib_module__:" + alias));
        return;
    }

    // Otherwise, try to load as block
    // Phase 8: Try BlockRegistry first (filesystem-based), then BlockLoader (database-based)
    auto& block_registry = runtime::BlockRegistry::instance();

    // Lazy initialization: only load block registry when actually needed
    if (!block_registry.isInitialized()) {
        std::string home_dir = std::getenv("HOME") ? std::getenv("HOME") : ".";
        std::string blocks_path = home_dir + "/.naab/language/blocks/library/";
        LOG_DEBUG("[INFO] Lazy-loading BlockRegistry from: {}\n", blocks_path);
        block_registry.initialize(blocks_path);
    }

    auto metadata_opt = block_registry.getBlock(node.getBlockId());

    runtime::BlockMetadata metadata;
    std::string code;

    if (metadata_opt.has_value()) {
        // Found in BlockRegistry (filesystem)
        metadata = *metadata_opt;
        code = block_registry.getBlockSource(node.getBlockId());

        LOG_DEBUG("[INFO] Loaded block {} from filesystem as {} ({})\n",
                   node.getBlockId(), alias, metadata.language);

    } else if (block_loader_) {
        // Fall back to BlockLoader (database)
        try {
            metadata = block_loader_->getBlock(node.getBlockId());
            code = block_loader_->loadBlockCode(node.getBlockId());

            LOG_DEBUG("[INFO] Loaded block {} from database as {} ({}, {} tokens)\n",
                       node.getBlockId(), alias, metadata.language, metadata.token_count);
        } catch (const std::exception& e) {
            fmt::print("[ERROR] Failed to load block {}: {}\n", node.getBlockId(), e.what());
            return;
        }
    } else {
        std::string error_msg = fmt::format("Module not found: {}", node.getBlockId());

        // Check if it's close to a stdlib module name
        static const std::vector<std::string> stdlib_names = {
            "io", "json", "string", "array", "math", "file", "http",
            "time", "regex", "crypto", "csv", "env", "collections",
            "log", "uuid", "validate", "process", "path", "dict", "debug", "bolo"
        };
        for (const auto& sn : stdlib_names) {
            if (module_name == sn) {
                error_msg += fmt::format("\n\n  Did you mean the built-in '{}' module?\n"
                    "    use {}    // stdlib", sn, sn);
                break;
            }
        }

        // Check common aliases from other languages
        static const std::unordered_map<std::string, std::string> module_aliases = {
            {"fs", "file"}, {"os", "env"}, {"re", "regex"},
            {"console", "io"}, {"sys", "env"}, {"system", "process"},
            {"random", "math"}, {"datetime", "time"}, {"net", "http"},
        };
        auto alias_it = module_aliases.find(module_name);
        if (alias_it != module_aliases.end()) {
            error_msg += fmt::format("\n\n  Did you mean: use {}\n"
                "  NAAb's '{}' module provides this functionality.",
                alias_it->second, alias_it->second);
        }

        throw std::runtime_error(error_msg);
    }

    try {
        // Store in loaded blocks map
        loaded_blocks_[alias] = metadata;

        // Phase 7: Create appropriate executor for this block
        // Governance check for block module code (same as inline polyglot blocks)
        if (governance_ && governance_->isActive()) {
            std::string err = governance_->checkPolyglotBlock(
                metadata.language, code, current_file_,
                node.getLocation().line, 0);
            if (!err.empty()) throw std::runtime_error(err);
            std::string count_err = governance_->incrementAndCheckPolyglotBlockCount();
            if (!count_err.empty()) throw std::runtime_error(count_err);
        }
        std::shared_ptr<BlockValue> block_value;

#ifndef _WIN32
        if (metadata.language == "cpp" || metadata.language == "c++") {
            // C++ blocks: Create dedicated executor instance per block
            LOG_DEBUG("[INFO] Creating dedicated C++ executor for block...\n");
            auto cpp_exec = std::make_unique<runtime::CppExecutorAdapter>();
            if (!cpp_exec->execute(code, runtime::CppExecutionMode::BLOCK_LIBRARY)) {
                fmt::print("[ERROR] Failed to compile/execute C++ block code\n");
                return;
            }
            block_value = std::make_shared<BlockValue>(metadata, code, std::move(cpp_exec));
        } else
#endif
        {
            // Other languages (JS, Python, etc.): Use shared executor from registry
            // These languages share runtime context across blocks
            auto& registry = runtime::LanguageRegistry::instance();
            auto* executor = registry.getExecutor(metadata.language);

            if (!executor) {
                fmt::print("[ERROR] No executor found for language: {}\n", metadata.language);
                auto langs = registry.supportedLanguages();
                std::string langs_str;
                for (size_t i = 0; i < langs.size(); i++) {
                    if (i > 0) langs_str += ", ";
                    langs_str += langs[i];
                }
                fmt::print("       Supported languages: {}\n", langs_str);
                return;
            }

            LOG_DEBUG("[INFO] Executing block with shared {} executor...\n", metadata.language);

#ifndef _WIN32
            if (metadata.language == "javascript") {
                auto* js_exec = dynamic_cast<runtime::JsExecutorAdapter*>(executor);
                if (js_exec) {
                    if (!js_exec->execute(code, runtime::JsExecutionMode::BLOCK_LIBRARY)) {
                        fmt::print("[ERROR] Failed to execute JavaScript block code\n");
                        return;
                    }
                } else {
                    fmt::print("[ERROR] Executor is not a JsExecutorAdapter\n");
                    return;
                }
            } else if (metadata.language == "cpp" || metadata.language == "c++") {
                auto* cpp_exec = dynamic_cast<runtime::CppExecutorAdapter*>(executor);
                if (cpp_exec) {
                    if (!cpp_exec->execute(code, runtime::CppExecutionMode::BLOCK_LIBRARY)) {
                        fmt::print("[ERROR] Failed to compile/execute C++ block code\n");
                        return;
                    }
                } else {
                    fmt::print("[ERROR] Executor is not a CppExecutorAdapter\n");
                    return;
                }
            } else
#endif
            {
                // Other languages use default execute()
                if (!executor->execute(code)) {
                    fmt::print("[ERROR] Failed to execute block code\n");
                    return;
                }
            }

            block_value = std::make_shared<BlockValue>(metadata, code, executor);
        }

        // Store BlockValue in environment
        current_env_->define(alias, NaabVal::makeBlock(block_value));

        LOG_DEBUG("[SUCCESS] Block {} loaded and ready as '{}'\n",
                   node.getBlockId(), alias);

        // Record usage statistics (only if using database loader)
        if (block_loader_ && metadata.token_count > 0) {
            block_loader_->recordBlockUsage(node.getBlockId(), metadata.token_count);
        }

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to load block {}: {}\n", node.getBlockId(), e.what());
    }
}

// Phase 4.0: Module use statement visitor (Rust-style modules)
// use math_utils
// use data.processor
void Interpreter::visit(ast::ModuleUseStmt& node) {
    const std::string& module_path = node.getModulePath();

    LOG_DEBUG("[MODULE] Processing: use {}\n", module_path);

    // BUGFIX: Check if it's a stdlib module first (like UseStatement does)
    if (stdlib_->hasModule(module_path)) {
        auto module = stdlib_->getModule(module_path);

        // Determine alias
        std::string alias = node.hasAlias() ? node.getAlias() : module_path;

        // Store in imported_modules_ for function calls
        imported_modules_[alias] = module;

        LOG_DEBUG("[MODULE] Loaded stdlib module: {} as {}\n", module_path, alias);

        // Store module marker in environment for member access
        current_env_->define(alias, NaabVal::makeString("__stdlib_module__:" + alias));

        return;
    }

    // Get current file directory for relative module resolution
    std::filesystem::path current_dir = current_file_.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(current_file_).parent_path();

    // Load module (this will parse it if not already loaded)
    modules::NaabModule* module = module_registry_->loadModule(module_path, current_dir);
    if (!module) {
        // Check if dropping the first path component finds the module (sibling import)
        // e.g., "modules.vessels" from within modules/ → try "vessels"
        auto dot_pos = module_path.find('.');
        if (dot_pos != std::string::npos) {
            std::string shortened = module_path.substr(dot_pos + 1);
            modules::NaabModule* alt = module_registry_->loadModule(shortened, current_dir);
            if (alt) {
                std::string parent_dir = module_path.substr(0, dot_pos);
                throw std::runtime_error(
                    fmt::format("Failed to load module: {}\n\n"
                        "  Help: Your script is already inside the '{}' directory.\n"
                        "  Sibling modules don't need the parent directory prefix:\n\n"
                        "    ✗ Wrong: use {}\n"
                        "    ✓ Right: use {}\n",
                        module_path, parent_dir, module_path, shortened)
                );
            }
        }

        // Check reverse: "paxos" not found, but "modules/paxos.naab" exists
        // Try prepending each subdirectory
        if (module_path.find('.') == std::string::npos) {
            // Simple name — check if it exists in a subdirectory
            for (const auto& entry : std::filesystem::directory_iterator(current_dir)) {
                if (entry.is_directory()) {
                    std::string subdir = entry.path().filename().string();
                    std::string qualified = subdir + "." + module_path;
                    modules::NaabModule* alt = module_registry_->loadModule(qualified, current_dir);
                    if (alt) {
                        throw std::runtime_error(
                            fmt::format("Failed to load module: {}\n\n"
                                "  Help: Module '{}' exists in the '{}' subdirectory.\n"
                                "  Include the directory prefix:\n\n"
                                "    ✗ Wrong: use {}\n"
                                "    ✓ Right: use {}\n",
                                module_path, module_path, subdir, module_path, qualified)
                        );
                    }
                }
            }
        }

        // Check common aliases from other languages before giving up
        static const std::unordered_map<std::string, std::string> use_aliases = {
            {"fs", "file"}, {"os", "env"}, {"re", "regex"},
            {"console", "io"}, {"sys", "env"}, {"system", "process"},
            {"random", "math"}, {"datetime", "time"}, {"net", "http"},
        };
        auto alias_it = use_aliases.find(module_path);
        if (alias_it != use_aliases.end()) {
            throw std::runtime_error(
                fmt::format("Failed to load module: {}\n\n"
                    "  Did you mean: use {}\n"
                    "  NAAb's '{}' module provides this functionality.",
                    module_path, alias_it->second, alias_it->second)
            );
        }

        throw std::runtime_error(
            fmt::format("Failed to load module: {}\n"
                       "  Searched in: {}\n"
                       "  See above for detailed error messages",
                       module_path, current_dir.string())
        );
    }

    // Check if module has already been executed
    if (module->isExecuted()) {
        LOG_DEBUG("[MODULE] Module '{}' already executed, reusing\n", module_path);

        // Import module into current namespace
        // Use alias if provided, otherwise use the last part of the module path
        // "data.processor" as dp -> "dp"
        // "data.processor" -> "processor"
        std::string module_name;
        if (node.hasAlias()) {
            module_name = node.getAlias();
        } else {
            module_name = module_path;
            auto last_dot = module_path.find_last_of('.');
            if (last_dot != std::string::npos) {
                module_name = module_path.substr(last_dot + 1);
            }
        }

        // Store module environment for member access (module.function)
        current_env_->define(module_name, NaabVal::makeString("__module__:" + module_path));

        return;
    }

    // Build dependency graph (topological sort with cycle detection)
    std::vector<modules::NaabModule*> execution_order;
    try {
        execution_order = module_registry_->buildDependencyGraph(module);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            fmt::format("Dependency error for module '{}': {}", module_path, e.what())
        );
    }

    // Execute modules in dependency order
    for (modules::NaabModule* dep_module : execution_order) {
        if (dep_module->isExecuted()) {
            continue;  // Skip already executed modules
        }

        LOG_DEBUG("[MODULE] Executing: {}\n", dep_module->getName());

        // Create module environment (child of global)
        auto module_env = std::make_shared<Environment>(global_env_);
        dep_module->setEnvironment(module_env);

        // Save current environment
        auto prev_env = current_env_;
        auto prev_file = current_file_;
        current_env_ = module_env;
        current_file_ = dep_module->getFilePath();

        try {
            // Execute module statements
            auto* program = dep_module->getAST();
            if (program) {
                // ISS-022 FIX: Execute use statements FIRST (including stdlib imports)
                // This allows custom modules to import stdlib modules
                for (const auto& module_use : program->getModuleUses()) {
                    module_use->accept(*this);
                }

                // Execute function declarations
                for (const auto& func : program->getFunctions()) {
                    func->accept(*this);
                }

                // Execute struct declarations
                for (const auto& struct_decl : program->getStructs()) {
                    struct_decl->accept(*this);
                    // Auto-export module structs so they're accessible via module.StructName
                    auto struct_def = runtime::StructRegistry::instance().getStruct(struct_decl->getName());
                    if (struct_def) {
                        current_env_->exported_structs_[struct_decl->getName()] = struct_def;
                    }
                }

                // Execute enum declarations
                for (const auto& enum_decl : program->getEnums()) {
                    enum_decl->accept(*this);
                }

                // Execute export statements (Phase 4.0)
                for (const auto& export_stmt : program->getExports()) {
                    export_stmt->accept(*this);
                }

                // Note: Main block is NOT executed for imported modules
                // Only declarations are processed
            }

            dep_module->markExecuted();
            LOG_DEBUG("[MODULE] Execution complete: {}\n", dep_module->getName());

            // ISS-024 Fix: Store module environment for struct resolution
            loaded_modules_[dep_module->getName()] = module_env;

        } catch (const std::exception& e) {
            // Restore environment before throwing
            current_env_ = prev_env;
            current_file_ = prev_file;
            throw std::runtime_error(
                fmt::format("Error executing module '{}': {}",
                           dep_module->getName(), e.what())
            );
        }

        // Restore environment
        current_env_ = prev_env;
        current_file_ = prev_file;
    }

    // Import the requested module into current namespace
    // Use alias if provided, otherwise use the last part of the module path
    // "data.processor" as dp -> "dp"
    // "data.processor" -> "processor"
    std::string module_name;
    if (node.hasAlias()) {
        module_name = node.getAlias();
    } else {
        module_name = module_path;
        auto last_dot = module_path.find_last_of('.');
        if (last_dot != std::string::npos) {
            module_name = module_path.substr(last_dot + 1);
        }
    }

    // Store module environment reference for member access
    current_env_->define(module_name, NaabVal::makeString("__module__:" + module_path));

    // ISS-024 Fix: Store alias mapping for module-qualified types
    // loaded_modules_ uses module_path as key, so if an alias is used,
    // also store it under the alias for struct resolution
    if (loaded_modules_.count(module_path) && module_name != module_path) {
        loaded_modules_[module_name] = loaded_modules_[module_path];
    }

    LOG_DEBUG("[MODULE] Successfully imported: {} (use as '{}')\n",
               module_path, module_name);
}

// Phase 3.1: Import statement visitor
void Interpreter::visit(ast::ImportStmt& node) {
    if (isVerboseMode()) {
        fmt::print("[VERBOSE] Loading module: {}\n", node.getModulePath());
    }

    // Issue #3: Get current file directory for relative imports
    // Resolve relative to the file being executed, not the working directory
    std::filesystem::path current_dir = getCurrentFileDirectory();

    // Finding E fix: for bare names (no path separator, no .naab extension) check stdlib
    // FIRST before the filesystem resolver. This prevents a local file named "time.naab"
    // from shadowing the trusted stdlib "time" module.
    // A bare name is unambiguously a stdlib name — filesystem imports always have a path
    // component (./time.naab, ../lib/time, /abs/path) or explicit .naab extension.
    {
        const std::string& mp = node.getModulePath();
        bool is_bare = (mp.find('/') == std::string::npos &&
                        mp.find('\\') == std::string::npos &&
                        (mp.size() < 5 || mp.substr(mp.size() - 5) != ".naab"));
        if (is_bare && stdlib_->hasModule(mp)) {
            auto module = stdlib_->getModule(mp);
            std::string alias = mp;
            if (node.isWildcard()) alias = node.getWildcardAlias();
            imported_modules_[alias] = module;
            current_env_->define(alias, NaabVal::makeString("__stdlib_module__:" + alias));
            return;
        }
    }

    // Resolve module path via filesystem
    auto resolved_path = module_resolver_->resolve(node.getModulePath(), current_dir);
    if (!resolved_path) {
        // Fall back to stdlib for non-bare names (e.g. "stdlib/time", "./time")
        std::string bare_name = node.getModulePath();
        // Strip .naab extension if present
        if (bare_name.size() > 5 && bare_name.substr(bare_name.size() - 5) == ".naab") {
            bare_name = bare_name.substr(0, bare_name.size() - 5);
        }
        // Strip path separators to get just the module name
        auto last_slash = bare_name.rfind('/');
        if (last_slash != std::string::npos) {
            bare_name = bare_name.substr(last_slash + 1);
        }

        if (stdlib_->hasModule(bare_name)) {
            // Handle as stdlib module
            auto module = stdlib_->getModule(bare_name);
            std::string alias = bare_name;
            if (node.isWildcard()) {
                alias = node.getWildcardAlias();
            }
            imported_modules_[alias] = module;
            current_env_->define(alias, NaabVal::makeString("__stdlib_module__:" + alias));
            return;
        }

        // Not a stdlib module either - provide helpful error
        std::string error_msg = fmt::format("Module not found: {}\nSearched:\n"
                       "  - Relative to current directory\n"
                       "  - naab_modules/ directories\n"
                       "  - ~/.naab/modules/\n"
                       "  - /usr/local/naab/modules/",
                       node.getModulePath());

        // Check if it's close to a stdlib module name
        static const std::vector<std::string> stdlib_names = {
            "io", "json", "string", "array", "math", "file", "http",
            "time", "regex", "crypto", "csv", "env", "collections",
            "log", "uuid", "validate", "process", "path", "dict", "debug", "bolo"
        };
        for (const auto& stdlib_name : stdlib_names) {
            if (bare_name == stdlib_name) {
                error_msg += fmt::format("\n\n  Did you mean the built-in '{}' module?\n"
                                        "    import {}    // stdlib (no quotes needed)", stdlib_name, stdlib_name);
                break;
            }
        }

        // Check common aliases from other languages
        static const std::unordered_map<std::string, std::string> module_aliases = {
            {"fs", "file"}, {"os", "env"}, {"re", "regex"},
            {"console", "io"}, {"sys", "env"}, {"system", "process"},
            {"random", "math"}, {"datetime", "time"}, {"net", "http"},
        };
        auto alias_it = module_aliases.find(bare_name);
        if (alias_it != module_aliases.end()) {
            error_msg += fmt::format("\n\n  Did you mean: use {}\n"
                "  NAAb's '{}' module provides this functionality.",
                alias_it->second, alias_it->second);
        }

        throw std::runtime_error(error_msg);
    }

    std::string canonical_path = modules::ModuleResolver::canonicalizePath(*resolved_path);

    LOG_DEBUG("[INFO] Importing module: {} ({})\n", node.getModulePath(), canonical_path);

    // Load and execute module (or get from cache)
    auto module_env = loadAndExecuteModule(canonical_path);

    // Handle wildcard import: import * as mod
    if (node.isWildcard()) {
        std::string alias = node.getWildcardAlias();

        // Create a dictionary containing all exports from the module
        std::unordered_map<std::string, NaabVal> module_dict;

        // Get only module's own names (not inherited from global_env_)
        for (const auto& name : module_env->getOwnNames()) {
            module_dict[name] = module_env->get(name);
        }

        // Add exported enum variants (defined in global_env_ by visit(EnumDecl))
        for (const auto& [name, enum_def] : module_env->exported_enums_) {
            for (const auto& [variant_name, value] : enum_def->variants) {
                std::string dotted = enum_def->name + "." + variant_name;
                auto val = NaabVal::makeInt(value);
                module_dict[dotted] = val;
                global_env_->define(alias + "." + dotted, val);
            }
        }

        auto dict_value = NaabVal::makeDict(module_dict);
        current_env_->define(alias, dict_value);

        // Define aliased names for 3-level dot access from own dotted keys
        for (const auto& [key, val] : module_dict) {
            if (key.find('.') != std::string::npos) {
                global_env_->define(alias + "." + key, val);
            }
        }

        // Import exported structs from wildcard imports too
        for (const auto& [name, struct_def] : module_env->exported_structs_) {
            runtime::StructRegistry::instance().registerStruct(struct_def);
        }

        LOG_DEBUG("[SUCCESS] Imported all from {} as '{}'\n", node.getModulePath(), alias);
        return;
    }

    // Handle named imports: import {name1, name2 as alias2}
    for (const auto& item : node.getItems()) {
        std::string import_name = item.name;
        std::string local_name = item.alias.empty() ? item.name : item.alias;

        // Get the imported symbol from module environment
        try {
            auto value = module_env->get(import_name);
            current_env_->define(local_name, value);

            LOG_DEBUG("[SUCCESS] Imported {} as '{}' from {}\n",
                      import_name, local_name, node.getModulePath());
        } catch (const std::exception& e) {
            throw std::runtime_error(
                fmt::format("Import error: '{}' not found in module {}\n  {}",
                           import_name, node.getModulePath(), e.what())
            );
        }
    }

    // Import exported structs (Week 7)
    for (const auto& [name, struct_def] : module_env->exported_structs_) {
        runtime::StructRegistry::instance().registerStruct(struct_def);
        LOG_DEBUG("[SUCCESS] Imported struct: {}\n", name);
    }

    // Import exported enums (Phase 4.1: Module System)
    for (const auto& [name, enum_def] : module_env->exported_enums_) {
        // Register enum for TAG_ENUM name resolution
        uint16_t enum_id = NaabVal::registerEnum(enum_def->name, enum_def->variants);
        // Define all enum variants in global environment as TAG_ENUM
        for (const auto& [variant_name, value] : enum_def->variants) {
            std::string full_name = enum_def->name + "." + variant_name;
            global_env_->define(full_name, NaabVal::makeEnum(value, enum_id));
        }
        LOG_DEBUG("[SUCCESS] Imported enum: {}\n", name);
    }
}

// Phase 3.1: Export statement visitor
void Interpreter::visit(ast::ExportStmt& node) {
    switch (node.getKind()) {
        case ast::ExportStmt::ExportKind::Function: {
            // Export function: execute the function declaration and mark it as exported
            auto* func_decl = node.getFunctionDecl();
            if (func_decl) {
                // EVA-8: GOV-6 scan moved to visit(FunctionDecl&) — all functions scanned
                func_decl->accept(*this);  // This will define the function in current_env_

                // Get the function value we just defined
                auto func_value = current_env_->get(func_decl->getName());

                // Store in module exports
                module_exports_[func_decl->getName()] = func_value;

                LOG_DEBUG("[INFO] Exported function: {}\n", func_decl->getName());
            }
            break;
        }

        case ast::ExportStmt::ExportKind::Variable: {
            // Export variable: execute the variable declaration and mark it as exported
            auto* var_decl = node.getVarDecl();
            if (var_decl) {
                var_decl->accept(*this);  // This will define the variable in current_env_

                // Get the variable value we just defined
                auto var_value = current_env_->get(var_decl->getName());

                // Store in module exports
                module_exports_[var_decl->getName()] = var_value;

                LOG_DEBUG("[INFO] Exported variable: {}\n", var_decl->getName());
            }
            break;
        }

        case ast::ExportStmt::ExportKind::DefaultExpr: {
            // Export default expression
            auto* expr = node.getExpr();
            if (expr) {
                auto value = eval(*expr);

                // Store as "default" export
                module_exports_["default"] = value;
                current_env_->define("default", value);

                LOG_DEBUG("[INFO] Exported default expression\n");
            }
            break;
        }

        case ast::ExportStmt::ExportKind::Struct: {
            // Export struct (Week 7)
            auto* struct_decl = node.getStructDecl();
            if (struct_decl) {
                // Execute struct declaration (registers in StructRegistry)
                struct_decl->accept(*this);

                // Get registered struct definition
                auto struct_def = runtime::StructRegistry::instance().getStruct(
                    struct_decl->getName());

                if (struct_def) {
                    // Store in module's exported structs
                    current_env_->exported_structs_[struct_decl->getName()] = struct_def;

                    LOG_DEBUG("[SUCCESS] Exported struct: {}\n", struct_decl->getName());
                } else {
                    fmt::print("[ERROR] Failed to export struct: {}\n", struct_decl->getName());
                }
            }
            break;
        }

        case ast::ExportStmt::ExportKind::Enum: {
            // Export enum (Phase 4.1: Module System)
            auto* enum_decl = node.getEnumDecl();
            if (enum_decl) {
                // Execute enum declaration (defines enum variants in environment)
                enum_decl->accept(*this);

                // Create EnumDef from the declaration
                std::vector<std::pair<std::string, int>> variants;
                int next_value = 0;
                for (const auto& variant : enum_decl->getVariants()) {
                    int variant_value = variant.value.value_or(next_value);
                    variants.emplace_back(variant.name, variant_value);
                    next_value = variant_value + 1;
                }

                auto enum_def = std::make_shared<interpreter::EnumDef>(
                    enum_decl->getName(), variants);

                // Store in module's exported enums
                current_env_->exported_enums_[enum_decl->getName()] = enum_def;

                LOG_DEBUG("[SUCCESS] Exported enum: {}\n", enum_decl->getName());
            }
            break;
        }
    }
}

// Phase 3.1: Load and execute a module
std::shared_ptr<Environment> Interpreter::loadAndExecuteModule(const std::string& module_path) {
    // Check if module is already loaded
    auto it = loaded_modules_.find(module_path);
    if (it != loaded_modules_.end()) {
        LOG_DEBUG("[INFO] Module already loaded (using cache): {}\n", module_path);
        return it->second;
    }

    LOG_DEBUG("[INFO] Loading module from: {}\n", module_path);

    // Load module using ModuleResolver
    auto module = module_resolver_->loadModule(std::filesystem::path(module_path));

    if (!module || !module->ast) {
        throw std::runtime_error(
            fmt::format("Failed to load module: {}", module_path)
        );
    }

    // Issue #3: Push file context for this module
    pushFileContext(module_path);

    // Create a new environment for the module
    auto module_env = std::make_shared<Environment>(global_env_);

    // Save current environment
    auto saved_env = current_env_;
    auto saved_exports = module_exports_;

    // BUG-O: Save taint state — module should not pollute caller's taint set
    std::unordered_set<std::string> saved_taint;
    if (governance_ && governance_->isActive()) {
        saved_taint = governance_->saveTaintState();
    }

    // Switch to module environment
    current_env_ = module_env;
    module_exports_.clear();

    try {
        // Execute module AST (skip main blocks during import)
        ++module_loading_depth_;
        module->ast->accept(*this);
        --module_loading_depth_;

        // Copy module exports to module environment
        for (const auto& [name, value] : module_exports_) {
            module_env->define(name, value);
        }

        // Cache the module environment
        loaded_modules_[module_path] = module_env;

        LOG_DEBUG("[SUCCESS] Module loaded successfully: {}\n", module_path);
        LOG_DEBUG("          Exported {} symbols\n", module_exports_.size());

    } catch (const std::exception& e) {
        --module_loading_depth_;
        // Issue #3: Pop file context on error
        popFileContext();

        // Restore environment on error
        current_env_ = saved_env;
        module_exports_ = saved_exports;
        // BUG-O: Restore taint state on error
        if (governance_ && governance_->isActive()) {
            governance_->restoreTaintState(saved_taint);
        }
        throw std::runtime_error(
            fmt::format("Error executing module {}: {}", module_path, e.what())
        );
    }

    // Issue #3: Pop file context on success
    popFileContext();

    // Restore previous environment
    current_env_ = saved_env;
    module_exports_ = saved_exports;

    // BUG-O: Restore taint state after module loading
    if (governance_ && governance_->isActive()) {
        governance_->restoreTaintState(saved_taint);
    }

    return module_env;
}

// Governance Plugin API (#23): Load a .naab plugin file
// Exported functions go into global_env_ so governance engine can call them by name.
// Uses module_loading_depth_ to skip main{} and governance checks during loading.
void Interpreter::loadPluginFile(const std::string& path) {
    // Prevent double-loading
    auto canonical = std::filesystem::canonical(path).string();
    if (loaded_plugin_files_.count(canonical)) {
        fprintf(stderr, "[governance] Plugin file already loaded: %s\n", path.c_str());
        return;
    }

    LOG_DEBUG("[INFO] Loading governance plugin: {}\n", path);

    // Load and parse the .naab file
    auto module = module_resolver_->loadModule(std::filesystem::path(path));
    if (!module || !module->ast) {
        fprintf(stderr, "[governance] Warning: Failed to parse plugin file: %s\n", path.c_str());
        return;
    }

    // Push file context
    pushFileContext(path);

    // Save current state
    auto saved_env = current_env_;
    auto saved_exports = module_exports_;

    // Save taint state — plugin loading should not pollute taint
    std::unordered_set<std::string> saved_taint;
    if (governance_ && governance_->isActive()) {
        saved_taint = governance_->saveTaintState();
    }

    // Create a temporary environment for plugin execution
    auto plugin_env = std::make_shared<Environment>(global_env_);
    current_env_ = plugin_env;
    module_exports_.clear();

    try {
        // Execute plugin AST (skip main blocks, skip governance on function bodies)
        ++module_loading_depth_;
        module->ast->accept(*this);
        --module_loading_depth_;

        // Copy exported functions to global_env_ (so governance engine can find them)
        int exported_count = 0;
        for (const auto& [name, value] : module_exports_) {
            // Collision guard: don't overwrite existing user functions
            try {
                auto existing = global_env_->get(name);
                if (existing) {
                    fprintf(stderr, "[governance] Warning: Plugin function '%s' collides with existing name, skipping\n",
                            name.c_str());
                    continue;
                }
            } catch (...) {
                // Name doesn't exist — safe to define
            }
            global_env_->define(name, value);
            ++exported_count;
        }

        loaded_plugin_files_.insert(canonical);
        LOG_DEBUG("[SUCCESS] Governance plugin loaded: {} ({} functions exported)\n", path, exported_count);

    } catch (const std::exception& e) {
        --module_loading_depth_;
        popFileContext();
        current_env_ = saved_env;
        module_exports_ = saved_exports;
        if (governance_ && governance_->isActive()) {
            governance_->restoreTaintState(saved_taint);
        }
        fprintf(stderr, "[governance] Warning: Error loading plugin %s: %s\n", path.c_str(), e.what());
        return;
    }

    // Restore state
    popFileContext();
    current_env_ = saved_env;
    module_exports_ = saved_exports;
    if (governance_ && governance_->isActive()) {
        governance_->restoreTaintState(saved_taint);
    }
}

} // namespace interpreter
} // namespace naab
