// NAAb Interpreter - Direct AST execution
// Core interpreter implementation

#include "naab/interpreter.h"
#include "naab/lexer.h"   // For string interpolation evaluation
#include "naab/parser.h"  // For string interpolation evaluation
#include "naab/limits.h"  // Week 1, Task 1.3: Call depth limits
#include "naab/error_helpers.h"
#include "naab/language_registry.h"
#include "naab/block_registry.h"
#include "naab/logger.h"  // Logging system
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#include "naab/python_executor_adapter.h"
#include "naab/shell_executor.h"  // Polyglot: Issue #2 - Shell environment variables
#include "naab/stdlib_new_modules.h"  // For ArrayModule type
#include "naab/struct_registry.h"
#include "cycle_detector.h"  // Phase 3.2: Garbage collection
#include "naab/polyglot_dependency_analyzer.h"  // Parallel polyglot execution
#include "naab/polyglot_async_executor.h"  // Parallel polyglot execution
#include "naab/sandbox.h"  // Enterprise security: Sandbox isolation
#include "naab/resource_limits.h"  // Enterprise security: Resource limits
#include "naab/source_mapper.h"  // Phase 12: Polyglot error mapping
#include "naab/json_result_parser.h"  // Phase 12: JSON sovereign pipe
#include <fmt/core.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <type_traits>
#include <stdexcept>
#include <climits>  // For INT_MAX, INT_MIN overflow detection
#include <filesystem>  // Phase 3.1: For module path resolution
#include <unordered_set>  // For constant lookup in stdlib modules
#include <map>  // Polyglot: Issue #2 - Shell environment variables
#include <algorithm>  // For std::transform in string methods

// Python embedding support
#ifdef __has_include
#  if __has_include(<Python.h>)
#    define NAAB_HAS_PYTHON 1
#    include <Python.h>
#  endif
#endif

namespace naab {
namespace interpreter {

// Issue #3: Global access to current interpreter (for stdlib path resolution)
thread_local Interpreter* g_current_interpreter = nullptr;

// Helper function to get type name as string for error messages
static std::string getTypeName(const std::shared_ptr<Value>& val) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            return "int";
        } else if constexpr (std::is_same_v<T, double>) {
            return "float";
        } else if constexpr (std::is_same_v<T, bool>) {
            return "bool";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return "string";
        } else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) {
            return "array";
        } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) {
            return "dict";
        } else if constexpr (std::is_same_v<T, std::shared_ptr<FunctionValue>>) {
            return "function";
        } else if constexpr (std::is_same_v<T, std::shared_ptr<StructValue>>) {
            return "struct";
        }
        return "unknown";
    }, val->data);
}

// Helper function to deep copy a Value (handles nested arrays/dicts)
// This prevents silent mutations through variable aliasing
static std::shared_ptr<Value> copyValue(const std::shared_ptr<Value>& val) {
    if (!val) return nullptr;

    return std::visit([](auto&& arg) -> std::shared_ptr<Value> {
        using T = std::decay_t<decltype(arg)>;

        // Monostate (null/undefined): use default constructor
        if constexpr (std::is_same_v<T, std::monostate>) {
            return std::make_shared<Value>();
        }
        // Primitives: copy directly (immutable in practice)
        else if constexpr (std::is_same_v<T, int> ||
                          std::is_same_v<T, double> ||
                          std::is_same_v<T, bool> ||
                          std::is_same_v<T, std::string>) {
            return std::make_shared<Value>(arg);
        }
        // Arrays: deep copy each element recursively
        else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) {
            std::vector<std::shared_ptr<Value>> new_vec;
            new_vec.reserve(arg.size());
            for (const auto& elem : arg) {
                new_vec.push_back(copyValue(elem));  // Recursive deep copy
            }
            return std::make_shared<Value>(new_vec);
        }
        // Dicts: deep copy each value recursively
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) {
            std::unordered_map<std::string, std::shared_ptr<Value>> new_dict;
            for (const auto& [key, val] : arg) {
                new_dict[key] = copyValue(val);  // Recursive deep copy
            }
            return std::make_shared<Value>(new_dict);
        }
        // Functions, blocks, structs, python objects: share (immutable or intentionally shared)
        else {
            return std::make_shared<Value>(arg);
        }
    }, val->data);
}

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
            // Use %g to trim trailing zeros (3.14 not 3.140000)
            char buf[64];
            snprintf(buf, sizeof(buf), "%.15g", arg);
            return std::string(buf);
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

    // LLM-friendly hints for common non-existent globals
    if (name == "Sys" || name == "System" || name == "sys") {
        error_msg += "\n\n  NAAb does not have a 'Sys' object. Use built-in functions directly:\n"
                     "    print(\"hello\")          // instead of Sys.print(\"hello\")\n"
                     "    print(\"error: oops\")    // instead of Sys.error(\"oops\")\n\n"
                     "  IMPORTANT - Sys.callFunction is NOT needed in NAAb:\n"
                     "    Functions are first-class values. Call them directly:\n"
                     "      let fn = someDict.get(\"myFunc\")\n"
                     "      let result = fn(arg1, arg2)    // NOT Sys.callFunction(fn, arg1, arg2)\n"
                     "      // or: someDict.myFunc(arg1, arg2)\n\n"
                     "  Common replacements:\n"
                     "    Sys.callFunction(fn, a, b) -> fn(a, b)\n"
                     "    Sys.print(msg)             -> print(msg)\n"
                     "    Sys.exit(code)             -> // just return or end the block\n\n"
                     "  Built-in functions: print, len, type, typeof, int, float, string, bool\n"
                     "  For sleep: import time; time.sleep(milliseconds)\n"
                     "  For exit:  NAAb has no exit(). End the main block or return from functions.";
    } else if (name == "Console" || name == "console") {
        error_msg += "\n\n  NAAb does not have a 'Console' object. Use:\n"
                     "    print(\"hello\")          // instead of Console.log(\"hello\")\n"
                     "    print(\"error: oops\")    // instead of Console.error(\"oops\")";
    } else if (name == "Math") {
        error_msg += "\n\n  NAAb math functions are in the 'math' module (lowercase):\n"
                     "    import math\n"
                     "    let x = math.sqrt(16)   // instead of Math.sqrt(16)\n"
                     "    let pi = math.PI";
    } else if (name == "Array") {
        error_msg += "\n\n  NAAb array functions are in the 'array' module (lowercase):\n"
                     "    import array\n"
                     "    array.push(myArr, item) // instead of Array.push(...)";
    } else if (name == "String") {
        error_msg += "\n\n  NAAb string functions are in the 'string' module (lowercase):\n"
                     "    import string\n"
                     "    string.upper(myStr)     // instead of String.toUpperCase(...)";
    } else if (name == "File" || name == "fs" || name == "FS") {
        error_msg += "\n\n  NAAb file functions are in the 'file' module:\n"
                     "    import file\n"
                     "    let content = file.read(\"path.txt\")";
    } else if (name == "sleep") {
        error_msg += "\n\n  'sleep' is not a global built-in. It's in the time module:\n"
                     "    import time\n"
                     "    time.sleep(1000)         // sleep for 1000 milliseconds";
    } else if (name == "exit") {
        error_msg += "\n\n  NAAb has no exit() function. To stop execution:\n"
                     "    return              // from a function\n"
                     "    // or just let the main block end naturally";
    } else if (name == "error") {
        error_msg += "\n\n  'error' is not a built-in function. To print errors:\n"
                     "    print(\"ERROR: something went wrong\")\n"
                     "  To throw an error:\n"
                     "    throw \"something went wrong\"";
    } else if (name == "require" || name == "include") {
        error_msg += "\n\n  NAAb uses 'import' for modules, not 'require':\n"
                     "    import \"path/to/module.naab\" as MyModule\n"
                     "    import math        // stdlib module";
    } else if (name == "callFunction") {
        error_msg += "\n\n  NAAb does not need callFunction(). Functions are first-class:\n"
                     "    let fn = myDict.get(\"funcName\")\n"
                     "    let result = fn(arg1, arg2)   // call directly\n"
                     "    // or: myDict.funcName(arg1, arg2)";
    } else if (name == "process" || name == "os" || name == "OS") {
        error_msg += "\n\n  NAAb does not have a '" + name + "' object.\n"
                     "    For environment variables: import env; env.get(\"PATH\")\n"
                     "    For command args: import env; let args = env.args()";
    } else if (name == "this" || name == "self") {
        error_msg += "\n\n  NAAb does not use '" + name + "'. In structs, access fields directly:\n"
                     "    struct Point { x: Int, y: Int }\n"
                     "  In closures/dicts, capture variables from the enclosing scope.";
    } else if (name == "new") {
        error_msg += "\n\n  NAAb does not use 'new'. Create struct instances directly:\n"
                     "    let p = Point { x: 1, y: 2 }\n"
                     "  For dicts: let d = {\"key\": \"value\"}";
    } else if (name == "None" || name == "nil" || name == "undefined") {
        error_msg += "\n\n  NAAb uses 'null' (not '" + name + "'):\n"
                     "    let x = null";
    } else if (name == "Object" || name == "Map") {
        error_msg += "\n\n  NAAb dicts are created with literal syntax:\n"
                     "    let d = {\"key\": \"value\"}\n"
                     "    d.get(\"key\")    // access values\n"
                     "    d.put(\"k\", v)   // set values";
    } else if (name == "JSON") {
        error_msg += "\n\n  NAAb does not have a JSON object. Dicts are native:\n"
                     "    let data = {\"key\": \"value\"}  // dict literal\n"
                     "    let val = data.get(\"key\")";
    } else {
        auto all_names = getAllNames();
        auto suggestion = error::suggestForUndefinedVariable(name, all_names);
        if (!suggestion.empty()) {
            error_msg += "\n  " + suggestion;
        }
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
      last_executed_block_id_(""),  // Phase 4.4: Initialize block pair tracking
      current_function_(nullptr),  // Phase 2.4.2: Initialize function tracking
      loop_depth_(0) {  // Initialize loop depth counter for break/continue validation
    // Phase 8: Skip BlockRegistry initialization for faster startup
    // It will be lazily initialized only when needed (in UseStatement)
    // This avoids loading 24,488 blocks unnecessarily for simple programs

    // BlockLoader (database) disabled - using filesystem-based BlockRegistry instead
    // BlockLoader is only used by CLI commands (blocks list/search/info) via BlockSearchIndex
    block_loader_ = nullptr;

    // Initialize Python interpreter
#ifdef NAAB_HAS_PYTHON
    if (!Py_IsInitialized()) {
        Py_Initialize();
        LOG_DEBUG("[INFO] Python interpreter initialized\n");
    }
#else
    fmt::print("[WARN] Python support not available (Python blocks disabled)\n");
#endif

    // Initialize C++ executor
    cpp_executor_ = std::make_unique<runtime::CppExecutor>();
    LOG_DEBUG("[INFO] C++ executor initialized\n");

    // Initialize standard library
    stdlib_ = std::make_unique<stdlib::StdLib>();
    LOG_DEBUG("[INFO] Standard library initialized: {} modules available\n",
               stdlib_->listModules().size());

    // Auto-import stdlib prelude (core modules available without 'use')
    std::vector<std::string> prelude_modules = {"array", "string", "io", "file", "debug"};
    for (const auto& mod_name : prelude_modules) {
        if (stdlib_->hasModule(mod_name)) {
            auto module = stdlib_->getModule(mod_name);
            imported_modules_[mod_name] = module;

            // Store module marker in global environment
            auto module_marker = std::make_shared<Value>(
                std::string("__stdlib_module__:" + mod_name)
            );
            global_env_->define(mod_name, module_marker);
        }
    }
    LOG_DEBUG("[INFO] Stdlib prelude auto-imported: array, string, io, file, debug\n");

    // Phase 3.1: Initialize module resolver
    module_resolver_ = std::make_unique<modules::ModuleResolver>();
    LOG_DEBUG("[INFO] Module resolver initialized\n");

    // Phase 4.0: Initialize module registry
    module_registry_ = std::make_unique<modules::ModuleRegistry>();
    LOG_DEBUG("[INFO] Module registry initialized (Phase 4.0)\n");

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
            LOG_DEBUG("[INFO] Array module configured with function evaluator\n");
        } else {
            fmt::print("[WARN] Failed to cast array module for function evaluator setup\n");
        }
    } else {
        fmt::print("[WARN] Array module not found for function evaluator setup\n");
    }

    // ISS-028: Set up args provider callback for env.get_args()
    auto env_module = stdlib_->getModule("env");
    if (env_module) {
        auto* env_mod = dynamic_cast<stdlib::EnvModule*>(env_module.get());
        if (env_mod) {
            // Create callback that returns this interpreter's script args
            env_mod->setArgsProvider(
                [this]() -> std::vector<std::string> {
                    return this->script_args_;
                }
            );
            LOG_DEBUG("[INFO] Env module configured with args provider\n");
        } else {
            fmt::print("[WARN] Failed to cast env module for args provider setup\n");
        }
    } else {
        fmt::print("[WARN] Env module not found for args provider setup\n");
    }

    // Phase 3.2: Initialize garbage collector
    cycle_detector_ = std::make_unique<CycleDetector>();
    LOG_DEBUG("[INFO] Garbage collector initialized (threshold: {} allocations)\n", gc_threshold_);

    // Issue #3: Set global interpreter pointer for stdlib path resolution
    g_current_interpreter = this;

    defineBuiltins();
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
    // ISS-035 FIX: Convert to absolute path for proper module resolution
    current_file_ = std::filesystem::absolute(filename).string();
    error_reporter_.setSource(source, filename);

    // Issue #3: Initialize file context stack with main script
    if (!filename.empty() && file_context_stack_.empty()) {
        pushFileContext(filename);
    }
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
    // Week 1, Task 1.3: Check call depth to prevent stack overflow
    if (++call_depth_ > limits::MAX_CALL_STACK_DEPTH) {
        --call_depth_;
        throw limits::RecursionLimitException(
            "Call stack depth exceeded: " +
            std::to_string(call_depth_) + " > " + std::to_string(limits::MAX_CALL_STACK_DEPTH)
        );
    }

    // Ensure depth is decremented on return
    struct CallDepthGuard {
        size_t& depth;
        explicit CallDepthGuard(size_t& d) : depth(d) {}
        ~CallDepthGuard() { --depth; }
    } guard(call_depth_);

    // Check if it's a function value
    auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&fn->data);
    if (!func_ptr) {
        std::ostringstream oss;
        oss << "Type error: Cannot call non-function value\n\n";
        oss << "  Attempted to call: " << getTypeName(fn) << "\n";
        oss << "  Expected: function\n\n";
        oss << "  Help:\n";
        oss << "  - Only functions can be called with ()\n";
        oss << "  - Check if the variable holds a function\n";
        oss << "  - Use typeof() or debug.type() to inspect the type\n\n";
        oss << "  Example:\n";
        oss << "    ✗ Wrong: let x = 42; x()  // calling an int\n";
        oss << "    ✓ Right: let f = function() { ... }; f()\n";
        throw std::runtime_error(oss.str());
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
        // Build parameter list for error message
        std::ostringstream oss;
        oss << "Function " << func->name << " expects " << min_args << "-"
            << func->params.size() << " arguments, got " << args.size() << "\n"
            << "  Function: " << func->name << "(";

        // Show parameters
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << func->params[i];
        }
        oss << ")\n";

        // Show what was provided
        oss << "  Provided: " << args.size() << " argument(s)";

        // Helpful hint if this might be a pipeline issue
        if (args.size() == 1 && func->params.size() > 1) {
            oss << "\n\n  Hint: If using pipeline operator (|>), it only passes the left side as the FIRST argument.\n"
                << "        For multi-arg functions: 100 |> subtract(50) becomes subtract(100, 50)";
        }

        throw std::runtime_error(oss.str());
    }

    // Create new environment for function execution with closure as parent (lexical scoping)
    auto parent_env = func->closure ? func->closure : global_env_;
    auto func_env = std::make_shared<Environment>(parent_env);

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

    // Issue #3: Push file context for function's source file
    if (!func->source_file.empty()) {
        pushFileContext(func->source_file);
    }

    pushStackFrame(func->name, func->source_line);  // Phase 3.1: Use actual line number

    try {
        executeStmt(*func->body);
    } catch (...) {
        popStackFrame();
        // Issue #3: Pop file context on error
        if (!func->source_file.empty()) {
            popFileContext();
        }
        current_env_ = saved_env;
        returning_ = saved_returning;
        current_file_ = saved_file;  // Phase 3.1: Restore file
        throw;
    }

    popStackFrame();

    // Issue #3: Pop file context on success
    if (!func->source_file.empty()) {
        popFileContext();
    }

    // Restore environment
    current_env_ = saved_env;
    current_file_ = saved_file;  // Phase 3.1: Restore file
    auto return_value = result_;
    returning_ = saved_returning;

    return return_value;
}

// Get variable from environment (for testing)
std::shared_ptr<Value> Interpreter::getVariable(const std::string& name) const {
    // Try current environment first, then global
    if (current_env_ && current_env_->has(name)) {
        return current_env_->get(name);
    }
    if (global_env_ && global_env_->has(name)) {
        return global_env_->get(name);
    }
    return nullptr;
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
    LOG_DEBUG("Processing {} standalone functions\n", node.getFunctions().size());
    for (auto& func : node.getFunctions()) {
        func->accept(*this);
    }

    // Phase 3.1: Process exports
    LOG_DEBUG("Processing {} export statements\n", node.getExports().size());
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

        LOG_DEBUG("[INFO] Imported stdlib module: {} as {}\n", module_name, alias);

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
            LOG_DEBUG("[INFO] Creating dedicated C++ executor for block...\n");
            auto cpp_exec = std::make_unique<runtime::CppExecutorAdapter>();

            // Use BLOCK_LIBRARY mode to compile to shared library (no wrapping)
            if (!cpp_exec->execute(code, runtime::CppExecutionMode::BLOCK_LIBRARY)) {
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

            LOG_DEBUG("[INFO] Executing block with shared {} executor...\n", metadata.language);

            // For JavaScript blocks, use BLOCK_LIBRARY mode (no IIFE wrapping)
            // This allows block functions to be defined in global scope and callable via callFunction()
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
                // For C++ blocks, use BLOCK_LIBRARY mode (compile to shared library)
                // This allows extern "C" functions to be compiled correctly and callable via FFI
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
            } else {
                // Other languages use default execute()
                if (!executor->execute(code)) {
                    fmt::print("[ERROR] Failed to execute block code\n");
                    return;
                }
            }

            block_value = std::make_shared<BlockValue>(metadata, code, executor);
        }

        // Store BlockValue in environment
        auto value = std::make_shared<Value>(block_value);
        current_env_->define(alias, value);

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

    // Resolve module path
    auto resolved_path = module_resolver_->resolve(node.getModulePath(), current_dir);
    if (!resolved_path) {
        // Fall back to stdlib: import "time" as Time -> treat as stdlib time module
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
            // Determine alias from import items or use module name
            std::string alias = bare_name;
            if (node.isWildcard()) {
                alias = node.getWildcardAlias();
            } else if (!node.getItems().empty() && !node.getItems()[0].alias.empty()) {
                // For import "time" as Time, the alias comes from import items
                // But wildcard import handles the alias differently
            }
            // Check for wildcard alias pattern (import "time" as Time)
            if (node.isWildcard()) {
                alias = node.getWildcardAlias();
            }

            imported_modules_[alias] = module;
            auto module_marker = std::make_shared<Value>(
                std::string("__stdlib_module__:" + alias)
            );
            current_env_->define(alias, module_marker);
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
            "time", "regex", "crypto", "csv", "env", "collections"
        };
        for (const auto& stdlib_name : stdlib_names) {
            if (bare_name == stdlib_name) {
                error_msg += fmt::format("\n\n  Did you mean the built-in '{}' module?\n"
                                        "    import {}    // stdlib (no quotes needed)", stdlib_name, stdlib_name);
                break;
            }
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
        std::unordered_map<std::string, std::shared_ptr<Value>> module_dict;

        // Get all names from module environment
        for (const auto& name : module_env->getAllNames()) {
            module_dict[name] = module_env->get(name);
        }

        auto dict_value = std::make_shared<Value>(module_dict);
        current_env_->define(alias, dict_value);

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
        // Define all enum variants in global environment
        for (const auto& [variant_name, value] : enum_def->variants) {
            std::string full_name = enum_def->name + "." + variant_name;
            global_env_->define(full_name, std::make_shared<Value>(value));
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

        LOG_DEBUG("[SUCCESS] Module loaded successfully: {}\n", module_path);
        LOG_DEBUG("          Exported {} symbols\n", module_exports_.size());

    } catch (const std::exception& e) {
        // Issue #3: Pop file context on error
        popFileContext();

        // Restore environment on error
        current_env_ = saved_env;
        module_exports_ = saved_exports;
        throw std::runtime_error(
            fmt::format("Error executing module {}: {}", module_path, e.what())
        );
    }

    // Issue #3: Pop file context on success
    popFileContext();

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

    // Return type: use explicitly specified type, or default to Any
    // Note: inferReturnType() is disabled because it uses eval() at declaration
    // time, which fails for any function that references parameters or local vars.
    // Types are checked at call-time instead.
    ast::Type return_type = node.getReturnType();

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

    LOG_DEBUG("[INFO] Defined function: {}({} params)",
               node.getName(), param_names.size());
    if (!node.getTypeParams().empty()) {
        LOG_DEBUG(" <");
        for (size_t i = 0; i < node.getTypeParams().size(); i++) {
            if (i > 0) LOG_DEBUG(", ");
            LOG_DEBUG("{}", node.getTypeParams()[i]);
        }
        LOG_DEBUG(">");
    }
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

    LOG_DEBUG("[INFO] Defined struct: {}\n", node.getName());

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

// Nested function declaration statement
void Interpreter::visit(ast::FunctionDeclStmt& node) {
    // Simply delegate to the existing FunctionDecl visitor
    // This handles closure capture, type inference, everything
    node.getDecl()->accept(*this);
}

// Nested struct declaration statement
void Interpreter::visit(ast::StructDeclStmt& node) {
    // Simply delegate to the existing StructDecl visitor
    // This handles struct registration, field setup, everything
    node.getDecl()->accept(*this);
}

// Phase 12: Persistent sub-runtime declaration
void Interpreter::visit(ast::RuntimeDeclStmt& node) {
    const std::string& name = node.getName();
    const std::string& language = node.getLanguage();

    explain("Creating persistent runtime '" + name + "' for language '" + language + "'");

    // Check if runtime name already exists
    if (named_runtimes_.count(name)) {
        throw std::runtime_error(
            "Runtime error: Runtime '" + name + "' already exists.\n\n"
            "  Each runtime name must be unique. Use a different name:\n"
            "    runtime " + name + "2 = " + language + ".start()\n");
    }

    // Get executor via LanguageRegistry
    auto& registry = runtime::LanguageRegistry::instance();
    auto* executor = registry.getExecutor(language);
    if (!executor) {
        throw std::runtime_error(
            "Runtime error: Unknown language '" + language + "' for persistent runtime.\n\n"
            "  Supported languages: python, javascript, js, shell, bash, sh,\n"
            "    rust, go, cpp, csharp, cs, ruby, php, typescript, ts\n\n"
            "  Example: runtime py = python.start()\n");
    }

    // Store the persistent runtime (borrowing the shared executor pointer)
    PersistentRuntime rt;
    rt.language = language;
    rt.executor = std::shared_ptr<runtime::Executor>(executor, [](runtime::Executor*){});  // Non-owning shared_ptr
    rt.code_buffer = "";
    named_runtimes_[name] = std::move(rt);

    // Define the runtime handle as a special value in the environment
    // Store it as a string marker that the .exec() handler recognizes
    auto value = std::make_shared<Value>("__NAAB_RUNTIME__:" + name);
    current_env_->define(name, value);
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

    LOG_DEBUG("[INFO] Defined enum: {} with {} variants\n",
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

    auto& statements = node.getStatements();

    // Parallel polyglot execution: Analyze dependencies
    PolyglotDependencyAnalyzer analyzer;

    // Convert unique_ptr vector to raw pointer vector for analysis
    std::vector<ast::Stmt*> stmt_ptrs;
    stmt_ptrs.reserve(statements.size());
    for (const auto& stmt : statements) {
        stmt_ptrs.push_back(stmt.get());
    }

    auto groups = analyzer.analyze(stmt_ptrs);

    // Debug: Print group information
    if (!groups.empty() && verbose_mode_) {
        fmt::print("[PARALLEL] Found {} polyglot group(s)\n", groups.size());
        for (size_t g = 0; g < groups.size(); ++g) {
            fmt::print("[PARALLEL] Group {}: {} block(s)\n", g, groups[g].parallel_blocks.size());
        }
    }

    if (groups.empty()) {
        // No polyglot blocks - execute statements normally (sequential)
        for (auto& stmt : statements) {
            stmt->accept(*this);
            if (returning_ || breaking_ || continuing_) break;
        }
    } else {
        // Polyglot blocks detected - execute in groups
        // Build a map from statement index to group index
        std::unordered_map<size_t, size_t> stmt_to_group;
        std::unordered_set<size_t> polyglot_indices;

        for (size_t g = 0; g < groups.size(); ++g) {
            for (const auto& block : groups[g].parallel_blocks) {
                stmt_to_group[block.statement_index] = g;
                polyglot_indices.insert(block.statement_index);
            }
        }

        // Execute statements in order, parallelizing groups
        // IMPORTANT: Execute non-polyglot statements BEFORE polyglot groups
        // so that variable declarations are available when capturing snapshots
        std::unordered_set<size_t> executed_groups;
        size_t last_executed = 0;  // Track last executed non-polyglot statement

        for (size_t i = 0; i < statements.size(); ++i) {
            // Check if this statement is part of a polyglot group
            auto it = stmt_to_group.find(i);

            if (it != stmt_to_group.end()) {
                size_t group_idx = it->second;

                // Execute this group (if not already executed)
                if (executed_groups.find(group_idx) == executed_groups.end()) {
                    // FIRST: Execute all non-polyglot statements between last_executed and i
                    // This ensures variable declarations run before polyglot blocks that use them
                    for (size_t j = last_executed; j < i; ++j) {
                        if (polyglot_indices.find(j) == polyglot_indices.end()) {
                            statements[j]->accept(*this);
                            if (returning_ || breaking_ || continuing_) break;
                        }
                    }

                    // THEN: Execute the polyglot group
                    executePolyglotGroupParallel(groups[group_idx]);
                    executed_groups.insert(group_idx);

                    // Find the range of statement indices covered by this group
                    size_t group_max_idx = 0;
                    for (const auto& block : groups[group_idx].parallel_blocks) {
                        if (block.statement_index > group_max_idx) {
                            group_max_idx = block.statement_index;
                        }
                    }

                    // Execute non-polyglot statements BETWEEN the group's blocks
                    // These are statements like print() that reference variables
                    // assigned by the group - they must run AFTER the group completes
                    for (size_t j = i + 1; j <= group_max_idx; ++j) {
                        if (polyglot_indices.find(j) == polyglot_indices.end()) {
                            statements[j]->accept(*this);
                            if (returning_ || breaking_ || continuing_) break;
                        }
                    }

                    // Update last_executed past all blocks and intermediate statements
                    last_executed = group_max_idx + 1;

                    if (returning_ || breaking_ || continuing_) break;
                }
                // Skip this statement (already executed as part of group)
                continue;
            }

            // Regular statement after last polyglot group - execute normally
            if (i >= last_executed) {
                statements[i]->accept(*this);
                last_executed = i + 1;

                if (returning_ || breaking_ || continuing_) break;
            }
        }

        // Execute any remaining non-polyglot statements after the last group
        for (size_t i = last_executed; i < statements.size(); ++i) {
            if (polyglot_indices.find(i) == polyglot_indices.end()) {
                statements[i]->accept(*this);
                if (returning_ || breaking_ || continuing_) break;
            }
        }
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
    // Check for assignment in condition (common mistake: = instead of ==)
    auto* condition_expr = node.getCondition();
    if (auto* binary = dynamic_cast<ast::BinaryExpr*>(condition_expr)) {
        if (binary->getOp() == ast::BinaryOp::Assign) {
            // Check if it's wrapped in parentheses (intentional)
            // For now, always warn since we can't detect extra parentheses in AST
            throw std::runtime_error(
                "Suspicious: Assignment in if condition\n\n"
                "  This is a common mistake - did you mean '==' instead of '='?\n\n"
                "  Current code uses assignment (=):\n"
                "    if x = 20  // assigns 20 to x, then checks if 20 is truthy\n\n"
                "  If you meant comparison, use:\n"
                "    if x == 20  // checks if x equals 20\n\n"
                "  If assignment is intentional, split into two statements:\n"
                "    x = 20\n"
                "    if x { ... }\n\n"
                "  Help:\n"
                "  - Assignment (=) sets a value\n"
                "  - Comparison (==) tests equality\n"
                "  - This error prevents a very common bug"
            );
        }
    }

    auto condition = eval(*node.getCondition());
    if (condition->toBool()) {
        node.getThenBranch()->accept(*this);
    } else if (node.getElseBranch()) {
        node.getElseBranch()->accept(*this);
    }
}

void Interpreter::visit(ast::IfExpr& node) {
    auto condition = eval(*node.getCondition());
    if (condition->toBool()) {
        node.getThenExpr()->accept(*this);
    } else {
        node.getElseExpr()->accept(*this);
    }
    // last_value_ is set by whichever branch expression was evaluated
}

void Interpreter::visit(ast::MatchExpr& node) {
    auto subject = eval(*node.getSubject());

    for (auto& arm : node.getArms()) {
        if (!arm.pattern) {
            // Wildcard (_) — always matches
            arm.body->accept(*this);
            return;
        }

        auto pattern_val = eval(*arm.pattern);

        // Compare subject to pattern using same logic as BinaryOp::Eq
        bool matches = false;
        bool subj_null = isNull(subject);
        bool pat_null = isNull(pattern_val);

        if (subj_null && pat_null) {
            matches = true;
        } else if (!subj_null && !pat_null) {
            bool subj_numeric = std::holds_alternative<int>(subject->data) ||
                                std::holds_alternative<double>(subject->data);
            bool pat_numeric = std::holds_alternative<int>(pattern_val->data) ||
                               std::holds_alternative<double>(pattern_val->data);

            if (subj_numeric && pat_numeric) {
                matches = subject->toFloat() == pattern_val->toFloat();
            } else if (std::holds_alternative<std::string>(subject->data) &&
                       std::holds_alternative<std::string>(pattern_val->data)) {
                matches = subject->toString() == pattern_val->toString();
            } else if (std::holds_alternative<bool>(subject->data) &&
                       std::holds_alternative<bool>(pattern_val->data)) {
                matches = subject->toBool() == pattern_val->toBool();
            }
        }

        if (matches) {
            arm.body->accept(*this);
            return;
        }
    }

    throw std::runtime_error(
        "Match error: no matching arm for value: " + subject->toString() + "\n\n"
        "  Help:\n"
        "  - Add a wildcard arm to handle all other cases:\n\n"
        "  Example:\n"
        "    match value {\n"
        "        1 => \"one\"\n"
        "        _ => \"default\"\n"
        "    }\n"
    );
}

void Interpreter::visit(ast::LambdaExpr& node) {
    // Create an anonymous FunctionValue with closure capture
    static int lambda_counter = 0;
    std::string name = "__lambda_" + std::to_string(lambda_counter++);

    // Extract parameter names and types
    std::vector<std::string> param_names;
    std::vector<ast::Type> param_types;
    std::vector<ast::Expr*> defaults;

    for (const auto& param : node.getParams()) {
        param_names.push_back(param.name);
        param_types.push_back(param.type);
        defaults.push_back(param.default_value.has_value() ? param.default_value->get() : nullptr);
    }

    // Create shared body (FunctionValue needs shared_ptr)
    auto body = std::shared_ptr<ast::CompoundStmt>(
        node.getBody(), [](ast::CompoundStmt*) {}  // non-owning - AST owns the body
    );

    auto func_val = std::make_shared<FunctionValue>(
        name,
        param_names,
        std::move(param_types),
        std::move(defaults),
        body,
        std::vector<std::string>{},  // no type parameters
        node.getReturnType(),
        current_file_,
        node.getLocation().line,
        current_env_  // capture current environment as closure
    );

    result_ = std::make_shared<Value>(func_val);
}

void Interpreter::visit(ast::ForStmt& node) {
    // Increment loop depth for break/continue validation
    ++loop_depth_;

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

    // Decrement loop depth after loop completes
    --loop_depth_;
}

void Interpreter::visit(ast::WhileStmt& node) {
    // Check for assignment in condition (common mistake: = instead of ==)
    auto* condition_expr = node.getCondition();
    if (auto* binary = dynamic_cast<ast::BinaryExpr*>(condition_expr)) {
        if (binary->getOp() == ast::BinaryOp::Assign) {
            throw std::runtime_error(
                "Suspicious: Assignment in while condition\n\n"
                "  This is a common mistake - did you mean '==' instead of '='?\n\n"
                "  Current code uses assignment (=):\n"
                "    while x = getNext()  // assigns to x, then checks if truthy\n\n"
                "  If you meant comparison, use:\n"
                "    while x == value  // checks if x equals value\n\n"
                "  If assignment is intentional, use a loop with break:\n"
                "    while true {\n"
                "      x = getNext()\n"
                "      if !x { break }\n"
                "      // use x\n"
                "    }\n\n"
                "  Help:\n"
                "  - Assignment (=) sets a value\n"
                "  - Comparison (==) tests equality\n"
                "  - This error prevents a very common bug"
            );
        }
    }

    // Increment loop depth for break/continue validation
    ++loop_depth_;

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

    // Decrement loop depth after loop completes
    --loop_depth_;
}

void Interpreter::visit(ast::BreakStmt& node) {
    (void)node;

    // Validate that break is used inside a loop
    if (loop_depth_ == 0) {
        throw std::runtime_error(
            "Control flow error: 'break' can only be used inside a loop\n\n"
            "  Help:\n"
            "  - break terminates the nearest enclosing loop\n"
            "  - It cannot be used in top-level code or functions\n"
            "  - Use 'return' to exit from functions early\n\n"
            "  Example:\n"
            "    ✗ Wrong: break outside loop\n"
            "    ✓ Right: for i in 0..10 {\n"
            "               if i == 5 { break }\n"
            "             }"
        );
    }

    breaking_ = true;
}

void Interpreter::visit(ast::ContinueStmt& node) {
    (void)node;

    // Validate that continue is used inside a loop
    if (loop_depth_ == 0) {
        throw std::runtime_error(
            "Control flow error: 'continue' can only be used inside a loop\n\n"
            "  Help:\n"
            "  - continue skips to the next iteration of the loop\n"
            "  - It cannot be used in top-level code or functions\n"
            "  - Use 'return' to exit from functions early\n\n"
            "  Example:\n"
            "    ✗ Wrong: continue outside loop\n"
            "    ✓ Right: for i in 0..10 {\n"
            "               if i % 2 == 0 { continue }\n"
            "             }"
        );
    }

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

            // Special case: infer null as any? (nullable any type)
            // This allows polyglot blocks to return null without explicit type annotations
            if (isNull(value)) {
                effective_type = ast::Type::makeAny();
                effective_type.is_nullable = true;
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

    // Deep copy arrays and dicts to prevent silent mutations
    // When you do "let arr2 = arr1", both should be independent copies
    if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data) ||
        std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data)) {
        value = copyValue(value);
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

        // Issue #6 Fix: Ensure error object has structured properties
        auto error_val = e.getValue();
        if (!error_val) {
            // Create structured error object if getValue() returned null
            std::unordered_map<std::string, std::shared_ptr<Value>> error_dict;
            error_dict["message"] = std::make_shared<Value>(e.getMessage());
            error_dict["type"] = std::make_shared<Value>(NaabError::errorTypeToString(e.getType()));
            error_val = std::make_shared<Value>(error_dict);
        }
        current_env_->define(catch_clause->error_name, error_val);

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
        // Convert std::exception to NaabError and execute catch block
        // This handles polyglot exceptions (Python, JavaScript, etc.)

        auto catch_env = std::make_shared<Environment>(current_env_);
        auto prev_env = current_env_;
        current_env_ = catch_env;

        // Create structured error object from std::exception
        // Issue #6 Fix: Create dict with "message" property for polyglot exceptions
        std::unordered_map<std::string, std::shared_ptr<Value>> error_dict;
        error_dict["message"] = std::make_shared<Value>(std::string(std_error.what()));
        error_dict["type"] = std::make_shared<Value>(std::string("PolyglotError"));
        auto error_value = std::make_shared<Value>(error_dict);

        // Bind the error value to the catch variable
        auto* catch_clause = node.getCatchClause();
        current_env_->define(catch_clause->error_name, error_value);

        try {
            // Execute catch body
            catch_clause->body->accept(*this);
        } catch (NaabError&) {
            // Exception thrown from catch block - propagate it
            current_env_ = prev_env;
            throw;
        } catch (const std::exception& nested_error) {
            // Another std::exception from catch block - convert and propagate
            current_env_ = prev_env;
            throw createError(nested_error.what(), ErrorType::RUNTIME_ERROR);
        }

        current_env_ = prev_env;
    }

    // CRITICAL FIX: Save return state BEFORE finally block
    // Bug: Finally blocks were overriding return values from try/catch
    // The finally block should execute but not affect return semantics
    bool try_catch_returned = returning_;
    auto try_catch_result = result_;

    // Execute finally block if present (always runs)
    if (node.hasFinally()) {
        // Reset return flags so finally can't override try/catch return
        returning_ = false;
        result_ = nullptr;

        try {
            node.getFinallyBody()->accept(*this);
        } catch (...) {
            // Finally block threw - propagate that exception instead
            throw;
        }
    }

    // CRITICAL FIX: Restore return state AFTER finally block
    // This ensures finally executes but doesn't break function returns
    if (try_catch_returned) {
        returning_ = true;
        result_ = try_catch_result;
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
            // Deep copy arrays and dicts to prevent silent mutations (same as VarDeclStmt)
            auto value_to_assign = right;
            if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(right->data) ||
                std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(right->data)) {
                value_to_assign = copyValue(right);
            }
            current_env_->set(id->getName(), value_to_assign);
            result_ = value_to_assign;
        } else if (auto* member = dynamic_cast<ast::MemberExpr*>(node.getLeft())) {
            // Struct field assignment: obj.field = value
            auto obj = eval(*member->getObject());

            if (auto* struct_ptr = std::get_if<std::shared_ptr<StructValue>>(&obj->data)) {
                auto& struct_val = *struct_ptr;
                struct_val->setField(member->getMember(), right);
                result_ = right;
            } else {
                std::ostringstream oss;
                oss << "Type error: Cannot assign to property of non-struct value\n\n";
                oss << "  Tried to assign to: " << getTypeName(obj) << "." << member->getMember() << "\n";
                oss << "  Expected: struct\n\n";
                oss << "  Help:\n";
                oss << "  - Only structs support property assignment with dot notation\n";
                oss << "  - For dictionaries, use subscript: dict[\"field\"] = value\n";
                oss << "  - Define a struct type if you need named fields\n\n";
                oss << "  Example:\n";
                oss << "    ✗ Wrong: let x = 42; x.field = 10\n";
                oss << "    ✓ Right: struct Point { x: int, y: int }\n";
                oss << "             let p = Point{x: 0, y: 0}; p.x = 10\n";
                throw std::runtime_error(oss.str());
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
                        std::ostringstream oss;
                        oss << "Index error: Array index out of bounds\n\n";
                        oss << "  Index: " << index << "\n";
                        oss << "  Array size: " << list.size() << "\n";
                        oss << "  Valid range: 0 to " << (list.size() > 0 ? list.size() - 1 : 0) << "\n\n";
                        oss << "  Help:\n";
                        oss << "  - Array indices start at 0\n";
                        oss << "  - Check array size before accessing\n";
                        oss << "  - Use array.length(arr) to get size\n\n";
                        oss << "  Example:\n";
                        oss << "    let arr = [10, 20, 30]  // size = 3\n";
                        oss << "    ✗ Wrong: arr[3]  // out of bounds\n";
                        oss << "    ✓ Right: arr[2]  // last element\n";
                        throw std::runtime_error(oss.str());
                    }

                    // Modify list in place (safe cast after bounds check)
                    list[static_cast<size_t>(index)] = right;
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
                    std::ostringstream oss;
                    oss << "Type error: Subscript assignment not supported\n\n";
                    oss << "  Tried to assign to: " << getTypeName(container) << "[...]\n";
                    oss << "  Supported types: array, dict\n\n";
                    oss << "  Help:\n";
                    oss << "  - Only arrays and dicts support subscript assignment\n";
                    oss << "  - Arrays use integer indices: arr[0] = value\n";
                    oss << "  - Dicts use string keys: dict[\"key\"] = value\n\n";
                    oss << "  Example:\n";
                    oss << "    ✗ Wrong: let x = 42; x[0] = 10\n";
                    oss << "    ✓ Right: let arr = [1, 2]; arr[0] = 10\n";
                    oss << "    ✓ Right: let dict = {}; dict[\"x\"] = 10\n";
                    throw std::runtime_error(oss.str());
                }
            } else {
                throw std::runtime_error(
                    "Syntax error: Invalid assignment target\n\n"
                    "  Assignment target must be:\n"
                    "  - Variable: name = value\n"
                    "  - Struct field: obj.field = value\n"
                    "  - Array element: arr[index] = value\n"
                    "  - Dict entry: dict[\"key\"] = value\n\n"
                    "  Example:\n"
                    "    ✗ Wrong: 42 = x  // can't assign to literal\n"
                    "    ✓ Right: x = 42\n"
                );
            }
        } else {
            throw std::runtime_error(
                "Syntax error: Invalid assignment target\n\n"
                "  Assignment target must be:\n"
                "  - Variable: name = value\n"
                "  - Struct field: obj.field = value\n"
                "  - Array element: arr[index] = value\n"
                "  - Dict entry: dict[\"key\"] = value\n\n"
                "  Help:\n"
                "  - Cannot assign to expressions or literals\n"
                "  - Use let to declare new variables\n\n"
                "  Example:\n"
                "    ✗ Wrong: getValue() = 10  // can't assign to function result\n"
                "    ✓ Right: let result = getValue(); result = 10\n"
            );
        }
        return;
    }

    // Handle Pipeline operator specially (like short-circuit operators)
    // Don't evaluate right side yet - it needs special handling
    std::shared_ptr<Value> left, right;
    if (node.getOp() == ast::BinaryOp::Pipeline) {
        left = eval(*node.getLeft());
        // Right side handling is in the switch case below - don't eval here!
        // This prevents the CallExpr from being evaluated with wrong arg count
    } else {
        // For all other operators, evaluate both sides
        left = eval(*node.getLeft());
        right = eval(*node.getRight());
    }

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
                // Integer addition with overflow detection
                int a = left->toInt();
                int b = right->toInt();

                // Check for overflow before addition
                if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b)) {
                    std::ostringstream oss;
                    oss << "Math error: Integer overflow in addition\n\n";
                    oss << "  Expression: " << a << " + " << b << "\n";
                    oss << "  INT_MAX: " << INT_MAX << "\n";
                    oss << "  INT_MIN: " << INT_MIN << "\n";
                    oss << "\n  Help:\n";
                    oss << "  - Integer overflow occurs when result exceeds 32-bit int range\n";
                    oss << "  - Use float for larger numbers: " << a << ".0 + " << b << ".0\n";
                    oss << "  - Or check values before adding:\n";
                    oss << "\n  Example:\n";
                    oss << "    ✗ Wrong: let result = " << INT_MAX << " + 1  (overflow!)\n";
                    oss << "    ✓ Right: let result = " << INT_MAX << ".0 + 1.0  (use float)\n";
                    throw std::runtime_error(oss.str());
                }

                result_ = std::make_shared<Value>(a + b);
            }
            break;

        case ast::BinaryOp::Sub: {
            // Type check: Subtraction requires numeric types
            bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                  std::holds_alternative<double>(left->data) ||
                                  std::holds_alternative<bool>(left->data);
            bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                   std::holds_alternative<double>(right->data) ||
                                   std::holds_alternative<bool>(right->data);

            if (!left_is_numeric || !right_is_numeric) {
                std::ostringstream oss;
                oss << "Type error: Subtraction (-) requires numeric types\n\n";

                if (!left_is_numeric && !right_is_numeric) {
                    oss << "  Both operands are non-numeric:\n";
                    oss << "    Left: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Right: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                } else if (!left_is_numeric) {
                    oss << "  Left operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                } else {
                    oss << "  Right operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                }

                oss << "\n  Help:\n";
                oss << "  - For numbers: Use int or float values\n";
                oss << "  - For strings: Parse to numeric first\n";
                oss << "  - String concatenation uses +, not -\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: \"10\" - 5    (string - int)\n";
                oss << "    ✓ Right: 10 - 5       (int - int)\n";

                throw std::runtime_error(oss.str());
            }

            if (std::holds_alternative<double>(left->data) ||
                std::holds_alternative<double>(right->data)) {
                result_ = std::make_shared<Value>(left->toFloat() - right->toFloat());
            } else {
                // Integer subtraction with overflow detection
                int a = left->toInt();
                int b = right->toInt();

                // Check for overflow before subtraction
                if ((b < 0 && a > INT_MAX + b) || (b > 0 && a < INT_MIN + b)) {
                    std::ostringstream oss;
                    oss << "Math error: Integer overflow in subtraction\n\n";
                    oss << "  Expression: " << a << " - " << b << "\n";
                    oss << "  INT_MAX: " << INT_MAX << "\n";
                    oss << "  INT_MIN: " << INT_MIN << "\n";
                    oss << "\n  Help:\n";
                    oss << "  - Integer overflow occurs when result exceeds 32-bit int range\n";
                    oss << "  - Use float for larger numbers: " << a << ".0 - " << b << ".0\n";
                    oss << "  - Or check values before subtracting:\n";
                    oss << "\n  Example:\n";
                    oss << "    ✗ Wrong: let result = " << INT_MIN << " - 1  (underflow!)\n";
                    oss << "    ✓ Right: let result = " << INT_MIN << ".0 - 1.0  (use float)\n";
                    throw std::runtime_error(oss.str());
                }

                result_ = std::make_shared<Value>(a - b);
            }
            break;
        }

        case ast::BinaryOp::Mul: {
            // Type check: Multiplication requires numeric types
            bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                  std::holds_alternative<double>(left->data) ||
                                  std::holds_alternative<bool>(left->data);
            bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                   std::holds_alternative<double>(right->data) ||
                                   std::holds_alternative<bool>(right->data);

            if (!left_is_numeric || !right_is_numeric) {
                std::ostringstream oss;
                oss << "Type error: Multiplication (*) requires numeric types\n\n";

                if (!left_is_numeric && !right_is_numeric) {
                    oss << "  Both operands are non-numeric:\n";
                    oss << "    Left: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Right: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                } else if (!left_is_numeric) {
                    oss << "  Left operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                } else {
                    oss << "  Right operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                }

                oss << "\n  Help:\n";
                oss << "  - For numbers: Use int or float values\n";
                oss << "  - For string repetition: Some languages support \"ab\" * 3, but NAAb doesn't\n";
                oss << "  - For concatenation: Use + operator\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: 5 * \"3\"      (int * string)\n";
                oss << "    ✓ Right: 5 * 3         (int * int)\n";

                throw std::runtime_error(oss.str());
            }

            if (std::holds_alternative<double>(left->data) ||
                std::holds_alternative<double>(right->data)) {
                result_ = std::make_shared<Value>(left->toFloat() * right->toFloat());
            } else {
                // Integer multiplication with overflow detection
                int a = left->toInt();
                int b = right->toInt();

                // Check for overflow before multiplication
                // Handle special cases first
                if (a == 0 || b == 0) {
                    result_ = std::make_shared<Value>(0);
                } else if (a == INT_MIN || b == INT_MIN) {
                    // INT_MIN * anything (except 0, 1) will overflow
                    if ((a == INT_MIN && (b != 1 && b != 0)) || (b == INT_MIN && (a != 1 && a != 0))) {
                        std::ostringstream oss;
                        oss << "Math error: Integer overflow in multiplication\n\n";
                        oss << "  Expression: " << a << " * " << b << "\n";
                        oss << "  INT_MAX: " << INT_MAX << "\n";
                        oss << "  INT_MIN: " << INT_MIN << "\n";
                        oss << "\n  Help:\n";
                        oss << "  - Integer overflow occurs when result exceeds 32-bit int range\n";
                        oss << "  - Use float for larger numbers: " << a << ".0 * " << b << ".0\n";
                        oss << "\n  Example:\n";
                        oss << "    ✗ Wrong: let result = 1000000 * 10000  (overflow!)\n";
                        oss << "    ✓ Right: let result = 1000000.0 * 10000.0  (use float)\n";
                        throw std::runtime_error(oss.str());
                    }
                    result_ = std::make_shared<Value>(a * b);
                } else if ((a > 0 && b > 0 && a > INT_MAX / b) ||
                           (a < 0 && b < 0 && a < INT_MAX / b) ||
                           (a > 0 && b < 0 && b < INT_MIN / a) ||
                           (a < 0 && b > 0 && a < INT_MIN / b)) {
                    std::ostringstream oss;
                    oss << "Math error: Integer overflow in multiplication\n\n";
                    oss << "  Expression: " << a << " * " << b << "\n";
                    oss << "  INT_MAX: " << INT_MAX << "\n";
                    oss << "  INT_MIN: " << INT_MIN << "\n";
                    oss << "\n  Help:\n";
                    oss << "  - Integer overflow occurs when result exceeds 32-bit int range\n";
                    oss << "  - Use float for larger numbers: " << a << ".0 * " << b << ".0\n";
                    oss << "\n  Example:\n";
                    oss << "    ✗ Wrong: let result = 1000000 * 10000  (overflow!)\n";
                    oss << "    ✓ Right: let result = 1000000.0 * 10000.0  (use float)\n";
                    throw std::runtime_error(oss.str());
                } else {
                    result_ = std::make_shared<Value>(a * b);
                }
            }
            break;
        }

        case ast::BinaryOp::Div: {
            // Type check: Division requires numeric types
            bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                  std::holds_alternative<double>(left->data) ||
                                  std::holds_alternative<bool>(left->data);
            bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                   std::holds_alternative<double>(right->data) ||
                                   std::holds_alternative<bool>(right->data);

            if (!left_is_numeric || !right_is_numeric) {
                std::ostringstream oss;
                oss << "Type error: Division (/) requires numeric types\n\n";

                if (!left_is_numeric && !right_is_numeric) {
                    oss << "  Both operands are non-numeric:\n";
                    oss << "    Left: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Right: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                } else if (!left_is_numeric) {
                    oss << "  Left operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                } else {
                    oss << "  Right operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                }

                oss << "\n  Help:\n";
                oss << "  - For numbers: Use int or float values\n";
                oss << "  - For string splitting: Use string.split() instead\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: \"10\" / 2     (string / int)\n";
                oss << "    ✓ Right: 10 / 2        (int / int)\n";

                throw std::runtime_error(oss.str());
            }

            // Check for division by zero
            double divisor = right->toFloat();
            if (divisor == 0.0) {
                std::ostringstream oss;
                oss << "Math error: Division by zero\n\n";
                oss << "  Expression: " << left->toString() << " / 0\n";
                oss << "\n  Help:\n";
                oss << "  - Division by zero is undefined in mathematics\n";
                oss << "  - Check if divisor is zero before dividing\n";
                oss << "  - Use conditional to handle zero case:\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: let result = x / 0\n";
                oss << "    ✓ Right: let result = if (y != 0) { x / y } else { 0 }\n";
                oss << "\n  Common causes:\n";
                oss << "  - User input not validated\n";
                oss << "  - Variable initialized to 0\n";
                oss << "  - Logic error in calculation\n";
                throw std::runtime_error(oss.str());
            }

            result_ = std::make_shared<Value>(left->toFloat() / divisor);
            break;
        }

        case ast::BinaryOp::Mod: {
            // Type check: Modulo requires integer types
            bool left_is_int = std::holds_alternative<int>(left->data) ||
                              std::holds_alternative<bool>(left->data);
            bool right_is_int = std::holds_alternative<int>(right->data) ||
                               std::holds_alternative<bool>(right->data);

            if (!left_is_int || !right_is_int) {
                std::ostringstream oss;
                oss << "Type error: Modulo (%) requires integer types\n\n";

                if (!left_is_int && !right_is_int) {
                    oss << "  Both operands are non-integer:\n";
                    oss << "    Left: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Right: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                } else if (!left_is_int) {
                    oss << "  Left operand is non-integer:\n";
                    oss << "    Got: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Expected: int or bool\n";
                } else {
                    oss << "  Right operand is non-integer:\n";
                    oss << "    Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "    Expected: int or bool\n";
                }

                oss << "\n  Help:\n";
                oss << "  - Modulo requires integers (int or bool)\n";
                oss << "  - For floats: Use fmod() or convert to int first\n";
                oss << "  - For string formatting: Use string interpolation\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: \"10\" % 3     (string % int)\n";
                oss << "    ✗ Wrong: 10.5 % 3     (float % int)\n";
                oss << "    ✓ Right: 10 % 3       (int % int)\n";

                throw std::runtime_error(oss.str());
            }

            // Check for modulo by zero
            int divisor = right->toInt();
            if (divisor == 0) {
                std::ostringstream oss;
                oss << "Math error: Modulo by zero\n\n";
                oss << "  Expression: " << left->toString() << " % 0\n";
                oss << "\n  Help:\n";
                oss << "  - Modulo by zero is undefined in mathematics\n";
                oss << "  - Check if divisor is zero before using modulo\n";
                oss << "  - Use conditional to handle zero case:\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: let remainder = x % 0\n";
                oss << "    ✓ Right: let remainder = if (y != 0) { x % y } else { 0 }\n";
                oss << "\n  Common causes:\n";
                oss << "  - User input not validated\n";
                oss << "  - Variable initialized to 0\n";
                oss << "  - Logic error in calculation\n";
                throw std::runtime_error(oss.str());
            }

            result_ = std::make_shared<Value>(left->toInt() % divisor);
            break;
        }

        case ast::BinaryOp::Eq: {
            // Type-aware equality comparison
            // For numeric types: compare numerically (10 == 10.0 is true)
            // For strings: compare as strings
            // For different types: false (no implicit coercion)

            // Check for null comparisons first (null == null should be true)
            bool left_null = isNull(left);
            bool right_null = isNull(right);

            if (left_null && right_null) {
                // Both null: equal
                result_ = std::make_shared<Value>(true);
            } else if (left_null || right_null) {
                // One null, one non-null: not equal
                result_ = std::make_shared<Value>(false);
            } else {
                // Neither is null - proceed with type-specific comparisons
                bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                      std::holds_alternative<double>(left->data) ||
                                      std::holds_alternative<bool>(left->data);
                bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                       std::holds_alternative<double>(right->data) ||
                                       std::holds_alternative<bool>(right->data);

                if (left_is_numeric && right_is_numeric) {
                    // Both numeric: compare as numbers
                    result_ = std::make_shared<Value>(left->toFloat() == right->toFloat());
                } else if (std::holds_alternative<std::string>(left->data) &&
                           std::holds_alternative<std::string>(right->data)) {
                    // Both strings: compare as strings
                    result_ = std::make_shared<Value>(left->toString() == right->toString());
                } else if (std::holds_alternative<bool>(left->data) &&
                           std::holds_alternative<bool>(right->data)) {
                    // Both bools: compare as bools
                    result_ = std::make_shared<Value>(left->toBool() == right->toBool());
                } else {
                    // Different types: not equal
                    result_ = std::make_shared<Value>(false);
                }
            }
            break;
        }

        case ast::BinaryOp::Ne: {
            // Type-aware inequality comparison (inverse of Eq)

            // Check for null comparisons first (null != null should be false)
            bool left_null = isNull(left);
            bool right_null = isNull(right);

            if (left_null && right_null) {
                // Both null: not different (false)
                result_ = std::make_shared<Value>(false);
            } else if (left_null || right_null) {
                // One null, one non-null: different (true)
                result_ = std::make_shared<Value>(true);
            } else {
                // Neither is null - proceed with type-specific comparisons
                bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                      std::holds_alternative<double>(left->data) ||
                                      std::holds_alternative<bool>(left->data);
                bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                       std::holds_alternative<double>(right->data) ||
                                       std::holds_alternative<bool>(right->data);

                if (left_is_numeric && right_is_numeric) {
                    // Both numeric: compare as numbers
                    result_ = std::make_shared<Value>(left->toFloat() != right->toFloat());
                } else if (std::holds_alternative<std::string>(left->data) &&
                           std::holds_alternative<std::string>(right->data)) {
                    // Both strings: compare as strings
                    result_ = std::make_shared<Value>(left->toString() != right->toString());
                } else if (std::holds_alternative<bool>(left->data) &&
                           std::holds_alternative<bool>(right->data)) {
                    // Both bools: compare as bools
                    result_ = std::make_shared<Value>(left->toBool() != right->toBool());
                } else {
                    // Different types: not equal
                    result_ = std::make_shared<Value>(true);
                }
            }
            break;
        }

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

            LOG_DEBUG("[Pipeline] Starting pipeline operation\n");
            LOG_DEBUG("[Pipeline] Left value: {}\n", left->toString());

            // Right side must be a function call or identifier
            if (auto* call = dynamic_cast<ast::CallExpr*>(node.getRight())) {
                LOG_DEBUG("[Pipeline] Right side is CallExpr with {} args\n", call->getArgs().size());

                // If right is a call: func(args...), insert left as first arg
                // Create a new argument list with left prepended
                std::vector<std::shared_ptr<Value>> args;
                args.push_back(left);  // Piped value goes first

                // Add existing arguments
                for (const auto& arg_expr : call->getArgs()) {
                    auto arg_val = eval(*arg_expr);
                    LOG_DEBUG("[Pipeline] Adding arg: {}\n", arg_val->toString());
                    args.push_back(arg_val);
                }

                LOG_DEBUG("[Pipeline] Total args after prepending: {}\n", args.size());

                // Evaluate the callee
                auto callee = eval(*call->getCallee());
                LOG_DEBUG("[Pipeline] Callee evaluated\n");

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
                    // User-defined function execution - use callFunction for proper validation
                    // This ensures argument count validation and default parameter handling
                    LOG_DEBUG("[Pipeline] Calling function {} with {} args\n", (*func)->name, args.size());
                    result_ = callFunction(callee, args);
                } else {
                    std::ostringstream oss;
                    oss << "Type error: Pipeline requires callable function\n\n";
                    oss << "  Right side type: " << getTypeName(callee) << "\n";
                    oss << "  Expected: function or block\n\n";
                    oss << "  Help:\n";
                    oss << "  - Pipeline operator |> passes left value to a function\n";
                    oss << "  - Right side must be a function call or identifier\n\n";
                    oss << "  Example:\n";
                    oss << "    ✗ Wrong: value |> 42\n";
                    oss << "    ✓ Right: value |> processFunc()\n";
                    oss << "    ✓ Right: value |> transform\n";
                    throw std::runtime_error(oss.str());
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
                    (void)func;  // Type check only, value not needed
                    // Use callFunction for proper validation
                    std::vector<std::shared_ptr<Value>> args = {left};
                    result_ = callFunction(callee, args);
                } else {
                    std::ostringstream oss;
                    oss << "Type error: Pipeline requires callable function\n\n";
                    oss << "  Identifier type: " << getTypeName(callee) << "\n";
                    oss << "  Expected: function or block\n\n";
                    oss << "  Help:\n";
                    oss << "  - Pipeline operator |> passes left value to a function\n";
                    oss << "  - The identifier must refer to a function\n\n";
                    oss << "  Example:\n";
                    oss << "    let transform = function(x) { return x * 2 }\n";
                    oss << "    ✗ Wrong: let x = 42; value |> x  // x is not a function\n";
                    oss << "    ✓ Right: value |> transform\n";
                    throw std::runtime_error(oss.str());
                }
            } else {
                // Try evaluating the right side as an expression (handles lambdas, etc.)
                auto callee = eval(*node.getRight());
                if (auto* func = std::get_if<std::shared_ptr<FunctionValue>>(&callee->data)) {
                    (void)func;
                    std::vector<std::shared_ptr<Value>> args = {left};
                    result_ = callFunction(callee, args);
                } else {
                    std::ostringstream oss;
                    oss << "Type error: Pipeline requires callable function\n\n";
                    oss << "  Right side type: " << getTypeName(callee) << "\n";
                    oss << "  Expected: function, lambda, or block\n\n";
                    oss << "  Help:\n";
                    oss << "  - Use function call: value |> func(arg1, arg2)\n";
                    oss << "  - Use identifier: value |> transform\n";
                    oss << "  - Use lambda: value |> (x) => x * 2\n\n";
                    oss << "  Example:\n";
                    oss << "    ✓ Right: 100 |> subtract(50)\n";
                    oss << "    ✓ Right: 100 |> double\n";
                    oss << "    ✓ Right: 100 |> (x) => x * 2\n";
                    throw std::runtime_error(oss.str());
                }
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
                    std::ostringstream oss;
                    oss << "Key error: Dictionary key not found\n\n";
                    oss << "  Key: \"" << key << "\"\n";

                    if (dict.empty()) {
                        oss << "  Dictionary is empty\n";
                    } else {
                        oss << "  Available keys: ";
                        size_t count = 0;
                        for (const auto& pair : dict) {
                            if (count > 0) oss << ", ";
                            oss << "\"" << pair.first << "\"";
                            if (++count >= 10) {
                                oss << "...";
                                break;
                            }
                        }
                        oss << "\n";
                    }

                    oss << "\n  Help:\n";
                    oss << "  - Check if the key exists before accessing\n";
                    oss << "  - Keys are case-sensitive\n";
                    oss << "  - Use dict.has_key() to check existence (if available)\n\n";
                    oss << "  Example:\n";
                    oss << "    let d = {\"name\": \"Alice\", \"age\": 30}\n";
                    oss << "    ✗ Wrong: d[\"Name\"]  // case mismatch\n";
                    oss << "    ✓ Right: d[\"name\"]\n";
                    throw std::runtime_error(oss.str());
                }

                result_ = it->second;
                break;
            }

            // Check if left is a list
            if (auto* list_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&left->data)) {
                auto& list = *list_ptr;

                // Type check: Array indices must be integers, not strings
                if (!std::holds_alternative<int>(right->data) &&
                    !std::holds_alternative<bool>(right->data)) {
                    std::ostringstream oss;
                    oss << "Type error: Array index must be an integer\n\n";
                    oss << "  Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "  Expected: int\n";
                    oss << "\n  Help:\n";
                    oss << "  - Array indices must be integers (int or bool)\n";
                    oss << "  - Strings are not automatically converted to numbers\n";
                    oss << "  - For string keys, use a dictionary instead\n";
                    oss << "\n  Example:\n";
                    oss << "    ✗ Wrong: arr[\"0\"]      (string index)\n";
                    oss << "    ✓ Right: arr[0]         (int index)\n";
                    oss << "    ✓ Right: dict[\"key\"]   (use dict for string keys)\n";
                    throw std::runtime_error(oss.str());
                }

                int index = right->toInt();

                if (index < 0 || index >= static_cast<int>(list.size())) {
                    std::ostringstream oss;
                    oss << "Index error: Array index out of bounds\n\n";
                    oss << "  Index: " << index << "\n";
                    oss << "  Array size: " << list.size() << "\n";
                    oss << "  Valid range: 0 to " << (list.size() > 0 ? list.size() - 1 : 0) << "\n\n";
                    oss << "  Help:\n";
                    oss << "  - Array indices start at 0\n";
                    oss << "  - Check array size before accessing\n";
                    oss << "  - Use array.length(arr) to get size\n\n";
                    oss << "  Example:\n";
                    oss << "    let arr = [10, 20, 30]  // size = 3\n";
                    oss << "    ✗ Wrong: arr[3]  // out of bounds\n";
                    oss << "    ✓ Right: arr[2]  // last element\n";
                    throw std::runtime_error(oss.str());
                }

                result_ = list[static_cast<size_t>(index)];
                break;
            }

            std::ostringstream oss;
            std::string type_name = getTypeName(left);
            oss << "Type error: Subscript operation not supported\n\n";
            oss << "  Tried to subscript: " << type_name << "\n";
            oss << "  Supported types: array, dict\n\n";
            oss << "  Help:\n";
            if (type_name == "null" || type_name == "unknown") {
                oss << "  - The value is null/undefined. This often means:\n";
                oss << "    - A polyglot block (<<python/js/cpp>>) failed and returned null\n";
                oss << "    - A function didn't return a value\n";
                oss << "    - A variable was never assigned\n";
                oss << "  - Check the output above for [PY ADAPTER ERROR] or similar messages\n";
                oss << "  - Add error handling: if result != null { result[\"key\"] }\n\n";
            } else {
                oss << "  - Only arrays and dictionaries support subscript access []\n";
                oss << "  - For arrays: use integer indices (arr[0], arr[1])\n";
                oss << "  - For dicts: use string keys (dict[\"key\"])\n\n";
            }
            oss << "  Example:\n";
            oss << "    ✗ Wrong: let x = 42; x[0]  // int doesn't support subscript\n";
            oss << "    ✓ Right: let arr = [1, 2, 3]; arr[0]\n";
            oss << "    ✓ Right: let dict = {\"a\": 1}; dict[\"a\"]\n";
            throw std::runtime_error(oss.str());
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
                int val = operand->toInt();
                if (val == INT_MIN) {
                    std::ostringstream oss;
                    oss << "Math error: Integer overflow in negation\n\n";
                    oss << "  Expression: -(" << val << ")\n";
                    oss << "  -INT_MIN (" << val << ") exceeds INT_MAX (" << INT_MAX << ")\n\n";
                    oss << "  Help:\n";
                    oss << "  - Use float for this value: -(" << val << ".0)\n";
                    throw std::runtime_error(oss.str());
                }
                result_ = std::make_shared<Value>(-val);
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
        // First check if the object is a dict/array/string with built-in methods
        // We need to evaluate the object BEFORE evaluating the full member access,
        // because built-in methods like .get(), .has() are not stored as dict keys
        {
            auto obj_val = eval(*member_expr->getObject());
            std::string method_name = member_expr->getMember();

            // Phase 12: Check if this is a persistent runtime .exec() call
            if (auto* str_val = std::get_if<std::string>(&obj_val->data)) {
                if (str_val->find("__NAAB_RUNTIME__:") == 0 && method_name == "exec") {
                    std::string runtime_name = str_val->substr(17);  // len("__NAAB_RUNTIME__:")
                    auto it = named_runtimes_.find(runtime_name);
                    if (it == named_runtimes_.end()) {
                        throw std::runtime_error("Runtime error: Runtime '" + runtime_name + "' not found");
                    }

                    // Extract the code from the argument (expects InlineCodeExpr or string)
                    if (args.empty()) {
                        throw std::runtime_error(
                            "Runtime error: " + runtime_name + ".exec() requires a polyglot block argument.\n\n"
                            "  Example: " + runtime_name + ".exec(<<" + it->second.language + "\n"
                            "    your code here\n"
                            "  >>)\n");
                    }

                    // The argument should be the result of an InlineCodeExpr evaluation
                    // OR a string value. For inline code blocks, the block was already executed
                    // by the InlineCodeExpr visitor. We need a different approach.
                    // For now: accept a string argument and execute it on the persistent runtime
                    std::string code;
                    if (auto* code_str = std::get_if<std::string>(&args[0]->data)) {
                        code = *code_str;
                    } else {
                        // If the argument is the result of an InlineCodeExpr, use result directly
                        result_ = args[0];
                        return;
                    }

                    auto& rt = it->second;

                    // For subprocess-based languages, accumulate code
                    bool is_embedded = (rt.language == "python" || rt.language == "javascript" ||
                                       rt.language == "js");
                    if (!is_embedded) {
                        rt.code_buffer += code + "\n";
                        code = rt.code_buffer;
                    }

                    // Detect if code is a statement (no return value expected)
                    // vs an expression (return value expected)
                    std::string trimmed_code = code;
                    size_t fc = trimmed_code.find_first_not_of(" \t\n\r");
                    if (fc != std::string::npos) trimmed_code = trimmed_code.substr(fc);

                    bool is_statement = (
                        trimmed_code.find("var ") == 0 ||
                        trimmed_code.find("let ") == 0 ||
                        trimmed_code.find("const ") == 0 ||
                        trimmed_code.find("function ") == 0 ||
                        trimmed_code.find("import ") == 0 ||
                        trimmed_code.find("class ") == 0 ||
                        trimmed_code.find("def ") == 0 ||
                        trimmed_code.find("from ") == 0 ||
                        trimmed_code.find("for ") == 0 ||
                        trimmed_code.find("while ") == 0 ||
                        trimmed_code.find("if ") == 0);

                    // Execute on the persistent executor
                    try {
                        bool is_js = (rt.language == "javascript" || rt.language == "js");
                        if (is_js) {
                            // For JS: Use BLOCK_LIBRARY mode for global scope persistence
                            auto* js_adapter = dynamic_cast<runtime::JsExecutorAdapter*>(rt.executor.get());
                            if (js_adapter) {
                                if (is_statement) {
                                    js_adapter->execute(code, runtime::JsExecutionMode::BLOCK_LIBRARY);
                                    result_ = std::make_shared<Value>();
                                } else {
                                    // For expressions: evaluate directly in global scope
                                    // executeWithReturn wraps in parens for single expr, which
                                    // accesses globals since QuickJS context is shared
                                    result_ = js_adapter->executeWithReturn(code);
                                }
                            } else {
                                result_ = rt.executor->executeWithReturn(code);
                            }
                        } else if (is_statement) {
                            // Statement mode: no return value
                            rt.executor->execute(code);
                            result_ = std::make_shared<Value>();
                        } else {
                            // Expression mode: capture return value
                            result_ = rt.executor->executeWithReturn(code);
                        }
                    } catch (const std::exception& e) {
                        std::string err = e.what();

                        // Detect scope isolation errors and provide helpful guidance
                        bool is_scope_error = (
                            err.find("NameError") != std::string::npos ||
                            err.find("ReferenceError") != std::string::npos ||
                            err.find("ModuleNotFoundError") != std::string::npos ||
                            err.find("ImportError") != std::string::npos ||
                            err.find("Cannot find module") != std::string::npos);

                        if (is_scope_error) {
                            // Extract the undefined name from quotes
                            std::string missing;
                            size_t q1 = err.find('\'');
                            if (q1 != std::string::npos) {
                                size_t q2 = err.find('\'', q1 + 1);
                                if (q2 != std::string::npos) {
                                    missing = err.substr(q1 + 1, q2 - q1 - 1);
                                }
                            }

                            std::ostringstream oss;
                            oss << "Persistent runtime '" << runtime_name << "' scope error: " << err << "\n\n"
                                << "  Help: Each .exec() call shares state with previous calls.\n"
                                << "  Import libraries in an earlier .exec() call:\n\n"
                                << "  Example:\n"
                                << "    runtime " << runtime_name << " = " << rt.language << ".start()\n";
                            if (!missing.empty()) {
                                oss << "    " << runtime_name << ".exec(<<" << rt.language << " import " << missing << " >>)\n";
                            } else {
                                oss << "    " << runtime_name << ".exec(<<" << rt.language << " import your_module >>)\n";
                            }
                            oss << "    let data = " << runtime_name << ".exec(<<" << rt.language << " ... >>)\n";
                            throw std::runtime_error(oss.str());
                        }

                        throw std::runtime_error(
                            "Runtime error in " + runtime_name + ".exec(): " + err);
                    }
                    return;
                }
            }

            // Built-in DICT methods
            if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&obj_val->data)) {
                auto& dict = *dict_ptr;

                if (method_name == "get" || method_name == "getString" || method_name == "getInt" ||
                    method_name == "getFloat" || method_name == "getBool" || method_name == "getMap" ||
                    method_name == "getList") {
                    if (args.empty()) throw std::runtime_error("dict." + method_name + "() requires at least 1 argument (key)");
                    auto key = args[0]->toString();
                    auto it = dict.find(key);
                    if (it != dict.end()) {
                        result_ = it->second;
                    } else if (args.size() >= 2) {
                        result_ = args[1];
                    } else {
                        result_ = std::make_shared<Value>();
                    }
                    return;
                }
                if (method_name == "has" || method_name == "contains" || method_name == "containsKey") {
                    if (args.empty()) throw std::runtime_error("dict.has() requires 1 argument (key)");
                    result_ = std::make_shared<Value>(dict.find(args[0]->toString()) != dict.end());
                    return;
                }
                if (method_name == "size" || method_name == "length") {
                    result_ = std::make_shared<Value>(static_cast<int>(dict.size()));
                    return;
                }
                if (method_name == "isEmpty") {
                    result_ = std::make_shared<Value>(dict.empty());
                    return;
                }
                if (method_name == "put" || method_name == "set") {
                    if (args.size() < 2) throw std::runtime_error("dict.put() requires 2 arguments (key, value)");
                    dict[args[0]->toString()] = args[1];
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    result_ = std::make_shared<Value>();
                    return;
                }
                if (method_name == "remove" || method_name == "delete") {
                    if (args.empty()) throw std::runtime_error("dict.remove() requires 1 argument (key)");
                    dict.erase(args[0]->toString());
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    result_ = std::make_shared<Value>();
                    return;
                }
                if (method_name == "keys") {
                    std::vector<std::shared_ptr<Value>> keys;
                    for (const auto& pair : dict) keys.push_back(std::make_shared<Value>(pair.first));
                    result_ = std::make_shared<Value>(keys);
                    return;
                }
                if (method_name == "values") {
                    std::vector<std::shared_ptr<Value>> vals;
                    for (const auto& pair : dict) vals.push_back(pair.second);
                    result_ = std::make_shared<Value>(vals);
                    return;
                }
                if (method_name == "clone" || method_name == "copy") {
                    result_ = std::make_shared<Value>(dict);
                    return;
                }
                // Not a built-in - fall through to check if it's a function stored in dict
            }

            // Built-in ARRAY methods
            if (auto* arr_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&obj_val->data)) {
                auto& arr = *arr_ptr;

                if (method_name == "size" || method_name == "length") {
                    result_ = std::make_shared<Value>(static_cast<int>(arr.size()));
                    return;
                }
                if (method_name == "isEmpty") {
                    result_ = std::make_shared<Value>(arr.empty());
                    return;
                }
                if (method_name == "add" || method_name == "push" || method_name == "append") {
                    if (args.empty()) throw std::runtime_error("array.add() requires 1 argument");
                    arr.push_back(args[0]);
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    result_ = obj_val;
                    return;
                }
                if (method_name == "get") {
                    if (args.empty()) throw std::runtime_error("array.get() requires 1 argument (index)");
                    int idx = std::get<int>(args[0]->data);
                    if (idx < 0 || idx >= static_cast<int>(arr.size())) {
                        throw std::runtime_error(fmt::format("Array index out of bounds: {} (size: {})", idx, arr.size()));
                    }
                    result_ = arr[static_cast<size_t>(idx)];
                    return;
                }
                if (method_name == "contains" || method_name == "includes") {
                    if (args.empty()) throw std::runtime_error("array.contains() requires 1 argument");
                    bool found = false;
                    for (const auto& item : arr) {
                        if (item->toString() == args[0]->toString()) { found = true; break; }
                    }
                    result_ = std::make_shared<Value>(found);
                    return;
                }
                if (method_name == "take") {
                    if (args.empty()) throw std::runtime_error("array.take() requires 1 argument (count)");
                    int count = std::get<int>(args[0]->data);
                    std::vector<std::shared_ptr<Value>> taken;
                    for (int i = 0; i < count && i < static_cast<int>(arr.size()); i++) {
                        taken.push_back(arr[static_cast<size_t>(i)]);
                    }
                    result_ = std::make_shared<Value>(taken);
                    return;
                }
                if (method_name == "clone" || method_name == "copy") {
                    result_ = std::make_shared<Value>(arr);
                    return;
                }
                if (method_name == "remove" || method_name == "removeAt") {
                    if (args.empty()) throw std::runtime_error("array.remove() requires 1 argument (index)");
                    int idx = std::get<int>(args[0]->data);
                    if (idx >= 0 && idx < static_cast<int>(arr.size())) {
                        arr.erase(arr.begin() + idx);
                    }
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    result_ = obj_val;
                    return;
                }
                // Type-cast no-ops: .asList(), .toList(), .asArray(), .toArray()
                if (method_name == "asList" || method_name == "toList" ||
                    method_name == "asArray" || method_name == "toArray") {
                    result_ = obj_val; // Already an array, return as-is
                    return;
                }
                // join(separator) - join array elements into a string
                if (method_name == "join") {
                    std::string sep = args.empty() ? "," : args[0]->toString();
                    std::string joined;
                    for (size_t i = 0; i < arr.size(); i++) {
                        if (i > 0) joined += sep;
                        joined += arr[i]->toString();
                    }
                    result_ = std::make_shared<Value>(joined);
                    return;
                }
                // reverse() - return reversed copy
                if (method_name == "reverse" || method_name == "reversed") {
                    std::vector<std::shared_ptr<Value>> rev(arr.rbegin(), arr.rend());
                    result_ = std::make_shared<Value>(rev);
                    return;
                }
                // indexOf(item) - find index of item, -1 if not found
                if (method_name == "indexOf" || method_name == "findIndex") {
                    if (args.empty()) throw std::runtime_error("array.indexOf() requires 1 argument");
                    for (int i = 0; i < static_cast<int>(arr.size()); i++) {
                        if (arr[i]->toString() == args[0]->toString()) {
                            result_ = std::make_shared<Value>(i);
                            return;
                        }
                    }
                    result_ = std::make_shared<Value>(-1);
                    return;
                }
                // Not a built-in array method - fall through
            }

            // Built-in STRING methods (skip module markers)
            if (auto* str_ptr = std::get_if<std::string>(&obj_val->data)) {
                auto& str = *str_ptr;

                // Skip stdlib/module markers - these are handled by the module system
                if (str.substr(0, 18) == "__stdlib_module__:" ||
                    str.substr(0, 10) == "__module__:") {
                    // Fall through to normal member access (module.function call)
                    goto normal_member_access;
                }

                if (method_name == "size" || method_name == "length") {
                    result_ = std::make_shared<Value>(static_cast<int>(str.size()));
                    return;
                }
                if (method_name == "isEmpty") {
                    result_ = std::make_shared<Value>(str.empty());
                    return;
                }
                if (method_name == "contains" || method_name == "includes") {
                    if (args.empty()) throw std::runtime_error("string.contains() requires 1 argument");
                    result_ = std::make_shared<Value>(str.find(args[0]->toString()) != std::string::npos);
                    return;
                }
                if (method_name == "indexOf") {
                    if (args.empty()) throw std::runtime_error("string.indexOf() requires 1 argument");
                    auto pos = str.find(args[0]->toString());
                    result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                    return;
                }
                if (method_name == "lastIndexOf") {
                    if (args.empty()) throw std::runtime_error("string.lastIndexOf() requires 1 argument");
                    auto pos = str.rfind(args[0]->toString());
                    result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                    return;
                }
                if (method_name == "substring" || method_name == "substr" || method_name == "slice") {
                    if (args.empty()) throw std::runtime_error("string.substring() requires at least 1 argument");
                    int start = std::get<int>(args[0]->data);
                    if (start < 0) start = 0;
                    if (start >= static_cast<int>(str.size())) {
                        result_ = std::make_shared<Value>(std::string(""));
                        return;
                    }
                    if (args.size() >= 2) {
                        int end = std::get<int>(args[1]->data);
                        if (end > static_cast<int>(str.size())) end = static_cast<int>(str.size());
                        result_ = std::make_shared<Value>(str.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
                    } else {
                        result_ = std::make_shared<Value>(str.substr(static_cast<size_t>(start)));
                    }
                    return;
                }
                if (method_name == "replace") {
                    if (args.size() < 2) throw std::runtime_error("string.replace() requires 2 arguments (old, new)");
                    std::string old_s = args[0]->toString(), new_s = args[1]->toString();
                    std::string result = str;
                    size_t pos = 0;
                    while ((pos = result.find(old_s, pos)) != std::string::npos) {
                        result.replace(pos, old_s.length(), new_s);
                        pos += new_s.length();
                    }
                    result_ = std::make_shared<Value>(result);
                    return;
                }
                if (method_name == "toUpperCase" || method_name == "upper") {
                    std::string result = str;
                    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
                    result_ = std::make_shared<Value>(result);
                    return;
                }
                if (method_name == "toLowerCase" || method_name == "lower") {
                    std::string result = str;
                    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
                    result_ = std::make_shared<Value>(result);
                    return;
                }
                if (method_name == "trim") {
                    std::string result = str;
                    result.erase(0, result.find_first_not_of(" \t\n\r"));
                    if (!result.empty()) result.erase(result.find_last_not_of(" \t\n\r") + 1);
                    result_ = std::make_shared<Value>(result);
                    return;
                }
                if (method_name == "split") {
                    if (args.empty()) throw std::runtime_error("string.split() requires 1 argument (separator)");
                    std::string sep = args[0]->toString();
                    std::vector<std::shared_ptr<Value>> parts;
                    if (sep.empty()) {
                        for (char c : str) parts.push_back(std::make_shared<Value>(std::string(1, c)));
                    } else {
                        size_t s = 0, p;
                        while ((p = str.find(sep, s)) != std::string::npos) {
                            parts.push_back(std::make_shared<Value>(str.substr(s, p - s)));
                            s = p + sep.size();
                        }
                        parts.push_back(std::make_shared<Value>(str.substr(s)));
                    }
                    result_ = std::make_shared<Value>(parts);
                    return;
                }
                if (method_name == "startsWith") {
                    if (args.empty()) throw std::runtime_error("string.startsWith() requires 1 argument");
                    result_ = std::make_shared<Value>(str.find(args[0]->toString()) == 0);
                    return;
                }
                if (method_name == "endsWith") {
                    if (args.empty()) throw std::runtime_error("string.endsWith() requires 1 argument");
                    std::string suffix = args[0]->toString();
                    result_ = std::make_shared<Value>(
                        str.size() >= suffix.size() &&
                        str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0
                    );
                    return;
                }
                // Not a built-in string method - fall through
            }
        }

        // Evaluate the member expression (returns PythonObjectValue for methods)
        normal_member_access:
        auto callable = eval(*member_expr);

        // If it's a Python object, call it
        if (auto* py_obj_ptr = std::get_if<std::shared_ptr<PythonObjectValue>>(&callable->data)) {
            auto& py_callable = *py_obj_ptr;

#ifdef NAAB_HAS_PYTHON
            LOG_TRACE("[CALL] Invoking Python method with {} args\n", args.size());

            // Build argument tuple for Python
            PyObject* py_args = PyTuple_New(static_cast<Py_ssize_t>(args.size()));
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

                PyTuple_SetItem(py_args, static_cast<Py_ssize_t>(i), py_arg);  // Steals reference
            }

            // Call the Python callable
            PyObject* py_result = PyObject_CallObject(py_callable->obj, py_args);
            Py_DECREF(py_args);

            if (py_result != nullptr) {
                // Convert result to NAAb Value
                if (PyLong_Check(py_result)) {
                    long val = PyLong_AsLong(py_result);
                    result_ = std::make_shared<Value>(static_cast<int>(val));
                    LOG_DEBUG("[SUCCESS] Method returned int: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyFloat_Check(py_result)) {
                    double val = PyFloat_AsDouble(py_result);
                    result_ = std::make_shared<Value>(val);
                    LOG_DEBUG("[SUCCESS] Method returned float: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyUnicode_Check(py_result)) {
                    const char* val = PyUnicode_AsUTF8(py_result);
                    result_ = std::make_shared<Value>(std::string(val));
                    LOG_DEBUG("[SUCCESS] Method returned string: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyBool_Check(py_result)) {
                    bool val = py_result == Py_True;
                    result_ = std::make_shared<Value>(val);
                    LOG_DEBUG("[SUCCESS] Method returned bool: {}\n", val);
                    Py_DECREF(py_result);
                } else if (py_result == Py_None) {
                    result_ = std::make_shared<Value>();
                    LOG_DEBUG("[SUCCESS] Method returned None\n");
                    Py_DECREF(py_result);
                } else {
                    // Complex object - wrap for further chaining
                    auto py_obj = std::make_shared<PythonObjectValue>(py_result);
                    result_ = std::make_shared<Value>(py_obj);
                    LOG_DEBUG("[SUCCESS] Method returned Python object: {}\n", py_obj->repr);
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

            LOG_TRACE("[CALL] Invoking block method {}.{} with {} args\n",
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
                result_ = executor->callFunction(block->member_path, args);
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
                profileEnd("BLOCK-JS calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                LOG_DEBUG("[SUCCESS] JavaScript function returned\n");

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
                result_ = executor->callFunction(block->member_path, args);
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
                profileEnd("BLOCK-CPP calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                LOG_DEBUG("[SUCCESS] C++ function returned\n");

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
                result_ = executor->callFunction(block->member_path, args);
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
                profileEnd("BLOCK-PY calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                LOG_DEBUG("[SUCCESS] Python function returned\n");

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

                    LOG_TRACE("[STDLIB] Calling {}.{}() with {} args\n",
                              module_alias, func_name, args.size());

                    // Call the stdlib function
                    result_ = module->call(func_name, args);
                    LOG_TRACE("[SUCCESS] Stdlib function returned\n");

                    // Auto-mutation: If this is a mutating function, update the original variable
                    if (module->isMutatingFunction(func_name) && !args.empty()) {
                        // Get the first argument expression (the variable being mutated)
                        auto& first_arg_expr = node.getArgs()[0];

                        // Only auto-mutate simple identifiers (not complex expressions)
                        if (auto* id_expr = dynamic_cast<ast::IdentifierExpr*>(first_arg_expr.get())) {
                            std::string var_name = id_expr->getName();

                            // Update the variable
                            if (current_env_->has(var_name)) {
                                // For pop/shift, the modified array is in args[0], not result
                                if (func_name == "pop" || func_name == "shift") {
                                    current_env_->set(var_name, args[0]);
                                } else {
                                    // For push/unshift/reverse/sort, use the result
                                    current_env_->set(var_name, result_);
                                }
                                LOG_TRACE("[MUTATION] Auto-updated {} after {}.{}()\n",
                                         var_name, module_alias, func_name);
                            }
                        }
                    }

                    return;
                }
            }
        }

        // Otherwise continue with normal handling below
    }

    // Handle member access calls (e.g., module.function(...))
    auto* member_call = dynamic_cast<ast::MemberExpr*>(node.getCallee());
    if (member_call) {
        std::string method_name = member_call->getMember();

        // Evaluate the object part to check for built-in methods on dicts/arrays/strings
        auto obj = eval(*member_call->getObject());

        // ===== Built-in DICT methods =====
        if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&obj->data)) {
            auto& dict = *dict_ptr;

            if (method_name == "get" || method_name == "getString" || method_name == "getInt" ||
                method_name == "getFloat" || method_name == "getBool" || method_name == "getMap" ||
                method_name == "getList") {
                if (args.empty()) throw std::runtime_error("dict." + method_name + "() requires at least 1 argument (key)");
                auto key = args[0]->toString();
                auto it = dict.find(key);
                if (it != dict.end()) {
                    result_ = it->second;
                } else if (args.size() >= 2) {
                    result_ = args[1];  // default value
                } else {
                    result_ = std::make_shared<Value>();  // null
                }
                return;
            }
            if (method_name == "has" || method_name == "contains" || method_name == "containsKey") {
                if (args.empty()) throw std::runtime_error("dict.has() requires 1 argument (key)");
                auto key = args[0]->toString();
                result_ = std::make_shared<Value>(dict.find(key) != dict.end());
                return;
            }
            if (method_name == "size" || method_name == "length") {
                result_ = std::make_shared<Value>(static_cast<int>(dict.size()));
                return;
            }
            if (method_name == "isEmpty") {
                result_ = std::make_shared<Value>(dict.empty());
                return;
            }
            if (method_name == "put" || method_name == "set") {
                if (args.size() < 2) throw std::runtime_error("dict.put() requires 2 arguments (key, value)");
                auto key = args[0]->toString();
                dict[key] = args[1];
                // Update the original variable
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                result_ = std::make_shared<Value>();
                return;
            }
            if (method_name == "remove" || method_name == "delete") {
                if (args.empty()) throw std::runtime_error("dict.remove() requires 1 argument (key)");
                auto key = args[0]->toString();
                dict.erase(key);
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                result_ = std::make_shared<Value>();
                return;
            }
            if (method_name == "keys") {
                std::vector<std::shared_ptr<Value>> keys;
                for (const auto& pair : dict) {
                    keys.push_back(std::make_shared<Value>(pair.first));
                }
                result_ = std::make_shared<Value>(keys);
                return;
            }
            if (method_name == "values") {
                std::vector<std::shared_ptr<Value>> vals;
                for (const auto& pair : dict) {
                    vals.push_back(pair.second);
                }
                result_ = std::make_shared<Value>(vals);
                return;
            }
            if (method_name == "clone" || method_name == "copy") {
                auto new_dict = dict;  // shallow copy
                result_ = std::make_shared<Value>(new_dict);
                return;
            }

            // Not a built-in dict method - check if it's a function stored in the dict
            auto it = dict.find(method_name);
            if (it != dict.end()) {
                auto func_value = it->second;
                if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&func_value->data)) {
                    result_ = callFunction(func_value, args);
                    return;
                }
                // Not a function - fall through to error
            }

            // Dict method not found error
            std::ostringstream oss;
            oss << "Name error: Unknown dict method '" << method_name << "'\n\n";
            oss << "  Available dict methods:\n";
            oss << "    .get(key), .get(key, default)   - get value by key\n";
            oss << "    .has(key)                       - check if key exists\n";
            oss << "    .size()                         - number of entries\n";
            oss << "    .isEmpty()                      - check if empty\n";
            oss << "    .put(key, value)                - add/update entry\n";
            oss << "    .remove(key)                    - remove entry\n";
            oss << "    .keys(), .values()              - get keys/values as array\n";
            oss << "    .clone()                        - shallow copy\n";
            if (!dict.empty()) {
                oss << "\n  Dict keys: ";
                size_t count = 0;
                for (const auto& pair : dict) {
                    if (count > 0) oss << ", ";
                    oss << pair.first;
                    if (++count >= 10) { oss << "..."; break; }
                }
                oss << "\n";
                oss << "  Access keys with: dict.keyName or dict.get(\"keyName\")\n";
            }
            throw std::runtime_error(oss.str());
        }

        // ===== Built-in ARRAY methods =====
        if (auto* arr_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&obj->data)) {
            auto& arr = *arr_ptr;

            if (method_name == "size" || method_name == "length") {
                result_ = std::make_shared<Value>(static_cast<int>(arr.size()));
                return;
            }
            if (method_name == "isEmpty") {
                result_ = std::make_shared<Value>(arr.empty());
                return;
            }
            if (method_name == "add" || method_name == "push" || method_name == "append") {
                if (args.empty()) throw std::runtime_error("array.add() requires 1 argument");
                arr.push_back(args[0]);
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                result_ = obj;
                return;
            }
            if (method_name == "get") {
                if (args.empty()) throw std::runtime_error("array.get() requires 1 argument (index)");
                int idx = std::get<int>(args[0]->data);
                if (idx < 0 || idx >= static_cast<int>(arr.size())) {
                    throw std::runtime_error(fmt::format("Array index out of bounds: {} (size: {})", idx, arr.size()));
                }
                result_ = arr[idx];
                return;
            }
            if (method_name == "contains" || method_name == "includes") {
                if (args.empty()) throw std::runtime_error("array.contains() requires 1 argument");
                bool found = false;
                for (const auto& item : arr) {
                    if (item->toString() == args[0]->toString()) { found = true; break; }
                }
                result_ = std::make_shared<Value>(found);
                return;
            }
            if (method_name == "take") {
                if (args.empty()) throw std::runtime_error("array.take() requires 1 argument (count)");
                int count = std::get<int>(args[0]->data);
                std::vector<std::shared_ptr<Value>> taken;
                for (int i = 0; i < count && i < static_cast<int>(arr.size()); i++) {
                    taken.push_back(arr[i]);
                }
                result_ = std::make_shared<Value>(taken);
                return;
            }
            if (method_name == "clone" || method_name == "copy") {
                auto new_arr = arr;
                result_ = std::make_shared<Value>(new_arr);
                return;
            }
            if (method_name == "remove" || method_name == "removeAt") {
                if (args.empty()) throw std::runtime_error("array.remove() requires 1 argument (index)");
                int idx = std::get<int>(args[0]->data);
                if (idx >= 0 && idx < static_cast<int>(arr.size())) {
                    arr.erase(arr.begin() + idx);
                }
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                result_ = obj;
                return;
            }
            // Type-cast no-ops: .asList(), .toList(), .asArray(), .toArray()
            if (method_name == "asList" || method_name == "toList" ||
                method_name == "asArray" || method_name == "toArray") {
                result_ = obj; // Already an array, return as-is
                return;
            }
            // join(separator)
            if (method_name == "join") {
                std::string sep = args.empty() ? "," : args[0]->toString();
                std::string joined;
                for (size_t i = 0; i < arr.size(); i++) {
                    if (i > 0) joined += sep;
                    joined += arr[i]->toString();
                }
                result_ = std::make_shared<Value>(joined);
                return;
            }
            // reverse()
            if (method_name == "reverse" || method_name == "reversed") {
                std::vector<std::shared_ptr<Value>> rev(arr.rbegin(), arr.rend());
                result_ = std::make_shared<Value>(rev);
                return;
            }
            // indexOf(item)
            if (method_name == "indexOf" || method_name == "findIndex") {
                if (args.empty()) throw std::runtime_error("array.indexOf() requires 1 argument");
                for (int i = 0; i < static_cast<int>(arr.size()); i++) {
                    if (arr[i]->toString() == args[0]->toString()) {
                        result_ = std::make_shared<Value>(i);
                        return;
                    }
                }
                result_ = std::make_shared<Value>(-1);
                return;
            }
            // Not a built-in array method - fall through to normal handling
        }

        // ===== Built-in STRING methods (skip module markers) =====
        if (auto* str_ptr = std::get_if<std::string>(&obj->data)) {
            auto& str = *str_ptr;

            if (str.substr(0, 18) == "__stdlib_module__:" ||
                str.substr(0, 10) == "__module__:") {
                // Fall through to normal member access
            } else

            if (method_name == "size" || method_name == "length") {
                result_ = std::make_shared<Value>(static_cast<int>(str.size()));
                return;
            }
            if (method_name == "isEmpty") {
                result_ = std::make_shared<Value>(str.empty());
                return;
            }
            if (method_name == "contains" || method_name == "includes") {
                if (args.empty()) throw std::runtime_error("string.contains() requires 1 argument");
                result_ = std::make_shared<Value>(str.find(args[0]->toString()) != std::string::npos);
                return;
            }
            if (method_name == "indexOf") {
                if (args.empty()) throw std::runtime_error("string.indexOf() requires 1 argument");
                auto pos = str.find(args[0]->toString());
                result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                return;
            }
            if (method_name == "lastIndexOf") {
                if (args.empty()) throw std::runtime_error("string.lastIndexOf() requires 1 argument");
                auto pos = str.rfind(args[0]->toString());
                result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                return;
            }
            if (method_name == "substring" || method_name == "substr" || method_name == "slice") {
                if (args.empty()) throw std::runtime_error("string.substring() requires at least 1 argument (start)");
                int start = std::get<int>(args[0]->data);
                if (start < 0) start = 0;
                if (start >= static_cast<int>(str.size())) {
                    result_ = std::make_shared<Value>(std::string(""));
                    return;
                }
                if (args.size() >= 2) {
                    int end = std::get<int>(args[1]->data);
                    if (end > static_cast<int>(str.size())) end = static_cast<int>(str.size());
                    result_ = std::make_shared<Value>(str.substr(start, end - start));
                } else {
                    result_ = std::make_shared<Value>(str.substr(start));
                }
                return;
            }
            if (method_name == "replace") {
                if (args.size() < 2) throw std::runtime_error("string.replace() requires 2 arguments (old, new)");
                std::string old_str = args[0]->toString();
                std::string new_str = args[1]->toString();
                std::string result = str;
                size_t pos = 0;
                while ((pos = result.find(old_str, pos)) != std::string::npos) {
                    result.replace(pos, old_str.length(), new_str);
                    pos += new_str.length();
                }
                result_ = std::make_shared<Value>(result);
                return;
            }
            if (method_name == "toUpperCase" || method_name == "upper") {
                std::string result = str;
                std::transform(result.begin(), result.end(), result.begin(), ::toupper);
                result_ = std::make_shared<Value>(result);
                return;
            }
            if (method_name == "toLowerCase" || method_name == "lower") {
                std::string result = str;
                std::transform(result.begin(), result.end(), result.begin(), ::tolower);
                result_ = std::make_shared<Value>(result);
                return;
            }
            if (method_name == "trim") {
                std::string result = str;
                result.erase(0, result.find_first_not_of(" \t\n\r"));
                result.erase(result.find_last_not_of(" \t\n\r") + 1);
                result_ = std::make_shared<Value>(result);
                return;
            }
            if (method_name == "split") {
                if (args.empty()) throw std::runtime_error("string.split() requires 1 argument (separator)");
                std::string sep = args[0]->toString();
                std::vector<std::shared_ptr<Value>> parts;
                if (sep.empty()) {
                    for (char c : str) parts.push_back(std::make_shared<Value>(std::string(1, c)));
                } else {
                    size_t start = 0, pos;
                    while ((pos = str.find(sep, start)) != std::string::npos) {
                        parts.push_back(std::make_shared<Value>(str.substr(start, pos - start)));
                        start = pos + sep.size();
                    }
                    parts.push_back(std::make_shared<Value>(str.substr(start)));
                }
                result_ = std::make_shared<Value>(parts);
                return;
            }
            if (method_name == "startsWith") {
                if (args.empty()) throw std::runtime_error("string.startsWith() requires 1 argument");
                result_ = std::make_shared<Value>(str.find(args[0]->toString()) == 0);
                return;
            }
            if (method_name == "endsWith") {
                if (args.empty()) throw std::runtime_error("string.endsWith() requires 1 argument");
                std::string suffix = args[0]->toString();
                result_ = std::make_shared<Value>(
                    str.size() >= suffix.size() &&
                    str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0
                );
                return;
            }
            // Not a built-in string method - fall through to normal handling
        }

        // ===== Normal member access call (functions stored in dicts, struct methods, etc.) =====
        // Evaluate the full member access
        member_call->accept(*this);
        auto func_value = result_;

        // Check if it's a function
        if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&func_value->data)) {
            result_ = callFunction(func_value, args);
            return;
        }

        std::ostringstream oss;
        oss << "Type error: Member is not callable\n\n";
        oss << "  Member type: " << getTypeName(func_value) << "\n";
        oss << "  Expected: function\n\n";

        // Detect stdlib constant access with () - e.g., math.PI()
        auto* obj_id_err = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
        if (obj_id_err && (method_name == "PI" || method_name == "E")) {
            std::string mod_name = obj_id_err->getName();
            oss << "  Help:\n";
            oss << "  - " << mod_name << "." << method_name << " is a constant, not a function\n";
            oss << "  - Access it without parentheses:\n\n";
            oss << "  Example:\n";
            oss << "    ✗ Wrong: " << mod_name << "." << method_name << "()\n";
            oss << "    ✓ Right: " << mod_name << "." << method_name << "\n";
        } else {
            oss << "  Help:\n";
            oss << "  - Only functions can be called with ()\n";
            oss << "  - If accessing a property or constant, don't use ()\n\n";
            oss << "  Example:\n";
            oss << "    ✗ Wrong: obj.value()    // value is not a function\n";
            oss << "    ✓ Right: obj.value       // access the property directly\n";
            oss << "    ✓ Right: obj.getValue()  // call a function instead\n";
        }
        throw std::runtime_error(oss.str());
    }

    // Try to get function name (for built-ins and named functions)
    auto* id_expr = dynamic_cast<ast::IdentifierExpr*>(node.getCallee());

    // If callee is not an identifier (e.g., array[0], higher-order function result),
    // evaluate it and check if it's a callable function
    if (!id_expr) {
        node.getCallee()->accept(*this);
        auto callee_value = result_;

        // Check if the result is a function
        if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&callee_value->data)) {
            (void)func_ptr;  // Type check only, value not needed
            // Call the function using the general callFunction helper
            result_ = callFunction(callee_value, args);
            return;
        }

        std::ostringstream oss;
        oss << "Type error: Expression is not callable\n\n";
        oss << "  Tried to call: " << getTypeName(callee_value) << "\n";
        oss << "  Expected: function\n\n";
        oss << "  Help:\n";
        oss << "  - Only functions can be called with ()\n";
        oss << "  - If you're calling arr[i], make sure arr contains functions\n";
        oss << "  - If you're using higher-order functions, verify they return functions\n\n";
        oss << "  Example:\n";
        oss << "    ✗ Wrong: let arr = [1, 2, 3]; arr[0]()  // int isn't callable\n";
        oss << "    ✓ Right: let fns = [function() { ... }]; fns[0]()\n";
        throw std::runtime_error(oss.str());
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
                // Build parameter list for error message
                std::ostringstream oss;
                oss << "Function " << func->name << " expects " << min_args << "-"
                    << func->params.size() << " arguments, got " << args.size() << "\n"
                    << "  Function: " << func->name << "(";

                // Show parameters
                for (size_t i = 0; i < func->params.size(); ++i) {
                    if (i > 0) oss << ", ";
                    oss << func->params[i];
                }
                oss << ")\n";

                // Show what was provided
                oss << "  Provided: " << args.size() << " argument(s)";

                throw std::runtime_error(oss.str());
            }

            // Phase 2.4.4 Phase 3: Handle generic type arguments (explicit or inferred)
            std::map<std::string, ast::Type> type_substitutions;
            if (!func->type_parameters.empty()) {
                LOG_DEBUG("[INFO] Function {} is generic with type parameters: ", func->name);
                for (const auto& tp : func->type_parameters) {
                    fmt::print("{} ", tp);
                }
                fmt::print("\n");

                // Check if explicit type arguments were provided
                const auto& explicit_type_args = node.getTypeArguments();
                if (!explicit_type_args.empty()) {
                    // Use explicit type arguments
                    LOG_DEBUG("[INFO] Using {} explicit type argument(s)\n", explicit_type_args.size());

                    if (explicit_type_args.size() != func->type_parameters.size()) {
                        throw std::runtime_error(fmt::format(
                            "Function {} expects {} type parameter(s), got {}",
                            func->name, func->type_parameters.size(), explicit_type_args.size()));
                    }

                    for (size_t i = 0; i < func->type_parameters.size(); i++) {
                        type_substitutions.insert({func->type_parameters[i], explicit_type_args[i]});
                        LOG_DEBUG("[INFO] Type parameter {} = {}\n",
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

            // Issue #3: Push file context for function's source file
            if (!func->source_file.empty()) {
                pushFileContext(func->source_file);
            }

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
                // Issue #3: Pop file context on error
                if (!func->source_file.empty()) {
                    popFileContext();
                }
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

            // Issue #3: Pop file context on success
            if (!func->source_file.empty()) {
                popFileContext();
            }

            // Restore environment and function
            current_env_ = saved_env;
            returning_ = saved_returning;
            current_function_ = saved_function;  // Phase 2.4.2: Restore
            current_type_substitutions_ = saved_type_subst;  // Phase 2.4.4: Restore
            current_file_ = saved_file;  // Phase 3.1: Restore file

            LOG_TRACE("[CALL] Function {} executed\n", func->name);
            return;
        }

        // Check for loaded block
        if (auto* block_ptr = std::get_if<std::shared_ptr<BlockValue>>(&value->data)) {
            auto& block = *block_ptr;

            LOG_TRACE("[CALL] Invoking block {} ({}) with {} args\n",
                      block->metadata.name, block->metadata.language, args.size());

            // Phase 7: Try executor-based calling first
            auto* executor = block->getExecutor();
            if (executor) {
                LOG_DEBUG("[INFO] Calling block via executor ({})...\n", block->metadata.language);

                // Determine function name to call:
                // - If member_path is set, this is a member accessor (e.g., block.method)
                // - Otherwise, use the function name being called
                std::string function_to_call = block->member_path.empty()
                    ? func_name
                    : block->member_path;

                LOG_DEBUG("[INFO] Calling function: {}\n", function_to_call);
                result_ = executor->callFunction(function_to_call, args);
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output

                if (result_) {
                    LOG_DEBUG("[SUCCESS] Block call completed\n");

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
                LOG_DEBUG("[INFO] Executing Python block: {}\n", block->metadata.name);

                // Add common imports automatically
                PyRun_SimpleString("from typing import Dict, List, Optional, Any, Union\n"
                                  "import sys\n");

                // Execute the block code to define classes/functions using exec()
                // This handles multi-line code with proper indentation
                std::string exec_code = "exec('''" + block->code + "''')";
                PyRun_SimpleString(exec_code.c_str());

                // Handle member access calls
                if (!block->member_path.empty()) {
                    LOG_DEBUG("[INFO] Calling member: {}\n", block->member_path);

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
                            LOG_DEBUG("[SUCCESS] Returned int: {}\n", val);
                        } else if (PyFloat_Check(py_result)) {
                            double val = PyFloat_AsDouble(py_result);
                            result_ = std::make_shared<Value>(val);
                            LOG_DEBUG("[SUCCESS] Returned float: {}\n", val);
                        } else if (PyUnicode_Check(py_result)) {
                            const char* val = PyUnicode_AsUTF8(py_result);
                            result_ = std::make_shared<Value>(std::string(val));
                            LOG_DEBUG("[SUCCESS] Returned string: {}\n", val);
                        } else if (PyBool_Check(py_result)) {
                            bool val = py_result == Py_True;
                            result_ = std::make_shared<Value>(val);
                            LOG_DEBUG("[SUCCESS] Returned bool: {}\n", val);
                        } else if (py_result == Py_None) {
                            result_ = std::make_shared<Value>();
                            LOG_DEBUG("[SUCCESS] Returned None\n");
                            Py_DECREF(py_result);
                        } else {
                            // Complex object - wrap in PythonObjectValue for method chaining
                            auto py_obj = std::make_shared<PythonObjectValue>(py_result);
                            result_ = std::make_shared<Value>(py_obj);
                            LOG_DEBUG("[SUCCESS] Returned Python object: {}\n", py_obj->repr);
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
                    LOG_DEBUG("[INFO] Injected {} args into Python context\n", args.size());
                }

                // Execute Python code - for blocks that are classes/functions
                // Try to evaluate as expression first (for simple cases)
                PyObject* main_module = PyImport_AddModule("__main__");
                PyObject* global_dict = PyModule_GetDict(main_module);
                (void)global_dict;  // Reserved for future expression evaluation

                // For blocks that define classes, just execute and return success
                int result = PyRun_SimpleString(block->code.c_str());

                if (result == 0) {
                    LOG_DEBUG("[SUCCESS] Python block executed successfully\n");
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
            } else if (auto* dict = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&args[0]->data)) {
                result_ = std::make_shared<Value>(static_cast<int>(dict->size()));
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
                else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) return "array";
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
    // range() builtin — range(end), range(start, end), range(start, end, step)
    else if (func_name == "range") {
        if (args.empty() || args.size() > 3) {
            throw std::runtime_error(
                "Argument error: range() takes 1-3 arguments (end), (start, end), or (start, end, step)\n\n"
                "  Example:\n"
                "    range(5)        // [0, 1, 2, 3, 4]\n"
                "    range(2, 6)     // [2, 3, 4, 5]\n"
                "    range(0, 10, 2) // [0, 2, 4, 6, 8]\n");
        }

        int start = 0, end = 0, step = 1;

        if (args.size() == 1) {
            if (auto* v = std::get_if<int>(&args[0]->data)) { end = *v; }
            else if (auto* d = std::get_if<double>(&args[0]->data)) { end = static_cast<int>(*d); }
            else { throw std::runtime_error("range() arguments must be numbers"); }
        } else if (args.size() >= 2) {
            if (auto* v = std::get_if<int>(&args[0]->data)) { start = *v; }
            else if (auto* d = std::get_if<double>(&args[0]->data)) { start = static_cast<int>(*d); }
            else { throw std::runtime_error("range() arguments must be numbers"); }

            if (auto* v = std::get_if<int>(&args[1]->data)) { end = *v; }
            else if (auto* d = std::get_if<double>(&args[1]->data)) { end = static_cast<int>(*d); }
            else { throw std::runtime_error("range() arguments must be numbers"); }
        }
        if (args.size() == 3) {
            if (auto* v = std::get_if<int>(&args[2]->data)) { step = *v; }
            else if (auto* d = std::get_if<double>(&args[2]->data)) { step = static_cast<int>(*d); }
            else { throw std::runtime_error("range() arguments must be numbers"); }
        }

        if (step == 0) {
            throw std::runtime_error("range() step cannot be zero");
        }

        std::vector<std::shared_ptr<Value>> result;
        if (step > 0) {
            for (int i = start; i < end; i += step) {
                result.push_back(std::make_shared<Value>(i));
            }
        } else {
            for (int i = start; i > end; i += step) {
                result.push_back(std::make_shared<Value>(i));
            }
        }

        result_ = std::make_shared<Value>(result);
    }
    // Phase 3.2: Manual garbage collection trigger
    else if (func_name == "gc_collect") {
        runGarbageCollection(current_env_);  // Pass current environment
        result_ = std::make_shared<Value>();  // Return void
    }
    else {
        // Function not found - provide targeted hints for common mistakes
        std::ostringstream oss;
        oss << "Name error: Undefined function\n\n";
        oss << "  Function: " << func_name << "\n\n";

        // Targeted hints for commonly misused function names
        if (func_name == "sleep") {
            oss << "  'sleep' is in the time module, not a global function:\n";
            oss << "    import time\n";
            oss << "    time.sleep(1000)  // sleep for 1000 milliseconds\n";
        } else if (func_name == "exit") {
            oss << "  NAAb has no exit() function.\n";
            oss << "  To stop: return from functions, or let main block end.\n";
        } else if (func_name == "error") {
            oss << "  'error' is not a built-in. To print errors:\n";
            oss << "    print(\"ERROR: something went wrong\")\n";
        } else if (func_name == "callFunction") {
            oss << "  NAAb does not need callFunction(). Functions are first-class:\n";
            oss << "    let result = fn(arg1, arg2)   // call directly\n";
        } else if (func_name == "parseInt" || func_name == "parseFloat" || func_name == "Number") {
            oss << "  Use NAAb type conversion functions:\n";
            oss << "    int(\"42\")     // instead of parseInt(\"42\")\n";
            oss << "    float(\"3.14\") // instead of parseFloat(\"3.14\")\n";
        } else if (func_name == "toString" || func_name == "str") {
            oss << "  Use NAAb type conversion:\n";
            oss << "    string(42)    // instead of toString(42)\n";
        } else if (func_name == "keys" || func_name == "values") {
            oss << "  '" << func_name << "' is a method on dicts, not a global function:\n";
            oss << "    myDict." << func_name << "()  // correct\n";
        } else if (func_name == "push" || func_name == "append" || func_name == "pop") {
            oss << "  '" << func_name << "' is a method on arrays, not a global function:\n";
            oss << "    myArray." << func_name << "(item)  // correct\n";
            oss << "    // or: import array; array.push(myArray, item)\n";
        } else if (func_name == "forEach" || func_name == "map" || func_name == "filter" || func_name == "reduce") {
            oss << "  NAAb uses for-in loops instead of " << func_name << ":\n";
            oss << "    for item in myArray { print(item) }\n";
        } else {
            oss << "  Help:\n";
            oss << "  - Check for typos in the function name\n";
            oss << "  - Make sure the function is defined before calling\n";
            oss << "  - For stdlib functions, use module.function() (e.g., array.push())\n";
        }
        oss << "\n  Common builtins: print, len, type, typeof, int, float, string, bool\n\n";
        oss << "  Example:\n";
        oss << "    ✗ Wrong: printt(\"hello\")  // typo\n";
        oss << "    ✓ Right: print(\"hello\")\n";
        oss << "    ✓ Right: array.length([1,2,3])  // stdlib module function\n";
        throw std::runtime_error(oss.str());
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
            LOG_DEBUG("[INFO] Created member accessor: {} ({})\n",
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
            LOG_DEBUG("[INFO] Created member accessor (legacy Python): {}\n", full_member_path);
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
            LOG_DEBUG("[INFO] Accessed Python object member: {}\n", member_name);
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

        std::ostringstream oss;
        oss << "Name error: Member not found in module\n\n";
        oss << "  Member: " << member_name << "\n";

        if (dict_ptr->empty()) {
            oss << "  Module has no exported members\n";
        } else {
            oss << "  Available members: ";
            size_t count = 0;
            for (const auto& pair : *dict_ptr) {
                if (count > 0) oss << ", ";
                oss << pair.first;
                if (++count >= 10) {
                    oss << "...";
                    break;
                }
            }
            oss << "\n";
        }

        oss << "\n  Help:\n";
        oss << "  - Check spelling of member name\n";
        oss << "  - Verify the member is exported\n";
        oss << "  - Member names are case-sensitive\n\n";
        oss << "  Example:\n";
        oss << "    import mymodule\n";
        oss << "    ✗ Wrong: mymodule.MyFunc()  // case mismatch\n";
        oss << "    ✓ Right: mymodule.myFunc()\n";
        throw std::runtime_error(oss.str());
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
                std::ostringstream oss;
                oss << "Import error: Module not found\n\n";
                oss << "  Module: " << module_alias << "\n\n";
                oss << "  Help:\n";
                oss << "  - Check if module is imported at top of file\n";
                oss << "  - Verify import statement: import " << module_alias << "\n";
                oss << "  - For stdlib: array, string, math, file, env, time, etc.\n\n";
                oss << "  Example:\n";
                oss << "    import array  // add at top of file\n";
                oss << "    let arr = [1, 2, 3]\n";
                oss << "    array.push(arr, 4)\n";
                throw std::runtime_error(oss.str());
            }

            auto module = it->second;

            // ISS-034 FIX: Check if this is a constant (zero-argument function like PI, E)
            // If so, invoke it immediately instead of creating a marker
            // This prevents constants from returning __stdlib_call__ markers
            static const std::unordered_set<std::string> math_constants = {"PI", "E", "pi", "e"};
            if (module_alias == "math" && math_constants.count(member_name) > 0) {
                // Invoke the constant immediately with no arguments
                std::vector<std::shared_ptr<Value>> no_args;
                result_ = module->call(member_name, no_args);
                return;
            }

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

    // Member access on other types — type-specific helpful errors
    std::ostringstream oss;
    std::string type_name = getTypeName(obj);

    if (type_name == "array") {
        oss << "Type error: Arrays don't support dot notation\n\n";
        oss << "  Tried to access: array." << member_name << "\n\n";
        oss << "  Help: Use the array module for array operations:\n";
        if (member_name == "length" || member_name == "size" || member_name == "count") {
            oss << "    ✗ Wrong: my_array.length\n";
            oss << "    ✓ Right: len(my_array)             // built-in\n";
            oss << "    ✓ Right: array.length(my_array)    // module function\n";
        } else if (member_name == "push" || member_name == "append" || member_name == "add") {
            oss << "    ✗ Wrong: my_array.push(item)\n";
            oss << "    ✓ Right: array.push(my_array, item)\n";
        } else if (member_name == "pop") {
            oss << "    ✗ Wrong: my_array.pop()\n";
            oss << "    ✓ Right: array.pop(my_array)\n";
        } else if (member_name == "map" || member_name == "filter" || member_name == "reduce") {
            oss << "    ✗ Wrong: my_array." << member_name << "(fn)\n";
            oss << "    ✓ Right: array." << member_name << "_fn(my_array, fn)\n";
        } else if (member_name == "sort") {
            oss << "    ✗ Wrong: my_array.sort()\n";
            oss << "    ✓ Right: array.sort(my_array)\n";
        } else if (member_name == "reverse") {
            oss << "    ✗ Wrong: my_array.reverse()\n";
            oss << "    ✓ Right: array.reverse(my_array)\n";
        } else {
            oss << "    ✗ Wrong: my_array." << member_name << "(...)\n";
            oss << "    ✓ Right: array." << member_name << "(my_array, ...)\n";
        }
    } else if (type_name == "string") {
        oss << "Type error: Strings don't support dot notation\n\n";
        oss << "  Tried to access: string." << member_name << "\n\n";
        oss << "  Help: Use the string module for string operations:\n";
        if (member_name == "length" || member_name == "size") {
            oss << "    ✗ Wrong: my_string.length\n";
            oss << "    ✓ Right: len(my_string)             // built-in\n";
            oss << "    ✓ Right: string.length(my_string)  // module function\n";
        } else if (member_name == "upper" || member_name == "toUpperCase" || member_name == "toUpper") {
            oss << "    ✗ Wrong: my_string." << member_name << "()\n";
            oss << "    ✓ Right: string.upper(my_string)\n";
        } else if (member_name == "lower" || member_name == "toLowerCase" || member_name == "toLower") {
            oss << "    ✗ Wrong: my_string." << member_name << "()\n";
            oss << "    ✓ Right: string.lower(my_string)\n";
        } else if (member_name == "trim") {
            oss << "    ✗ Wrong: my_string.trim()\n";
            oss << "    ✓ Right: string.trim(my_string)\n";
        } else if (member_name == "split") {
            oss << "    ✗ Wrong: my_string.split(delim)\n";
            oss << "    ✓ Right: string.split(my_string, delim)\n";
        } else {
            oss << "    ✗ Wrong: my_string." << member_name << "(...)\n";
            oss << "    ✓ Right: string." << member_name << "(my_string, ...)\n";
        }
    } else if (type_name == "dict") {
        oss << "Type error: Dictionaries don't support dot notation for data access\n\n";
        oss << "  Tried to access: dict." << member_name << "\n\n";
        oss << "  Help: Use bracket notation for dict values:\n";
        oss << "    ✗ Wrong: my_dict." << member_name << "\n";
        oss << "    ✓ Right: my_dict[\"" << member_name << "\"]\n\n";
        oss << "  For iterating keys: for key in my_dict.keys() { }\n";
    } else {
        oss << "Type error: Member access not supported\n\n";
        oss << "  Tried to access: " << type_name << "." << member_name << "\n";
        oss << "  Supported types: struct, dict (for modules), block\n\n";
        oss << "  Help:\n";
        oss << "  - Structs support dot notation: obj.field\n";
        oss << "  - Dictionaries use bracket notation: dict[\"key\"]\n";
        oss << "  - Modules support member access: module.function()\n";
    }
    throw std::runtime_error(oss.str());
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

            error_reporter_.error(main_msg, static_cast<size_t>(loc.line), static_cast<size_t>(loc.column));
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
        case ast::LiteralKind::Int: {
            // Use stod first to avoid overflow, then check if it fits in int
            try {
                double d = std::stod(node.getValue());
                if (d >= INT_MIN && d <= INT_MAX && d == static_cast<int>(d)) {
                    result_ = std::make_shared<Value>(static_cast<int>(d));
                } else {
                    // Too large for int, store as double
                    result_ = std::make_shared<Value>(d);
                }
            } catch (const std::exception& e) {
                throw std::runtime_error("Invalid integer literal: " + node.getValue());
            }
            break;
        }
        case ast::LiteralKind::Float:
            result_ = std::make_shared<Value>(std::stod(node.getValue()));
            break;

        case ast::LiteralKind::String: {
            const std::string& raw = node.getValue();
            // Check if string contains interpolation ${...}
            if (raw.find("${") != std::string::npos) {
                std::string result;
                size_t i = 0;
                while (i < raw.size()) {
                    if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
                        // Extract expression inside ${...}
                        i += 2; // skip ${
                        int depth = 1;
                        std::string expr_text;
                        while (i < raw.size() && depth > 0) {
                            if (raw[i] == '{') depth++;
                            else if (raw[i] == '}') {
                                depth--;
                                if (depth == 0) break;
                            }
                            expr_text += raw[i];
                            i++;
                        }
                        if (i < raw.size()) i++; // skip closing }

                        // Lex, parse, and evaluate the expression
                        try {
                            naab::lexer::Lexer expr_lexer(expr_text);
                            auto expr_tokens = expr_lexer.tokenize();
                            naab::parser::Parser expr_parser(expr_tokens);
                            auto expr_ast = expr_parser.parseExpression();
                            expr_ast->accept(*this);
                            if (result_) {
                                result += result_->toString();
                            }
                        } catch (const std::exception& e) {
                            // Rethrow with context about the interpolation
                            std::string interp_err = std::string(e.what());
                            interp_err += "\n\n  Error occurred inside string interpolation: ${" + expr_text + "}\n"
                                         "  The expression inside ${...} must be a valid NAAb expression.\n"
                                         "  If calling a function stored in a variable, call it directly:\n"
                                         "    \"${myFunc()}\"              // correct\n"
                                         "    \"${Sys.callFunction(fn)}\"  // WRONG - no Sys in NAAb";
                            throw std::runtime_error(interp_err);
                        }
                    } else {
                        result += raw[i];
                        i++;
                    }
                }
                result_ = std::make_shared<Value>(result);
            } else {
                result_ = std::make_shared<Value>(raw);
            }
            break;
        }

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

// Phase 12: Header-aware injection for languages that require specific first lines
std::string Interpreter::injectDeclarationsAfterHeaders(
    const std::string& declarations, const std::string& code, const std::string& language) {

    if (declarations.empty()) return code;

    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Find the insertion point after all header lines
    int insert_after = -1;  // -1 means prepend (no headers found)
    bool in_block_import = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string trimmed = lines[i];
        // Trim leading whitespace
        size_t first_non_space = trimmed.find_first_not_of(" \t");
        if (first_non_space != std::string::npos) {
            trimmed = trimmed.substr(first_non_space);
        } else {
            // Empty/whitespace line — skip but don't break header scanning
            if (in_block_import) {
                insert_after = static_cast<int>(i);
            }
            continue;
        }

        if (language == "go") {
            // Go: package declaration must be first, then imports
            if (trimmed.substr(0, 8) == "package ") {
                insert_after = static_cast<int>(i);
                continue;
            }
            if (trimmed.substr(0, 7) == "import " && trimmed.find("(") != std::string::npos) {
                // Block import: import (
                in_block_import = true;
                insert_after = static_cast<int>(i);
                continue;
            }
            if (in_block_import) {
                insert_after = static_cast<int>(i);
                if (trimmed[0] == ')') {
                    in_block_import = false;
                }
                continue;
            }
            if (trimmed.substr(0, 7) == "import ") {
                // Single import
                insert_after = static_cast<int>(i);
                continue;
            }
            // If we haven't seen any header yet and this is a non-header line, stop scanning
            if (insert_after >= 0) break;
            // If no headers at all, stop immediately
            break;
        } else if (language == "php") {
            // PHP: <?php tag must be first
            if (trimmed.substr(0, 5) == "<?php" || trimmed.substr(0, 2) == "<?") {
                insert_after = static_cast<int>(i);
                continue;
            }
            break;
        } else if (language == "typescript" || language == "ts") {
            // TypeScript: import statements at top
            if (trimmed.substr(0, 7) == "import ") {
                insert_after = static_cast<int>(i);
                continue;
            }
            break;
        } else {
            // Unknown language — no header awareness, prepend
            break;
        }
    }

    // Build result: header lines + declarations + remaining lines
    std::string result;
    if (insert_after < 0) {
        // No headers found — prepend declarations
        result = declarations + code;
    } else {
        // Insert declarations after the header lines
        for (size_t i = 0; i <= static_cast<size_t>(insert_after); ++i) {
            result += lines[i] + "\n";
        }
        result += declarations;
        for (size_t i = static_cast<size_t>(insert_after) + 1; i < lines.size(); ++i) {
            result += lines[i] + "\n";
        }
    }
    return result;
}

void Interpreter::visit(ast::InlineCodeExpr& node) {
    std::string language = node.getLanguage();
    std::string raw_code = node.getCode();

    const auto& bound_vars = node.getBoundVariables();  // Phase 2.2

    // Get the executor early (needed for object-based variable passing)
    auto& registry = runtime::LanguageRegistry::instance();
    auto* executor = registry.getExecutor(language);
    if (!executor) {
        throw std::runtime_error("No executor found for language: " + language);
    }

    // Phase 2.2: Bind variables using string serialization
    std::string var_declarations;

    for (const auto& var_name : bound_vars) {
        // Look up variable in current environment
        if (!current_env_->has(var_name)) {
            throw std::runtime_error("Variable '" + var_name + "' not found in scope for inline code binding");
        }

        auto value = current_env_->get(var_name);

        // For all languages: use string serialization
        std::string serialized = serializeValueForLanguage(value, language);

        if (language == "python") {
            var_declarations += var_name + " = " + serialized + "\n";
        } else if (language == "shell" || language == "sh" || language == "bash") {
            // Use export for shell variables
            var_declarations += "export " + var_name + "=" + serialized + "\n";
        } else {
            if (language == "javascript" || language == "js") {
                var_declarations += "const " + var_name + " = " + serialized + ";\n";
            } else if (language == "go") {
                // Go: const only works for primitives; use var for complex types
                bool is_complex = value && (
                    std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data) ||
                    std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data));
                if (is_complex) {
                    var_declarations += "var " + var_name + " = " + serialized + "\n";
                } else {
                    var_declarations += "const " + var_name + " = " + serialized + "\n";
                }
            } else if (language == "rust") {
                var_declarations += "let " + var_name + " = " + serialized + ";\n";
            } else if (language == "cpp" || language == "c++") {
                var_declarations += "const auto " + var_name + " = " + serialized + ";\n";
            } else if (language == "ruby") {
                var_declarations += var_name + " = " + serialized + "\n";
            } else if (language == "csharp" || language == "cs") {
                var_declarations += "var " + var_name + " = " + serialized + ";\n";
            } else if (language == "typescript" || language == "ts") {
                var_declarations += "const " + var_name + " = " + serialized + ";\n";
            } else if (language == "php") {
                // PHP vars must come after <?php tag. Add tag once, then vars.
                if (var_declarations.find("<?php") == std::string::npos) {
                    var_declarations += "<?php\n";
                }
                var_declarations += "$" + var_name + " = " + serialized + ";\n";
            }
        }
    }

    // Phase 12: Inject naab_return() helper function per language
    // Only inject if naab_return is actually used in the code (avoids breaking IIFE wrapping)
    std::string return_type = node.getReturnType();
    bool code_uses_naab_return = (raw_code.find("naab_return") != std::string::npos);
    if (code_uses_naab_return) {
        std::string helper;
        if (language == "python") {
            // Python: naab_return returns data directly — CPython eval captures the return value
            helper = "def naab_return(data):\n    return data\n";
        } else if (language == "javascript" || language == "js") {
            // JS/QuickJS: naab_return just returns data — IIFE wrapping makes it the return value
            helper = "function naab_return(data) { return data; }\n";
        } else if (language == "typescript" || language == "ts") {
            helper = "function naab_return(data) { return data; }\n";
        } else if (language == "ruby") {
            helper = "require 'json'\ndef naab_return(data); puts \"__NAAB_RETURN__:\" + data.to_json; end\n";
        } else if (language == "php") {
            if (var_declarations.find("<?php") == std::string::npos) {
                helper = "<?php\n";
            }
            helper += "function naab_return($data) { echo \"__NAAB_RETURN__:\" . json_encode($data) . \"\\n\"; }\n";
        } else if (language == "shell" || language == "sh" || language == "bash") {
            helper = "naab_return() { echo \"__NAAB_RETURN__:$1\"; }\n";
        } else if (language == "rust") {
            helper = "macro_rules! naab_return { ($val:expr) => { println!(\"__NAAB_RETURN__:{}\", $val); }; }\n";
        } else if (language == "go") {
            // Go's naab_return needs to be inside func main, handled by executor wrapping
            helper = ""; // Will be added inside main by the executor
        } else if (language == "cpp" || language == "c++") {
            helper = "#include <sstream>\n#define naab_return(val) do { std::ostringstream __os; __os << \"__NAAB_RETURN__:\" << (val); std::cout << __os.str() << std::endl; } while(0)\n";
        } else if (language == "csharp" || language == "cs") {
            helper = ""; // C# needs it inside the class, handled by executor wrapping
        }
        if (!helper.empty()) {
            var_declarations = helper + var_declarations;
        }
    }

    // Strip common leading whitespace from all lines
    std::vector<std::string> lines;
    std::istringstream stream(raw_code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Find minimum indentation (ignoring empty lines)
    // ISS-028 Fix: Include first line in indentation calculation
    size_t min_indent = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {  // Include all lines
        const auto& l = lines[i];
        if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) continue;
        size_t indent = l.find_first_not_of(" \t");
        if (indent < min_indent) min_indent = indent;
    }

    // Strip the common indentation from ALL lines
    std::string code;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];

        if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) {
            // Empty or whitespace-only line
            code += "\n";
        } else {
            // Strip common indentation from all non-empty lines
            if (min_indent != std::string::npos && l.length() > min_indent) {
                code += l.substr(min_indent) + "\n";
            } else {
                code += l + "\n";
            }
        }
    }

    // Phase 2.2/12: Inject variable declarations with header awareness
    std::string final_code;
    if (!var_declarations.empty() &&
        (language == "go" || language == "php" ||
         language == "typescript" || language == "ts")) {
        final_code = injectDeclarationsAfterHeaders(var_declarations, code, language);
    } else {
        final_code = var_declarations + code;
    }

    // Phase 12: For Python with -> JSON, wrap code to capture stdout and extract last JSON line
    if (!return_type.empty() && (language == "python")) {
        std::string preamble =
            "import sys as __naab_sys, io as __naab_io, json as __naab_json\n"
            "__naab_buf = __naab_io.StringIO()\n"
            "__naab_orig = __naab_sys.stdout\n"
            "__naab_sys.stdout = __naab_buf\n";
        std::string postamble =
            "\n__naab_sys.stdout = __naab_orig\n"
            "__naab_captured = __naab_buf.getvalue().strip().split('\\n')\n"
            "__naab_result = None\n"
            "for __naab_l in reversed(__naab_captured):\n"
            "    __naab_l = __naab_l.strip()\n"
            "    if not __naab_l:\n"
            "        continue\n"
            "    try:\n"
            "        __naab_result = __naab_json.loads(__naab_l)\n"
            "        break\n"
            "    except:\n"
            "        __naab_sys.stdout.write(__naab_l + '\\n')\n"
            "__naab_result\n";
        final_code = preamble + final_code + postamble;
    }

    explain("Executing inline " + language + " code" +
            (bound_vars.empty() ? "" : " with " + std::to_string(bound_vars.size()) + " bound variables"));

    // Enterprise Security: Activate sandbox for polyglot execution
    auto& sandbox_manager = security::SandboxManager::instance();
    security::SandboxConfig sandbox_config = sandbox_manager.getDefaultConfig();

    security::ScopedSandbox scoped_sandbox(sandbox_config);

    // Phase 12: Create source mapper for error translation
    int var_decl_lines = static_cast<int>(std::count(var_declarations.begin(), var_declarations.end(), '\n'));
    runtime::SourceMapper source_mapper(current_file_, node.getLocation().line, node.getLocation().column);
    source_mapper.setOffset(var_decl_lines);

    // Phase 2.3: Execute the code and capture return value
    // Suspend GC during polyglot execution to prevent collecting live values
    gc_suspended_ = true;
    try {
        result_ = executor->executeWithReturn(final_code);

        // Phase 12: Check for sentinel/JSON return values
        // Strategy 1: Check executor's captured output buffer (works for Python)
        std::string captured = executor->getCapturedOutput();
        bool sentinel_found = false;
        if (!captured.empty()) {
            auto polyglot_result = runtime::parsePolyglotOutput(captured, return_type);
            if (polyglot_result.return_value) {
                result_ = polyglot_result.return_value;
                sentinel_found = true;
            }
            // Print remaining log output
            if (!polyglot_result.log_output.empty()) {
                std::cout << polyglot_result.log_output << std::flush;
            }
        } else {
            // Flush stdout from executor (no captured output to parse)
            flushExecutorOutput(executor);
        }

        // Strategy 2: Check if result_ is a string containing the sentinel
        // (works for shell executor which returns stdout as string value)
        if (!sentinel_found && result_) {
            if (auto* str_val = std::get_if<std::string>(&result_->data)) {
                if (str_val->find("__NAAB_RETURN__:") != std::string::npos) {
                    auto polyglot_result = runtime::parsePolyglotOutput(*str_val, return_type);
                    if (polyglot_result.return_value) {
                        result_ = polyglot_result.return_value;
                    }
                    // Print log output that was mixed with the sentinel
                    if (!polyglot_result.log_output.empty()) {
                        std::cout << polyglot_result.log_output << std::flush;
                    }
                } else if (!return_type.empty()) {
                    // Strategy 3: If -> JSON header specified, try parsing result as JSON
                    auto polyglot_result = runtime::parsePolyglotOutput(*str_val, return_type);
                    if (polyglot_result.return_value) {
                        result_ = polyglot_result.return_value;
                    }
                }
            }
        }

        // Phase 12: BLOCK_CONTRACT_VIOLATION — -> JSON declared but no JSON produced
        if (!return_type.empty() && return_type == "JSON") {
            bool has_valid_result = result_ && !std::holds_alternative<std::monostate>(result_->data);
            if (!has_valid_result) {
                std::ostringstream oss;
                oss << "Block contract violation: <<" << language << " -> JSON>> expected a JSON return value, "
                    << "but no valid JSON was found in stdout.\n\n"
                    << "  Help:\n"
                    << "  - Use naab_return({...}) to explicitly return JSON data\n"
                    << "  - Or print valid JSON as the last line of output\n\n"
                    << "  Example:\n"
                    << "    let data = <<" << language << " -> JSON\n";
                if (language == "python") {
                    oss << "    import json\n"
                        << "    result = {\"key\": [1, 2, 3]}\n"
                        << "    naab_return(result)\n";
                } else if (language == "javascript" || language == "js") {
                    oss << "    naab_return({key: [1, 2, 3]})\n";
                } else {
                    oss << "    naab_return(your_data)\n";
                }
                oss << "    >>\n";
                gc_suspended_ = false;
                throw std::runtime_error(oss.str());
            }

            // AMBIGUOUS_OUTPUT warning: result looks like an error, not structured data
            if (auto* str_val = std::get_if<std::string>(&result_->data)) {
                if (str_val->find("Traceback") != std::string::npos ||
                    str_val->find("Error") != std::string::npos ||
                    str_val->find("error:") != std::string::npos) {
                    std::cerr << "Warning: <<" << language << " -> JSON>> returned a string that looks "
                              << "like an error message, not JSON data. Consider using try/catch "
                              << "inside the polyglot block.\n";
                }
            }
        }

        gc_suspended_ = false;

    } catch (const std::exception& e) {
        gc_suspended_ = false;
        std::string error_msg = e.what();

        // Phase 12: Translate temp file paths to NAAb source locations
        std::string translated = source_mapper.translateError(error_msg);
        if (!translated.empty() && translated != error_msg) {
            // Prepend the NAAb source context to the error
            error_msg = translated + "\n  Original error: " + error_msg;
        }

        // Detect undefined variable errors and provide helpful guidance
        bool is_undefined_var = false;
        std::string var_name;

        // Python: "NameError: name 'x' is not defined"
        if (error_msg.find("NameError") != std::string::npos &&
            error_msg.find("not defined") != std::string::npos) {
            is_undefined_var = true;
            // Try to extract variable name between quotes
            size_t quote1 = error_msg.find('\'');
            if (quote1 != std::string::npos) {
                size_t quote2 = error_msg.find('\'', quote1 + 1);
                if (quote2 != std::string::npos) {
                    var_name = error_msg.substr(quote1 + 1, quote2 - quote1 - 1);
                }
            }
        }

        // JavaScript: "ReferenceError: x is not defined"
        if (error_msg.find("ReferenceError") != std::string::npos &&
            error_msg.find("is not defined") != std::string::npos) {
            is_undefined_var = true;
            // Extract variable name before "is not defined"
            size_t pos = error_msg.find("is not defined");
            if (pos != std::string::npos) {
                // Look backwards for the variable name
                std::string prefix = error_msg.substr(0, pos);
                size_t last_space = prefix.find_last_of(" :");
                if (last_space != std::string::npos) {
                    var_name = prefix.substr(last_space + 1);
                    // Trim whitespace
                    var_name.erase(0, var_name.find_first_not_of(" \t"));
                    var_name.erase(var_name.find_last_not_of(" \t") + 1);
                }
            }
        }

        if (is_undefined_var) {
            std::ostringstream oss;
            oss << "Inline " << language << " execution failed: " << error_msg << "\n\n";
            oss << "  Help: Did you forget to bind a NAAb variable?\n";
            oss << "  Inline polyglot code requires explicit variable binding syntax.\n\n";

            if (!var_name.empty()) {
                oss << "  ✗ Wrong - variable not bound:\n";
                oss << "    let result = <<" << language << "\n";
                oss << "    " << var_name << " * 2\n";
                oss << "    >>\n\n";
                oss << "  ✓ Right - explicit variable binding:\n";
                oss << "    let result = <<" << language << "[" << var_name << "]\n";
                oss << "    " << var_name << " * 2\n";
                oss << "    >>\n\n";
            } else {
                oss << "  Syntax: <<language[var1, var2, ...]\n";
                oss << "    your code here\n";
                oss << "  >>\n\n";
            }

            oss << "  Example with multiple variables:\n";
            oss << "    let a = 10\n";
            oss << "    let b = 20\n";
            oss << "    let sum = <<" << language << "[a, b]\n";
            oss << "    a + b\n";
            oss << "    >>\n";

            throw std::runtime_error(oss.str());
        }

        // Detect common polyglot errors and add helpful context
        std::ostringstream oss;
        oss << "Inline " << language << " execution failed: " << error_msg << "\n";

        // Python indentation errors
        if (error_msg.find("IndentationError") != std::string::npos ||
            error_msg.find("unexpected indent") != std::string::npos) {
            oss << "\n  Help: Python indentation error in polyglot block.\n"
                << "  Common causes:\n"
                << "  - Mixing tabs and spaces\n"
                << "  - Code inside the block has inconsistent indentation\n"
                << "  - All lines in the block should use the same indentation style\n\n"
                << "  ✗ Wrong - inconsistent indentation:\n"
                << "    let r = <<python\n"
                << "    x = 1\n"
                << "      y = 2   # extra indent!\n"
                << "    >>\n\n"
                << "  ✓ Right - consistent indentation:\n"
                << "    let r = <<python\n"
                << "    x = 1\n"
                << "    y = 2\n"
                << "    >>\n";
        }
        // Python SyntaxError
        else if (language == "python" && error_msg.find("SyntaxError") != std::string::npos) {
            oss << "\n  Help: Python syntax error in polyglot block.\n"
                << "  Common causes:\n"
                << "  - Missing colons after if/for/def/class\n"
                << "  - Unclosed parentheses or brackets\n"
                << "  - Python 3 syntax required (print is a function)\n\n"
                << "  Tip: The last expression in the block is the return value.\n"
                << "  For multi-line blocks, put the result on the last line:\n"
                << "    let r = <<python\n"
                << "    x = compute()\n"
                << "    x  # this value is returned to NAAb\n"
                << "    >>\n";
        }
        // Python/JS import errors
        else if (error_msg.find("ModuleNotFoundError") != std::string::npos ||
                 error_msg.find("ImportError") != std::string::npos ||
                 error_msg.find("Cannot find module") != std::string::npos) {
            oss << "\n  Help: Missing module/package in " << language << " polyglot block.\n"
                << "  The module needs to be installed in your system's " << language << " environment.\n\n"
                << "  For Python: pip install <module_name>\n"
                << "  For JavaScript: npm install <module_name>\n\n"
                << "  Note: Only standard library modules are available by default.\n";
        }
        // Compilation errors (Rust, C++, C#)
        else if (error_msg.find("compilation failed") != std::string::npos) {
            oss << "\n  Help: Compilation error in " << language << " polyglot block.\n"
                << "  The " << language << " compiler rejected the generated code.\n"
                << "  Check that the code is valid " << language << " and that\n"
                << "  the compiler (" << (language == "rust" ? "rustc" : language == "csharp" ? "mcs" : "g++") << ") is installed.\n\n"
                << "  Tip: NAAb wraps single expressions automatically.\n"
                << "  For multi-statement blocks, write a complete program.\n";
        }
        // JavaScript: 'return' keyword in expression context
        else if (language == "javascript" &&
                 (error_msg.find("unexpected token") != std::string::npos ||
                  error_msg.find("SyntaxError") != std::string::npos) &&
                 error_msg.find("return") != std::string::npos) {
            oss << "\n  Help: Don't use 'return' in JavaScript polyglot blocks.\n"
                << "  The last expression is automatically returned to NAAb.\n\n"
                << "  ✗ Wrong:\n"
                << "    let x = <<javascript\n"
                << "    return 42\n"
                << "    >>\n\n"
                << "  ✓ Right:\n"
                << "    let x = <<javascript\n"
                << "    42\n"
                << "    >>\n\n"
                << "  For multi-line blocks:\n"
                << "    let x = <<javascript\n"
                << "    let result = someComputation();\n"
                << "    result   // last expression is the return value\n"
                << "    >>\n";
        }
        // TypeScript syntax errors (tsx/tsc)
        else if ((language == "typescript" || language == "ts") &&
                 (error_msg.find("Expected") != std::string::npos ||
                  error_msg.find("SyntaxError") != std::string::npos ||
                  error_msg.find("error TS") != std::string::npos ||
                  error_msg.find("Cannot find") != std::string::npos)) {
            oss << "\n  Help: TypeScript syntax error in polyglot block.\n"
                << "  NAAb injects bound variables as `const name = value;` before your code\n"
                << "  and wraps the last expression in console.log() for return capture.\n\n"
                << "  Common causes:\n"
                << "  - Braces/blocks confuse the auto-wrapping (use explicit console.log)\n"
                << "  - Variable injection collides with import statements\n"
                << "  - Type annotations on injected values (NAAb injects `const`, not typed)\n\n"
                << "  ✗ Fragile — auto-wrapping may break with blocks:\n"
                << "    let r = <<typescript[x]\n"
                << "    if (x > 0) { \"positive\" } else { \"negative\" }\n"
                << "    >>\n\n"
                << "  ✓ Robust — explicit console.log:\n"
                << "    let r = <<typescript[x]\n"
                << "    const result = x > 0 ? \"positive\" : \"negative\";\n"
                << "    console.log(result);\n"
                << "    >>\n\n"
                << "  ✓ Best — use naab_return() for structured data:\n"
                << "    let r = <<typescript[x]\n"
                << "    naab_return({value: x, label: \"result\"});\n"
                << "    >>\n\n"
                << "  Tip: Put imports FIRST in the block (before any logic).\n"
                << "  NAAb injects variables after import lines automatically.\n";
        }
        // Go: package main collision with variable injection
        else if ((language == "go") &&
                 (error_msg.find("expected 'package'") != std::string::npos ||
                  error_msg.find("expected package") != std::string::npos)) {
            oss << "\n  Help: Go requires 'package main' as the first line.\n"
                << "  NAAb injects bound variables after package/import headers,\n"
                << "  but if the block structure is unusual, injection can collide.\n\n"
                << "  ✓ Correct — package main first, then imports:\n"
                << "    let r = <<go[x]\n"
                << "    package main\n"
                << "    import \"fmt\"\n"
                << "    func main() {\n"
                << "        fmt.Println(x)\n"
                << "    }\n"
                << "    >>\n\n"
                << "  ✓ Simple — let NAAb auto-wrap (no package main needed):\n"
                << "    let r = <<go[x]\n"
                << "    x * 2\n"
                << "    >>\n\n"
                << "  Tip: For simple expressions, omit package main entirely.\n"
                << "  NAAb wraps Go expressions in package main automatically.\n";
        }
        // Rust: common injection issues
        else if ((language == "rust") &&
                 (error_msg.find("expected") != std::string::npos ||
                  error_msg.find("cannot find") != std::string::npos)) {
            oss << "\n  Help: Rust compilation error in polyglot block.\n"
                << "  NAAb injects bound variables as `let name = value;` before your code.\n"
                << "  For complex types (arrays, dicts), NAAb uses a JSON context file.\n\n"
                << "  Common causes:\n"
                << "  - Variable type mismatch (NAAb infers types from values)\n"
                << "  - Missing use/extern crate for libraries\n"
                << "  - Rust's strict type system rejecting injected values\n\n"
                << "  ✓ Simple expressions (auto-wrapped in fn main):\n"
                << "    let r = <<rust[x]\n"
                << "    x * 2\n"
                << "    >>\n\n"
                << "  ✓ Full programs:\n"
                << "    let r = <<rust[x]\n"
                << "    fn main() {\n"
                << "        println!(\"{}\", x * 2);\n"
                << "    }\n"
                << "    >>\n";
        }
        // Generic: Python None return causing null
        else if (error_msg.find("Cannot infer type") != std::string::npos &&
                 error_msg.find("null") != std::string::npos) {
            oss << "\n  Help: Polyglot block returned null (Python None).\n"
                << "  Make sure the last expression in the block has a value:\n\n"
                << "  ✗ Wrong - print() returns None:\n"
                << "    let x = <<python\n"
                << "    print('hello')\n"
                << "    >>\n\n"
                << "  ✓ Right - last expression has a value:\n"
                << "    let x = <<python\n"
                << "    result = 'hello'\n"
                << "    result\n"
                << "    >>\n";
        }

        throw std::runtime_error(oss.str());
    }
}

// ============================================================================
// StructValue Methods
// ============================================================================

// Profile mode methods
void Interpreter::profileStart(const std::string& name) {
    (void)name;  // Reserved for future use with named profiling sections
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
    fmt::print("Total time: {:.2f}ms\n\n", static_cast<double>(total) / 1000.0);

    // Sort by time descending
    std::vector<std::pair<std::string, long long>> sorted(
        profile_timings_.begin(), profile_timings_.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& [name, time] : sorted) {
        double ms = static_cast<double>(time) / 1000.0;
        double pct = (total > 0) ? (100.0 * static_cast<double>(time) / static_cast<double>(total)) : 0.0;
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

// Parallel polyglot execution: Capture variables for thread-safe parallel execution
void Interpreter::VariableSnapshot::capture(
    Environment* env,
    const std::vector<std::string>& var_names,
    Interpreter* interp
) {
    for (const auto& name : var_names) {
        if (env->has(name)) {
            // Deep copy the value to avoid shared mutable state
            auto original_value = env->get(name);
            auto copied_value = interp->copyValue(original_value);
            variables[name] = copied_value;
        }
    }
}

// Parallel polyglot execution: Execute a group of polyglot blocks in parallel
void Interpreter::executePolyglotGroupParallel(const DependencyGroup& group) {

    if (group.parallel_blocks.empty()) {
        return;  // Nothing to execute
    }

    // Enterprise Security: Activate sandbox for parallel polyglot execution
    auto& sandbox_manager = security::SandboxManager::instance();

    security::SandboxConfig sandbox_config = sandbox_manager.getDefaultConfig();

    security::ScopedSandbox scoped_sandbox(sandbox_config);


    // Always use parallel execution, even for single blocks
    // This avoids Python segfault in sequential path and ensures consistency
    // Step 1: Capture variable snapshots for each block (thread-safe deep copy)
    std::vector<VariableSnapshot> snapshots;
    for (size_t block_idx = 0; block_idx < group.parallel_blocks.size(); ++block_idx) {
        const auto& block = group.parallel_blocks[block_idx];

        VariableSnapshot snapshot;
        snapshot.capture(current_env_.get(), block.read_vars, this);

        snapshots.push_back(std::move(snapshot));
    }

    // Step 2: Prepare code for each block with variable bindings
    std::vector<std::tuple<
        polyglot::PolyglotAsyncExecutor::Language,
        std::string,
        std::vector<interpreter::Value>
    >> tasks;

    // Track which blocks from group.parallel_blocks were submitted for parallel execution
    // Sequential blocks (lang_supported=false) are executed inline and skipped from tasks
    std::vector<size_t> parallel_block_indices;

    for (size_t i = 0; i < group.parallel_blocks.size(); ++i) {
        const auto& block = group.parallel_blocks[i];
        const auto& snapshot = snapshots[i];
        auto* inline_code = block.node;

        // Convert language string to enum
        std::string lang_str = inline_code->getLanguage();
        polyglot::PolyglotAsyncExecutor::Language lang;

        // Check if language is supported by PolyglotAsyncExecutor
        bool lang_supported = true;

        if (lang_str == "python") {
            // Execute Python sequentially on main thread to avoid fragmenting
            // address space with CFI shadow entries in worker threads.
            // Python thread pool execution creates new CFI mappings that make
            // subsequent fork/posix_spawn calls fail with SIGABRT on Android.
            // The main thread uses PyGILState_Ensure which is safe.
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::Python;
        } else if (lang_str == "javascript" || lang_str == "js") {
            // Thread pool allows safe parallel execution
            lang = polyglot::PolyglotAsyncExecutor::Language::JavaScript;
        } else if (lang_str == "cpp" || lang_str == "c++") {
            // C++ uses fork/exec for compilation - sequential to avoid CFI crash
            // fork/exec already creates parallel subprocesses
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::Cpp;
        } else if (lang_str == "rust") {
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::Rust;
        } else if (lang_str == "csharp" || lang_str == "cs") {
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::CSharp;
        } else if (lang_str == "shell" || lang_str == "bash" || lang_str == "sh") {
            // Shell uses fork/exec which triggers Android bionic CFI crash
            // from thread pool workers. No need for thread pool anyway -
            // fork/exec already creates a parallel subprocess.
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::Shell;
        } else {
            // Unsupported language for parallel execution (e.g., go, ruby, perl)
            // Fall back to sequential execution using LanguageRegistry
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::GenericSubprocess;  // Placeholder
        }

        // If language not supported for parallel execution, execute sequentially
        if (!lang_supported) {
            // Execute the full statement sequentially (e.g., VarDeclStmt)
            // This ensures the variable gets properly assigned
            auto* stmt = block.statement;
            stmt->accept(*this);

            // Skip adding to parallel tasks
            continue;
        }

        // Prepare variable declarations by serializing snapshot values
        std::string var_declarations;
        for (const auto& [var_name, value] : snapshot.variables) {
            std::string serialized = serializeValueForLanguage(value, lang_str);

            // Language-specific variable declaration syntax
            if (lang_str == "python") {
                var_declarations += var_name + " = " + serialized + "\n";
            } else if (lang_str == "javascript" || lang_str == "js") {
                var_declarations += "const " + var_name + " = " + serialized + ";\n";
            } else if (lang_str == "rust") {
                var_declarations += "let " + var_name + " = " + serialized + ";\n";
            } else if (lang_str == "cpp" || lang_str == "c++") {
                var_declarations += "const auto " + var_name + " = " + serialized + ";\n";
            } else if (lang_str == "csharp" || lang_str == "cs") {
                var_declarations += "var " + var_name + " = " + serialized + ";\n";
            } else if (lang_str == "shell" || lang_str == "bash") {
                var_declarations += var_name + "=" + serialized + "\n";
            } else {
                // Generic: assume C-like syntax
                var_declarations += var_name + " = " + serialized + ";\n";
            }
        }

        // Get raw code and strip common indentation
        std::string raw_code = inline_code->getCode();
        std::vector<std::string> lines;
        std::istringstream stream(raw_code);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }

        // Find minimum indentation (ignoring empty lines)
        size_t min_indent = std::string::npos;
        for (const auto& l : lines) {
            if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) continue;
            size_t indent = l.find_first_not_of(" \t");
            if (indent < min_indent) min_indent = indent;
        }

        // Strip the common indentation from all lines
        std::string code;
        for (const auto& l : lines) {
            if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) {
                code += "\n";
            } else {
                if (min_indent != std::string::npos && l.length() > min_indent) {
                    code += l.substr(min_indent) + "\n";
                } else {
                    code += l + "\n";
                }
            }
        }

        // Phase 12: Inject naab_return() helper for parallel execution path
        // Only inject if naab_return is actually used in the code (avoids breaking IIFE wrapping)
        if (raw_code.find("naab_return") != std::string::npos) {
            std::string helper;
            if (lang_str == "python") {
                helper = "def naab_return(data):\n    return data\n";
            } else if (lang_str == "javascript" || lang_str == "js") {
                helper = "function naab_return(data) { return data; }\n";
            } else if (lang_str == "typescript" || lang_str == "ts") {
                helper = "function naab_return(data) { return data; }\n";
            } else if (lang_str == "ruby") {
                helper = "require 'json'\ndef naab_return(data); puts \"__NAAB_RETURN__:\" + data.to_json; end\n";
            } else if (lang_str == "php") {
                helper = "function naab_return($data) { echo \"__NAAB_RETURN__:\" . json_encode($data) . \"\\n\"; }\n";
            } else if (lang_str == "shell" || lang_str == "sh" || lang_str == "bash") {
                helper = "naab_return() { echo \"__NAAB_RETURN__:$1\"; }\n";
            } else if (lang_str == "rust") {
                helper = "macro_rules! naab_return { ($val:expr) => { println!(\"__NAAB_RETURN__:{}\", $val); }; }\n";
            } else if (lang_str == "cpp" || lang_str == "c++") {
                helper = "#include <sstream>\n#define naab_return(val) do { std::ostringstream __os; __os << \"__NAAB_RETURN__:\" << (val); std::cout << __os.str() << std::endl; } while(0)\n";
            }
            if (!helper.empty()) {
                var_declarations = helper + var_declarations;
            }
        }

        // Prepend variable declarations with header awareness
        std::string final_code;
        if (!var_declarations.empty() &&
            (lang_str == "go" || lang_str == "php" ||
             lang_str == "typescript" || lang_str == "ts")) {
            final_code = injectDeclarationsAfterHeaders(var_declarations, code, lang_str);
        } else {
            final_code = var_declarations + code;
        }

        // Create task with empty args (variables are injected into code)
        std::vector<interpreter::Value> args;
        tasks.emplace_back(lang, final_code, args);
        parallel_block_indices.push_back(i);
    }

    // Step 3: Execute in parallel using PolyglotAsyncExecutor
    polyglot::PolyglotAsyncExecutor executor;
    auto results = executor.executeParallel(tasks, std::chrono::milliseconds(30000));

    // Step 4: Store results back to environment (sequential, thread-safe)
    // IMPORTANT: results[j] corresponds to parallel_block_indices[j], NOT group.parallel_blocks[j]
    // because sequential blocks were already executed and skipped from tasks
    for (size_t j = 0; j < results.size(); ++j) {
        size_t block_idx = parallel_block_indices[j];
        const auto& block = group.parallel_blocks[block_idx];
        const auto& result = results[j];

        if (result.success) {
            // Store result value
            if (!block.assigned_var.empty()) {
                // Result already contains interpreter::Value
                auto value = std::make_shared<Value>(result.value);
                current_env_->define(block.assigned_var, value);
            }
        } else {
            // Handle error - check for undefined variable errors
            std::string error_msg = result.error_message;
            bool is_undefined_var = false;
            std::string var_name;
            std::string language = block.node ? block.node->getLanguage() : "unknown";

            // Python: "NameError: name 'x' is not defined"
            if (error_msg.find("NameError") != std::string::npos &&
                error_msg.find("not defined") != std::string::npos) {
                is_undefined_var = true;
                // Try to extract variable name between quotes
                size_t quote1 = error_msg.find('\'');
                if (quote1 != std::string::npos) {
                    size_t quote2 = error_msg.find('\'', quote1 + 1);
                    if (quote2 != std::string::npos) {
                        var_name = error_msg.substr(quote1 + 1, quote2 - quote1 - 1);
                    }
                }
            }

            // JavaScript: "ReferenceError: x is not defined"
            if (error_msg.find("ReferenceError") != std::string::npos &&
                error_msg.find("is not defined") != std::string::npos) {
                is_undefined_var = true;
                // Extract variable name before "is not defined"
                size_t pos = error_msg.find("is not defined");
                if (pos != std::string::npos) {
                    // Look backwards for the variable name
                    std::string prefix = error_msg.substr(0, pos);
                    size_t last_space = prefix.find_last_of(" :");
                    if (last_space != std::string::npos) {
                        var_name = prefix.substr(last_space + 1);
                        // Trim whitespace
                        var_name.erase(0, var_name.find_first_not_of(" \t"));
                        var_name.erase(var_name.find_last_not_of(" \t") + 1);
                    }
                }
            }

            if (is_undefined_var) {
                std::ostringstream oss;
                oss << "Parallel polyglot execution failed in block " << j << ": " << error_msg << "\n\n";
                oss << "  Help: Did you forget to bind a NAAb variable?\n";
                oss << "  Inline polyglot code requires explicit variable binding syntax.\n\n";

                if (!var_name.empty()) {
                    oss << "  ✗ Wrong - variable not bound:\n";
                    oss << "    let result = <<" << language << "\n";
                    oss << "    " << var_name << " * 2\n";
                    oss << "    >>\n\n";
                    oss << "  ✓ Right - explicit variable binding:\n";
                    oss << "    let result = <<" << language << "[" << var_name << "]\n";
                    oss << "    " << var_name << " * 2\n";
                    oss << "    >>\n\n";
                } else {
                    oss << "  Syntax: <<language[var1, var2, ...]\n";
                    oss << "    your code here\n";
                    oss << "  >>\n\n";
                }

                oss << "  Example with multiple variables:\n";
                oss << "    let a = 10\n";
                oss << "    let b = 20\n";
                oss << "    let sum = <<" << language << "[a, b]\n";
                oss << "    a + b\n";
                oss << "    >>\n";

                throw std::runtime_error(oss.str());
            }

            // Detect common polyglot errors and add helpful context
            std::ostringstream oss;
            oss << "Parallel polyglot execution failed in block " << j << ": " << error_msg << "\n";

            if (error_msg.find("IndentationError") != std::string::npos ||
                error_msg.find("unexpected indent") != std::string::npos) {
                oss << "\n  Help: Python indentation error in polyglot block.\n"
                    << "  All lines should use consistent indentation (spaces, not tabs).\n"
                    << "  NAAb strips common leading whitespace, but mixed indentation breaks Python.\n";
            }
            else if (language == "python" && error_msg.find("SyntaxError") != std::string::npos) {
                oss << "\n  Help: Python syntax error. Check colons, brackets, and Python 3 syntax.\n"
                    << "  The last expression in the block is the return value.\n";
            }
            else if (error_msg.find("ModuleNotFoundError") != std::string::npos ||
                     error_msg.find("ImportError") != std::string::npos) {
                oss << "\n  Help: Missing Python module. Install with: pip install <module>\n";
            }
            else if (error_msg.find("compilation failed") != std::string::npos) {
                oss << "\n  Help: " << language << " compilation failed. Check syntax and compiler installation.\n";
            }
            // JavaScript: 'return' keyword in expression
            else if (language == "javascript" &&
                     (error_msg.find("unexpected token") != std::string::npos ||
                      error_msg.find("SyntaxError") != std::string::npos) &&
                     error_msg.find("return") != std::string::npos) {
                oss << "\n  Help: Don't use 'return' in JavaScript polyglot blocks.\n"
                    << "  The last expression is automatically returned to NAAb.\n\n"
                    << "  ✗ Wrong:  return 42\n"
                    << "  ✓ Right:  42\n";
            }

            throw std::runtime_error(oss.str());
        }
    }
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
            else if (c == '\r') escaped += "\\r";
            else if (c == '\t') escaped += "\\t";
            else if (c == '\0') escaped += "\\0";
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

    // List - language-specific array serialization
    if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data)) {
        const auto& list = std::get<std::vector<std::shared_ptr<Value>>>(value->data);

        // PHP: array() syntax
        if (language == "php") {
            std::string result = "array(";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += ")";
            return result;
        }

        // Rust: vec![] macro
        if (language == "rust") {
            std::string result = "vec![";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += "]";
            return result;
        }

        // Go: []interface{}{}
        if (language == "go") {
            std::string result = "[]interface{}{";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += "}";
            return result;
        }

        // C#: new List<object>{}
        if (language == "csharp" || language == "cs") {
            std::string result = "new System.Collections.Generic.List<object>{";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += "}";
            return result;
        }

        // C++: std::vector with initializer list
        if (language == "cpp" || language == "c++") {
            // For C++, we use initializer list but need to figure out element type
            std::string result = "std::vector<std::string>{";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                // Serialize all as strings for simplicity
                auto elem_str = serializeValueForLanguage(list[i], language);
                result += elem_str;
            }
            result += "}";
            return result;
        }

        // Default: JSON-like array (Python, JS, TS, Ruby, Shell)
        std::string result = "[";
        for (size_t i = 0; i < list.size(); i++) {
            if (i > 0) result += ", ";
            result += serializeValueForLanguage(list[i], language);
        }
        result += "]";
        return result;
    }

    // Dict - language-specific serialization
    if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data)) {
        const auto& dict = std::get<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data);

        // Ruby: use hash rocket syntax {key => value}
        if (language == "ruby") {
            std::string result = "{";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "\"" + key + "\" => " + serializeValueForLanguage(val, language);
            }
            result += "}";
            return result;
        }

        // PHP: array("key" => "value")
        if (language == "php") {
            std::string result = "array(";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "\"" + key + "\" => " + serializeValueForLanguage(val, language);
            }
            result += ")";
            return result;
        }

        // Go: map[string]interface{}{}
        if (language == "go") {
            std::string result = "map[string]interface{}{";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "\"" + key + "\": " + serializeValueForLanguage(val, language);
            }
            result += "}";
            return result;
        }

        // Rust: HashMap (use context file for complex, but inline for simple)
        if (language == "rust") {
            // Generate a block expression that creates a HashMap
            std::string result = "{ let mut __m = std::collections::HashMap::new(); ";
            for (const auto& [key, val] : dict) {
                result += "__m.insert(\"" + key + "\".to_string(), " + serializeValueForLanguage(val, language) + "); ";
            }
            result += "__m }";
            return result;
        }

        // C#: Dictionary
        if (language == "csharp" || language == "cs") {
            std::string result = "new System.Collections.Generic.Dictionary<string, object>{";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "{\"" + key + "\", " + serializeValueForLanguage(val, language) + "}";
            }
            result += "}";
            return result;
        }

        // C++: std::map
        if (language == "cpp" || language == "c++") {
            std::string result = "std::map<std::string, std::string>{";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "{\"" + key + "\", " + serializeValueForLanguage(val, language) + "}";
            }
            result += "}";
            return result;
        }

        // Default: JSON-like object (Python, JS, TS)
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
    (void)type_params;  // Reserved for future generic type inference
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
        return "array";
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
    else if (type.kind == ast::TypeKind::List) base_name = "array";
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
            LOG_DEBUG("[INFO] Inferred type argument {}: {}\n",
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

    if (!gc_enabled_ || gc_suspended_) {
        return;  // GC disabled or suspended
    }

    if (verbose_mode_) {
        fmt::print("[GC] Running garbage collection...\n");
    }

    // Use provided environment or fall back to global environment
    auto root_env = env ? env : global_env_;

    // Build extra roots: result_ and any in-flight values
    std::vector<std::shared_ptr<Value>> extra_roots;
    if (result_) {
        extra_roots.push_back(result_);
    }

    // Build extra environments: always include global_env_ if root is not global
    std::vector<std::shared_ptr<Environment>> extra_envs;
    if (root_env != global_env_ && global_env_) {
        extra_envs.push_back(global_env_);
    }

    // Run mark-and-sweep cycle detection with complete root set
    size_t collected = cycle_detector_->detectAndCollect(root_env, tracked_values_, extra_roots, extra_envs);

    if (verbose_mode_) {
        if (collected > 0) {
            fmt::print("[GC] Collected {} cyclic values\n", collected);
        } else {
            fmt::print("[GC] No cycles detected\n");
        }
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
    if (!gc_enabled_ || !cycle_detector_ || gc_suspended_) {
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

// ============================================================================
// Issue #3: File Context Management for Path Resolution
// ============================================================================

void Interpreter::pushFileContext(const std::filesystem::path& file_path) {
    std::filesystem::path absolute_path = std::filesystem::absolute(file_path);
    file_context_stack_.push_back(absolute_path);

    if (verbose_mode_) {
        fmt::print("[FileContext] Pushed: {} (depth: {})\n",
                   absolute_path.string(), file_context_stack_.size());
    }

    // Update current_file_ for error reporting
    current_file_ = absolute_path.string();
}

void Interpreter::popFileContext() {
    if (file_context_stack_.empty()) {
        throw std::runtime_error("File context stack underflow");
    }

    if (verbose_mode_) {
        fmt::print("[FileContext] Popped: {} (depth: {})\n",
                   file_context_stack_.back().string(), file_context_stack_.size());
    }

    file_context_stack_.pop_back();

    // Update current_file_ to parent context (or empty)
    if (!file_context_stack_.empty()) {
        current_file_ = file_context_stack_.back().string();
    } else {
        current_file_ = "";
    }
}

std::filesystem::path Interpreter::getCurrentFileDirectory() const {
    if (file_context_stack_.empty()) {
        // No file context - use current working directory
        return std::filesystem::current_path();
    }

    // Return directory of current file
    return file_context_stack_.back().parent_path();
}

std::filesystem::path Interpreter::resolveRelativePath(const std::string& path) const {
    std::filesystem::path path_obj(path);

    // If already absolute, return as-is
    if (path_obj.is_absolute()) {
        return path_obj;
    }

    // Resolve relative to current file's directory
    std::filesystem::path base_dir = getCurrentFileDirectory();
    std::filesystem::path resolved = base_dir / path_obj;

    // Canonicalize to remove .. and . components
    if (std::filesystem::exists(resolved)) {
        resolved = std::filesystem::canonical(resolved);
    }

    if (verbose_mode_) {
        fmt::print("[PathResolve] '{}' -> '{}' (base: '{}')\n",
                   path, resolved.string(), base_dir.string());
    }

    return resolved;
}

} // namespace interpreter
} // namespace naab
