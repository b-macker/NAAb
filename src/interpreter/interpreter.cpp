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
#include "cycle_detector.h"  // Phase 3.2: Garbage collection
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

// Phase 3.2: Traverse all referenced values (for cycle detection)
void Value::traverse(std::function<void(std::shared_ptr<Value>)> visitor) const {
    std::visit([&visitor](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;

        // Visit list elements
        if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) {
            for (const auto& elem : arg) {
                if (elem) {
                    visitor(elem);
                }
            }
        }
        // Visit dict values
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) {
            for (const auto& [key, val] : arg) {
                if (val) {
                    visitor(val);
                }
            }
        }
        // Visit struct fields
        else if constexpr (std::is_same_v<T, std::shared_ptr<StructValue>>) {
            if (arg) {
                for (const auto& field_val : arg->field_values) {
                    if (field_val) {
                        visitor(field_val);
                    }
                }
            }
        }
        // Other types (int, double, bool, string, etc.) have no child values
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

// GEMINI FIX: Modified constructor to accept script arguments
Interpreter::Interpreter(const std::vector<std::string>& script_args)
    : global_env_(std::make_shared<Environment>()),
      current_env_(global_env_),
      result_(std::make_shared<Value>()),
      returning_(false),
      breaking_(false),
      continuing_(false),
      last_executed_block_id_(""),
      current_function_(nullptr),
      script_args_(script_args) // GEMINI FIX: Initialize script_args_
{
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
    // GEMINI FIX: Pass interpreter instance to StdLib constructor for module function access
    stdlib_ = std::make_unique<stdlib::StdLib>(this);
    fmt::print("[INFO] Standard library initialized: {} modules available\n",
               stdlib_->listModules().size());

    // Phase 3.1: Initialize module resolver
    module_resolver_ = std::make_unique<modules::ModuleResolver>();
    fmt::print("[INFO] Module resolver initialized\n");

    // Phase 4.0: Initialize module registry
    module_registry_ = std::make_unique<modules::ModuleRegistry>();
    fmt::print("[INFO] Module registry initialized (Phase 4.0)\n");

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

    // Phase 3.2: Initialize garbage collector
    cycle_detector_ = std::make_unique<CycleDetector>();
    fmt::print("[INFO] Garbage collector initialized (threshold: {} allocations)\n", gc_threshold_);

    defineBuiltins();
}

// GEMINI FIX: Implement setScriptArgs
void Interpreter::setScriptArgs(const std::vector<std::string>& args) {
    script_args_ = args;
}

// GEMINI FIX: Implement getScriptArgs
const std::vector<std::string>& Interpreter::getScriptArgs() const {
    return script_args_;
}

// Phase 3.2: Destructor must be defined in .cpp where CycleDetector is complete
Interpreter::~Interpreter() {
    // Default destruction is fine, but must be defined here for unique_ptr<CycleDetector>
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

// Phase 3.1: Set source code for enhanced error messages
void Interpreter::setSourceCode(const std::string& source, const std::string& filename) {
    source_code_ = source;
    error_reporter_.setSource(source, filename);
}

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
    auto saved_file = current_file_;  // Phase 3.1: Save current file for cross-module calls
    current_env_ = func_env;
    returning_ = false;
    current_file_ = func->source_file;  // Phase 3.1: Set file to function's source file

    pushStackFrame(func->name, func->source_line);  // Phase 3.1: Use actual line number

    try {
        executeStmt(*func->body);
    } catch (...) {
        popStackFrame();
        current_env_ = saved_env;
        returning_ = saved_returning;
        current_file_ = saved_file;  // Phase 3.1: Restore file
        throw;
    }

    popStackFrame();

    // Restore environment
    current_env_ = saved_env;
    current_file_ = saved_file;  // Phase 3.1: Restore file
    auto return_value = result_;
    returning_ = saved_returning;

    return return_value;
}

// Phase 11.1: Helper to flush captured output from polyglot executors
void Interpreter::flushExecutorOutput(runtime::Executor* executor) {
    if (!executor) return;

    std::string captured_output = executor->getCapturedOutput();
    if (!captured_output.empty()) {
        fmt::print("{}", captured_output);
        std::cout.flush(); // Ensure immediate output
    }
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

    // Phase 4.0: Process module use statements (Rust-style imports)
    for (auto& module_use : node.getModuleUses()) {
        module_use->accept(*this);
    }

    // Store use statements (for now, just note them)
    for (auto& use_stmt : node.getImports()) {
        use_stmt->accept(*this);
    }

    // Process struct declarations
    for (auto& struct_decl : node.getStructs()) {
        struct_decl->accept(*this);
    }

    // Phase 2.4.3: Process enum declarations
    for (auto& enum_decl : node.getEnums()) {
        enum_decl->accept(*this);
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

// Phase 4.0: Module use statement visitor (Rust-style modules)
// use math_utils
// use data.processor
void Interpreter::visit(ast::ModuleUseStmt& node) {
    const std::string& module_path = node.getModulePath();

    fmt::print("[MODULE] Processing: use {}\n", module_path);

    // BUGFIX: Check if it's a stdlib module first (like UseStatement does)
    if (stdlib_->hasModule(module_path)) {
        auto module = stdlib_->getModule(module_path);

        // Determine alias
        std::string alias = node.hasAlias() ? node.getAlias() : module_path;

        // Store in imported_modules_ for function calls
        imported_modules_[alias] = module;

        fmt::print("[MODULE] Loaded stdlib module: {} as {}\n", module_path, alias);

        // Store module marker in environment for member access
        auto module_marker = std::make_shared<Value>(
            std::string("__stdlib_module__:" + alias)
        );
        current_env_->define(alias, module_marker);

        return;
    }

    // Get current file directory for relative module resolution
    std::filesystem::path current_dir = current_file_.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(current_file_).parent_path();

    // Load module (this will parse it if not already loaded)
    modules::NaabModule* module = module_registry_->loadModule(module_path, current_dir);
    if (!module) {
        throw std::runtime_error(
            fmt::format("Failed to load module: {}\n"
                       "  Searched in: {}\n"
                       "  See above for detailed error messages",
                       module_path, current_dir.string())
        );
    }

    // Check if module has already been executed
    if (module->isExecuted()) {
        fmt::print("[MODULE] Module '{}' already executed, reusing\n", module_path);

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
        auto module_marker = std::make_shared<Value>(
            std::string("__module__:" + module_path)
        );
        current_env_->define(module_name, module_marker);

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

        fmt::print("[MODULE] Executing: {}\n", dep_module->getName());

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
            fmt::print("[MODULE] Execution complete: {}\n", dep_module->getName());

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
    auto module_marker = std::make_shared<Value>(
        std::string("__module__:" + module_path)
    );
    current_env_->define(module_name, module_marker);

    // ISS-024 Fix: Store alias mapping for module-qualified types
    // loaded_modules_ uses module_path as key, so if an alias is used,
    // also store it under the alias for struct resolution
    if (loaded_modules_.count(module_path) && module_name != module_path) {
        loaded_modules_[module_name] = loaded_modules_[module_path];
    }

    fmt::print("[MODULE] Successfully imported: {} (use as '{}')\n",
               module_path, module_name);
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

    // Import exported enums (Phase 4.1: Module System)
    for (const auto& [name, enum_def] : module_env->exported_enums_) {
        // Define all enum variants in global environment
        for (const auto& [variant_name, value] : enum_def->variants) {
            std::string full_name = enum_def->name + "." + variant_name;
            global_env_->define(full_name, std::make_shared<Value>(value));
        }
        fmt::print("[SUCCESS] Imported enum: {}\n", name);
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

                fmt::print("[SUCCESS] Exported enum: {}\n", enum_decl->getName());
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
    // Phase 2.1: Extract parameter names, types, and default values
    std::vector<std::string> param_names;
    std::vector<ast::Type> param_types;
    std::vector<ast::Expr*> param_defaults;

    for (const auto& param : node.getParams()) {
        param_names.push_back(param.name);
        param_types.push_back(param.type);  // Phase 2.1: Store parameter type (includes is_reference)

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

    // Phase 2.4.4 Phase 2: Infer return type if not explicitly provided
    ast::Type return_type = node.getReturnType();
    if (return_type.kind == ast::TypeKind::Any) {
        // Return type was not specified - infer it from function body
        return_type = inferReturnType(body);
        fmt::print("[INFO] Inferred return type for function '{}': {}\n",
                   node.getName(), formatTypeName(return_type));
    }

    // Phase 2.4.1: Create FunctionValue with types and type parameters
    // ISS-022: Capture closure environment for lexical scoping
    auto func_value = std::make_shared<FunctionValue>(
        node.getName(),
        param_names,
        std::move(param_types),  // Phase 2.1: Pass parameter types
        std::move(param_defaults),
        std::shared_ptr<ast::CompoundStmt>(body, [](ast::CompoundStmt*){})  ,  // Non-owning shared_ptr
        node.getTypeParams(),  // Phase 2.4.1: Pass type parameters
        return_type,   // Phase 2.4.4 Phase 2: Use inferred return type
        node.getLocation().filename,  // Phase 3.1: Source file for stack traces
        node.getLocation().line,  // Phase 3.1: Line number for stack traces
        current_env_  // ISS-022: Capture closure for module imports
    );

    // Store in environment
    // ISS-022 Fix: Use current_env_ instead of global_env_ so module functions
    // can access module imports (like stdlib modules)
    auto value = std::make_shared<Value>(func_value);
    current_env_->define(node.getName(), value);

    fmt::print("[INFO] Defined function: {}({} params)",
               node.getName(), param_names.size());
    if (!node.getTypeParams().empty()) {
        fmt::print(" <");
        for (size_t i = 0; i < node.getTypeParams().size(); i++) {
            if (i > 0) fmt::print(", ");
            fmt::print("{}", node.getTypeParams()[i]);
        }
        fmt::print(">");
    }
    fmt::print("\n");
}

void Interpreter::visit(ast::StructDecl& node) {
    explain("Defining struct '" + node.getName() + "' with " +
            std::to_string(node.getFields().size()) + " fields");

    auto struct_def = std::make_shared<StructDef>();
    struct_def->name = node.getName();
    struct_def->type_parameters = node.getTypeParams();  // Phase 2.4.1: Store type parameters

    size_t field_idx = 0;
    for (const auto& field : node.getFields()) {
        // Copy field metadata without the default_value expr (not needed at runtime)
        ast::StructField runtime_field{field.name, field.type, std::nullopt};
        struct_def->fields.push_back(std::move(runtime_field));
        struct_def->field_index[field.name] = field_idx++;
    }

    // Validate (detect cycles) - skip for generic structs (will validate specializations)
    if (struct_def->type_parameters.empty()) {
        std::set<std::string> visiting;
        runtime::StructRegistry::instance().validateStructDef(*struct_def, visiting);
    }

    // Register
    runtime::StructRegistry::instance().registerStruct(struct_def);

    fmt::print("[INFO] Defined struct: {}\n", node.getName());

    if (isVerboseMode()) {
        fmt::print("[VERBOSE] Registered struct '{}' with {} fields",
                   node.getName(), node.getFields().size());
        if (!struct_def->type_parameters.empty()) {
            fmt::print(" (generic: <");
            for (size_t i = 0; i < struct_def->type_parameters.size(); i++) {
                if (i > 0) fmt::print(", ");
                fmt::print("{}", struct_def->type_parameters[i]);
            }
            fmt::print(">)");
        }
        fmt::print("\n");
    }

    // Struct declarations don't produce values
    result_ = std::make_shared<Value>();
}

// Phase 2.4.3: Enum declaration visitor
void Interpreter::visit(ast::EnumDecl& node) {
    explain("Defining enum '" + node.getName() + "' with " +
            std::to_string(node.getVariants().size()) + " variants");

    // Assign values to variants (either explicit or auto-increment)
    int next_value = 0;
    for (const auto& variant : node.getVariants()) {
        int variant_value = variant.value.value_or(next_value);

        // Store enum variant as: EnumName.VariantName = value
        std::string full_name = node.getName() + "." + variant.name;
        auto value = std::make_shared<Value>(variant_value);
        global_env_->define(full_name, value);

        next_value = variant_value + 1;
    }

    fmt::print("[INFO] Defined enum: {} with {} variants\n",
               node.getName(), node.getVariants().size());

    // Enum declarations don't produce values
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

    // Phase 2.4.2 & 2.4.5: Validate return type (union types and null safety)
    if (current_function_) {
        // Phase 2.4.4: Substitute type parameters in return type for generic functions
        ast::Type return_type = current_function_->return_type;
        if (!current_type_substitutions_.empty()) {
            return_type = substituteTypeParams(return_type, current_type_substitutions_);
        }

        // Phase 2.4.5: Null safety - cannot return null from non-nullable function
        if (!return_type.is_nullable && return_type.kind != ast::TypeKind::Void && isNull(result_)) {
            throw std::runtime_error(
                "Null safety error: Cannot return null from function '" +
                current_function_->name + "' with non-nullable return type " +
                formatTypeName(return_type) +
                "\n  Help: Change return type to nullable: " +
                formatTypeName(return_type) + "?"
            );
        }

        // Check union return types
        if (return_type.kind == ast::TypeKind::Union) {
            if (!valueMatchesUnion(result_, return_type.union_types)) {
                throw std::runtime_error(
                    "Type error: Function '" + current_function_->name +
                    "' expects return type " + formatTypeName(return_type) +
                    ", but got " + getValueTypeName(result_)
                );
            }
        }
        // Check non-union return types (if not Any or Void)
        else if (return_type.kind != ast::TypeKind::Any && return_type.kind != ast::TypeKind::Void) {
            if (!valueMatchesType(result_, return_type)) {
                throw std::runtime_error(
                    "Type error: Function '" + current_function_->name +
                    "' expects return type " + formatTypeName(return_type) +
                    ", but got " + getValueTypeName(result_)
                );
            }
        }
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

    // Check if it's a range (dict with __is_range marker)
    if (auto* dict = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&iterable->data)) {
        auto it = dict->find("__is_range");
        if (it != dict->end() && it->second->toBool()) {
            // This is a range - iterate from start to end
            int start = (*dict)["__range_start"]->toInt();
            int end = (*dict)["__range_end"]->toInt();
            bool inclusive = false;

            // Check for inclusive flag (..= operator)
            auto inc_it = dict->find("__range_inclusive");
            if (inc_it != dict->end()) {
                inclusive = inc_it->second->toBool();
            }

            // Use <= for inclusive ranges, < for exclusive
            if (inclusive) {
                for (int i = start; i <= end; i++) {
                    current_env_->define(node.getVar(), std::make_shared<Value>(i));
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
            } else {
                for (int i = start; i < end; i++) {
                    current_env_->define(node.getVar(), std::make_shared<Value>(i));
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
            return;
        }
    }

    // Otherwise, handle as list
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
    (void)node; // Node info not needed for break control flow
    breaking_ = true;
}

void Interpreter::visit(ast::ContinueStmt& node) {
    (void)node; // Node info not needed for continue control flow
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

    // Phase 2.4.4: Type inference - if no type annotation, infer from value
    ast::Type effective_type = ast::Type::makeAny();  // Initialize with default
    bool has_explicit_type = node.getType().has_value();

    if (has_explicit_type) {
        effective_type = node.getType().value();
    } else {
        // Phase 2.4.4: Infer type from initializer value
        if (node.getInit()) {
            effective_type = inferTypeFromValue(value);

            // Special case: cannot infer from null alone (ambiguous)
            if (isNull(value)) {
                throw std::runtime_error(
                    "Type inference error: Cannot infer type for variable '" + node.getName() + "' from 'null'\n" +
                    "  Help: 'null' can be any nullable type, add explicit annotation\n" +
                    "    let " + node.getName() + ": string? = null\n" +
                    "    let " + node.getName() + ": int? = null"
                );
            }
        } else {
            // No initializer and no type annotation - error
            throw std::runtime_error(
                "Type inference error: Cannot infer type for variable '" + node.getName() + "' without initializer\n" +
                "  Help: Add an initializer or explicit type annotation\n" +
                "    let " + node.getName() + " = 0           // with initializer\n" +
                "    let " + node.getName() + ": int          // with type annotation"
            );
        }
    }

    // Phase 2.4.2 & 2.4.5: Type validation (union types and null safety)
    if (has_explicit_type) {
        // Phase 2.4.5: Null safety - cannot assign null to non-nullable type
        if (!effective_type.is_nullable && isNull(value)) {
            throw std::runtime_error(
                "Null safety error: Cannot assign null to non-nullable variable '" +
                node.getName() + "' of type " + formatTypeName(effective_type) +
                "\n  Help: Change to nullable type if null values are expected: " +
                formatTypeName(effective_type) + "?"
            );
        }

        // For union types, validate value matches one of the union members
        if (effective_type.kind == ast::TypeKind::Union) {
            if (!valueMatchesUnion(value, effective_type.union_types)) {
                throw std::runtime_error(
                    "Type error: Variable '" + node.getName() +
                    "' expects " + formatTypeName(effective_type) +
                    ", but got " + getValueTypeName(value)
                );
            }
        }
        // For other types, validate value matches declared type
        else if (!valueMatchesType(value, effective_type)) {
            throw std::runtime_error(
                "Type error: Variable '" + node.getName() +
                "' expects " + formatTypeName(effective_type) +
                ", but got " + getValueTypeName(value)
            );
        }
    }
    // If type was inferred, it already matches the value by construction

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

    // Phase 2.4.6: Handle assignment specially to avoid evaluating left side as a read operation
    if (node.getOp() == ast::BinaryOp::Assign) {
        auto right = eval(*node.getRight());

        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(node.getLeft())) {
            // Simple variable assignment: x = value
            current_env_->set(id->getName(), right);
            result_ = right;
        } else if (auto* member = dynamic_cast<ast::MemberExpr*>(node.getLeft())) {
            // Struct field assignment: obj.field = value
            auto obj = eval(*member->getObject());

            if (auto* struct_ptr = std::get_if<std::shared_ptr<StructValue>>(&obj->data)) {
                auto& struct_val = *struct_ptr;
                struct_val->setField(member->getMember(), right);
                result_ = right;
            } else {
                throw std::runtime_error("Cannot assign to property of non-struct value");
            }
        } else if (auto* subscript = dynamic_cast<ast::BinaryExpr*>(node.getLeft())) {
            // Array/dict element assignment: arr[index] = value, dict[key] = value
            if (subscript->getOp() == ast::BinaryOp::Subscript) {
                // Evaluate the container (e.g., arr or dict)
                auto container = eval(*subscript->getLeft());
                // Evaluate the index/key
                auto index_or_key = eval(*subscript->getRight());

                // Check if container is a list
                if (auto* list_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&container->data)) {
                    auto& list = *list_ptr;
                    int index = index_or_key->toInt();

                    if (index < 0 || index >= static_cast<int>(list.size())) {
                        throw std::runtime_error("List index out of bounds: " + std::to_string(index));
                    }

                    // Modify list in place
                    list[index] = right;
                    result_ = right;
                }
                // Check if container is a dictionary
                else if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&container->data)) {
                    auto& dict = *dict_ptr;
                    std::string key = index_or_key->toString();

                    // Insert or update the key
                    dict[key] = right;
                    result_ = right;
                } else {
                    throw std::runtime_error("Subscript assignment requires list or dictionary");
                }
            } else {
                throw std::runtime_error("Invalid assignment target");
            }
        } else {
            throw std::runtime_error("Invalid assignment target");
        }
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
                    flushExecutorOutput(executor);  // Phase 11.1: Flush captured output

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
                    auto saved_returning = returning_;  // ISS-003: Save returning_ flag
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
                    returning_ = saved_returning;  // ISS-003: Restore returning_ flag
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
                    flushExecutorOutput(executor);  // Phase 11.1: Flush captured output

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
                    auto saved_returning = returning_;  // ISS-003: Save returning_ flag
                    current_env_ = std::make_shared<Environment>(global_env_);

                    if (!(*func)->params.empty()) {
                        current_env_->define((*func)->params[0], left);
                    }

                    (*func)->body->accept(*this);
                    current_env_ = saved_env;
                    returning_ = saved_returning;  // ISS-003: Restore returning_ flag
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

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
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

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
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
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
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
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
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
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
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

            // ISS-022: Create new environment with closure as parent (lexical scoping)
            auto parent_env = func->closure ? func->closure : global_env_;
            auto func_env = std::make_shared<Environment>(parent_env);

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

            // Phase 2.4.4 Phase 3: Handle generic type arguments (explicit or inferred)
            std::map<std::string, ast::Type> type_substitutions;
            if (!func->type_parameters.empty()) {
                fmt::print("[INFO] Function {} is generic with type parameters: ", func->name);
                for (const auto& tp : func->type_parameters) {
                    fmt::print("{} ", tp);
                }
                fmt::print("\n");

                // Check if explicit type arguments were provided
                const auto& explicit_type_args = node.getTypeArguments();
                if (!explicit_type_args.empty()) {
                    // Use explicit type arguments
                    fmt::print("[INFO] Using {} explicit type argument(s)\n", explicit_type_args.size());

                    if (explicit_type_args.size() != func->type_parameters.size()) {
                        throw std::runtime_error(fmt::format(
                            "Function {} expects {} type parameter(s), got {}",
                            func->name, func->type_parameters.size(), explicit_type_args.size()));
                    }

                    for (size_t i = 0; i < func->type_parameters.size(); i++) {
                        type_substitutions.insert({func->type_parameters[i], explicit_type_args[i]});
                        fmt::print("[INFO] Type parameter {} = {}\n",
                                  func->type_parameters[i],
                                  formatTypeName(explicit_type_args[i]));
                    }
                } else {
                    // Infer type arguments from actual arguments
                    auto inferred_types = inferGenericArgs(func, args);

                    // Build substitution map
                    for (size_t i = 0; i < func->type_parameters.size() && i < inferred_types.size(); i++) {
                        type_substitutions.insert({func->type_parameters[i], inferred_types[i]});
                    }
                }
            }

            // Phase 2.4.2 & 2.4.5: Validate argument types (union types and null safety)
            for (size_t i = 0; i < args.size(); i++) {
                // Phase 2.4.4 Phase 3: Substitute type parameters in param type
                ast::Type param_type = func->param_types[i];
                if (!type_substitutions.empty()) {
                    param_type = substituteTypeParams(param_type, type_substitutions);
                }

                // Phase 2.4.5: Null safety - cannot pass null to non-nullable parameter
                if (!param_type.is_nullable && isNull(args[i])) {
                    throw std::runtime_error(
                        "Null safety error: Cannot pass null to non-nullable parameter '" +
                        func->params[i] + "' of function '" + func->name + "'" +
                        "\n  Expected: " + formatTypeName(param_type) +
                        "\n  Got: null" +
                        "\n  Help: Change parameter to nullable: " +
                        formatTypeName(param_type) + "?"
                    );
                }

                // Check union types
                if (param_type.kind == ast::TypeKind::Union) {
                    if (!valueMatchesUnion(args[i], param_type.union_types)) {
                        throw std::runtime_error(
                            "Type error: Parameter '" + func->params[i] +
                            "' of function '" + func->name +
                            "' expects " + formatTypeName(param_type) +
                            ", but got " + getValueTypeName(args[i])
                        );
                    }
                }
                // Check non-union types (if not Any)
                else if (param_type.kind != ast::TypeKind::Any) {
                    if (!valueMatchesType(args[i], param_type)) {
                        throw std::runtime_error(
                            "Type error: Parameter '" + func->params[i] +
                            "' of function '" + func->name +
                            "' expects " + formatTypeName(param_type) +
                            ", but got " + getValueTypeName(args[i])
                        );
                    }
                }
            }

            // ISS-022: Create new environment for function execution with closure as parent
            auto parent_env = func->closure ? func->closure : global_env_;
            auto func_env = std::make_shared<Environment>(parent_env);

            // Phase 2.1: Bind provided arguments (ref vs value semantics)
            for (size_t i = 0; i < args.size(); i++) {
                if (func->param_types[i].is_reference) {
                    // Reference parameter: pass the shared_ptr directly (share the value)
                    func_env->define(func->params[i], args[i]);
                } else {
                    // Value parameter: copy the value (default behavior)
                    func_env->define(func->params[i], copyValue(args[i]));
                }
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

                    // Phase 2.1: Apply ref vs value semantics to default parameters
                    if (func->param_types[i].is_reference) {
                        func_env->define(func->params[i], default_val);
                    } else {
                        func_env->define(func->params[i], copyValue(default_val));
                    }
                } else {
                    throw std::runtime_error(fmt::format(
                        "Function {} parameter {} has no default value",
                        func->name, func->params[i]));
                }
            }

            // Save current environment and function
            auto saved_env = current_env_;
            auto saved_returning = returning_;
            auto saved_function = current_function_;  // Phase 2.4.2: Save for return type validation
            auto saved_type_subst = current_type_substitutions_;  // Phase 2.4.4: Save type substitutions
            auto saved_file = current_file_;  // Phase 3.1: Save current file for cross-module calls
            current_env_ = func_env;
            returning_ = false;
            current_function_ = func;  // Phase 2.4.2: Track current function
            current_type_substitutions_ = type_substitutions;  // Phase 2.4.4: Set type substitutions for generics
            current_file_ = func->source_file;  // Phase 3.1: Set file to function's source file

            // Phase 4.1: Push stack frame for error reporting
            pushStackFrame(func->name, func->source_line);  // Phase 3.1: Use actual line number

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
                current_function_ = saved_function;  // Phase 2.4.2: Restore
                current_type_substitutions_ = saved_type_subst;  // Phase 2.4.4: Restore
                current_file_ = saved_file;  // Phase 3.1: Restore file
                throw;  // Re-throw with stack frame info already captured
            }

            // Pop call frame if debugger is active
            if (debugger_ && debugger_->isActive()) {
                debugger_->popFrame();
            }

            // Phase 4.1: Pop stack frame
            popStackFrame();

            // Restore environment and function
            current_env_ = saved_env;
            returning_ = saved_returning;
            current_function_ = saved_function;  // Phase 2.4.2: Restore
            current_type_substitutions_ = saved_type_subst;  // Phase 2.4.4: Restore
            current_file_ = saved_file;  // Phase 3.1: Restore file

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
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output

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
    // Phase 2.4.2: typeof operator for union type checking
    else if (func_name == "typeof") {
        if (args.empty()) {
            throw std::runtime_error("typeof() requires exactly 1 argument");
        }
        if (args.size() > 1) {
            throw std::runtime_error("typeof() requires exactly 1 argument, got " + std::to_string(args.size()));
        }
        std::string type_name = getValueTypeName(args[0]);
        result_ = std::make_shared<Value>(type_name);
    }
    // Phase 3.2: Manual garbage collection trigger
    else if (func_name == "gc_collect") {
        runGarbageCollection(current_env_);  // Pass current environment
        result_ = std::make_shared<Value>();  // Return void
    }
    else {
        // Function not found
        throw std::runtime_error("Undefined function: " + func_name);
    }

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::MemberExpr& node) {
    std::string member_name = node.getMember();

    // Phase 2.4.3: Check for enum member access first (EnumName.VariantName)
    // If the object is a simple identifier, check if EnumName.VariantName exists
    auto* id_expr = dynamic_cast<ast::IdentifierExpr*>(node.getObject());
    if (id_expr) {
        std::string qualified_name = id_expr->getName() + "." + member_name;
        if (current_env_->has(qualified_name)) {
            result_ = current_env_->get(qualified_name);
            return;
        }
    }

    auto obj = eval(*node.getObject());

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

        // Phase 4.0: Check if it's a module marker (Rust-style use statements)
        if (marker.substr(0, 11) == "__module__:") {
            std::string module_path = marker.substr(11);

            // Look up the module in the registry
            modules::NaabModule* module = module_registry_->getModule(module_path);
            if (!module) {
                throw std::runtime_error("Module not found: " + module_path);
            }

            // Get module environment and look up member
            auto module_env = module->getEnvironment();
            if (!module_env) {
                throw std::runtime_error("Module not executed: " + module_path);
            }

            if (!module_env->has(member_name)) {
                // Check if it's marked as exported
                // For now, we allow access to all items in the module
                // TODO: Enforce export visibility
                throw std::runtime_error(
                    fmt::format("Module '{}' has no member '{}'", module_path, member_name)
                );
            }

            result_ = module_env->get(member_name);
            return;
        }
    }

    // Member access on other types (TODO)
    throw std::runtime_error("Member access not supported on this type");
}

void Interpreter::visit(ast::IdentifierExpr& node) {
    // Get all names from current scope for better error suggestions
    auto all_names = current_env_->getAllNames();

    try {
        result_ = current_env_->get(node.getName());
    } catch (const std::runtime_error& e) {
        // Phase 3.1: Enhanced error reporting with source context and suggestions from current scope
        if (!source_code_.empty()) {
            auto loc = node.getLocation();

            // Generate main error message
            std::string main_msg = "Undefined variable: " + node.getName();

            // Generate suggestion from current scope (not from parent where error was thrown)
            auto suggestion = error::suggestForUndefinedVariable(node.getName(), all_names);

            error_reporter_.error(main_msg, loc.line, loc.column);
            if (!suggestion.empty()) {
                error_reporter_.addSuggestion(suggestion);
            }

            error_reporter_.printAllWithSource();
            error_reporter_.clear();
        }

        // BUGFIX: Convert to NaabError with full call stack for cross-module error reporting
        throw createError(e.what(), ErrorType::RUNTIME_ERROR);
    }
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

        case ast::LiteralKind::Null:
            result_ = std::make_shared<Value>();  // monostate represents null
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

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::ListExpr& node) {
    std::vector<std::shared_ptr<Value>> list;
    for (auto& elem : node.getElements()) {
        list.push_back(eval(*elem));
    }
    result_ = std::make_shared<Value>(list);

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::RangeExpr& node) {
    // Evaluate start and end expressions
    auto start_val = eval(*node.getStart());
    auto end_val = eval(*node.getEnd());

    // Both must be integers
    int start = start_val->toInt();
    int end = end_val->toInt();

    // Create a special dict to represent the range
    // This is a lightweight representation - actual values generated during iteration
    std::unordered_map<std::string, std::shared_ptr<Value>> range_dict;
    range_dict["__is_range"] = std::make_shared<Value>(true);
    range_dict["__range_start"] = std::make_shared<Value>(start);
    range_dict["__range_end"] = std::make_shared<Value>(end);
    range_dict["__range_inclusive"] = std::make_shared<Value>(node.isInclusive());

    result_ = std::make_shared<Value>(range_dict);

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::StructLiteralExpr& node) {
    explain("Creating instance of struct '" + node.getStructName() + "'");

    profileStart("Struct creation");

    std::shared_ptr<StructDef> struct_def;
    std::string struct_name = node.getStructName();

    // ISS-024 Fix: Check for module-qualified struct name (module.StructName)
    size_t dot_pos = struct_name.find('.');
    if (dot_pos != std::string::npos) {
        // Split into module and struct name
        std::string module_alias = struct_name.substr(0, dot_pos);
        std::string actual_struct_name = struct_name.substr(dot_pos + 1);

        // Look up module environment
        auto module_it = loaded_modules_.find(module_alias);
        if (module_it == loaded_modules_.end()) {
            throw std::runtime_error("Module not found: " + module_alias);
        }

        // Get struct from module's exported structs
        auto& module_env = module_it->second;
        auto struct_it = module_env->exported_structs_.find(actual_struct_name);
        if (struct_it == module_env->exported_structs_.end()) {
            throw std::runtime_error("Struct '" + actual_struct_name +
                                   "' not found in module '" + module_alias + "'");
        }
        struct_def = struct_it->second;
        struct_name = actual_struct_name;  // Use unqualified name for error messages
    } else {
        // Normal struct lookup
        struct_def = runtime::StructRegistry::instance().getStruct(struct_name);
        if (!struct_def) {
            throw std::runtime_error("Undefined struct: " + struct_name);
        }
    }

    // Phase 2.4.1: Handle generic structs via monomorphization
    std::shared_ptr<StructDef> actual_def = struct_def;
    std::string actual_type_name = struct_name;  // ISS-024 Fix: Use unqualified name

    if (!struct_def->type_parameters.empty()) {
        // This is a generic struct - infer type arguments from field values
        auto type_bindings = inferTypeBindings(
            struct_def->type_parameters,
            struct_def->fields,
            node.getFieldInits()
        );

        // Create monomorphized version
        actual_def = monomorphizeStruct(struct_def, type_bindings);
        actual_type_name = actual_def->name;  // Use mangled name (e.g., "Box_int")

        // Register the specialized struct if not already registered
        if (!runtime::StructRegistry::instance().getStruct(actual_type_name)) {
            runtime::StructRegistry::instance().registerStruct(actual_def);

            if (isVerboseMode()) {
                fmt::print("[VERBOSE] Monomorphized {} -> {}\n",
                           node.getStructName(), actual_type_name);
            }
        }
    }

    auto struct_val = std::make_shared<StructValue>();
    struct_val->type_name = actual_type_name;
    struct_val->definition = actual_def;
    struct_val->field_values.resize(actual_def->fields.size());

    // Initialize from literals
    for (const auto& [field_name, init_expr] : node.getFieldInits()) {
        if (!actual_def->field_index.count(field_name)) {
            throw std::runtime_error("Unknown field '" + field_name +
                                   "' in struct '" + node.getStructName() + "'");
        }

        auto field_value = eval(*init_expr);
        size_t idx = actual_def->field_index.at(field_name);

        // Phase 2.4.2: Validate field value type matches declared type
        const ast::Type& field_type = actual_def->fields[idx].type;

        // Check union field types
        if (field_type.kind == ast::TypeKind::Union) {
            if (!valueMatchesUnion(field_value, field_type.union_types)) {
                throw std::runtime_error(
                    "Type error: Field '" + field_name +
                    "' of struct '" + node.getStructName() +
                    "' expects " + formatTypeName(field_type) +
                    ", but got " + getValueTypeName(field_value)
                );
            }
        }
        // Check non-union field types (if not Any)
        else if (field_type.kind != ast::TypeKind::Any) {
            if (!valueMatchesType(field_value, field_type)) {
                throw std::runtime_error(
                    "Type error: Field '" + field_name +
                    "' of struct '" + node.getStructName() +
                    "' expects " + formatTypeName(field_type) +
                    ", but got " + getValueTypeName(field_value)
                );
            }
        }

        struct_val->field_values[idx] = field_value;
    }

    // Check required fields (all fields are currently required - no default values yet)
    for (size_t i = 0; i < actual_def->fields.size(); ++i) {
        if (!struct_val->field_values[i]) {
            const auto& field = actual_def->fields[i];
            throw std::runtime_error("Missing required field '" + field.name +
                                   "' in struct '" + node.getStructName() + "'");
        }
    }

    result_ = std::make_shared<Value>(struct_val);

    profileEnd("Struct creation");

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::InlineCodeExpr& node) {
    std::string language = node.getLanguage();
    std::string raw_code = node.getCode();
    const auto& bound_vars = node.getBoundVariables();  // Phase 2.2

    // Phase 2.2: Generate variable declarations for bound variables
    std::string var_declarations;
    for (const auto& var_name : bound_vars) {
        // Look up variable in current environment
        if (!current_env_->has(var_name)) {
            throw std::runtime_error("Variable '" + var_name + "' not found in scope for inline code binding");
        }

        auto value = current_env_->get(var_name);
        std::string serialized = serializeValueForLanguage(value, language);

        // Generate declaration based on language
        if (language == "python") {
            var_declarations += var_name + " = " + serialized + "\n";
        } else if (language == "javascript" || language == "js") {
            var_declarations += "const " + var_name + " = " + serialized + ";\n";
        } else if (language == "shell" || language == "sh" || language == "bash") {
            var_declarations += var_name + "=" + serialized + "\n";
        } else if (language == "go") {
            var_declarations += "const " + var_name + " = " + serialized + "\n";
        } else if (language == "rust") {
            var_declarations += "let " + var_name + " = " + serialized + ";\n";
        } else if (language == "cpp" || language == "c++") {
            var_declarations += "const auto " + var_name + " = " + serialized + ";\n";
        } else if (language == "ruby") {
            var_declarations += var_name + " = " + serialized + "\n";
        } else if (language == "csharp" || language == "cs") {
            var_declarations += "var " + var_name + " = " + serialized + ";\n";
        }
    }

    // Strip common leading whitespace from all lines
    std::vector<std::string> lines;
    std::istringstream stream(raw_code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Find minimum indentation (ignoring empty lines and first line)
    size_t min_indent = std::string::npos;
    for (size_t i = 1; i < lines.size(); ++i) {  // Skip first line (i=0)
        const auto& l = lines[i];
        if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) continue;
        size_t indent = l.find_first_not_of(" \t");
        if (indent < min_indent) min_indent = indent;
    }

    // Strip the common indentation from all lines (except first)
    std::string code;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];

        if (i == 0) {
            // Keep first line as-is
            code += l + "\n";
        } else if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) {
            code += "\n";
        } else {
            if (min_indent != std::string::npos && l.length() > min_indent) {
                code += l.substr(min_indent) + "\n";
            } else {
                code += l + "\n";
            }
        }
    }

    // Phase 2.2: Prepend variable declarations
    std::string final_code = var_declarations + code;

    explain("Executing inline " + language + " code" +
            (bound_vars.empty() ? "" : " with " + std::to_string(bound_vars.size()) + " bound variables"));

    // Get the executor for this language
    auto& registry = runtime::LanguageRegistry::instance();
    auto* executor = registry.getExecutor(language);
    if (!executor) {
        throw std::runtime_error("No executor found for language: " + language);
    }

    // Phase 2.3: Execute the code and capture return value
    try {
        result_ = executor->executeWithReturn(final_code);

        // Flush stdout from executor
        flushExecutorOutput(executor);

    } catch (const std::exception& e) {
        throw std::runtime_error("Inline " + language + " execution failed: " + e.what());
    }
}

// ============================================================================
// StructValue Methods
// ============================================================================

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

// Phase 2.1: Deep copy a value (for value parameters)
std::shared_ptr<Value> Interpreter::copyValue(const std::shared_ptr<Value>& value) {
    if (!value) {
        return std::make_shared<Value>();
    }

    // Check variant type and copy accordingly
    if (std::holds_alternative<int>(value->data)) {
        return std::make_shared<Value>(std::get<int>(value->data));
    } else if (std::holds_alternative<double>(value->data)) {
        return std::make_shared<Value>(std::get<double>(value->data));
    } else if (std::holds_alternative<bool>(value->data)) {
        return std::make_shared<Value>(std::get<bool>(value->data));
    } else if (std::holds_alternative<std::string>(value->data)) {
        return std::make_shared<Value>(std::get<std::string>(value->data));
    } else if (std::holds_alternative<std::monostate>(value->data)) {
        return std::make_shared<Value>();
    } else if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data)) {
        // Deep copy list
        const auto& list = std::get<std::vector<std::shared_ptr<Value>>>(value->data);
        std::vector<std::shared_ptr<Value>> new_list;
        for (const auto& item : list) {
            new_list.push_back(copyValue(item));  // Recursive copy
        }
        return std::make_shared<Value>(new_list);
    } else if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data)) {
        // Deep copy dict
        const auto& dict = std::get<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data);
        std::unordered_map<std::string, std::shared_ptr<Value>> new_dict;
        for (const auto& [key, val] : dict) {
            new_dict[key] = copyValue(val);  // Recursive copy
        }
        return std::make_shared<Value>(new_dict);
    } else if (std::holds_alternative<std::shared_ptr<StructValue>>(value->data)) {
        // Deep copy struct
        const auto& struct_val = std::get<std::shared_ptr<StructValue>>(value->data);
        auto new_struct = std::make_shared<StructValue>(struct_val->type_name, struct_val->definition);
        for (size_t i = 0; i < struct_val->field_values.size(); i++) {
            new_struct->field_values[i] = copyValue(struct_val->field_values[i]);  // Recursive copy
        }
        return std::make_shared<Value>(new_struct);
    } else if (std::holds_alternative<std::shared_ptr<FunctionValue>>(value->data)) {
        // Functions are not copied - they're immutable, share the reference
        return value;
    } else if (std::holds_alternative<std::shared_ptr<BlockValue>>(value->data)) {
        // Blocks are not copied - they're immutable, share the reference
        return value;
    } else if (std::holds_alternative<std::shared_ptr<PythonObjectValue>>(value->data)) {
        // Python objects are not copied - they're managed by Python, share the reference
        return value;
    }

    // Default: return original (shouldn't reach here)
    return value;
}

// Phase 2.2: Serialize a value for injection into target language
std::string Interpreter::serializeValueForLanguage(const std::shared_ptr<Value>& value, const std::string& language) {
    if (!value) {
        return "null";
    }

    // Int
    if (std::holds_alternative<int>(value->data)) {
        return std::to_string(std::get<int>(value->data));
    }

    // Float
    if (std::holds_alternative<double>(value->data)) {
        return std::to_string(std::get<double>(value->data));
    }

    // String
    if (std::holds_alternative<std::string>(value->data)) {
        const auto& str = std::get<std::string>(value->data);

        // Shell doesn't need quotes for simple values
        if (language == "shell" || language == "sh" || language == "bash") {
            // Escape shell special characters
            std::string escaped;
            for (char c : str) {
                if (c == ' ' || c == '$' || c == '`' || c == '"' || c == '\'' || c == '\\') {
                    escaped += '\\';
                }
                escaped += c;
            }
            return escaped;
        }

        // Other languages need quoted strings with escaping
        std::string escaped;
        for (char c : str) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else if (c == '\n') escaped += "\\n";
            else if (c == '\t') escaped += "\\t";
            else escaped += c;
        }
        return "\"" + escaped + "\"";
    }

    // Bool
    if (std::holds_alternative<bool>(value->data)) {
        bool b = std::get<bool>(value->data);
        if (language == "python") {
            return b ? "True" : "False";
        }
        return b ? "true" : "false";
    }

    // Null/void
    if (std::holds_alternative<std::monostate>(value->data)) {
        if (language == "python") return "None";
        return "null";
    }

    // List - serialize as JSON array for simplicity
    if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data)) {
        const auto& list = std::get<std::vector<std::shared_ptr<Value>>>(value->data);
        std::string result = "[";
        for (size_t i = 0; i < list.size(); i++) {
            if (i > 0) result += ", ";
            result += serializeValueForLanguage(list[i], language);
        }
        result += "]";
        return result;
    }

    // Dict - serialize as JSON object
    if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data)) {
        const auto& dict = std::get<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data);
        std::string result = "{";
        bool first = true;
        for (const auto& [key, val] : dict) {
            if (!first) result += ", ";
            first = false;
            result += "\"" + key + "\": " + serializeValueForLanguage(val, language);
        }
        result += "}";
        return result;
    }

    // Struct - serialize as JSON object with field names
    if (std::holds_alternative<std::shared_ptr<StructValue>>(value->data)) {
        const auto& struct_val = std::get<std::shared_ptr<StructValue>>(value->data);
        std::string result = "{";
        bool first = true;
        for (size_t i = 0; i < struct_val->definition->fields.size(); i++) {
            if (!first) result += ", ";
            first = false;
            const auto& field = struct_val->definition->fields[i];
            result += "\"" + field.name + "\": " + serializeValueForLanguage(struct_val->field_values[i], language);
        }
        result += "}";
        return result;
    }

    // Unsupported types
    return "null";
}

// ============================================================================
// Phase 2.4.1: Generics/Monomorphization Implementation
// ============================================================================

// Infer the type of a runtime value
ast::Type Interpreter::inferValueType(const std::shared_ptr<Value>& value) {
    if (std::holds_alternative<int>(value->data)) {
        return ast::Type::makeInt();
    } else if (std::holds_alternative<double>(value->data)) {
        return ast::Type::makeFloat();
    } else if (std::holds_alternative<std::string>(value->data)) {
        return ast::Type::makeString();
    } else if (std::holds_alternative<bool>(value->data)) {
        return ast::Type::makeBool();
    } else if (std::holds_alternative<std::monostate>(value->data)) {
        return ast::Type::makeVoid();
    } else if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data)) {
        // For lists, try to infer element type from first element
        const auto& list = std::get<std::vector<std::shared_ptr<Value>>>(value->data);
        if (!list.empty()) {
            ast::Type list_type(ast::TypeKind::List);
            list_type.element_type = std::make_shared<ast::Type>(inferValueType(list[0]));
            return list_type;
        }
        return ast::Type(ast::TypeKind::List);
    } else if (std::holds_alternative<std::shared_ptr<StructValue>>(value->data)) {
        const auto& struct_val = std::get<std::shared_ptr<StructValue>>(value->data);
        return ast::Type::makeStruct(struct_val->type_name);
    }

    return ast::Type::makeAny();
}

// Infer type bindings for generic struct from field initializers
std::map<std::string, ast::Type> Interpreter::inferTypeBindings(
    const std::vector<std::string>& type_params,
    const std::vector<ast::StructField>& fields,
    const std::vector<std::pair<std::string, std::unique_ptr<ast::Expr>>>& field_inits
) {
    std::map<std::string, ast::Type> bindings;

    // For each field initializer, match it against the field definition
    for (const auto& [field_name, init_expr] : field_inits) {
        // Find corresponding field definition
        for (const auto& field : fields) {
            if (field.name == field_name) {
                // Check if this field's type is a type parameter
                if (field.type.kind == ast::TypeKind::TypeParameter) {
                    // Infer the type from the initializer value
                    auto init_value = eval(*init_expr);
                    ast::Type inferred = inferValueType(init_value);
                    // Use insert instead of operator[] to avoid default construction
                    bindings.insert({field.type.type_parameter_name, inferred});
                }
                break;
            }
        }
    }

    return bindings;
}

// Substitute type parameters with concrete types
ast::Type Interpreter::substituteType(
    const ast::Type& type,
    const std::map<std::string, ast::Type>& bindings
) {
    // If this is a type parameter, substitute it
    if (type.kind == ast::TypeKind::TypeParameter) {
        auto it = bindings.find(type.type_parameter_name);
        if (it != bindings.end()) {
            return it->second;
        }
        // Unbound parameter - return as-is
        return type;
    }

    // For list types, recursively substitute element type
    if (type.kind == ast::TypeKind::List && type.element_type) {
        ast::Type result = type;
        result.element_type = std::make_shared<ast::Type>(
            substituteType(*type.element_type, bindings)
        );
        return result;
    }

    // For dict types, recursively substitute key and value types
    if (type.kind == ast::TypeKind::Dict && type.key_value_types) {
        ast::Type result = type;
        result.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(
            substituteType(type.key_value_types->first, bindings),
            substituteType(type.key_value_types->second, bindings)
        );
        return result;
    }

    // For struct types with type arguments, substitute each argument
    if (type.kind == ast::TypeKind::Struct && !type.type_arguments.empty()) {
        ast::Type result = type;
        result.type_arguments.clear();
        for (const auto& arg : type.type_arguments) {
            result.type_arguments.push_back(substituteType(arg, bindings));
        }
        return result;
    }

    // No substitution needed
    return type;
}

// Create a specialized (monomorphized) version of a generic struct
std::shared_ptr<StructDef> Interpreter::monomorphizeStruct(
    const std::shared_ptr<StructDef>& generic_def,
    const std::map<std::string, ast::Type>& type_bindings
) {
    // Create specialized field list with substituted types
    std::vector<ast::StructField> specialized_fields;
    for (const auto& field : generic_def->fields) {
        // Construct new StructField explicitly (can't copy due to unique_ptr)
        ast::StructField specialized_field{
            field.name,
            substituteType(field.type, type_bindings),
            std::nullopt  // Default value not needed at runtime
        };
        specialized_fields.push_back(std::move(specialized_field));
    }

    // Create mangled name for specialized struct (e.g., "Box_int")
    std::string mangled_name = generic_def->name;
    for (const auto& param : generic_def->type_parameters) {
        auto it = type_bindings.find(param);
        if (it != type_bindings.end()) {
            mangled_name += "_";
            if (it->second.kind == ast::TypeKind::Int) {
                mangled_name += "int";
            } else if (it->second.kind == ast::TypeKind::Float) {
                mangled_name += "float";
            } else if (it->second.kind == ast::TypeKind::String) {
                mangled_name += "string";
            } else if (it->second.kind == ast::TypeKind::Bool) {
                mangled_name += "bool";
            } else if (it->second.kind == ast::TypeKind::Struct) {
                mangled_name += it->second.struct_name;
            } else {
                mangled_name += "any";
            }
        }
    }

    // Create specialized struct definition (no type parameters)
    auto specialized_def = std::make_shared<StructDef>(
        mangled_name,
        std::move(specialized_fields),  // Move to avoid copying
        std::vector<std::string>{}  // No type parameters in specialized version
    );

    return specialized_def;
}

// ============================================================================
// Phase 2.4.2: Union Type Validation Implementation
// ============================================================================

// Check if a value's runtime type matches a declared type
bool Interpreter::valueMatchesType(
    const std::shared_ptr<Value>& value,
    const ast::Type& type
) {
    // Phase 2.4.5: If type is nullable and value is null, that's valid
    if (type.is_nullable && isNull(value)) {
        return true;
    }

    // Handle union types specially
    if (type.kind == ast::TypeKind::Union) {
        return valueMatchesUnion(value, type.union_types);
    }

    // Check specific types
    if (type.kind == ast::TypeKind::Int) {
        return std::holds_alternative<int>(value->data);
    } else if (type.kind == ast::TypeKind::Float) {
        return std::holds_alternative<double>(value->data);
    } else if (type.kind == ast::TypeKind::String) {
        return std::holds_alternative<std::string>(value->data);
    } else if (type.kind == ast::TypeKind::Bool) {
        return std::holds_alternative<bool>(value->data);
    } else if (type.kind == ast::TypeKind::Void) {
        return std::holds_alternative<std::monostate>(value->data);
    } else if (type.kind == ast::TypeKind::List) {
        return std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data);
    } else if (type.kind == ast::TypeKind::Dict) {
        return std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data);
    } else if (type.kind == ast::TypeKind::Struct) {
        if (auto* struct_val = std::get_if<std::shared_ptr<StructValue>>(&value->data)) {
            // Check if struct name matches (or is a specialization of a generic struct)
            // For now, exact match or prefix match for specialized generics
            const std::string& actual_name = (*struct_val)->type_name;
            std::string expected_name = type.struct_name;

            // ISS-024 Fix: If type has module prefix, compare without it
            // e.g., if expected is "types.Config", just compare "Config"
            if (!type.module_prefix.empty()) {
                // Module-qualified type - use only the struct name part
                expected_name = type.struct_name;  // This already has module prefix stripped by parser
            }

            // Exact match
            if (actual_name == expected_name) {
                return true;
            }

            // Check if actual is a specialization of expected (e.g., Box_int is a Box)
            std::string prefix = expected_name + "_";
            if (actual_name.size() >= prefix.size() &&
                actual_name.substr(0, prefix.size()) == prefix) {
                return true;
            }

            return false;
        }
        return false;
    } else if (type.kind == ast::TypeKind::Function) {
        // ISS-002: Check if value is a function
        return std::holds_alternative<std::shared_ptr<FunctionValue>>(value->data);
    } else if (type.kind == ast::TypeKind::Enum) {
        // Phase 4.1: Enum types - enum variants are integers at runtime
        return std::holds_alternative<int>(value->data);
    }

    // Any type matches anything
    if (type.kind == ast::TypeKind::Any) {
        return true;
    }

    return false;
}

// Check if a value matches any type in a union
bool Interpreter::valueMatchesUnion(
    const std::shared_ptr<Value>& value,
    const std::vector<ast::Type>& union_types
) {
    for (const auto& type : union_types) {
        if (valueMatchesType(value, type)) {
            return true;
        }
    }
    return false;
}

// Get runtime type name of a value
std::string Interpreter::getValueTypeName(const std::shared_ptr<Value>& value) {
    if (std::holds_alternative<int>(value->data)) {
        return "int";
    } else if (std::holds_alternative<double>(value->data)) {
        return "float";
    } else if (std::holds_alternative<std::string>(value->data)) {
        return "string";
    } else if (std::holds_alternative<bool>(value->data)) {
        return "bool";
    } else if (std::holds_alternative<std::monostate>(value->data)) {
        return "null";
    } else if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data)) {
        return "list";
    } else if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data)) {
        return "dict";
    } else if (auto* struct_val = std::get_if<std::shared_ptr<StructValue>>(&value->data)) {
        return (*struct_val)->type_name;
    } else if (std::holds_alternative<std::shared_ptr<FunctionValue>>(value->data)) {
        return "function";
    } else if (std::holds_alternative<std::shared_ptr<BlockValue>>(value->data)) {
        return "block";
    }
    return "unknown";
}

// Format a type name for error messages
std::string Interpreter::formatTypeName(const ast::Type& type) {
    std::string base_name;

    if (type.kind == ast::TypeKind::Int) base_name = "int";
    else if (type.kind == ast::TypeKind::Float) base_name = "float";
    else if (type.kind == ast::TypeKind::String) base_name = "string";
    else if (type.kind == ast::TypeKind::Bool) base_name = "bool";
    else if (type.kind == ast::TypeKind::Void) base_name = "null";
    else if (type.kind == ast::TypeKind::List) base_name = "list";
    else if (type.kind == ast::TypeKind::Dict) base_name = "dict";
    else if (type.kind == ast::TypeKind::Any) base_name = "any";
    else if (type.kind == ast::TypeKind::Function) base_name = "function";  // ISS-002
    else if (type.kind == ast::TypeKind::Struct) base_name = type.struct_name;
    else if (type.kind == ast::TypeKind::Enum) base_name = type.enum_name;  // Phase 4.1
    else if (type.kind == ast::TypeKind::Union) {
        std::string result;
        for (size_t i = 0; i < type.union_types.size(); i++) {
            if (i > 0) result += " | ";
            result += formatTypeName(type.union_types[i]);
        }
        base_name = result;
    }
    else {
        base_name = "unknown";
    }

    // Phase 2.4.5: Add nullable marker
    if (type.is_nullable) {
        base_name += "?";
    }

    return base_name;
}

// Phase 2.4.5: Null safety helper - check if value is null
bool Interpreter::isNull(const std::shared_ptr<Value>& value) {
    // Null shared_ptr
    if (!value) {
        return true;
    }

    // Value holds std::monostate (null/void)
    return std::holds_alternative<std::monostate>(value->data);
}

// Phase 2.4.4: Type inference - infer ast::Type from runtime Value
ast::Type Interpreter::inferTypeFromValue(const std::shared_ptr<Value>& value) {
    // Handle null/void
    if (!value || std::holds_alternative<std::monostate>(value->data)) {
        // Null is ambiguous - could be any nullable type
        // For inference, we'll return nullable any
        ast::Type t = ast::Type::makeAny();
        t.is_nullable = true;
        return t;
    }

    // Check actual type held in variant
    if (std::holds_alternative<int>(value->data)) {
        return ast::Type::makeInt();
    }
    else if (std::holds_alternative<double>(value->data)) {
        return ast::Type::makeFloat();
    }
    else if (std::holds_alternative<std::string>(value->data)) {
        return ast::Type::makeString();
    }
    else if (std::holds_alternative<bool>(value->data)) {
        return ast::Type::makeBool();
    }
    else if (auto* list_val = std::get_if<std::vector<std::shared_ptr<Value>>>(&value->data)) {
        // Infer list element type from first element
        ast::Type list_type(ast::TypeKind::List);
        if (!list_val->empty()) {
            // Infer from first element
            list_type.element_type = std::make_shared<ast::Type>(inferTypeFromValue((*list_val)[0]));
        } else {
            // Empty list - use any as element type
            list_type.element_type = std::make_shared<ast::Type>(ast::Type::makeAny());
        }
        return list_type;
    }
    else if (auto* dict_val = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&value->data)) {
        // Infer dict key/value types from first entry
        ast::Type dict_type(ast::TypeKind::Dict);
        if (!dict_val->empty()) {
            // Keys are always strings in NAAb
            auto first_entry = dict_val->begin();
            auto key_type = ast::Type::makeString();
            auto value_type = inferTypeFromValue(first_entry->second);
            dict_type.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(key_type, value_type);
        } else {
            // Empty dict - use any for value type
            auto key_type = ast::Type::makeString();
            auto value_type = ast::Type::makeAny();
            dict_type.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(key_type, value_type);
        }
        return dict_type;
    }
    else if (auto* struct_val = std::get_if<std::shared_ptr<StructValue>>(&value->data)) {
        // Struct type
        return ast::Type(ast::TypeKind::Struct, (*struct_val)->type_name);
    }
    else if (std::holds_alternative<std::shared_ptr<FunctionValue>>(value->data)) {
        // ISS-002: Function type - now we have TypeKind::Function
        return ast::Type::makeFunction();
    }
    else if (std::holds_alternative<std::shared_ptr<BlockValue>>(value->data)) {
        // Block type
        return ast::Type(ast::TypeKind::Block);
    }
    else if (std::holds_alternative<std::shared_ptr<PythonObjectValue>>(value->data)) {
        // Python object - treat as any
        return ast::Type::makeAny();
    }

    // Unknown type - fallback to any
    return ast::Type::makeAny();
}

// Phase 2.4.4 Phase 2: Collect all return types from a statement (recursively)
void Interpreter::collectReturnTypes(ast::Stmt* stmt, std::vector<ast::Type>& return_types) {
    if (!stmt) return;

    // Check if this is a return statement
    if (auto* return_stmt = dynamic_cast<ast::ReturnStmt*>(stmt)) {
        if (return_stmt->getExpr()) {
            // Has a return value - try to evaluate it to get its type
            // BUGFIX: Catch errors during type inference - don't crash on undefined vars
            try {
                auto return_value = eval(*return_stmt->getExpr());
                ast::Type inferred_type = inferTypeFromValue(return_value);
                return_types.push_back(inferred_type);
            } catch (...) {
                // Can't infer type (e.g., undefined variable) - use unknown type
                return_types.push_back(ast::Type(ast::TypeKind::Any));
            }
        } else {
            // Return with no value -> void
            return_types.push_back(ast::Type::makeVoid());
        }
        return;
    }

    // Recursively search compound statements
    if (auto* compound = dynamic_cast<ast::CompoundStmt*>(stmt)) {
        for (auto& child : compound->getStatements()) {
            collectReturnTypes(child.get(), return_types);
        }
        return;
    }

    // Recursively search if statements
    if (auto* if_stmt = dynamic_cast<ast::IfStmt*>(stmt)) {
        collectReturnTypes(if_stmt->getThenBranch(), return_types);
        if (if_stmt->getElseBranch()) {
            collectReturnTypes(if_stmt->getElseBranch(), return_types);
        }
        return;
    }

    // Recursively search while loops
    if (auto* while_stmt = dynamic_cast<ast::WhileStmt*>(stmt)) {
        collectReturnTypes(while_stmt->getBody(), return_types);
        return;
    }

    // Recursively search for loops
    if (auto* for_stmt = dynamic_cast<ast::ForStmt*>(stmt)) {
        collectReturnTypes(for_stmt->getBody(), return_types);
        return;
    }

    // Other statement types don't contain returns
}

// Phase 2.4.4 Phase 2: Infer function return type from body
ast::Type Interpreter::inferReturnType(ast::Stmt* body) {
    std::vector<ast::Type> return_types;

    // Collect all return statement types
    collectReturnTypes(body, return_types);

    // If no return statements found, type is void
    if (return_types.empty()) {
        return ast::Type::makeVoid();
    }

    // If single return statement, use that type
    if (return_types.size() == 1) {
        return return_types[0];
    }

    // Multiple return statements - check if they're all the same type
    ast::Type first_type = return_types[0];
    bool all_same = true;
    for (size_t i = 1; i < return_types.size(); i++) {
        // Simple type equality check
        if (return_types[i].kind != first_type.kind) {
            all_same = false;
            break;
        }
    }

    if (all_same) {
        // All returns have the same type
        return first_type;
    }

    // Different return types - create union type
    // Phase 2.4.2: Use union types for multiple incompatible returns
    ast::Type union_type(ast::TypeKind::Union);
    union_type.union_types = return_types;
    return union_type;
}

// Phase 2.4.4 Phase 3: Collect type constraints from parameter-argument matching
void Interpreter::collectTypeConstraints(
    const ast::Type& param_type,
    const ast::Type& arg_type,
    std::map<std::string, ast::Type>& constraints
) {
    // If parameter is a type parameter (e.g., "T"), record the constraint
    if (param_type.kind == ast::TypeKind::TypeParameter) {
        const std::string& type_param_name = param_type.type_parameter_name;

        // If we already have a constraint for this type parameter, check compatibility
        auto it = constraints.find(type_param_name);
        if (it != constraints.end()) {
            // TODO: In a full implementation, we'd unify the types
            // For now, just check if they're the same
            if (it->second.kind != arg_type.kind) {
                fmt::print("[WARN] Type parameter {} has conflicting constraints\n", type_param_name);
            }
        } else {
            // Record new constraint
            constraints.insert({type_param_name, arg_type});
        }
        return;
    }

    // If parameter is a generic container (e.g., list<T>), recurse
    if (param_type.kind == ast::TypeKind::List && arg_type.kind == ast::TypeKind::List) {
        if (param_type.element_type && arg_type.element_type) {
            collectTypeConstraints(*param_type.element_type, *arg_type.element_type, constraints);
        }
        return;
    }

    if (param_type.kind == ast::TypeKind::Dict && arg_type.kind == ast::TypeKind::Dict) {
        if (param_type.key_value_types && arg_type.key_value_types) {
            collectTypeConstraints(param_type.key_value_types->first, arg_type.key_value_types->first, constraints);
            collectTypeConstraints(param_type.key_value_types->second, arg_type.key_value_types->second, constraints);
        }
        return;
    }

    // For concrete types, no constraints to collect
}

// Phase 2.4.4 Phase 3: Substitute type parameters with concrete types
ast::Type Interpreter::substituteTypeParams(
    const ast::Type& type,
    const std::map<std::string, ast::Type>& substitutions
) {
    // If this is a type parameter, substitute it
    if (type.kind == ast::TypeKind::TypeParameter) {
        auto it = substitutions.find(type.type_parameter_name);
        if (it != substitutions.end()) {
            return it->second;
        }
        // If not found in substitutions, return as-is
        return type;
    }

    // If this is a container type, recursively substitute element types
    if (type.kind == ast::TypeKind::List && type.element_type) {
        ast::Type substituted_type(ast::TypeKind::List);
        substituted_type.element_type = std::make_shared<ast::Type>(
            substituteTypeParams(*type.element_type, substitutions)
        );
        substituted_type.is_nullable = type.is_nullable;
        return substituted_type;
    }

    if (type.kind == ast::TypeKind::Dict && type.key_value_types) {
        ast::Type substituted_type(ast::TypeKind::Dict);
        substituted_type.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(
            substituteTypeParams(type.key_value_types->first, substitutions),
            substituteTypeParams(type.key_value_types->second, substitutions)
        );
        substituted_type.is_nullable = type.is_nullable;
        return substituted_type;
    }

    // For concrete types, return as-is
    return type;
}

// Phase 2.4.4 Phase 3: Infer generic type arguments from call site
std::vector<ast::Type> Interpreter::inferGenericArgs(
    const std::shared_ptr<FunctionValue>& func,
    const std::vector<std::shared_ptr<Value>>& args
) {
    // Build type constraint system
    std::map<std::string, ast::Type> constraints;

    // Match each argument to its parameter type
    for (size_t i = 0; i < args.size() && i < func->param_types.size(); i++) {
        const ast::Type& param_type = func->param_types[i];
        ast::Type arg_type = inferTypeFromValue(args[i]);

        // Collect constraints from this parameter-argument pair
        collectTypeConstraints(param_type, arg_type, constraints);
    }

    // Solve constraints: for each type parameter, look up its inferred type
    std::vector<ast::Type> type_args;
    for (const std::string& type_param : func->type_parameters) {
        auto it = constraints.find(type_param);
        if (it != constraints.end()) {
            type_args.push_back(it->second);
            fmt::print("[INFO] Inferred type argument {}: {}\n",
                       type_param, formatTypeName(it->second));
        } else {
            // Could not infer this type parameter
            fmt::print("[WARN] Could not infer type parameter {}, defaulting to Any\n", type_param);
            type_args.push_back(ast::Type::makeAny());
        }
    }

    return type_args;
}

// ============================================================================
// Phase 3.2: Garbage Collection
// ============================================================================

void Interpreter::runGarbageCollection(std::shared_ptr<Environment> env) {
    if (!cycle_detector_) {
        return;  // GC not initialized
    }

    if (!gc_enabled_) {
        return;  // GC disabled
    }

    fmt::print("[GC] Running garbage collection...\n");

    // Use provided environment or fall back to global environment
    auto root_env = env ? env : global_env_;

    // Run mark-and-sweep cycle detection with global value tracking
    size_t collected = cycle_detector_->detectAndCollect(root_env, tracked_values_);

    if (collected > 0) {
        fmt::print("[GC] Collected {} cyclic values\n", collected);
    } else {
        fmt::print("[GC] No cycles detected\n");
    }

    // Reset allocation counter after GC
    allocation_count_ = 0;
}

void Interpreter::registerValue(std::shared_ptr<Value> value) {
    if (!value || !gc_enabled_) {
        return;
    }
    tracked_values_.push_back(value);
}

void Interpreter::trackAllocation() {
    if (!gc_enabled_ || !cycle_detector_) {
        return;
    }

    allocation_count_++;

    // Register the newly created value for global tracking (complete GC)
    if (result_) {
        registerValue(result_);
    }

    // Trigger GC when threshold reached
    if (allocation_count_ >= gc_threshold_) {
        if (verbose_mode_) {
            fmt::print("[GC] Allocation threshold reached ({}/{}), triggering automatic GC\n",
                      allocation_count_, gc_threshold_);
        }
        runGarbageCollection(current_env_);
    }
}

size_t Interpreter::getGCCollectionCount() const {
    if (!cycle_detector_) {
        return 0;
    }
    return cycle_detector_->getTotalCollected();
}

} // namespace interpreter
} // namespace naab
