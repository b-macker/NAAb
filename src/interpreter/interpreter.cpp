// NAAb Interpreter - Direct AST execution
// Core interpreter implementation

#include "naab/interpreter.h"
#include "naab/error_helpers.h"
#include "naab/language_registry.h"
#include "naab/block_registry.h"
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#include "naab/stdlib_new_modules.h"  // For ArrayModule type
#include "naab/struct_registry.h"
#include <fmt/core.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <filesystem>  // Phase 3.1: For module path resolution

// Python embedding support
#ifdef __has_include
#  if __has_include(<Python.h>)
#    define NAAB_HAS_PYTHON 1
#    include <Python.h>
#  endif
#endif

namespace naab {
namespace interpreter {

// ============================================================================
// Phase 4.1: Exception Handling
// ============================================================================

// ============================================================================
// Error Handling Implementation - Phase 4.1
// ============================================================================

std::string StackFrame::toString() const {
    std::stringstream ss;
    ss << "  at " << function_name;
    if (!file_path.empty()) {
        ss << " (" << file_path << ":" << line_number;
        if (column_number > 0) {
            ss << ":" << column_number;
        }
        ss << ")";
    } else {
        ss << " (line " << line_number << ")";
    }
    return ss.str();
}

std::string NaabError::formatError() const {
    std::stringstream ss;
    ss << errorTypeToString(error_type_) << ": " << message_ << "\n";

    if (!stack_trace_.empty()) {
        ss << "Stack trace:\n";
        for (const auto& frame : stack_trace_) {
            ss << frame.toString() << "\n";
        }
    }

    return ss.str();
}

NaabError::NaabError(std::shared_ptr<Value> value)
    : std::runtime_error("NaabError"), error_type_(ErrorType::GENERIC) {
    value_ = value;
    if (value) {
        message_ = value->toString();
    }
}

std::string NaabError::errorTypeToString(ErrorType type) {
    switch (type) {
        case ErrorType::GENERIC:         return "Error";
        case ErrorType::TYPE_ERROR:      return "TypeError";
        case ErrorType::RUNTIME_ERROR:   return "RuntimeError";
        case ErrorType::REFERENCE_ERROR: return "ReferenceError";
        case ErrorType::SYNTAX_ERROR:    return "SyntaxError";
        case ErrorType::IMPORT_ERROR:    return "ImportError";
        case ErrorType::BLOCK_ERROR:     return "BlockError";
        case ErrorType::ASSERTION_ERROR: return "AssertionError";
        default:                         return "UnknownError";
    }
}

// Backward compatibility alias
using NaabException = NaabError;

// ============================================================================
// Value Implementation
// ============================================================================

std::string Value::toString() const {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return "null";
        } else if constexpr (std::is_same_v<T, int>) {
            return std::to_string(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            return std::to_string(arg);
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return arg;
        } else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) {
            std::string result = "[";
            for (size_t i = 0; i < arg.size(); i++) {
                if (i > 0) result += ", ";
                result += arg[i]->toString();
            }
            result += "]";
            return result;
        } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) {
            std::string result = "{";
            size_t i = 0;
            for (const auto& [k, v] : arg) {
                if (i++ > 0) result += ", ";
                result += "\"" + k + "\": " + v->toString();
            }
            result += "}";
            return result;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<BlockValue>>) {
            return "<Block:" + arg->metadata.block_id + " (" + arg->metadata.language + ")>";
        } else if constexpr (std::is_same_v<T, std::shared_ptr<FunctionValue>>) {
            return "<Function:" + arg->name + "(" + std::to_string(arg->params.size()) + " params)>";
        } else if constexpr (std::is_same_v<T, std::shared_ptr<PythonObjectValue>>) {
            return arg->repr;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<StructValue>>) {
            std::string result = arg->type_name + " { ";
            for (size_t i = 0; i < arg->definition->fields.size(); ++i) {
                if (i > 0) result += ", ";
                result += arg->definition->fields[i].name + ": ";
                result += arg->field_values[i]->toString();
            }
            result += " }";
            return result;
        }
        return "unknown";
    }, data);
}

bool Value::toBool() const {
    return std::visit([](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return false;
        } else if constexpr (std::is_same_v<T, int>) {
            return arg != 0;
        } else if constexpr (std::is_same_v<T, double>) {
            return arg != 0.0;
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return !arg.empty();
        } else {
            return true;
        }
    }, data);
}

int Value::toInt() const {
    return std::visit([](auto&& arg) -> int {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            return arg;
        } else if constexpr (std::is_same_v<T, double>) {
            return static_cast<int>(arg);
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg ? 1 : 0;
        }
        return 0;
    }, data);
}

double Value::toFloat() const {
    return std::visit([](auto&& arg) -> double {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            return static_cast<double>(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            return arg;
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg ? 1.0 : 0.0;
        }
        return 0.0;
    }, data);
}

// ============================================================================
// Environment Implementation
// ============================================================================

void Environment::define(const std::string& name, std::shared_ptr<Value> value) {
    values_[name] = value;
}

std::shared_ptr<Value> Environment::get(const std::string& name) {
    auto it = values_.find(name);
    if (it != values_.end()) {
        return it->second;
    }
    if (parent_) {
        return parent_->get(name);
    }

    // Generate helpful error message with suggestions
    std::string error_msg = "Undefined variable: " + name;
    auto all_names = getAllNames();
    auto suggestion = error::suggestForUndefinedVariable(name, all_names);
    if (!suggestion.empty()) {
        error_msg += "\n  " + suggestion;
    }
    throw std::runtime_error(error_msg);
}

void Environment::set(const std::string& name, std::shared_ptr<Value> value) {
    auto it = values_.find(name);
    if (it != values_.end()) {
        it->second = value;
        return;
    }
    if (parent_) {
        parent_->set(name, value);
        return;
    }

    // Generate helpful error message with suggestions
    std::string error_msg = "Undefined variable: " + name;
    auto all_names = getAllNames();
    auto suggestion = error::suggestForUndefinedVariable(name, all_names);
    if (!suggestion.empty()) {
        error_msg += "\n  " + suggestion;
    }
    throw std::runtime_error(error_msg);
}

bool Environment::has(const std::string& name) const {
    if (values_.find(name) != values_.end()) {
        return true;
    }
    if (parent_) {
        return parent_->has(name);
    }
    return false;
}

std::vector<std::string> Environment::getAllNames() const {
    std::vector<std::string> names;

    // Get names from current scope
    for (const auto& [name, _] : values_) {
        names.push_back(name);
    }

    // Get names from parent scopes
    if (parent_) {
        auto parent_names = parent_->getAllNames();
        names.insert(names.end(), parent_names.begin(), parent_names.end());
    }

    return names;
}

// ============================================================================
// Interpreter Implementation
// ============================================================================

Interpreter::Interpreter()
    : global_env_(std::make_shared<Environment>()),
      current_env_(global_env_),
      result_(std::make_shared<Value>()),
      returning_(false),
      breaking_(false),
      continuing_(false),
      last_executed_block_id_("") {  // Phase 4.4: Initialize block pair tracking
    // Phase 8: Skip BlockRegistry initialization for faster startup
    // It will be lazily initialized only when needed (in UseStatement)
    // This avoids loading 24,488 blocks unnecessarily for simple programs

    // Skip BlockLoader for now - using lazy BlockRegistry instead
    // This avoids loading 24,482 database blocks upfront
    block_loader_ = nullptr;
    fmt::print("[INFO] Using lazy BlockRegistry (BlockLoader disabled for faster startup)\n");

    // Initialize Python interpreter
#ifdef NAAB_HAS_PYTHON
    if (!Py_IsInitialized()) {
        Py_Initialize();
        fmt::print("[INFO] Python interpreter initialized\n");
    }
#else
    fmt::print("[WARN] Python support not available (Python blocks disabled)\n");
#endif

    // Initialize C++ executor
    cpp_executor_ = std::make_unique<runtime::CppExecutor>();
    fmt::print("[INFO] C++ executor initialized\n");

    // Initialize standard library
    stdlib_ = std::make_unique<stdlib::StdLib>();
    fmt::print("[INFO] Standard library initialized: {} modules available\n",
               stdlib_->listModules().size());

    // Phase 3.1: Initialize module resolver
    module_resolver_ = std::make_unique<modules::ModuleResolver>();
    fmt::print("[INFO] Module resolver initialized\n");

    // Set up function evaluator callback for array higher-order functions
    // Note: Do this AFTER all other initialization to avoid potential issues
    auto array_module = stdlib_->getModule("array");
    if (array_module) {
        auto* array_mod = dynamic_cast<stdlib::ArrayModule*>(array_module.get());
        if (array_mod) {
            // Create callback that uses this interpreter's callFunction method
            array_mod->setFunctionEvaluator(
                [this](std::shared_ptr<Value> fn, const std::vector<std::shared_ptr<Value>>& args) {
                    return this->callFunction(fn, args);
                }
            );
            fmt::print("[INFO] Array module configured with function evaluator\n");
        } else {
            fmt::print("[WARN] Failed to cast array module for function evaluator setup\n");
        }
    } else {
        fmt::print("[WARN] Array module not found for function evaluator setup\n");
    }

    defineBuiltins();
}

void Interpreter::defineBuiltins() {
    // Built-in functions are handled specially in CallExpr
}

// ============================================================================
// Debugger Support
// ============================================================================

void Interpreter::setDebugger(std::shared_ptr<debugger::Debugger> debugger) {
    debugger_ = debugger;
}

// ============================================================================
// Execution
// ============================================================================

void Interpreter::execute(ast::Program& program) {
    program.accept(*this);
}

std::shared_ptr<Value> Interpreter::eval(ast::Expr& expr) {
    expr.accept(*this);
    return result_;
}

// Call a function value with arguments (for higher-order functions like map/filter/reduce)
std::shared_ptr<Value> Interpreter::callFunction(std::shared_ptr<Value> fn,
                                                  const std::vector<std::shared_ptr<Value>>& args) {
    // Check if it's a function value
    auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&fn->data);
    if (!func_ptr) {
        throw std::runtime_error("callFunction requires a function value");
    }
    auto& func = *func_ptr;

    // Check parameter count
    size_t min_args = 0;
    for (size_t i = 0; i < func->params.size(); i++) {
        if (!func->defaults[i]) {
            min_args = i + 1;
        }
    }

    if (args.size() < min_args || args.size() > func->params.size()) {
        throw std::runtime_error(fmt::format(
            "Function {} expects {}-{} args, got {}",
            func->name, min_args, func->params.size(), args.size()));
    }

    // Create new environment for function execution
    auto func_env = std::make_shared<Environment>(global_env_);

    // Bind provided arguments
    for (size_t i = 0; i < args.size(); i++) {
        func_env->define(func->params[i], args[i]);
    }

    // Bind default values for missing arguments
    for (size_t i = args.size(); i < func->params.size(); i++) {
        if (func->defaults[i]) {
            auto saved_env = current_env_;
            current_env_ = func_env;
            func->defaults[i]->accept(*this);
            auto default_val = result_;
            current_env_ = saved_env;
            func_env->define(func->params[i], default_val);
        }
    }

    // Save current environment and execute function body
    auto saved_env = current_env_;
    auto saved_returning = returning_;
    current_env_ = func_env;
    returning_ = false;

    pushStackFrame(func->name, 0);

    try {
        executeStmt(*func->body);
    } catch (...) {
        popStackFrame();
        current_env_ = saved_env;
        returning_ = saved_returning;
        throw;
    }

    popStackFrame();

    // Restore environment
    current_env_ = saved_env;
    auto return_value = result_;
    returning_ = saved_returning;

    return return_value;
}

// ============================================================================
// Phase 4.1: Stack Trace Helpers
// ============================================================================

void Interpreter::pushStackFrame(const std::string& function_name, int line) {
    call_stack_.emplace_back(function_name, current_file_, line);
}

void Interpreter::popStackFrame() {
    if (!call_stack_.empty()) {
        call_stack_.pop_back();
    }
}

NaabError Interpreter::createError(const std::string& message, ErrorType type) {
    NaabError error(message, type, call_stack_);
    return error;
}

void Interpreter::executeStmt(ast::Stmt& stmt) {
    // Update current environment for debugger variable inspection
    if (debugger_ && debugger_->isActive()) {
        debugger_->setCurrentEnvironment(current_env_);
    }

    stmt.accept(*this);
}

// ============================================================================
// Visitor Implementations
// ============================================================================

void Interpreter::visit(ast::Program& node) {
    // Phase 3.1: Process module imports first
    for (auto& import_stmt : node.getModuleImports()) {
        import_stmt->accept(*this);
    }

    // Store use statements (for now, just note them)
    for (auto& use_stmt : node.getImports()) {
        use_stmt->accept(*this);
    }

    // Process struct declarations
    for (auto& struct_decl : node.getStructs()) {
        struct_decl->accept(*this);
    }

    // Store function definitions
    fmt::print("[DEBUG] Processing {} standalone functions\n", node.getFunctions().size());
    for (auto& func : node.getFunctions()) {
        func->accept(*this);
    }

    // Phase 3.1: Process exports
    fmt::print("[DEBUG] Processing {} export statements\n", node.getExports().size());
    for (auto& export_stmt : node.getExports()) {
        export_stmt->accept(*this);
    }

    // Execute main block if present
    if (node.getMainBlock()) {
        node.getMainBlock()->accept(*this);
    }
}

void Interpreter::visit(ast::UseStatement& node) {
    std::string module_name = node.getBlockId();
    std::string alias = node.getAlias().empty() ? module_name : node.getAlias();

    // Check if it's a stdlib module first
    if (stdlib_->hasModule(module_name)) {
        auto module = stdlib_->getModule(module_name);
        imported_modules_[alias] = module;

        fmt::print("[INFO] Imported stdlib module: {} as {}\n", module_name, alias);

        // Store module reference in environment (we'll handle calls specially)
        // For now, create a special marker value
        auto module_marker = std::make_shared<Value>(
            std::string("__stdlib_module__:" + alias)
        );
        current_env_->define(alias, module_marker);
        return;
    }

    // Otherwise, try to load as block
    // Phase 8: Try BlockRegistry first (filesystem-based), then BlockLoader (database-based)
    auto& block_registry = runtime::BlockRegistry::instance();

    // Lazy initialization: only load block registry when actually needed
    if (!block_registry.isInitialized()) {
        std::string home_dir = std::getenv("HOME") ? std::getenv("HOME") : ".";
        std::string blocks_path = home_dir + "/.naab/language/blocks/library/";
        fmt::print("[INFO] Lazy-loading BlockRegistry from: {}\n", blocks_path);
        block_registry.initialize(blocks_path);
    }

    auto metadata_opt = block_registry.getBlock(node.getBlockId());

    runtime::BlockMetadata metadata;
    std::string code;

    if (metadata_opt.has_value()) {
        // Found in BlockRegistry (filesystem)
        metadata = *metadata_opt;
        code = block_registry.getBlockSource(node.getBlockId());

        fmt::print("[INFO] Loaded block {} from filesystem as {} ({})\n",
                   node.getBlockId(), alias, metadata.language);
        fmt::print("       Source: {}\n", metadata.file_path);
        fmt::print("       Code size: {} bytes\n", code.size());

    } else if (block_loader_) {
        // Fall back to BlockLoader (database)
        try {
            metadata = block_loader_->getBlock(node.getBlockId());
            code = block_loader_->loadBlockCode(node.getBlockId());

            fmt::print("[INFO] Loaded block {} from database as {} ({}, {} tokens)\n",
                       node.getBlockId(), alias, metadata.language, metadata.token_count);
            fmt::print("       Source: {}\n", metadata.file_path);
            fmt::print("       Code size: {} bytes\n", code.size());
        } catch (const std::exception& e) {
            fmt::print("[ERROR] Failed to load block {}: {}\n", node.getBlockId(), e.what());
            return;
        }
    } else {
        fmt::print("[ERROR] Block not found: {}\n", node.getBlockId());
        fmt::print("[ERROR] Checked BlockRegistry ({} blocks) and BlockLoader (unavailable)\n",
                   block_registry.blockCount());
        return;
    }

    try {
        // Store in loaded blocks map
        loaded_blocks_[alias] = metadata;

        // Phase 7: Create appropriate executor for this block
        std::shared_ptr<BlockValue> block_value;

        if (metadata.language == "cpp" || metadata.language == "c++") {
            // C++ blocks: Create dedicated executor instance per block
            // Each C++ block compiles to a separate .so file
            fmt::print("[INFO] Creating dedicated C++ executor for block...\n");
            auto cpp_exec = std::make_unique<runtime::CppExecutorAdapter>();

            if (!cpp_exec->execute(code)) {
                fmt::print("[ERROR] Failed to compile/execute C++ block code\n");
                return;
            }

            block_value = std::make_shared<BlockValue>(metadata, code, std::move(cpp_exec));

        } else {
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

            fmt::print("[INFO] Executing block with shared {} executor...\n", metadata.language);
            if (!executor->execute(code)) {
                fmt::print("[ERROR] Failed to execute block code\n");
                return;
            }

            block_value = std::make_shared<BlockValue>(metadata, code, executor);
        }

        // Store BlockValue in environment
        auto value = std::make_shared<Value>(block_value);
        current_env_->define(alias, value);

        fmt::print("[SUCCESS] Block {} loaded and ready as '{}'\n",
                   node.getBlockId(), alias);

        // Record usage statistics (only if using database loader)
        if (block_loader_ && metadata.token_count > 0) {
            block_loader_->recordBlockUsage(node.getBlockId(), metadata.token_count);
        }

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to load block {}: {}\n", node.getBlockId(), e.what());
    }
}

// Phase 3.1: Import statement visitor
void Interpreter::visit(ast::ImportStmt& node) {
    if (isVerboseMode()) {
        fmt::print("[VERBOSE] Loading module: {}\n", node.getModulePath());
    }

    // Get current file directory (for relative imports)
    // For now, we'll use the current working directory
    std::filesystem::path current_dir = std::filesystem::current_path();

    // Resolve module path
    auto resolved_path = module_resolver_->resolve(node.getModulePath(), current_dir);
    if (!resolved_path) {
        throw std::runtime_error(
            fmt::format("Module not found: {}\nSearched:\n"
                       "  - Relative to current directory\n"
                       "  - naab_modules/ directories\n"
                       "  - ~/.naab/modules/\n"
                       "  - /usr/local/naab/modules/",
                       node.getModulePath())
        );
    }

    std::string canonical_path = modules::ModuleResolver::canonicalizePath(*resolved_path);

    fmt::print("[INFO] Importing module: {} ({})\n", node.getModulePath(), canonical_path);

    // Load and execute module (or get from cache)
    auto module_env = loadAndExecuteModule(canonical_path);

    // Handle wildcard import: import * as mod
    if (node.isWildcard()) {
        std::string alias = node.getWildcardAlias();

        // Create a dictionary containing all exports from the module
        std::unordered_map<std::string, std::shared_ptr<Value>> module_dict;

        // Get all names from module environment
        for (const auto& name : module_env->getAllNames()) {
            module_dict[name] = module_env->get(name);
        }

        auto dict_value = std::make_shared<Value>(module_dict);
        current_env_->define(alias, dict_value);

        fmt::print("[SUCCESS] Imported all from {} as '{}'\n", node.getModulePath(), alias);
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

            fmt::print("[SUCCESS] Imported {} as '{}' from {}\n",
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
        fmt::print("[SUCCESS] Imported struct: {}\n", name);
    }
}

// Phase 3.1: Export statement visitor
void Interpreter::visit(ast::ExportStmt& node) {
    switch (node.getKind()) {
        case ast::ExportStmt::ExportKind::Function: {
            // Export function: execute the function declaration and mark it as exported
            auto* func_decl = node.getFunctionDecl();
            if (func_decl) {
                func_decl->accept(*this);  // This will define the function in current_env_

                // Get the function value we just defined
                auto func_value = current_env_->get(func_decl->getName());

                // Store in module exports
                module_exports_[func_decl->getName()] = func_value;

                fmt::print("[INFO] Exported function: {}\n", func_decl->getName());
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

                fmt::print("[INFO] Exported variable: {}\n", var_decl->getName());
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

                fmt::print("[INFO] Exported default expression\n");
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

                    fmt::print("[SUCCESS] Exported struct: {}\n", struct_decl->getName());
                } else {
                    fmt::print("[ERROR] Failed to export struct: {}\n", struct_decl->getName());
                }
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
        fmt::print("[INFO] Module already loaded (using cache): {}\n", module_path);
        return it->second;
    }

    fmt::print("[INFO] Loading module from: {}\n", module_path);

    // Load module using ModuleResolver
    auto module = module_resolver_->loadModule(std::filesystem::path(module_path));

    if (!module || !module->ast) {
        throw std::runtime_error(
            fmt::format("Failed to load module: {}", module_path)
        );
    }

    // Create a new environment for the module
    auto module_env = std::make_shared<Environment>(global_env_);

    // Save current environment
    auto saved_env = current_env_;
    auto saved_exports = module_exports_;

    // Switch to module environment
    current_env_ = module_env;
    module_exports_.clear();

    try {
        // Execute module AST
        module->ast->accept(*this);

        // Copy module exports to module environment
        for (const auto& [name, value] : module_exports_) {
            module_env->define(name, value);
        }

        // Cache the module environment
        loaded_modules_[module_path] = module_env;

        fmt::print("[SUCCESS] Module loaded successfully: {}\n", module_path);
        fmt::print("          Exported {} symbols\n", module_exports_.size());

    } catch (const std::exception& e) {
        // Restore environment on error
        current_env_ = saved_env;
        module_exports_ = saved_exports;
        throw std::runtime_error(
            fmt::format("Error executing module {}: {}", module_path, e.what())
        );
    }

    // Restore previous environment
    current_env_ = saved_env;
    module_exports_ = saved_exports;

    return module_env;
}

void Interpreter::visit(ast::FunctionDecl& node) {
    // Extract parameter names and default values
    std::vector<std::string> param_names;
    std::vector<ast::Expr*> param_defaults;

    for (const auto& param : node.getParams()) {
        param_names.push_back(param.name);

        // Store raw pointer to default value expression if present
        if (param.default_value) {
            param_defaults.push_back(param.default_value->get());
        } else {
            param_defaults.push_back(nullptr);
        }
    }

    // Store function body as CompoundStmt
    auto* body = dynamic_cast<ast::CompoundStmt*>(node.getBody());
    if (!body) {
        fmt::print("[ERROR] Function body must be a compound statement\n");
        return;
    }

    // Create FunctionValue with defaults
    auto func_value = std::make_shared<FunctionValue>(
        node.getName(),
        param_names,
        std::move(param_defaults),
        std::shared_ptr<ast::CompoundStmt>(body, [](ast::CompoundStmt*){})  // Non-owning shared_ptr
    );

    // Store in environment
    auto value = std::make_shared<Value>(func_value);
    global_env_->define(node.getName(), value);

    fmt::print("[INFO] Defined function: {}({} params)\n",
               node.getName(), param_names.size());
}

void Interpreter::visit(ast::StructDecl& node) {
    explain("Defining struct '" + node.getName() + "' with " +
            std::to_string(node.getFields().size()) + " fields");

    auto struct_def = std::make_shared<StructDef>();
    struct_def->name = node.getName();

    size_t field_idx = 0;
    for (const auto& field : node.getFields()) {
        // Copy field metadata without the default_value expr (not needed at runtime)
        ast::StructField runtime_field{field.name, field.type, std::nullopt};
        struct_def->fields.push_back(std::move(runtime_field));
        struct_def->field_index[field.name] = field_idx++;
    }

    // Validate (detect cycles)
    std::set<std::string> visiting;
    runtime::StructRegistry::instance().validateStructDef(*struct_def, visiting);

    // Register
    runtime::StructRegistry::instance().registerStruct(struct_def);

    fmt::print("[INFO] Defined struct: {}\n", node.getName());

    if (isVerboseMode()) {
        fmt::print("[VERBOSE] Registered struct '{}' with {} fields\n",
                   node.getName(), node.getFields().size());
    }

    // Struct declarations don't produce values
    result_ = std::make_shared<Value>();
}

void Interpreter::visit(ast::MainBlock& node) {
    node.getBody()->accept(*this);
}

void Interpreter::visit(ast::CompoundStmt& node) {
    // Create new scope
    auto prev_env = current_env_;
    current_env_ = std::make_shared<Environment>(current_env_);

    for (auto& stmt : node.getStatements()) {
        stmt->accept(*this);
        if (returning_ || breaking_ || continuing_) break;
    }

    // Restore scope
    current_env_ = prev_env;
}

void Interpreter::visit(ast::ExprStmt& node) {
    eval(*node.getExpr());
}

void Interpreter::visit(ast::ReturnStmt& node) {
    if (node.getExpr()) {
        result_ = eval(*node.getExpr());
    } else {
        result_ = std::make_shared<Value>();
    }
    returning_ = true;
}

void Interpreter::visit(ast::IfStmt& node) {
    auto condition = eval(*node.getCondition());
    if (condition->toBool()) {
        node.getThenBranch()->accept(*this);
    } else if (node.getElseBranch()) {
        node.getElseBranch()->accept(*this);
    }
}

void Interpreter::visit(ast::ForStmt& node) {
    auto iterable = eval(*node.getIter());

    // For now, simple range-based iteration
    if (auto* list = std::get_if<std::vector<std::shared_ptr<Value>>>(&iterable->data)) {
        for (auto& item : *list) {
            current_env_->define(node.getVar(), item);
            node.getBody()->accept(*this);
            if (returning_) break;
            if (breaking_) {
                breaking_ = false;  // Reset flag after breaking
                break;
            }
            if (continuing_) {
                continuing_ = false;  // Reset flag and continue to next iteration
                continue;
            }
        }
    }
}

void Interpreter::visit(ast::WhileStmt& node) {
    while (true) {
        auto condition = eval(*node.getCondition());
        if (!condition->toBool()) break;

        node.getBody()->accept(*this);
        if (returning_) break;
        if (breaking_) {
            breaking_ = false;  // Reset flag after breaking
            break;
        }
        if (continuing_) {
            continuing_ = false;  // Reset flag and continue to next iteration
            continue;
        }
    }
}

void Interpreter::visit(ast::BreakStmt& node) {
    breaking_ = true;
}

void Interpreter::visit(ast::ContinueStmt& node) {
    continuing_ = true;
}

void Interpreter::visit(ast::VarDeclStmt& node) {
    explain("Declaring variable '" + node.getName() + "'");

    std::shared_ptr<Value> value;
    if (node.getInit()) {
        value = eval(*node.getInit());
    } else {
        value = std::make_shared<Value>();
    }
    current_env_->define(node.getName(), value);
}

// Phase 4.1: Exception handling
void Interpreter::visit(ast::TryStmt& node) {
    try {
        // Execute try block
        node.getTryBody()->accept(*this);
    } catch (NaabError& e) {
        // Phase 4.1: Error propagation - exception caught

        // Execute catch block with error bound to catch variable
        auto catch_env = std::make_shared<Environment>(current_env_);
        auto prev_env = current_env_;
        current_env_ = catch_env;

        // Bind the error value to the catch variable
        auto* catch_clause = node.getCatchClause();
        current_env_->define(catch_clause->error_name, e.getValue());

        try {
            // Execute catch body - successfully handled if no exception
            catch_clause->body->accept(*this);
        } catch (NaabError&) {
            // Exception thrown from catch block - propagate it
            current_env_ = prev_env;
            throw;
        } catch (const std::exception& std_error) {
            // Convert std::exception to NaabError
            current_env_ = prev_env;
            throw createError(std_error.what(), ErrorType::RUNTIME_ERROR);
        }

        current_env_ = prev_env;
    } catch (const std::exception& std_error) {
        // Convert any other std::exception to NaabError
        throw createError(std_error.what(), ErrorType::RUNTIME_ERROR);
    }

    // Execute finally block if present (always runs)
    if (node.hasFinally()) {
        try {
            node.getFinallyBody()->accept(*this);
        } catch (...) {
            // Finally block threw - propagate that exception instead
            throw;
        }
    }

    // If break/continue/return was triggered, they are handled by the flags
    // and will be checked by the enclosing loop/function
}

void Interpreter::visit(ast::ThrowStmt& node) {
    auto value = eval(*node.getExpr());
    throw NaabException(value);
}

void Interpreter::visit(ast::BinaryExpr& node) {
    // Handle short-circuit operators BEFORE evaluating right side
    if (node.getOp() == ast::BinaryOp::And) {
        auto left = eval(*node.getLeft());
        if (!left->toBool()) {
            // Short-circuit: left is false, so result is false without evaluating right
            result_ = std::make_shared<Value>(false);
            return;
        }
        // Left is true, now evaluate right
        auto right = eval(*node.getRight());
        result_ = std::make_shared<Value>(right->toBool());
        return;
    }

    if (node.getOp() == ast::BinaryOp::Or) {
        auto left = eval(*node.getLeft());
        if (left->toBool()) {
            // Short-circuit: left is true, so result is true without evaluating right
            result_ = std::make_shared<Value>(true);
            return;
        }
        // Left is false, now evaluate right
        auto right = eval(*node.getRight());
        result_ = std::make_shared<Value>(right->toBool());
        return;
    }

    // For all other operators, evaluate both sides
    auto left = eval(*node.getLeft());
    auto right = eval(*node.getRight());

    switch (node.getOp()) {
        case ast::BinaryOp::Add:
            // List concatenation
            if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(left->data) &&
                std::holds_alternative<std::vector<std::shared_ptr<Value>>>(right->data)) {
                auto& left_vec = std::get<std::vector<std::shared_ptr<Value>>>(left->data);
                auto& right_vec = std::get<std::vector<std::shared_ptr<Value>>>(right->data);

                std::vector<std::shared_ptr<Value>> combined;
                combined.reserve(left_vec.size() + right_vec.size());
                combined.insert(combined.end(), left_vec.begin(), left_vec.end());
                combined.insert(combined.end(), right_vec.begin(), right_vec.end());

                result_ = std::make_shared<Value>(combined);
            }
            // String concatenation or numeric addition
            else if (std::holds_alternative<std::string>(left->data) ||
                std::holds_alternative<std::string>(right->data)) {
                result_ = std::make_shared<Value>(left->toString() + right->toString());
            } else if (std::holds_alternative<double>(left->data) ||
                       std::holds_alternative<double>(right->data)) {
                result_ = std::make_shared<Value>(left->toFloat() + right->toFloat());
            } else {
                result_ = std::make_shared<Value>(left->toInt() + right->toInt());
            }
            break;

        case ast::BinaryOp::Sub:
            if (std::holds_alternative<double>(left->data) ||
                std::holds_alternative<double>(right->data)) {
                result_ = std::make_shared<Value>(left->toFloat() - right->toFloat());
            } else {
                result_ = std::make_shared<Value>(left->toInt() - right->toInt());
            }
            break;

        case ast::BinaryOp::Mul:
            if (std::holds_alternative<double>(left->data) ||
                std::holds_alternative<double>(right->data)) {
                result_ = std::make_shared<Value>(left->toFloat() * right->toFloat());
            } else {
                result_ = std::make_shared<Value>(left->toInt() * right->toInt());
            }
            break;

        case ast::BinaryOp::Div:
            result_ = std::make_shared<Value>(left->toFloat() / right->toFloat());
            break;

        case ast::BinaryOp::Mod:
            result_ = std::make_shared<Value>(left->toInt() % right->toInt());
            break;

        case ast::BinaryOp::Eq:
            result_ = std::make_shared<Value>(left->toString() == right->toString());
            break;

        case ast::BinaryOp::Ne:
            result_ = std::make_shared<Value>(left->toString() != right->toString());
            break;

        case ast::BinaryOp::Lt:
            result_ = std::make_shared<Value>(left->toFloat() < right->toFloat());
            break;

        case ast::BinaryOp::Le:
            result_ = std::make_shared<Value>(left->toFloat() <= right->toFloat());
            break;

        case ast::BinaryOp::Gt:
            result_ = std::make_shared<Value>(left->toFloat() > right->toFloat());
            break;

        case ast::BinaryOp::Ge:
            result_ = std::make_shared<Value>(left->toFloat() >= right->toFloat());
            break;

        case ast::BinaryOp::Assign:
            if (auto* id = dynamic_cast<ast::IdentifierExpr*>(node.getLeft())) {
                current_env_->set(id->getName(), right);
                result_ = right;
            }
            break;

        case ast::BinaryOp::Pipeline: {
            // Phase 3.4: Pipeline operator (|>)
            // Passes left value as first argument to right function
            // Example: data |> func means func(data)
            //         data |> func(x) means func(data, x)

            // Right side must be a function call or identifier
            if (auto* call = dynamic_cast<ast::CallExpr*>(node.getRight())) {
                // If right is a call: func(args...), insert left as first arg
                // Create a new argument list with left prepended
                std::vector<std::shared_ptr<Value>> args;
                args.push_back(left);  // Piped value goes first

                // Add existing arguments
                for (const auto& arg_expr : call->getArgs()) {
                    args.push_back(eval(*arg_expr));
                }

                // Evaluate the callee
                auto callee = eval(*call->getCallee());

                // Call the function with the modified arguments
                if (auto* block = std::get_if<std::shared_ptr<BlockValue>>(&callee->data)) {
                    // Block execution - call the block's main function
                    auto* executor = (*block)->getExecutor();
                    if (!executor) {
                        throw std::runtime_error("No executor for block in pipeline");
                    }
                    result_ = executor->callFunction((*block)->metadata.block_id, args);

                    // Phase 4.4: Record block usage for analytics
                    if (block_loader_) {
                        int tokens_saved = ((*block)->metadata.token_count > 0)
                            ? (*block)->metadata.token_count
                            : 50;
                        block_loader_->recordBlockUsage((*block)->metadata.block_id, tokens_saved);

                        // Record block pair if there was a previous block
                        if (!last_executed_block_id_.empty()) {
                            block_loader_->recordBlockPair(last_executed_block_id_, (*block)->metadata.block_id);
                        }
                        last_executed_block_id_ = (*block)->metadata.block_id;
                    }
                } else if (auto* func = std::get_if<std::shared_ptr<FunctionValue>>(&callee->data)) {
                    // User-defined function execution
                    auto saved_env = current_env_;
                    current_env_ = std::make_shared<Environment>(global_env_);

                    // Bind parameters
                    for (size_t i = 0; i < (*func)->params.size(); ++i) {
                        if (i < args.size()) {
                            current_env_->define((*func)->params[i], args[i]);
                        }
                    }

                    // Execute body
                    (*func)->body->accept(*this);
                    current_env_ = saved_env;
                } else {
                    throw std::runtime_error("Pipeline right side must be a callable");
                }

            } else if (auto* id = dynamic_cast<ast::IdentifierExpr*>(node.getRight())) {
                // If right is identifier: funcName, create call with left as argument
                auto callee = eval(*id);
                std::vector<std::shared_ptr<Value>> args = {left};

                if (auto* block = std::get_if<std::shared_ptr<BlockValue>>(&callee->data)) {
                    auto* executor = (*block)->getExecutor();
                    if (!executor) {
                        throw std::runtime_error("No executor for block in pipeline");
                    }
                    result_ = executor->callFunction((*block)->metadata.block_id, args);

                    // Phase 4.4: Record block usage for analytics
                    if (block_loader_) {
                        int tokens_saved = ((*block)->metadata.token_count > 0)
                            ? (*block)->metadata.token_count
                            : 50;
                        block_loader_->recordBlockUsage((*block)->metadata.block_id, tokens_saved);

                        // Record block pair if there was a previous block
                        if (!last_executed_block_id_.empty()) {
                            block_loader_->recordBlockPair(last_executed_block_id_, (*block)->metadata.block_id);
                        }
                        last_executed_block_id_ = (*block)->metadata.block_id;
                    }
                } else if (auto* func = std::get_if<std::shared_ptr<FunctionValue>>(&callee->data)) {
                    auto saved_env = current_env_;
                    current_env_ = std::make_shared<Environment>(global_env_);

                    if (!(*func)->params.empty()) {
                        current_env_->define((*func)->params[0], left);
                    }

                    (*func)->body->accept(*this);
                    current_env_ = saved_env;
                } else {
                    throw std::runtime_error("Pipeline right side must be a callable");
                }
            } else {
                throw std::runtime_error("Pipeline right side must be a function call or identifier");
            }
            break;
        }

        case ast::BinaryOp::Subscript: {
            // Dictionary or list subscript: obj[key] or arr[index]

            // Check if left is a dictionary
            if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&left->data)) {
                auto& dict = *dict_ptr;
                std::string key = right->toString();

                auto it = dict.find(key);
                if (it == dict.end()) {
                    throw std::runtime_error("Dictionary key not found: " + key);
                }

                result_ = it->second;
                break;
            }

            // Check if left is a list
            if (auto* list_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&left->data)) {
                auto& list = *list_ptr;
                int index = right->toInt();

                if (index < 0 || index >= static_cast<int>(list.size())) {
                    throw std::runtime_error("List index out of bounds: " + std::to_string(index));
                }

                result_ = list[index];
                break;
            }

            throw std::runtime_error("Subscript operation requires dictionary or list");
        }

        default:
            result_ = std::make_shared<Value>();
    }
}

void Interpreter::visit(ast::UnaryExpr& node) {
    auto operand = eval(*node.getOperand());

    switch (node.getOp()) {
        case ast::UnaryOp::Neg:
            if (std::holds_alternative<double>(operand->data)) {
                result_ = std::make_shared<Value>(-operand->toFloat());
            } else {
                result_ = std::make_shared<Value>(-operand->toInt());
            }
            break;

        case ast::UnaryOp::Not:
            result_ = std::make_shared<Value>(!operand->toBool());
            break;

        default:
            result_ = operand;
    }
}

void Interpreter::visit(ast::CallExpr& node) {
    // Evaluate arguments first
    std::vector<std::shared_ptr<Value>> args;
    for (auto& arg : node.getArgs()) {
        args.push_back(eval(*arg));
    }

    // Check if this is a member expression call (for method chaining)
    auto* member_expr = dynamic_cast<ast::MemberExpr*>(node.getCallee());
    if (member_expr) {
        // Evaluate the member expression (returns PythonObjectValue for methods)
        auto callable = eval(*member_expr);

        // If it's a Python object, call it
        if (auto* py_obj_ptr = std::get_if<std::shared_ptr<PythonObjectValue>>(&callable->data)) {
            auto& py_callable = *py_obj_ptr;

#ifdef NAAB_HAS_PYTHON
            fmt::print("[CALL] Invoking Python method with {} args\n", args.size());

            // Build argument tuple for Python
            PyObject* py_args = PyTuple_New(args.size());
            for (size_t i = 0; i < args.size(); i++) {
                PyObject* py_arg = nullptr;

                // Convert Value to Python object
                if (auto* intval = std::get_if<int>(&args[i]->data)) {
                    py_arg = PyLong_FromLong(*intval);
                } else if (auto* floatval = std::get_if<double>(&args[i]->data)) {
                    py_arg = PyFloat_FromDouble(*floatval);
                } else if (auto* strval = std::get_if<std::string>(&args[i]->data)) {
                    py_arg = PyUnicode_FromString(strval->c_str());
                } else if (auto* boolval = std::get_if<bool>(&args[i]->data)) {
                    py_arg = *boolval ? Py_True : Py_False;
                    Py_INCREF(py_arg);
                } else {
                    py_arg = Py_None;
                    Py_INCREF(py_arg);
                }

                PyTuple_SetItem(py_args, i, py_arg);  // Steals reference
            }

            // Call the Python callable
            PyObject* py_result = PyObject_CallObject(py_callable->obj, py_args);
            Py_DECREF(py_args);

            if (py_result != nullptr) {
                // Convert result to NAAb Value
                if (PyLong_Check(py_result)) {
                    long val = PyLong_AsLong(py_result);
                    result_ = std::make_shared<Value>(static_cast<int>(val));
                    fmt::print("[SUCCESS] Method returned int: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyFloat_Check(py_result)) {
                    double val = PyFloat_AsDouble(py_result);
                    result_ = std::make_shared<Value>(val);
                    fmt::print("[SUCCESS] Method returned float: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyUnicode_Check(py_result)) {
                    const char* val = PyUnicode_AsUTF8(py_result);
                    result_ = std::make_shared<Value>(std::string(val));
                    fmt::print("[SUCCESS] Method returned string: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyBool_Check(py_result)) {
                    bool val = py_result == Py_True;
                    result_ = std::make_shared<Value>(val);
                    fmt::print("[SUCCESS] Method returned bool: {}\n", val);
                    Py_DECREF(py_result);
                } else if (py_result == Py_None) {
                    result_ = std::make_shared<Value>();
                    fmt::print("[SUCCESS] Method returned None\n");
                    Py_DECREF(py_result);
                } else {
                    // Complex object - wrap for further chaining
                    auto py_obj = std::make_shared<PythonObjectValue>(py_result);
                    result_ = std::make_shared<Value>(py_obj);
                    fmt::print("[SUCCESS] Method returned Python object: {}\n", py_obj->repr);
                    Py_DECREF(py_result);
                }
            } else {
                PyErr_Print();
                fmt::print("[ERROR] Python method call failed\n");
                result_ = std::make_shared<Value>();
            }
#else
            throw std::runtime_error("Python support required for method calls");
#endif
            return;
        }

        // Check if it's a BlockValue (for JavaScript/C++ block method calls)
        if (auto* block_ptr = std::get_if<std::shared_ptr<BlockValue>>(&callable->data)) {
            auto& block = *block_ptr;

            fmt::print("[CALL] Invoking block method {}.{} with {} args\n",
                      block->metadata.block_id, block->member_path, args.size());

            // Get executor
            auto* executor = block->getExecutor();
            if (!executor) {
                throw std::runtime_error("No executor for block: " + block->metadata.block_id);
            }

            // Call the specific function in the block
            if (block->metadata.language == "javascript") {
                explain("Calling JavaScript block to evaluate: " + block->member_path);
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Calling {}::{}\n", block->metadata.block_id, block->member_path);
                }
                profileStart("BLOCK-JS calls");
                fmt::print("[JS CALL] Calling function: {}\n", block->member_path);
                result_ = executor->callFunction(block->member_path, args);
                profileEnd("BLOCK-JS calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                fmt::print("[SUCCESS] JavaScript function returned\n");

                // Phase 4.4: Record block usage
                if (block_loader_) {
                    int tokens_saved = (block->metadata.token_count > 0)
                        ? block->metadata.token_count : 50;
                    block_loader_->recordBlockUsage(block->metadata.block_id, tokens_saved);

                    // Record block pair if there was a previous block
                    if (!last_executed_block_id_.empty()) {
                        block_loader_->recordBlockPair(last_executed_block_id_, block->metadata.block_id);
                    }
                    last_executed_block_id_ = block->metadata.block_id;
                }
                return;
            }

            if (block->metadata.language == "cpp") {
                explain("Calling C++ block to evaluate: " + block->member_path);
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Calling {}::{}\n", block->metadata.block_id, block->member_path);
                }
                profileStart("BLOCK-CPP calls");
                fmt::print("[CPP CALL] Calling function: {}\n", block->member_path);
                result_ = executor->callFunction(block->member_path, args);
                profileEnd("BLOCK-CPP calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                fmt::print("[SUCCESS] C++ function returned\n");

                // Phase 4.4: Record block usage
                if (block_loader_) {
                    int tokens_saved = (block->metadata.token_count > 0)
                        ? block->metadata.token_count : 50;
                    block_loader_->recordBlockUsage(block->metadata.block_id, tokens_saved);

                    // Record block pair if there was a previous block
                    if (!last_executed_block_id_.empty()) {
                        block_loader_->recordBlockPair(last_executed_block_id_, block->metadata.block_id);
                    }
                    last_executed_block_id_ = block->metadata.block_id;
                }
                return;
            }

            if (block->metadata.language == "python") {
                explain("Calling Python block to evaluate: " + block->member_path);
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Calling {}::{}\n", block->metadata.block_id, block->member_path);
                }
                profileStart("BLOCK-PY calls");
                fmt::print("[PY CALL] Calling function: {}\n", block->member_path);
                result_ = executor->callFunction(block->member_path, args);
                profileEnd("BLOCK-PY calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                fmt::print("[SUCCESS] Python function returned\n");

                // Phase 4.4: Record block usage
                if (block_loader_) {
                    int tokens_saved = (block->metadata.token_count > 0)
                        ? block->metadata.token_count : 50;
                    block_loader_->recordBlockUsage(block->metadata.block_id, tokens_saved);

                    // Record block pair if there was a previous block
                    if (!last_executed_block_id_.empty()) {
                        block_loader_->recordBlockPair(last_executed_block_id_, block->metadata.block_id);
                    }
                    last_executed_block_id_ = block->metadata.block_id;
                }
                return;
            }

            throw std::runtime_error("Member function calls not yet supported for " +
                                    block->metadata.language + " blocks");
        }

        // Check if it's a stdlib function call marker
        if (auto* str_ptr = std::get_if<std::string>(&callable->data)) {
            std::string marker = *str_ptr;

            // Check if it's a stdlib call marker
            if (marker.substr(0, 16) == "__stdlib_call__:") {
                // Parse: __stdlib_call__:module:function
                size_t first_colon = marker.find(':', 16);
                if (first_colon != std::string::npos) {
                    std::string module_alias = marker.substr(16, first_colon - 16);
                    std::string func_name = marker.substr(first_colon + 1);

                    // Look up module
                    auto it = imported_modules_.find(module_alias);
                    if (it == imported_modules_.end()) {
                        throw std::runtime_error("Module not found: " + module_alias);
                    }

                    auto module = it->second;

                    fmt::print("[STDLIB] Calling {}.{}() with {} args\n",
                              module_alias, func_name, args.size());

                    // Call the stdlib function
                    result_ = module->call(func_name, args);
                    fmt::print("[SUCCESS] Stdlib function returned\n");
                    return;
                }
            }
        }

        // Otherwise continue with normal handling below
    }

    // Handle member access calls (e.g., module.function(...))
    auto* member_call = dynamic_cast<ast::MemberExpr*>(node.getCallee());
    if (member_call) {
        // Evaluate the member access to get the function
        member_call->accept(*this);
        auto func_value = result_;

        // Check if it's a function
        if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&func_value->data)) {
            auto& func = *func_ptr;

            // Check parameter count (simplified - no defaults for now)
            if (args.size() != func->params.size()) {
                throw std::runtime_error(
                    fmt::format("Function {} expects {} arguments, got {}",
                               func->name, func->params.size(), args.size())
                );
            }

            // Create new environment for function call
            auto func_env = std::make_shared<Environment>(global_env_);

            // Bind parameters
            for (size_t i = 0; i < func->params.size(); i++) {
                func_env->define(func->params[i], args[i]);
            }

            // Execute function body
            auto saved_env = current_env_;
            auto saved_returning = returning_;
            current_env_ = func_env;
            returning_ = false;

            func->body->accept(*this);

            current_env_ = saved_env;
            returning_ = saved_returning;
            return;
        }

        throw std::runtime_error("Member access did not return a callable function");
    }

    // Get function name (for built-ins and named functions)
    auto* id_expr = dynamic_cast<ast::IdentifierExpr*>(node.getCallee());
    if (!id_expr) {
        throw std::runtime_error("Unsupported call expression type");
    }

    std::string func_name = id_expr->getName();

    // Check if it's a user-defined function or loaded block
    if (current_env_->has(func_name)) {
        auto value = current_env_->get(func_name);

        // Check for user-defined function
        if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&value->data)) {
            auto& func = *func_ptr;

            // Check parameter count and defaults
            size_t min_args = 0;
            for (size_t i = 0; i < func->params.size(); i++) {
                if (!func->defaults[i]) {
                    min_args = i + 1;  // Last non-default parameter + 1
                }
            }

            if (args.size() < min_args || args.size() > func->params.size()) {
                throw std::runtime_error(fmt::format(
                    "Function {} expects {}-{} args, got {}",
                    func->name, min_args, func->params.size(), args.size()));
            }

            // Create new environment for function execution
            auto func_env = std::make_shared<Environment>(global_env_);

            // Bind provided arguments
            for (size_t i = 0; i < args.size(); i++) {
                func_env->define(func->params[i], args[i]);
            }

            // Bind default values for missing arguments
            for (size_t i = args.size(); i < func->params.size(); i++) {
                if (func->defaults[i]) {
                    // Evaluate default expression in current environment
                    auto saved_env = current_env_;
                    current_env_ = func_env;
                    func->defaults[i]->accept(*this);
                    auto default_val = result_;
                    current_env_ = saved_env;

                    func_env->define(func->params[i], default_val);
                } else {
                    throw std::runtime_error(fmt::format(
                        "Function {} parameter {} has no default value",
                        func->name, func->params[i]));
                }
            }

            // Save current environment
            auto saved_env = current_env_;
            auto saved_returning = returning_;
            current_env_ = func_env;
            returning_ = false;

            // Phase 4.1: Push stack frame for error reporting
            pushStackFrame(func->name, 0);

            // Push call frame if debugger is active
            if (debugger_ && debugger_->isActive()) {
                debugger::CallFrame frame;
                frame.function_name = func->name;
                frame.source_location = "unknown:0:0";  // TODO: Get from AST node
                frame.env = func_env;
                frame.frame_depth = debugger_->getCurrentDepth();

                // Populate locals map
                for (size_t i = 0; i < args.size(); i++) {
                    frame.locals[func->params[i]] = args[i];
                }

                debugger_->pushFrame(frame);
            }

            // Phase 4.1: Execute function body with proper error propagation
            try {
                func->body->accept(*this);
            } catch (...) {
                // Clean up before re-throwing
                if (debugger_ && debugger_->isActive()) {
                    debugger_->popFrame();
                }
                popStackFrame();
                current_env_ = saved_env;
                returning_ = saved_returning;
                throw;  // Re-throw with stack frame info already captured
            }

            // Pop call frame if debugger is active
            if (debugger_ && debugger_->isActive()) {
                debugger_->popFrame();
            }

            // Phase 4.1: Pop stack frame
            popStackFrame();

            // Restore environment
            current_env_ = saved_env;
            returning_ = saved_returning;

            fmt::print("[CALL] Function {} executed\n", func->name);
            return;
        }

        // Check for loaded block
        if (auto* block_ptr = std::get_if<std::shared_ptr<BlockValue>>(&value->data)) {
            auto& block = *block_ptr;

            fmt::print("[CALL] Invoking block {} ({}) with {} args\n",
                      block->metadata.name, block->metadata.language, args.size());

            // Phase 7: Try executor-based calling first
            auto* executor = block->getExecutor();
            if (executor) {
                fmt::print("[INFO] Calling block via executor ({})...\n", block->metadata.language);

                // Determine function name to call:
                // - If member_path is set, this is a member accessor (e.g., block.method)
                // - Otherwise, use the function name being called
                std::string function_to_call = block->member_path.empty()
                    ? func_name
                    : block->member_path;

                fmt::print("[INFO] Calling function: {}\n", function_to_call);
                result_ = executor->callFunction(function_to_call, args);

                if (result_) {
                    fmt::print("[SUCCESS] Block call completed\n");

                    // Phase 4.4: Record block usage for analytics
                    if (block_loader_) {
                        // Estimate tokens saved (use block's token_count or default to 50)
                        int tokens_saved = (block->metadata.token_count > 0)
                            ? block->metadata.token_count
                            : 50;
                        block_loader_->recordBlockUsage(block->metadata.block_id, tokens_saved);

                        // Record block pair if there was a previous block
                        if (!last_executed_block_id_.empty()) {
                            block_loader_->recordBlockPair(last_executed_block_id_, block->metadata.block_id);
                        }
                        last_executed_block_id_ = block->metadata.block_id;
                    }
                } else {
                    fmt::print("[WARN] Block call returned null\n");
                    result_ = std::make_shared<Value>();
                }
                return;
            }

            // Fallback: Legacy Python handling for blocks without executor
            if (block->metadata.language == "python") {
                // Python blocks: Execute using embedded Python interpreter
#ifdef NAAB_HAS_PYTHON
                fmt::print("[INFO] Executing Python block: {}\n", block->metadata.name);

                // Add common imports automatically
                PyRun_SimpleString("from typing import Dict, List, Optional, Any, Union\n"
                                  "import sys\n");

                // Execute the block code to define classes/functions using exec()
                // This handles multi-line code with proper indentation
                std::string exec_code = "exec('''" + block->code + "''')";
                PyRun_SimpleString(exec_code.c_str());

                // Handle member access calls
                if (!block->member_path.empty()) {
                    fmt::print("[INFO] Calling member: {}\n", block->member_path);

                    // Build argument list for Python
                    std::string args_str = "(";
                    for (size_t i = 0; i < args.size(); i++) {
                        if (i > 0) args_str += ", ";

                        // Convert Value to Python representation
                        if (auto* intval = std::get_if<int>(&args[i]->data)) {
                            args_str += std::to_string(*intval);
                        } else if (auto* floatval = std::get_if<double>(&args[i]->data)) {
                            args_str += std::to_string(*floatval);
                        } else if (auto* strval = std::get_if<std::string>(&args[i]->data)) {
                            args_str += "\"" + *strval + "\"";
                        } else if (auto* boolval = std::get_if<bool>(&args[i]->data)) {
                            args_str += *boolval ? "True" : "False";
                        } else {
                            args_str += "None";
                        }
                    }
                    args_str += ")";

                    // Call the member and capture return value
                    std::string call_expr = block->member_path + args_str;

                    // Get main module
                    PyObject* main_module = PyImport_AddModule("__main__");
                    PyObject* global_dict = PyModule_GetDict(main_module);

                    // Compile and evaluate the expression
                    PyObject* py_result = PyRun_String(call_expr.c_str(),
                                                       Py_eval_input,
                                                       global_dict,
                                                       global_dict);

                    if (py_result != nullptr) {
                        // Convert Python result to NAAb Value
                        if (PyLong_Check(py_result)) {
                            long val = PyLong_AsLong(py_result);
                            result_ = std::make_shared<Value>(static_cast<int>(val));
                            fmt::print("[SUCCESS] Returned int: {}\n", val);
                        } else if (PyFloat_Check(py_result)) {
                            double val = PyFloat_AsDouble(py_result);
                            result_ = std::make_shared<Value>(val);
                            fmt::print("[SUCCESS] Returned float: {}\n", val);
                        } else if (PyUnicode_Check(py_result)) {
                            const char* val = PyUnicode_AsUTF8(py_result);
                            result_ = std::make_shared<Value>(std::string(val));
                            fmt::print("[SUCCESS] Returned string: {}\n", val);
                        } else if (PyBool_Check(py_result)) {
                            bool val = py_result == Py_True;
                            result_ = std::make_shared<Value>(val);
                            fmt::print("[SUCCESS] Returned bool: {}\n", val);
                        } else if (py_result == Py_None) {
                            result_ = std::make_shared<Value>();
                            fmt::print("[SUCCESS] Returned None\n");
                            Py_DECREF(py_result);
                        } else {
                            // Complex object - wrap in PythonObjectValue for method chaining
                            auto py_obj = std::make_shared<PythonObjectValue>(py_result);
                            result_ = std::make_shared<Value>(py_obj);
                            fmt::print("[SUCCESS] Returned Python object: {}\n", py_obj->repr);
                            Py_DECREF(py_result);  // PythonObjectValue has its own reference
                        }
                    } else {
                        PyErr_Print();
                        fmt::print("[ERROR] Member call failed\n");
                        result_ = std::make_shared<Value>();
                    }

                    return;
                }

                // Regular block execution (no member access)
                // Inject arguments as Python variables
                if (!args.empty()) {
                    // Create args list
                    std::string args_setup = "args = [";
                    for (size_t i = 0; i < args.size(); i++) {
                        if (i > 0) args_setup += ", ";

                        // Convert Value to Python representation
                        if (auto* intval = std::get_if<int>(&args[i]->data)) {
                            args_setup += std::to_string(*intval);
                        } else if (auto* floatval = std::get_if<double>(&args[i]->data)) {
                            args_setup += std::to_string(*floatval);
                        } else if (auto* strval = std::get_if<std::string>(&args[i]->data)) {
                            args_setup += "\"" + *strval + "\"";
                        } else if (auto* boolval = std::get_if<bool>(&args[i]->data)) {
                            args_setup += *boolval ? "True" : "False";
                        } else {
                            args_setup += "None";
                        }
                    }
                    args_setup += "]\n";

                    PyRun_SimpleString(args_setup.c_str());
                    fmt::print("[INFO] Injected {} args into Python context\n", args.size());
                }

                // Execute Python code - for blocks that are classes/functions
                // Try to evaluate as expression first (for simple cases)
                PyObject* main_module = PyImport_AddModule("__main__");
                PyObject* global_dict = PyModule_GetDict(main_module);

                // For blocks that define classes, just execute and return success
                int result = PyRun_SimpleString(block->code.c_str());

                if (result == 0) {
                    fmt::print("[SUCCESS] Python block executed successfully\n");
                    result_ = std::make_shared<Value>();  // Return null for definition blocks
                } else {
                    fmt::print("[ERROR] Python block execution failed\n");
                    result_ = std::make_shared<Value>();
                }
#else
                fmt::print("[WARN] Python execution not available\n");
                result_ = std::make_shared<Value>();
#endif
                return;
            } else {
                fmt::print("[WARN] Unsupported block language: {}\n", block->metadata.language);
                result_ = std::make_shared<Value>();
                return;
            }
        }
    }

    // Built-in functions
    if (func_name == "print") {
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) std::cout << " ";
            std::cout << args[i]->toString();
        }
        std::cout << std::endl;
        result_ = std::make_shared<Value>();
    }
    else if (func_name == "len") {
        if (!args.empty()) {
            if (auto* str = std::get_if<std::string>(&args[0]->data)) {
                result_ = std::make_shared<Value>(static_cast<int>(str->length()));
            } else if (auto* list = std::get_if<std::vector<std::shared_ptr<Value>>>(&args[0]->data)) {
                result_ = std::make_shared<Value>(static_cast<int>(list->size()));
            } else {
                result_ = std::make_shared<Value>(0);
            }
        }
    }
    else if (func_name == "type") {
        if (!args.empty()) {
            std::string type_name = std::visit([](auto&& arg) -> std::string {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, int>) return "int";
                else if constexpr (std::is_same_v<T, double>) return "float";
                else if constexpr (std::is_same_v<T, bool>) return "bool";
                else if constexpr (std::is_same_v<T, std::string>) return "string";
                else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) return "list";
                else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) return "dict";
                else if constexpr (std::is_same_v<T, std::shared_ptr<BlockValue>>) return "block";
                else if constexpr (std::is_same_v<T, std::shared_ptr<FunctionValue>>) return "function";
                else if constexpr (std::is_same_v<T, std::shared_ptr<PythonObjectValue>>) return "python_object";
                return "unknown";
            }, args[0]->data);
            result_ = std::make_shared<Value>(type_name);
        }
    }
    else {
        // Function not found
        throw std::runtime_error("Undefined function: " + func_name);
    }
}

void Interpreter::visit(ast::MemberExpr& node) {
    auto obj = eval(*node.getObject());
    std::string member_name = node.getMember();

    // Handle struct member access
    if (std::holds_alternative<std::shared_ptr<StructValue>>(obj->data)) {
        auto struct_val = std::get<std::shared_ptr<StructValue>>(obj->data);
        result_ = struct_val->getField(member_name);
        return;
    }

    // Check if object is a block
    if (auto* block_ptr = std::get_if<std::shared_ptr<BlockValue>>(&obj->data)) {
        auto& block = *block_ptr;

        fmt::print("[MEMBER] Accessing {}.{} on {} block\n",
                   block->metadata.block_id, member_name, block->metadata.language);

        // Phase 7: Executor-based member access
        auto* executor = block->getExecutor();
        if (executor) {
            // For blocks with executors, create a member accessor
            // Build member path (support chaining like obj.member1.member2)
            std::string full_member_path = block->member_path.empty()
                ? member_name
                : block->member_path + "." + member_name;

            // Create new BlockValue representing the member accessor
            // Copy executor reference (borrowed) or owned_executor (owned)
            std::shared_ptr<BlockValue> member_block;

            if (block->owned_executor_) {
                // Can't share owned executor - must be borrowed for member access
                // Store pointer to original block's executor
                member_block = std::make_shared<BlockValue>(
                    block->metadata,
                    block->code,
                    block->owned_executor_.get()
                );
            } else {
                // Borrowed executor - can share
                member_block = std::make_shared<BlockValue>(
                    block->metadata,
                    block->code,
                    block->executor_
                );
            }

            member_block->member_path = full_member_path;

            result_ = std::make_shared<Value>(member_block);
            fmt::print("[INFO] Created member accessor: {} ({})\n",
                      full_member_path, block->metadata.language);
            return;
        }

        // Fallback: Legacy Python handling for blocks without executor
        if (block->metadata.language == "python") {
#ifdef NAAB_HAS_PYTHON
            // Execute the block code using exec() to handle multi-line properly
            std::string exec_code = "exec('''" + block->code + "''')";
            PyRun_SimpleString(exec_code.c_str());

            // Build member path
            std::string full_member_path = block->member_path.empty()
                ? member_name
                : block->member_path + "." + member_name;

            // Create new BlockValue representing the member
            auto member_block = std::make_shared<BlockValue>(
                block->metadata,
                block->code,
                block->python_namespace,
                full_member_path
            );

            result_ = std::make_shared<Value>(member_block);
            fmt::print("[INFO] Created member accessor (legacy Python): {}\n", full_member_path);
#else
            throw std::runtime_error("Python support required for member access");
#endif
            return;
        } else {
            throw std::runtime_error("Member access not supported for " +
                                   block->metadata.language + " blocks without executor");
        }
    }

    // Check if object is a Python object (for method chaining)
    if (auto* py_obj_ptr = std::get_if<std::shared_ptr<PythonObjectValue>>(&obj->data)) {
        auto& py_obj = *py_obj_ptr;

#ifdef NAAB_HAS_PYTHON
        fmt::print("[MEMBER] Accessing .{} on Python object\n", member_name);

        // Get the member attribute from the Python object
        PyObject* py_member = PyObject_GetAttrString(py_obj->obj, member_name.c_str());

        if (py_member != nullptr) {
            // Wrap the member in a new PythonObjectValue
            auto member_obj = std::make_shared<PythonObjectValue>(py_member);
            result_ = std::make_shared<Value>(member_obj);
            Py_DECREF(py_member);  // PythonObjectValue has its own reference
            fmt::print("[INFO] Accessed Python object member: {}\n", member_name);
        } else {
            PyErr_Print();
            throw std::runtime_error("Python object has no attribute: " + member_name);
        }
#else
        throw std::runtime_error("Python support required for Python object member access");
#endif
        return;
    }

    // Check if object is a dictionary (for module imports)
    if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&obj->data)) {
        auto it = dict_ptr->find(member_name);
        if (it != dict_ptr->end()) {
            result_ = it->second;
            return;
        }
        throw std::runtime_error("Member '" + member_name + "' not found in module");
    }

    // Check if object is a stdlib module marker
    if (auto* str_ptr = std::get_if<std::string>(&obj->data)) {
        std::string marker = *str_ptr;

        // Check if it's a stdlib module marker
        if (marker.substr(0, 18) == "__stdlib_module__:") {
            std::string module_alias = marker.substr(18);

            // Look up the module
            auto it = imported_modules_.find(module_alias);
            if (it == imported_modules_.end()) {
                throw std::runtime_error("Module not found: " + module_alias);
            }

            auto module = it->second;

            // Create a marker for the function call
            // Format: __stdlib_call__:module_alias:function_name
            std::string func_marker = "__stdlib_call__:" + module_alias + ":" + member_name;
            result_ = std::make_shared<Value>(func_marker);
            return;
        }
    }

    // Member access on other types (TODO)
    throw std::runtime_error("Member access not supported on this type");
}

void Interpreter::visit(ast::IdentifierExpr& node) {
    result_ = current_env_->get(node.getName());
}

void Interpreter::visit(ast::LiteralExpr& node) {
    switch (node.getLiteralKind()) {
        case ast::LiteralKind::Int:
            result_ = std::make_shared<Value>(std::stoi(node.getValue()));
            break;

        case ast::LiteralKind::Float:
            result_ = std::make_shared<Value>(std::stod(node.getValue()));
            break;

        case ast::LiteralKind::String:
            result_ = std::make_shared<Value>(node.getValue());
            break;

        case ast::LiteralKind::Bool:
            result_ = std::make_shared<Value>(node.getValue() == "true");
            break;
    }
}

void Interpreter::visit(ast::DictExpr& node) {
    std::unordered_map<std::string, std::shared_ptr<Value>> dict;
    for (const auto& [key_expr, val_expr] : node.getEntries()) {
        auto key = eval(*key_expr);
        auto val = eval(*val_expr);
        dict[key->toString()] = val;
    }
    result_ = std::make_shared<Value>(dict);
}

void Interpreter::visit(ast::ListExpr& node) {
    std::vector<std::shared_ptr<Value>> list;
    for (auto& elem : node.getElements()) {
        list.push_back(eval(*elem));
    }
    result_ = std::make_shared<Value>(list);
}

void Interpreter::visit(ast::StructLiteralExpr& node) {
    explain("Creating instance of struct '" + node.getStructName() + "'");

    profileStart("Struct creation");

    auto struct_def = runtime::StructRegistry::instance().getStruct(node.getStructName());
    if (!struct_def) {
        throw std::runtime_error("Undefined struct: " + node.getStructName());
    }

    auto struct_val = std::make_shared<StructValue>();
    struct_val->type_name = node.getStructName();
    struct_val->definition = struct_def;
    struct_val->field_values.resize(struct_def->fields.size());

    // Initialize from literals
    for (const auto& [field_name, init_expr] : node.getFieldInits()) {
        if (!struct_def->field_index.count(field_name)) {
            throw std::runtime_error("Unknown field '" + field_name +
                                   "' in struct '" + node.getStructName() + "'");
        }

        auto field_value = eval(*init_expr);
        size_t idx = struct_def->field_index.at(field_name);
        struct_val->field_values[idx] = field_value;
    }

    // Check required fields (all fields are currently required - no default values yet)
    for (size_t i = 0; i < struct_def->fields.size(); ++i) {
        if (!struct_val->field_values[i]) {
            const auto& field = struct_def->fields[i];
            throw std::runtime_error("Missing required field '" + field.name +
                                   "' in struct '" + node.getStructName() + "'");
        }
    }

    result_ = std::make_shared<Value>(struct_val);

    profileEnd("Struct creation");
}

// ============================================================================
// StructValue Methods
// ============================================================================

std::shared_ptr<Value> StructValue::getField(const std::string& name) const {
    if (!definition) {
        throw std::runtime_error("Struct has no definition");
    }
    auto it = definition->field_index.find(name);
    if (it == definition->field_index.end()) {
        throw std::runtime_error("Field '" + name + "' not found in struct '" +
                               type_name + "'");
    }
    return field_values[it->second];
}

void StructValue::setField(const std::string& name, std::shared_ptr<Value> value) {
    if (!definition) {
        throw std::runtime_error("Struct has no definition");
    }
    auto it = definition->field_index.find(name);
    if (it == definition->field_index.end()) {
        throw std::runtime_error("Field '" + name + "' not found in struct '" +
                               type_name + "'");
    }
    field_values[it->second] = value;
}

// Profile mode methods
void Interpreter::profileStart(const std::string& name) {
    if (!profile_mode_) return;
    profile_start_ = std::chrono::high_resolution_clock::now();
}

void Interpreter::profileEnd(const std::string& name) {
    if (!profile_mode_) return;
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        end - profile_start_).count();
    profile_timings_[name] += duration;
}

void Interpreter::printProfile() const {
    if (!profile_mode_ || profile_timings_.empty()) return;

    long long total = 0;
    for (const auto& [name, time] : profile_timings_) {
        total += time;
    }

    fmt::print("\n=== Execution Profile ===\n");
    fmt::print("Total time: {:.2f}ms\n\n", total / 1000.0);

    // Sort by time descending
    std::vector<std::pair<std::string, long long>> sorted(
        profile_timings_.begin(), profile_timings_.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& [name, time] : sorted) {
        double ms = time / 1000.0;
        double pct = (total > 0) ? (100.0 * time / total) : 0.0;
        fmt::print("  {}: {:.2f}ms ({:.1f}%)\n", name, ms, pct);
    }
    fmt::print("=========================\n");
}

// Explain mode method
void Interpreter::explain(const std::string& message) const {
    if (explain_mode_) {
        fmt::print("[EXPLAIN] {}\n", message);
    }
}

} // namespace interpreter
} // namespace naab
