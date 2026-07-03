// NAAb Interpreter - Direct AST execution
// Core interpreter implementation

#include "naab/interpreter.h"
#include "naab/governance.h"  // GovernanceHardError — uncatchable exception
#include "naab/lexer.h"   // For string interpolation evaluation
#include "naab/parser.h"  // For string interpolation evaluation
#include "naab/limits.h"  // Week 1, Task 1.3: Call depth limits
#include "naab/error_helpers.h"
#include "naab/language_registry.h"
#include "naab/block_registry.h"
#include "naab/logger.h"  // Logging system
#ifndef _WIN32
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#include "naab/shell_executor.h"  // Polyglot: Issue #2 - Shell environment variables
#endif
#include "naab/python_executor_adapter.h"
#include "naab/stdlib_new_modules.h"  // For ArrayModule type
#include "naab/struct_registry.h"
#include "cycle_detector.h"  // Phase 3.2: Garbage collection
#include "naab/polyglot_dependency_analyzer.h"  // Parallel polyglot execution
#ifndef _WIN32
#include "naab/polyglot_async_executor.h"  // Parallel polyglot execution
#endif
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
#include <chrono>     // Phase 1: Empirical profiling timing
#include <functional> // For std::hash

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

NaabError::NaabError(NaabVal value)
    : std::runtime_error(value.toString()), error_type_(ErrorType::GENERIC),
      value_(std::move(value)) {
    message_ = value_.toString();
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
        } else if constexpr (std::is_same_v<T, std::vector<NaabVal>>) {
            std::string result = "[";
            for (size_t i = 0; i < arg.size(); i++) {
                if (i > 0) result += ", ";
                result += arg[i].toString();
            }
            result += "]";
            return result;
        } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, NaabVal>>) {
            std::string result = "{";
            size_t i = 0;
            for (const auto& [k, v] : arg) {
                if (i++ > 0) result += ", ";
                result += "\"" + k + "\": " + v.toString();
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
            // FIX 30: ShellResult toString — return stdout for success, error context for failure
            if (arg->type_name == "ShellResult" && arg->field_values.size() >= 3) {
                auto exit_code_nv = arg->field_values[0];
                auto stdout_nv = arg->field_values[1];
                auto stderr_nv = arg->field_values[2];
                int exit_code = exit_code_nv.isInt() ? exit_code_nv.asInt() : -1;
                if (exit_code == 0) {
                    return stdout_nv.isNull() ? "" : stdout_nv.toString();
                }
                // Non-zero exit: return stdout if available, otherwise stderr
                std::string out = stdout_nv.isNull() ? "" : stdout_nv.toString();
                if (!out.empty()) return out;
                return stderr_nv.isNull() ? "" : stderr_nv.toString();
            }
            std::string result = arg->type_name + " { ";
            for (size_t i = 0; i < arg->definition->fields.size(); ++i) {
                if (i > 0) result += ", ";
                result += arg->definition->fields[i].name + ": ";
                result += arg->field_values[i].toString();
            }
            result += " }";
            return result;
        } else if constexpr (std::is_same_v<T, std::shared_ptr<FutureValue>>) {
            return "<Future:" + arg->description + ">";
        } else if constexpr (std::is_same_v<T, std::shared_ptr<GeneratorValue>>) {
            return "<Generator:" + (arg->func ? arg->func->name : "anonymous") + ">";
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
void Value::traverse(std::function<void(const NaabVal&)> visitor) const {
    std::visit([&visitor](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;

        // Visit list elements (NaabVal)
        if constexpr (std::is_same_v<T, std::vector<NaabVal>>) {
            for (const auto& elem : arg) {
                if (!elem.isNull()) {
                    visitor(elem);
                }
            }
        }
        // Visit dict values (NaabVal)
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, NaabVal>>) {
            for (const auto& [key, val] : arg) {
                if (!val.isNull()) {
                    visitor(val);
                }
            }
        }
        // Visit struct fields (NaabVal)
        else if constexpr (std::is_same_v<T, std::shared_ptr<StructValue>>) {
            if (arg) {
                for (const auto& field_val : arg->field_values) {
                    if (!field_val.isNull()) {
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

void Environment::define(const std::string& name, NaabVal value) {
    values_[name] = std::move(value);
}

NaabVal Environment::get(const std::string& name) {
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
                     "  For sleep: import time; time.sleep(seconds)\n"
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
                     "    time.sleep(1.0)          // sleep for 1 second";
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
    } else if (name == "True" || name == "False") {
        std::string correct = (name == "True") ? "true" : "false";
        error_msg += "\n\n  NAAb uses '" + correct + "' (lowercase), not Python's '" + name + "':\n"
                     "    let x = " + correct + "\n"
                     "  Inside <<python>> blocks, use Python's " + name + ".";
    } else if (name == "None" || name == "nil" || name == "undefined") {
        error_msg += "\n\n  NAAb uses 'null' (not '" + name + "'):\n"
                     "    let x = null\n"
                     "  Inside <<python>> blocks, use Python's None.";
    } else if (name == "Object" || name == "Map") {
        error_msg += "\n\n  NAAb dicts are created with literal syntax:\n"
                     "    let d = {\"key\": \"value\"}\n"
                     "    d.get(\"key\")    // access values\n"
                     "    d.put(\"k\", v)   // set values";
    } else if (name == "JSON") {
        error_msg += "\n\n  NAAb does not have a JSON object. Dicts are native:\n"
                     "    let data = {\"key\": \"value\"}  // dict literal\n"
                     "    let val = data.get(\"key\")";
    } else if (name == "round" || name == "abs" || name == "floor" || name == "ceil" ||
               name == "sqrt" || name == "pow" || name == "min" || name == "max" ||
               name == "random" || name == "sin" || name == "cos") {
        error_msg += "\n\n  '" + name + "' is not a builtin. It's in the math module:\n"
                     "    use math\n"
                     "    let x = math." + name + "(...)\n";
    } else if (name == "PI" || name == "pi" || name == "E") {
        std::string correct = (name == "E") ? "E" : "PI";
        error_msg += "\n\n  '" + name + "' is not a builtin. It's a math constant:\n"
                     "    use math\n"
                     "    let x = math." + correct + "\n";
    } else if (name == "keys" || name == "values" || name == "entries") {
        error_msg += "\n\n  '" + name + "' is not a global function. Use dot-notation on a dict:\n"
                     "    let k = my_dict." + name + "()\n";
    } else if (name == "push" || name == "pop" || name == "shift" || name == "unshift" ||
               name == "sort" || name == "sorted" || name == "reverse" || name == "contains" || name == "find") {
        error_msg += "\n\n  '" + name + "' is not a global function. Use dot-notation:\n"
                     "    my_array." + name + "(...)\n";
    } else if (name == "upper" || name == "lower" || name == "trim" || name == "split" ||
               name == "replace" || name == "starts_with" || name == "ends_with") {
        error_msg += "\n\n  '" + name + "' is not a global function. Use dot-notation:\n"
                     "    my_string." + name + "(...)\n"
                     "  Or use the string module:\n"
                     "    use string\n"
                     "    string." + name + "(my_string, ...)\n";
    } else if (name == "parseInt" || name == "parseFloat" || name == "Number") {
        error_msg += "\n\n  NAAb uses type cast builtins instead:\n"
                     "    let x = int(\"42\")      // parseInt\n"
                     "    let y = float(\"3.14\")  // parseFloat\n";
    } else if (name == "toString" || name == "str") {
        error_msg += "\n\n  NAAb uses the string() cast builtin:\n"
                     "    let s = string(42)   // \"42\"\n";
    } else if (name == "reversed") {
        error_msg += "\n\n  'reversed' is not a builtin. Arrays have a mutable method:\n"
                     "    my_array.reverse()   // mutates in place\n";
    } else if (name == "enumerate" || name == "zip") {
        error_msg += "\n\n  NAAb does not have '" + name + "'. Use index-based loops:\n"
                     "    for i in 0..len(arr) {\n"
                     "        let item = arr[i]\n"
                     "    }\n";
    } else if (name == "map" || name == "filter" || name == "reduce") {
        error_msg += "\n\n  '" + name + "' is not a global function. Use the array module:\n"
                     "    use array\n"
                     "    array." + name + "_fn(arr, fn(x) { return x })\n";
    } else if (name == "append" || name == "extend") {
        error_msg += "\n\n  NAAb arrays use push() instead of '" + name + "':\n"
                     "    my_array.push(item)\n";
    } else if (name == "spawn" || name == "task") {
        error_msg += "\n\n  NAAb uses 'async function' for concurrent tasks:\n"
                     "    async function myTask() { ... }\n"
                     "    let future = myTask()\n"
                     "    let result = await future\n";
    } else if (name == "wait" || name == "wait_all" || name == "waitAll") {
        error_msg += "\n\n  NAAb uses 'await' keyword, not wait()/wait_all():\n"
                     "    let result = await future\n";
    } else if (name == "format" || name == "sprintf" || name == "printf") {
        error_msg += "\n\n  NAAb uses string concatenation with + operator:\n"
                     "    let msg = \"Hello \" + name + \"!\"\n"
                     "  Or use string() for type conversion:\n"
                     "    let msg = \"Count: \" + string(count)\n";
    } else {
        auto all_names = getAllNames();
        auto suggestion = error::suggestForUndefinedVariable(name, all_names);
        if (!suggestion.empty()) {
            error_msg += "\n  " + suggestion;
        }
    }
    throw std::runtime_error(error_msg);
}

void Environment::set(const std::string& name, NaabVal value) {
    auto it = values_.find(name);
    if (it != values_.end()) {
        it->second = std::move(value);
        return;
    }
    if (parent_) {
        parent_->set(name, std::move(value));
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

std::vector<std::string> Environment::getOwnNames() const {
    std::vector<std::string> names;
    names.reserve(values_.size());
    for (const auto& [name, _] : values_) {
        names.push_back(name);
    }
    return names;
}

// ============================================================================
// Interpreter Implementation
// ============================================================================

Interpreter::Interpreter()
    : global_env_(std::make_shared<Environment>()),
      current_env_(global_env_),
      result_(NaabVal::makeNull()),
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
    fmt::print(stderr, "[WARN] Python support not available (Python blocks disabled)\n");
#endif

#ifndef _WIN32
    // Initialize C++ executor
    cpp_executor_ = std::make_unique<runtime::CppExecutor>();
    LOG_DEBUG("[INFO] C++ executor initialized\n");
#endif

    // Initialize standard library
    stdlib_ = std::make_unique<stdlib::StdLib>();
    LOG_DEBUG("[INFO] Standard library initialized: {} modules available\n",
               stdlib_->listModules().size());

    // Auto-import stdlib prelude (core modules available without 'use')
    std::vector<std::string> prelude_modules = {"array", "string", "dict", "io", "file", "debug", "bolo", "env"};
    for (const auto& mod_name : prelude_modules) {
        if (stdlib_->hasModule(mod_name)) {
            auto module = stdlib_->getModule(mod_name);
            imported_modules_[mod_name] = module;

            // Store module marker in global environment
            global_env_->define(mod_name, NaabVal::makeString("__stdlib_module__:" + mod_name));
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
                [this](NaabVal fn, const std::vector<NaabVal>& args) -> NaabVal {
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

    // Initialize governance engine (will be loaded when setSourceCode is called)
    governance_ = std::make_unique<governance::GovernanceEngine>();

    // Initialize scanner engine (will be loaded from govern.json scanner section)
    scanner_ = std::make_unique<scanner::ScannerEngine>();

    // Issue #3: Set global interpreter pointer for stdlib path resolution
    g_current_interpreter = this;

    // Set thread-local governance engine pointer for stdlib modules (e.g., agent)
    if (governance_) {
        governance::GovernanceEngine::setCurrent(governance_.get());
    }

    // Wire debug module with interpreter access for scope inspection
    auto debug_module = stdlib_->getModule("debug");
    if (debug_module) {
        stdlib::DebugModule::setInterpreter(this);
        LOG_DEBUG("[INFO] Debug module configured with interpreter access\n");
    }

    defineBuiltins();
}

// Phase 3.2: Destructor must be defined in .cpp where CycleDetector is complete
Interpreter::~Interpreter() {
    // Bug 2: Null out static debug interpreter pointer to prevent dangling access
    stdlib::DebugModule::setInterpreter(nullptr);
    // Clear thread-local governance pointer to prevent dangling access
    governance::GovernanceEngine::setCurrent(nullptr);
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

    // Governance: Discover and load govern.json from script directory
    if (governance_ && !filename.empty()) {
        auto dir = std::filesystem::path(current_file_).parent_path();
        if (governance_->discoverAndLoad(dir.string())) {
            auto mode = governance_->getMode();
            std::string mode_str = (mode == governance::GovernanceMode::ENFORCE) ? "enforce"
                                 : (mode == governance::GovernanceMode::AUDIT)   ? "audit"
                                 : "off";
            fprintf(stderr, "[governance] Loaded: %s (mode: %s)\n",
                    governance_->getLoadedPath().c_str(), mode_str.c_str());

            // Scanner: Load config from the same govern.json (quiet — no output if no scanner section)
            if (scanner_) {
                scanner_->loadConfigFromPath(governance_->getLoadedPath(), true);
            }
        } else {
            // govern.json not found in directory hierarchy
            if (require_governance_) {
                throw std::runtime_error(
                    "Governance error: --require-governance set but no govern.json found.\n\n"
                    "  Search root: " + dir.string() + "\n"
                    "  Create a govern.json or remove --require-governance.\n"
                );
            }
            fprintf(stderr,
                "[governance] Warning: No govern.json found in '%s' or any parent directory.\n"
                "  Running WITHOUT governance restrictions.\n"
                "  To enforce governance, create a govern.json or use --require-governance.\n",
                dir.string().c_str());
        }
    }
}

void Interpreter::execute(ast::Program& program) {
    program.accept(*this);
}

NaabVal Interpreter::eval(ast::Expr& expr) {
    expr.accept(*this);
    return result_;
}

// Phase 6: Execute a function body in a given environment (for async)
NaabVal Interpreter::executeBodyInEnv(ast::CompoundStmt& body, std::shared_ptr<Environment> env) {
    auto saved_env = current_env_;
    auto saved_returning = returning_;
    env_stack_.push_back(current_env_);  // BUG-10 fix
    current_env_ = env;
    returning_ = false;

    try {
        executeStmt(body);
    } catch (...) {
        if (!env_stack_.empty()) env_stack_.pop_back();  // BUG-10 fix
        current_env_ = saved_env;
        returning_ = saved_returning;
        throw;
    }

    if (!env_stack_.empty()) env_stack_.pop_back();  // BUG-10 fix
    current_env_ = saved_env;
    auto return_value = result_;
    returning_ = saved_returning;
    return return_value;
}

// Get variable from environment (for testing)
NaabVal Interpreter::getVariable(const std::string& name) const {
    // Try current environment first, then global
    if (current_env_ && current_env_->has(name)) {
        return current_env_->get(name);
    }
    if (global_env_ && global_env_->has(name)) {
        return global_env_->get(name);
    }
    return NaabVal::makeNull();
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

    // Phase 6: Process interface declarations (before structs so they're registered)
    for (auto& iface_decl : node.getInterfaces()) {
        iface_decl->accept(*this);
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

    // Phase 6: Validate interface implementations (after functions are registered)
    for (auto& struct_decl : node.getStructs()) {
        for (const auto& iface_name : struct_decl->getImplements()) {
            auto it = interface_registry_.find(iface_name);
            if (it == interface_registry_.end()) continue;  // Error already caught in StructDecl visitor
            for (const auto& method : it->second.methods) {
                std::string fn_name = struct_decl->getName() + "." + method.name;
                bool found = current_env_->has(fn_name);
                if (!found) {
                    std::string params_str;
                    for (size_t i = 0; i < method.params.size(); i++) {
                        if (i > 0) params_str += ", ";
                        params_str += method.params[i].first;
                        if (method.params[i].second.kind != ast::TypeKind::Any) {
                            auto& ptype = method.params[i].second;
                            std::string tname;
                            switch (ptype.kind) {
                                case ast::TypeKind::Int: tname = "int"; break;
                                case ast::TypeKind::Float: tname = "float"; break;
                                case ast::TypeKind::String: tname = "string"; break;
                                case ast::TypeKind::Bool: tname = "bool"; break;
                                case ast::TypeKind::Struct: tname = ptype.struct_name; break;
                                default: tname = "any"; break;
                            }
                            params_str += ": " + tname;
                        }
                    }
                    throw std::runtime_error(
                        "Interface error: Missing method '" + method.name + "'\n\n"
                        "  Struct '" + struct_decl->getName() + "' implements '" + iface_name + "'\n"
                        "  but is missing required method: " + method.name + "\n\n"
                        "  Help:\n"
                        "  - Add the function: fn " + fn_name + "(" + params_str + ")\n\n"
                        "  Example:\n"
                        "    fn " + fn_name + "(" + params_str + ") {\n"
                        "        // implementation\n"
                        "    }\n");
                }
            }
        }
    }

    // S11: Process top-level runtime declarations (before main, after functions)
    for (auto& rd : node.getRuntimeDecls()) {
        rd->accept(*this);
    }

    // Phase 3.1: Process exports
    LOG_DEBUG("Processing {} export statements\n", node.getExports().size());
    for (auto& export_stmt : node.getExports()) {
        export_stmt->accept(*this);
    }

    // Fire pre_check hook before governance validation
    if (governance_ && governance_->isActive() && module_loading_depth_ == 0) {
        governance_->fireHook(governance_->getRules().hooks.pre_check, {
            {"file", current_file_}
        });
    }

    // FIX-DX-8: Validate scope patterns against actual function names
    if (governance_ && governance_->isActive() && module_loading_depth_ == 0) {
        auto all_names = current_env_->getAllNames();
        std::vector<std::string> func_names;
        for (const auto& fname : all_names) {
            auto val = current_env_->get(fname);
            if (!val.isNull() && val.isFunction()) {
                func_names.push_back(fname);
            }
        }
        governance_->validateScopePatterns(func_names);
    }

    // Governance: Check require_main_block before execution
    if (governance_ && governance_->isActive() && governance_->requiresMainBlock() && module_loading_depth_ == 0) {
        if (!node.getMainBlock()) {
            auto level = governance_->getRules().main_block_level;
            std::string err = governance_->checkLanguageAllowed("__main_block_check__");
            // Use enforce directly via a check
            std::string msg = "Governance error: Program requires a main {{ }} block ["
                + std::string(level == governance::EnforcementLevel::HARD ? "HARD-MANDATORY" :
                              level == governance::EnforcementLevel::SOFT ? "SOFT-MANDATORY" : "ADVISORY")
                + "]\n\n"
                "  Rule: main block required\n\n"
                "  Help:\n"
                "  - All programs must have a main {{ }} block when governance requires it\n"
                "  - Wrap your top-level code in: main {{ ... }}\n\n"
                "  Example:\n"
                "    main {{\n"
                "        let x = 42\n"
                "        print(x)\n"
                "    }}\n";
            if (level == governance::EnforcementLevel::HARD) {
                throw std::runtime_error(msg);
            } else if (level == governance::EnforcementLevel::SOFT) {
                if (!governance_->isOverrideEnabled()) {
                    throw std::runtime_error(msg + "\n  This rule is enforced by the project's governance configuration.\n");
                }
                fprintf(stderr, "[governance] OVERRIDE requirements.main_block: %s\n", msg.c_str());
            } else {
                fprintf(stderr, "[governance] WARNING requirements.main_block: Program has no main block\n");
            }
        }
    }

    // Execute main block if present (skip when loading as module)
    if (node.getMainBlock() && module_loading_depth_ == 0) {
        node.getMainBlock()->accept(*this);
    }

    // Flush grouped advisories (duplicate calls, polyglot try/catch)
    if (governance_ && governance_->isActive() && module_loading_depth_ == 0) {
        governance_->flushGroupedAdvisories();
    }

    // Governance: Print execution summary and write reports (only for top-level program)
    if (governance_ && governance_->isActive() && module_loading_depth_ == 0) {
        governance_->runGovernanceVoice();
        if (governance_verbose_) {
            // Full detail: summary line + every check
            std::string summary = governance_->formatSummary();
            if (!summary.empty()) {
                fprintf(stderr, "\n%s\n", summary.c_str());
            }
        } else {
            // Clean: one-line summary only, and only if there were warnings/blocks
            std::string oneline = governance_->formatSummaryOneLine();
            if (!oneline.empty()) {
                fprintf(stderr, "\n%s\n", oneline.c_str());
            }
        }
        // Write any configured report files (JSON, SARIF, JUnit, CSV, HTML)
        governance_->writeReports();

        // Fire post_check and on_complete hooks (only reachable on clean completion)
        governance_->fireHook(governance_->getRules().hooks.post_check, {
            {"file", current_file_},
            {"status", governance::g_governance_hard_block ? "blocked" : "passed"}
        });
        governance_->fireHook(governance_->getRules().hooks.on_complete, {
            {"file", current_file_},
            {"status", "passed"}
        });
    }

    // Scanner: Auto-run if govern.json has a "scanner" section
    if (scanner_ && scanner_->hasConfig() && module_loading_depth_ == 0) {
        auto result = scanner_->scan(current_file_, "auto");
        if (!result.issues.empty()) {
            // Count by level
            int hard_count = 0, soft_count = 0, advisory_count = 0;
            for (const auto& issue : result.issues) {
                if (issue.level == "hard") hard_count++;
                else if (issue.level == "soft") soft_count++;
                else advisory_count++;
            }

            // Print compact summary to stderr (like governance)
            fprintf(stderr, "\n[scanner] %d issues (%d hard, %d soft, %d advisory) in %s\n",
                    static_cast<int>(result.issues.size()),
                    hard_count, soft_count, advisory_count,
                    current_file_.c_str());

            // Show hard violations with details
            if (hard_count > 0) {
                fprintf(stderr, "[scanner] HARD violations:\n");
                for (const auto& issue : result.issues) {
                    if (issue.level == "hard") {
                        fprintf(stderr, "  X Line %d: %s.%s — %s\n",
                                issue.line, issue.category.c_str(),
                                issue.rule.c_str(), issue.message.c_str());
                        if (!issue.fix.empty()) {
                            fprintf(stderr, "    Fix: %s\n", issue.fix.c_str());
                        }
                    }
                }
            }

            // Show soft violations summary
            if (soft_count > 0) {
                fprintf(stderr, "[scanner] SOFT violations:\n");
                int shown = 0;
                for (const auto& issue : result.issues) {
                    if (issue.level == "soft" && shown < 5) {
                        fprintf(stderr, "  ! Line %d: %s.%s — %s\n",
                                issue.line, issue.category.c_str(),
                                issue.rule.c_str(), issue.message.c_str());
                        shown++;
                    }
                }
                if (soft_count > 5) {
                    fprintf(stderr, "  ... and %d more soft violations\n", soft_count - 5);
                }
            }

            // Save reports
            scanner_->saveReports(result);
        }
    }
}


void Interpreter::visit(ast::FunctionDecl& node) {
    // EVA-8: GOV-6 extended — scan ALL function bodies (not just exported)
    // Full checks skip during module loading (expensive regex/heuristic overhead).
    // Behavioral contracts (must_call, must_contain) always run — they are user-defined.
    if (governance_) {
        auto loc = node.getLocation();
        if (loc.line > 0 && !current_file_.empty()) {
            std::ifstream src_file(current_file_);
            if (src_file.is_open()) {
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(src_file, line)) {
                    lines.push_back(line);
                }
                src_file.close();

                if (loc.line <= static_cast<int>(lines.size())) {
                    std::string body_text;
                    int brace_depth = 0;
                    bool found_start = false;
                    for (size_t i = static_cast<size_t>(loc.line - 1); i < lines.size(); ++i) {
                        body_text += lines[i] + "\n";
                        for (char c : lines[i]) {
                            if (c == '{') { brace_depth++; found_start = true; }
                            if (c == '}') brace_depth--;
                        }
                        if (found_start && brace_depth <= 0) break;
                    }
                    if (module_loading_depth_ == 0) {
                        // Main file: full governance checks (includes behavioral contracts)
                        std::string err = governance_->checkNaabFunctionBody(
                            node.getName(), body_text, loc.line, current_file_,
                            static_cast<int>(node.getParams().size()));
                        if (!err.empty()) {
                            throw std::runtime_error(err);
                        }
                        // Intent validation
                        err = governance_->checkIntentValidation(
                            node.getName(), node.getIntent(), body_text, loc.line);
                        if (!err.empty()) {
                            throw std::runtime_error(err);
                        }
                    } else {
                        // Module loading: only behavioral contracts (must_call, must_contain)
                        std::string err = governance_->checkFunctionBehavioralContract(
                            node.getName(), body_text, loc.line,
                            static_cast<int>(node.getParams().size()));
                        if (!err.empty()) {
                            throw std::runtime_error(err);
                        }
                    }
                }
            }
        }
    }

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
        current_env_,  // ISS-022: Capture closure for module imports
        node.isAsync()  // Phase 6: async function flag
    );

    // Phase 5: Detect if function body contains yield (mark as generator)
    // Simple check: walk top-level statements for YieldExpr
    std::function<bool(ast::Stmt*)> containsYield = [&](ast::Stmt* stmt) -> bool {
        if (!stmt) return false;
        if (auto* compound = dynamic_cast<ast::CompoundStmt*>(stmt)) {
            for (auto& s : compound->getStatements()) {
                if (containsYield(s.get())) return true;
            }
        } else if (auto* expr_stmt = dynamic_cast<ast::ExprStmt*>(stmt)) {
            if (dynamic_cast<ast::YieldExpr*>(expr_stmt->getExpr())) return true;
        } else if (auto* if_stmt = dynamic_cast<ast::IfStmt*>(stmt)) {
            if (containsYield(if_stmt->getThenBranch())) return true;
            if (if_stmt->getElseBranch() && containsYield(if_stmt->getElseBranch())) return true;
        } else if (auto* for_stmt = dynamic_cast<ast::ForStmt*>(stmt)) {
            if (containsYield(for_stmt->getBody())) return true;
        } else if (auto* while_stmt = dynamic_cast<ast::WhileStmt*>(stmt)) {
            if (containsYield(while_stmt->getBody())) return true;
        } else if (auto* try_stmt = dynamic_cast<ast::TryStmt*>(stmt)) {
            if (containsYield(try_stmt->getTryBody())) return true;
            if (try_stmt->getCatchClause()) {
                if (containsYield(try_stmt->getCatchClause()->body.get())) return true;
            }
        }
        return false;
    };
    func_value->is_generator = containsYield(body);

    // Store in environment
    // ISS-022 Fix: Use current_env_ instead of global_env_ so module functions
    // can access module imports (like stdlib modules)
    current_env_->define(node.getName(), NaabVal::makeFunction(func_value));

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

    // Phase 6: Check that declared interfaces exist (method validation deferred to Program visitor)
    for (const auto& iface_name : node.getImplements()) {
        auto it = interface_registry_.find(iface_name);
        if (it == interface_registry_.end()) {
            throw std::runtime_error(
                "Interface error: Unknown interface '" + iface_name + "'\n\n"
                "  Struct '" + node.getName() + "' declares 'implements " + iface_name + "'\n"
                "  but no interface with that name has been defined.\n\n"
                "  Help:\n"
                "  - Define the interface before the struct\n\n"
                "  Example:\n"
                "    interface " + iface_name + " {\n"
                "        fn some_method() -> string\n"
                "    }\n");
        }
    }

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
    result_ = NaabVal::makeNull();
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
    current_env_->define(name, NaabVal::makeString("__NAAB_RUNTIME__:" + name));
}

// Phase 2.4.3: Enum declaration visitor
void Interpreter::visit(ast::EnumDecl& node) {
    explain("Defining enum '" + node.getName() + "' with " +
            std::to_string(node.getVariants().size()) + " variants");

    // Build variant list and register for TAG_ENUM name resolution
    std::vector<std::pair<std::string, int>> variant_pairs;
    int next_value = 0;
    for (const auto& variant : node.getVariants()) {
        int variant_value = variant.value.value_or(next_value);
        variant_pairs.emplace_back(variant.name, variant_value);
        next_value = variant_value + 1;
    }
    uint16_t enum_id = NaabVal::registerEnum(node.getName(), variant_pairs);

    // Define each variant as TAG_ENUM
    for (auto& [vname, val] : variant_pairs) {
        std::string full_name = node.getName() + "." + vname;
        auto value = NaabVal::makeEnum(val, enum_id);
        current_env_->define(full_name, value);
        if (current_env_ != global_env_) {
            global_env_->define(full_name, value);
        }
    }

    LOG_DEBUG("[INFO] Defined enum: {} with {} variants\n",
               node.getName(), node.getVariants().size());

    result_ = NaabVal::makeNull();
}

// Phase 6: Interface declaration visitor
void Interpreter::visit(ast::InterfaceDecl& node) {
    explain("Defining interface '" + node.getName() + "' with " +
            std::to_string(node.getMethods().size()) + " methods");

    // Store in interface registry (convert to copyable InterfaceMethodSig)
    InterfaceInfo info;
    for (const auto& m : node.getMethods()) {
        InterfaceMethodSig sig;
        sig.name = m.name;
        sig.return_type = m.return_type;
        for (const auto& p : m.params) {
            sig.params.emplace_back(p.name, p.type);
        }
        info.methods.push_back(std::move(sig));
    }
    interface_registry_[node.getName()] = std::move(info);

    LOG_DEBUG("[INFO] Defined interface: {} with {} methods\n",
               node.getName(), node.getMethods().size());

    result_ = NaabVal::makeNull();
}

void Interpreter::visit(ast::MainBlock& node) {
    // EVA-8: Scan main block body for stubs/oversimplification
    if (governance_) {
        auto loc = node.getLocation();
        if (loc.line > 0 && !current_file_.empty()) {
            std::ifstream src_file(current_file_);
            if (src_file.is_open()) {
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(src_file, line)) {
                    lines.push_back(line);
                }
                src_file.close();

                if (loc.line <= static_cast<int>(lines.size())) {
                    std::string body_text;
                    int brace_depth = 0;
                    bool found_start = false;
                    for (size_t i = static_cast<size_t>(loc.line - 1); i < lines.size(); ++i) {
                        body_text += lines[i] + "\n";
                        for (char c : lines[i]) {
                            if (c == '{') { brace_depth++; found_start = true; }
                            if (c == '}') brace_depth--;
                        }
                        if (found_start && brace_depth <= 0) break;
                    }
                    std::string err = governance_->checkNaabFunctionBody(
                        "main", body_text, loc.line, current_file_);
                    if (!err.empty()) {
                        throw std::runtime_error(err);
                    }
                    // Intent validation for main block
                    err = governance_->checkIntentValidation(
                        "main", node.getIntent(), body_text, loc.line);
                    if (!err.empty()) {
                        throw std::runtime_error(err);
                    }
                }
            }
        }
    }

    if (repl_mode_) {
        // In REPL mode, execute main block body statements directly
        // in current_env_ (global) so variables persist between inputs
        auto* body = dynamic_cast<ast::CompoundStmt*>(node.getBody());
        if (body) {
            for (auto& stmt : body->getStatements()) {
                stmt->accept(*this);
                if (returning_ || breaking_ || continuing_) break;
            }
        } else {
            node.getBody()->accept(*this);
        }
    } else {
        node.getBody()->accept(*this);
    }
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
            if (returning_ || breaking_ || continuing_) break;
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
    if (debugger_ && debugger_->isActive()) {
        debugger_->setCurrentEnvironment(current_env_);
        auto loc = node.getLocation();
        std::string loc_str = (current_file_.empty() ? "<unknown>" : current_file_) +
            ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
        debugger_->shouldBreak(loc_str);
    }
    eval(*node.getExpr());

    // V-GOV-013: file-local duplicate of findRootIdentifier (defined in expressions.cpp).
    // Needed here to fix the same cast failure for arr[0].push(secret).
    static const auto findRootId = [](auto& self, ast::Expr* expr) -> ast::IdentifierExpr* {
        if (!expr) return nullptr;
        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(expr)) return id;
        if (auto* bin = dynamic_cast<ast::BinaryExpr*>(expr))
            if (bin->getOp() == ast::BinaryOp::Subscript)
                return self(self, bin->getLeft());
        if (auto* mem = dynamic_cast<ast::MemberExpr*>(expr))
            return self(self, mem->getObject());
        return nullptr;
    };

    // V-GOV-012: propagate taint for container mutation calls.
    // Storing a tainted value via push/insert/append/set bypasses name-based
    // taint tracking unless we explicitly mark the container as tainted here.
    if (governance_ && governance_->isActive()) {
        if (auto* call = dynamic_cast<ast::CallExpr*>(node.getExpr())) {
            if (auto* member = dynamic_cast<ast::MemberExpr*>(call->getCallee())) {
                static const std::unordered_set<std::string> MUTATION_METHODS = {
                    "push", "insert", "append", "set", "prepend"
                };
                const std::string& method = member->getMember();
                if (MUTATION_METHODS.count(method)) {
                    bool arg_tainted = false;
                    for (const auto& arg : call->getArgs()) {
                        if (expressionContainsTaint(arg.get())) {
                            arg_tainted = true;
                            break;
                        }
                    }
                    if (arg_tainted) {
                        // V-GOV-013: use recursive unwrap so arr[0].push(s) marks arr
                        if (auto* root_id = findRootId(findRootId, member->getObject())) {
                            governance_->markTainted(root_id->getName());
                        }
                    }
                }
            }
        }

        // V13-S7 fix: Consume stale lastReturnWasTainted flag.
        // ExprStmt discards its result, so taint from a standalone call like
        // env.get("SECRET") must not leak to the next statement's checkRhsTainted().
        governance_->setLastReturnTainted(false);
    }
}

void Interpreter::visit(ast::ReturnStmt& node) {
    if (debugger_ && debugger_->isActive()) {
        debugger_->setCurrentEnvironment(current_env_);
        auto loc = node.getLocation();
        std::string loc_str = (current_file_.empty() ? "<unknown>" : current_file_) +
            ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
        debugger_->shouldBreak(loc_str);
    }
    if (node.getExpr()) {
        result_ = eval(*node.getExpr());
    } else {
        result_ = NaabVal::makeNull();
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

    // BUG-D + BUG-3: Track if return expression contains tainted data (REFACTOR-1)
    if (governance_ && governance_->isActive() && node.getExpr()) {
        governance_->setLastReturnTainted(checkRhsTainted(node.getExpr()));
    }

    returning_ = true;
}

void Interpreter::visit(ast::IfStmt& node) {
    if (debugger_ && debugger_->isActive()) {
        debugger_->setCurrentEnvironment(current_env_);
        auto loc = node.getLocation();
        std::string loc_str = (current_file_.empty() ? "<unknown>" : current_file_) +
            ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
        debugger_->shouldBreak(loc_str);
    }
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
    if (condition.toBool()) {
        node.getThenBranch()->accept(*this);
    } else if (node.getElseBranch()) {
        node.getElseBranch()->accept(*this);
    }
}

void Interpreter::visit(ast::IfExpr& node) {
    auto condition = eval(*node.getCondition());
    if (condition.toBool()) {
        node.getThenExpr()->accept(*this);
    } else {
        node.getElseExpr()->accept(*this);
    }
    // last_value_ is set by whichever branch expression was evaluated
}

void Interpreter::visit(ast::ThrowExpr& node) {
    auto val = eval(*node.getExpr());
    // V-TAINT-THROW: propagate taint across throw/catch
    NaabError err(val);
    if (governance_ && governance_->isActive()) {
        err.setTainted(checkRhsTainted(node.getExpr()));
    }
    throw err;
}

void Interpreter::visit(ast::TryCatchExpr& node) {
    try {
        node.getTryExpr()->accept(*this);
        // result_ now holds try expression value
    } catch (const governance::GovernanceHardError&) {
        throw;  // uncatchable — propagate to main
    } catch (NaabError& e) {
        // Bind error to catch variable in new scope
        NaabVal error_val = e.getValue();
        if (error_val.isNull()) {
            error_val = NaabVal::makeString(e.getMessage());
        }
        auto catch_env = std::make_shared<Environment>(current_env_);
        catch_env->define(node.getErrorName(), error_val);
        auto saved = current_env_;
        current_env_ = catch_env;
        // V-TAINT-THROW: propagate taint from thrown value to catch variable
        bool prev_taint = false;
        if (governance_ && governance_->isActive()) {
            prev_taint = governance_->isTainted(node.getErrorName());
            if (e.isTainted()) {
                governance_->markTainted(node.getErrorName());
            } else {
                governance_->clearTaint(node.getErrorName());
            }
        }
        node.getCatchExpr()->accept(*this);
        // Restore outer taint state for the error variable name
        if (governance_ && governance_->isActive()) {
            if (prev_taint) governance_->markTainted(node.getErrorName());
            else governance_->clearTaint(node.getErrorName());
        }
        current_env_ = saved;
    } catch (const std::exception& ex) {
        auto error_val = NaabVal::makeString(ex.what());
        auto catch_env = std::make_shared<Environment>(current_env_);
        catch_env->define(node.getErrorName(), error_val);
        auto saved = current_env_;
        current_env_ = catch_env;
        // V-TAINT-THROW: std::exception from polyglot/stdlib — mark as tainted
        bool prev_taint = false;
        if (governance_ && governance_->isActive()) {
            prev_taint = governance_->isTainted(node.getErrorName());
            governance_->markTainted(node.getErrorName());
        }
        node.getCatchExpr()->accept(*this);
        if (governance_ && governance_->isActive()) {
            if (prev_taint) governance_->markTainted(node.getErrorName());
            else governance_->clearTaint(node.getErrorName());
        }
        current_env_ = saved;
    }
}

void Interpreter::visit(ast::MatchExpr& node) {
    auto subject = eval(*node.getSubject());

    for (auto& arm : node.getArms()) {
        // Create a new scope for each arm (for binding patterns)
        auto arm_env = std::make_shared<Environment>(current_env_);
        auto saved_env = current_env_;

        if (!arm.pattern) {
            // Wildcard (_) — always matches
            // Evaluate guard if present
            if (arm.guard) {
                current_env_ = arm_env;
                auto guard_val = eval(*arm.guard);
                current_env_ = saved_env;
                if (!guard_val.toBool()) continue;
            }
            current_env_ = arm_env;
            arm.body->accept(*this);
            current_env_ = saved_env;
            return;
        }

        bool matches = false;

        // Check if pattern is a binding identifier (a lone variable name, not a literal/expr)
        auto* ident = dynamic_cast<ast::IdentifierExpr*>(arm.pattern.get());
        bool is_binding = (ident != nullptr);

        // Check if pattern is array destructuring [a, b, c]
        auto* list_pat = dynamic_cast<ast::ListExpr*>(arm.pattern.get());

        if (is_binding) {
            // Binding pattern: always matches, binds subject to name
            matches = true;
            arm_env->define(ident->getName(), subject);
        } else if (list_pat) {
            // Array destructuring pattern
            if (subject.isList() && subject.asListConst().size() == list_pat->getElements().size()) {
                const auto& subj_arr = subject.asListConst();
                matches = true;
                for (size_t i = 0; i < list_pat->getElements().size(); i++) {
                    auto* elem_ident = dynamic_cast<ast::IdentifierExpr*>(list_pat->getElements()[i].get());
                    if (elem_ident) {
                        // Identifier element: bind the value
                        arm_env->define(elem_ident->getName(), subj_arr[i]);
                    } else {
                        // Literal element: must match exactly
                        auto pat_elem = eval(*list_pat->getElements()[i]);
                        NaabVal subj_elem_nv = subj_arr[i];
                        bool elem_match = false;
                        bool s_null = subj_elem_nv.isNull(), p_null = pat_elem.isNull();
                        if (s_null && p_null) {
                            elem_match = true;
                        } else if (!s_null && !p_null) {
                            bool s_num = subj_elem_nv.isInt() || subj_elem_nv.isDouble();
                            bool p_num = pat_elem.isInt() || pat_elem.isDouble();
                            if (s_num && p_num) elem_match = subj_elem_nv.toFloat() == pat_elem.toFloat();
                            else elem_match = subj_elem_nv.toString() == pat_elem.toString();
                        }
                        if (!elem_match) { matches = false; break; }
                    }
                }
            }
        } else {
            // Value pattern: evaluate and compare
            auto pattern_val = eval(*arm.pattern);
            bool subj_null = subject.isNull();
            bool pat_null = pattern_val.isNull();

            if (subj_null && pat_null) {
                matches = true;
            } else if (!subj_null && !pat_null) {
                bool subj_numeric = subject.isInt() || subject.isDouble();
                bool pat_numeric = pattern_val.isInt() || pattern_val.isDouble();

                if (subj_numeric && pat_numeric) {
                    matches = subject.toFloat() == pattern_val.toFloat();
                } else if (subject.isString() && pattern_val.isString()) {
                    matches = subject.toString() == pattern_val.toString();
                } else if (subject.isBool() && pattern_val.isBool()) {
                    matches = subject.toBool() == pattern_val.toBool();
                }
            }
        }

        if (matches) {
            // Evaluate guard in arm scope (bindings are available)
            if (arm.guard) {
                current_env_ = arm_env;
                auto guard_val = eval(*arm.guard);
                current_env_ = saved_env;
                if (!guard_val.toBool()) continue;  // Guard failed, try next arm
            }
            current_env_ = arm_env;
            arm.body->accept(*this);
            current_env_ = saved_env;
            return;
        }
    }

    throw std::runtime_error(
        "Match error: no matching arm for value: " + subject.toString() + "\n\n"
        "  Help:\n"
        "  - Add a wildcard arm to handle all other cases:\n\n"
        "  Example:\n"
        "    match value {\n"
        "        1 => \"one\"\n"
        "        _ => \"default\"\n"
        "    }\n"
    );
}

void Interpreter::visit(ast::AwaitExpr& node) {
    auto value = eval(*node.getExpr());

    // If it's a FutureValue, block until resolved
    if (value.isFuture()) {
        auto& future_val = value.asFutureConst();
        std::string awaited_func_name = future_val->func_name;
        try {
            result_ = future_val->future.get();
        } catch (const governance::GovernanceHardError&) {
            throw;  // uncatchable — propagate to main
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Await error: " + future_val->description + " failed\n\n"
                "  Cause: " + std::string(e.what()) + "\n"
            );
        }

        // BUG-K: Check return contract at await resolution point
        if (governance_ && governance_->isActive() && !awaited_func_name.empty()) {
            std::string result_str = result_.isNull() ? "null" : result_.toString();
            std::string result_type = result_.getTypeName();
            std::string contract_err = governance_->checkFunctionContract(
                awaited_func_name, result_str, result_type, node.getLocation().line, current_file_);
            if (!contract_err.empty()) {
                governance_->logContractCheck(awaited_func_name, "FAIL", contract_err,
                                              current_file_, node.getLocation().line);
                throw std::runtime_error(contract_err);
            }
        }

        // BUG-AwaitExpr fix: Propagate async return taint to current context
        if (future_val->return_tainted->load() && governance_ && governance_->isActive()) {
            governance_->setLastReturnTainted(true);
        }

        return;
    }

    // Await on non-future = pass through (identity)
    result_ = value;
}

// Phase 5: Yield expression — suspend generator and pass value to consumer
void Interpreter::visit(ast::YieldExpr& node) {
    if (!active_generator_) {
        throw std::runtime_error(
            "Yield error: 'yield' used outside of a generator function\n\n"
            "  'yield' can only be used inside functions that are called as generators.\n\n"
            "  Example:\n"
            "    fn count_up(n) {\n"
            "        for i in 0..n {\n"
            "            yield i\n"
            "        }\n"
            "    }\n"
            "    for x in count_up(5) {\n"
            "        print(x)\n"
            "    }\n");
    }

    // Evaluate the yielded value and collect it
    auto value = eval(*node.getExpr());
    active_generator_->collected_values.push_back(value);
    result_ = NaabVal::makeNull();  // yield itself returns void
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

    result_ = NaabVal::makeFunction(func_val);
}

void Interpreter::visit(ast::ForStmt& node) {
    if (debugger_ && debugger_->isActive()) {
        debugger_->setCurrentEnvironment(current_env_);
        auto loc = node.getLocation();
        std::string loc_str = (current_file_.empty() ? "<unknown>" : current_file_) +
            ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
        debugger_->shouldBreak(loc_str);
    }
    // Increment loop depth for break/continue validation
    ++loop_depth_;

    auto iterable = eval(*node.getIter());

    // BUG-E + BUG-2: If iterable is tainted, mark loop variable as tainted
    if (governance_ && governance_->isActive()) {
        if (checkRhsTainted(node.getIter())) {
            if (node.isDestructuring()) {
                for (const auto& name : node.getDestructureNames()) {
                    governance_->markTainted(name);
                }
            } else {
                governance_->markTainted(node.getVar());
            }
        }
    }

    // Helper lambda: bind loop element to destructured names or single var
    auto defineLoopVar = [&](NaabVal item) {
        if (!node.isDestructuring()) {
            current_env_->define(node.getVar(), item);
            return;
        }
        const auto& names = node.getDestructureNames();
        int rest_idx = node.getRestIndex();
        // Array element destructuring
        if (item.isList()) {
            const auto& arr = item.asListConst();
            size_t required = (rest_idx >= 0) ? static_cast<size_t>(rest_idx) : names.size();
            if (arr.size() < required) {
                throw std::runtime_error(
                    "For loop destructuring error: element has " + std::to_string(arr.size()) +
                    " items, need at least " + std::to_string(required));
            }
            for (size_t i = 0; i < names.size(); ++i) {
                if (rest_idx >= 0 && i == static_cast<size_t>(rest_idx)) {
                    std::vector<NaabVal> rest_arr;
                    for (size_t j = static_cast<size_t>(rest_idx); j < arr.size(); ++j) {
                        rest_arr.push_back(arr[j]);
                    }
                    current_env_->define(names[i], NaabVal::makeList(std::move(rest_arr)));
                } else {
                    current_env_->define(names[i], arr[i]);
                }
            }
        }
        // Dict element destructuring (for [key, val] in dict)
        else if (item.isDict()) {
            const auto& dict_item = item.asDictConst();
            for (const auto& name : names) {
                auto it = dict_item.find(name);
                if (it != dict_item.end()) {
                    current_env_->define(name, it->second);
                } else {
                    current_env_->define(name, NaabVal::makeNull());
                }
            }
        }
        else {
            throw std::runtime_error(
                "For loop destructuring error: cannot destructure " + item.getTypeName() +
                " — each element must be an array or dict");
        }
    };

    // Check if it's a range (dict with __is_range marker)
    if (iterable.isDict()) {
        const auto& dict = iterable.asDictConst();
        auto it = dict.find("__is_range");
        if (it != dict.end() && it->second.toBool()) {
            // This is a range - iterate from start to end
            // V-RT-006: Guard range dict key access
            auto rs_it = dict.find("__range_start");
            auto re_it = dict.find("__range_end");
            if (rs_it == dict.end() || re_it == dict.end()) {
                throw std::runtime_error("Runtime error: malformed range object");
            }
            int start = rs_it->second.toInt();
            int end_val = re_it->second.toInt();
            bool inclusive = false;

            // Check for inclusive flag (..= operator)
            auto inc_it = dict.find("__range_inclusive");
            if (inc_it != dict.end()) {
                inclusive = inc_it->second.toBool();
            }

            // Use <= for inclusive ranges, < for exclusive
            if (inclusive) {
                size_t iter_count = 0;
                int range_idx = 0;
                for (int i = start; i <= end_val; i++) {
                    if (governance_ && governance_->isActive()) {
                        std::string err = governance_->checkLoopIterations(++iter_count);
                        if (!err.empty()) throw std::runtime_error(err);
                    }
                    if (node.hasIndex()) {
                        current_env_->define(node.getIndexVar(), NaabVal::makeInt(range_idx++));
                    }
                    current_env_->define(node.getVar(), NaabVal::makeInt(i));
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
                size_t iter_count = 0;
                int range_idx = 0;
                for (int i = start; i < end_val; i++) {
                    if (governance_ && governance_->isActive()) {
                        std::string err = governance_->checkLoopIterations(++iter_count);
                        if (!err.empty()) throw std::runtime_error(err);
                    }
                    if (node.hasIndex()) {
                        current_env_->define(node.getIndexVar(), NaabVal::makeInt(range_idx++));
                    }
                    current_env_->define(node.getVar(), NaabVal::makeInt(i));
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
            --loop_depth_;
            return;
        }

        // Not a range — iterate as dict (over keys, or destructure [key, val])
        size_t iter_count = 0;
        for (const auto& [key, val] : dict) {
            if (governance_ && governance_->isActive()) {
                std::string err = governance_->checkLoopIterations(++iter_count);
                if (!err.empty()) throw std::runtime_error(err);
            }
            if (node.isDestructuring()) {
                // for [k, v] in dict — bind key and value
                std::vector<NaabVal> pair;
                pair.push_back(NaabVal::makeString(key));
                pair.push_back(val);
                defineLoopVar(NaabVal::makeList(std::move(pair)));
            } else if (node.hasIndex()) {
                // for (k, v) in dict — bind k=key, v=value (unified with [k,v])
                current_env_->define(node.getIndexVar(), NaabVal::makeString(key));
                current_env_->define(node.getVar(), val);
            } else {
                // for k in dict — bind k=key only
                current_env_->define(node.getVar(), NaabVal::makeString(key));
            }
            node.getBody()->accept(*this);
            if (returning_) break;
            if (breaking_) { breaking_ = false; break; }
            if (continuing_) { continuing_ = false; continue; }
        }
        --loop_depth_;
        return;
    }

    // Handle as list
    if (iterable.isList()) {
        const auto& list = iterable.asListConst();
        size_t iter_count = 0;
        for (size_t idx = 0; idx < list.size(); ++idx) {
            if (governance_ && governance_->isActive()) {
                std::string err = governance_->checkLoopIterations(++iter_count);
                if (!err.empty()) throw std::runtime_error(err);
            }
            // U2: Define index variable if present
            if (node.hasIndex()) {
                current_env_->define(node.getIndexVar(), NaabVal::makeInt(static_cast<int>(idx)));
            }
            defineLoopVar(list[idx]);
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
        --loop_depth_;
        return;
    }

    // Phase 5: Generator iteration — eager collection approach
    // Run generator function body eagerly, collecting yielded values into a list.
    // Then iterate over the collected list.
    if (iterable.isGenerator()) {
        auto gen = iterable.asGenerator();
        auto func = gen->func;

        // Set up function environment
        auto parent_env = func->closure ? func->closure : global_env_;
        auto func_env = std::make_shared<Environment>(parent_env);
        for (size_t i = 0; i < gen->args.size() && i < func->params.size(); i++) {
            func_env->define(func->params[i], gen->args[i]);
        }
        for (size_t i = gen->args.size(); i < func->params.size(); i++) {
            if (func->defaults[i]) {
                func_env->define(func->params[i], eval(*func->defaults[i]));
            }
        }

        // Save interpreter state
        auto saved_env = current_env_;
        auto saved_result = result_;
        auto saved_returning = returning_;
        auto saved_generator = active_generator_;

        // Run generator body eagerly — yield pushes to gen->collected_values
        current_env_ = func_env;
        returning_ = false;
        active_generator_ = gen.get();

        // FIX-E2: Exception safety — restore state even if generator body throws
        try {
            func->body->accept(*this);
        } catch (...) {
            current_env_ = saved_env;
            result_ = saved_result;
            returning_ = saved_returning;
            active_generator_ = saved_generator;
            throw;
        }

        // Restore state
        current_env_ = saved_env;
        result_ = saved_result;
        returning_ = saved_returning;
        active_generator_ = saved_generator;

        // Now iterate over the collected values
        size_t iter_count = 0;
        for (auto& item : gen->collected_values) {
            if (governance_ && governance_->isActive()) {
                std::string err = governance_->checkLoopIterations(++iter_count);
                if (!err.empty()) throw std::runtime_error(err);
            }
            defineLoopVar(item);
            node.getBody()->accept(*this);
            if (returning_) break;
            if (breaking_) { breaking_ = false; break; }
            if (continuing_) { continuing_ = false; continue; }
        }

        --loop_depth_;
        return;
    }

    // Non-iterable type - error
    --loop_depth_;
    throw std::runtime_error(
        "Type error: Cannot iterate over " + iterable.getTypeName() + "\n\n"
        "  for loops work with:\n"
        "  - Arrays:  for item in [1, 2, 3] { ... }\n"
        "  - Ranges:  for i in 0..10 { ... }\n"
        "  - Dicts:   for key in {\"a\": 1} { ... }  (iterates keys)\n"
        "  - Generators: for x in gen_func() { ... }\n"
    );
}

void Interpreter::visit(ast::WhileStmt& node) {
    if (debugger_ && debugger_->isActive()) {
        debugger_->setCurrentEnvironment(current_env_);
        auto loc = node.getLocation();
        std::string loc_str = (current_file_.empty() ? "<unknown>" : current_file_) +
            ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
        debugger_->shouldBreak(loc_str);
    }
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

    size_t iter_count = 0;
    while (true) {
        // V-API-004 (R24): honor ScopedTimeout in tree-walker hot loop. The
        // signal handler sets a thread_local flag; check it here so an
        // infinite loop in REST-served code can't pin a worker thread.
        if (naab::security::ResourceLimiter::isTimeoutTriggered()) {
            throw std::runtime_error("Execution timeout exceeded (while loop)");
        }

        auto condition = eval(*node.getCondition());
        if (!condition.toBool()) break;

        if (governance_ && governance_->isActive()) {
            std::string err = governance_->checkLoopIterations(++iter_count);
            if (!err.empty()) throw std::runtime_error(err);
        }

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
    if (debugger_ && debugger_->isActive()) {
        debugger_->setCurrentEnvironment(current_env_);
        auto loc = node.getLocation();
        std::string loc_str = (current_file_.empty() ? "<unknown>" : current_file_) +
            ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
        debugger_->shouldBreak(loc_str);
    }
    explain("Declaring variable '" + node.getName() + "'");

    // FIX-DX-4: Warn if variable name shadows interpreter internals
    {
        static const std::unordered_set<std::string> RESERVED_NAMES = {
            "result_", "returning_", "breaking_", "continuing_",
            "current_env_", "global_env_", "governance_"
        };
        if (RESERVED_NAMES.count(node.getName())) {
            fmt::print(stderr, "[WARN] Variable '{}' shadows an internal name at {}:{}. "
                       "Consider using a different name (e.g., 'result' instead of 'result_').\n",
                       node.getName(), current_file_, node.getLocation().line);
        }
    }

    NaabVal value;
    if (node.getInit()) {
        value = eval(*node.getInit());
    } else {
        value = NaabVal::makeNull();
    }

    // Governance v4: Taint tracking — mark variables from taint sources (REFACTOR-1)
    if (governance_ && governance_->isActive() && node.getInit()) {
        if (checkRhsTainted(node.getInit())) {
            // Pass lineage origin info if available (from setLastReturnTainted with source)
            std::string origin = governance_->lastTaintSource();
            std::string arg_hint;
            // Try to extract argument from AST (e.g., env.get("KEY") -> "KEY")
            if (auto* call = dynamic_cast<ast::CallExpr*>(node.getInit())) {
                if (!call->getArgs().empty()) {
                    if (auto* lit = dynamic_cast<ast::LiteralExpr*>(call->getArgs()[0].get())) {
                        if (lit->getLiteralKind() == ast::LiteralKind::String) {
                            arg_hint = lit->getValue();
                        }
                    }
                }
            }
            // If no direct source (taint propagated from another variable), inherit lineage
            if (origin.empty()) {
                if (auto* id = dynamic_cast<ast::IdentifierExpr*>(node.getInit())) {
                    auto meta = governance_->getTaintLineage(id->getName());
                    if (meta) { origin = meta->origin_function; arg_hint = meta->origin_argument; }
                } else if (auto* bin = dynamic_cast<ast::BinaryExpr*>(node.getInit())) {
                    // For concat: check both sides for tainted identifier with lineage
                    for (auto* side : {bin->getLeft(), bin->getRight()}) {
                        if (auto* sid = dynamic_cast<ast::IdentifierExpr*>(side)) {
                            auto meta = governance_->getTaintLineage(sid->getName());
                            if (meta) { origin = meta->origin_function; arg_hint = meta->origin_argument; break; }
                        }
                    }
                }
            }
            governance_->markTainted(node.getName(), origin, arg_hint,
                current_file_, node.getLocation().line);
        } else {
            governance_->clearTaint(node.getName());
        }
        if (checkRhsSanitized(node.getInit())) {
            governance_->clearTaint(node.getName());
        }
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
    if (value.isList() || value.isDict()) {
        value = copyValue(value);
    }

    current_env_->define(node.getName(), value);
}

// Destructuring: let [a, b] = expr  OR  let {x, y} = expr
void Interpreter::visit(ast::DestructureStmt& node) {
    auto value = eval(*node.getInit());
    const auto& names = node.getNames();

    if (node.getDestructureKind() == ast::DestructureStmt::Kind::Array) {
        // Array destructuring: value must be an array
        if (!value.isList()) {
            throw std::runtime_error(
                "Destructuring error: Cannot destructure non-array value\n\n"
                "  Expected: array with " + std::to_string(names.size()) + " elements\n"
                "  Got: " + value.getTypeName() + "\n\n"
                "  Help:\n"
                "  - Array destructuring requires an array on the right side\n\n"
                "  Example:\n"
                "    let [a, b, c] = [1, 2, 3]\n");
        }
        const auto& arr = value.asListConst();
        int rest_idx = node.getRestIndex();
        size_t required = (rest_idx >= 0) ? static_cast<size_t>(rest_idx) : names.size();
        if (arr.size() < required) {
            throw std::runtime_error(
                "Destructuring error: Not enough elements to destructure\n\n"
                "  Expected: at least " + std::to_string(required) + " elements\n"
                "  Got: " + std::to_string(arr.size()) + " elements\n\n"
                "  Help:\n"
                "  - The array must have at least as many elements as names\n");
        }

        bool is_tainted = governance_ && governance_->isActive() && checkRhsTainted(node.getInit());

        for (size_t i = 0; i < names.size(); ++i) {
            if (rest_idx >= 0 && i == static_cast<size_t>(rest_idx)) {
                // This is the ...rest element — collect remaining into array
                std::vector<NaabVal> rest_arr;
                for (size_t j = static_cast<size_t>(rest_idx); j < arr.size(); ++j) {
                    rest_arr.push_back(copyValue(arr[j]));
                }
                current_env_->define(names[i], NaabVal::makeList(std::move(rest_arr)));
            } else {
                current_env_->define(names[i], copyValue(arr[i]));
            }

            if (is_tainted) {
                governance_->markTainted(names[i]);
            }
        }
    } else {
        // Dict destructuring: value must be a dict
        if (!value.isDict()) {
            throw std::runtime_error(
                "Destructuring error: Cannot destructure non-dict value\n\n"
                "  Expected: dict with keys: " + [&]() {
                    std::string keys;
                    for (size_t i = 0; i < names.size(); ++i) {
                        if (i > 0) keys += ", ";
                        keys += "\"" + names[i] + "\"";
                    }
                    return keys;
                }() + "\n"
                "  Got: " + value.getTypeName() + "\n\n"
                "  Help:\n"
                "  - Dict destructuring requires a dict on the right side\n\n"
                "  Example:\n"
                "    let {name, age} = {\"name\": \"Alice\", \"age\": 30}\n");
        }
        const auto& dict = value.asDictConst();
        for (const auto& name : names) {
            auto it = dict.find(name);
            if (it != dict.end()) {
                current_env_->define(name, copyValue(it->second));
            } else {
                current_env_->define(name, NaabVal::makeNull());  // null for missing keys
            }

            // Governance taint: propagate from dict source
            if (governance_ && governance_->isActive()) {
                if (checkRhsTainted(node.getInit())) {
                    governance_->markTainted(name);
                }
            }
        }
    }
}

// Phase 4.1: Exception handling
void Interpreter::visit(ast::TryStmt& node) {
    try {
        // Execute try block
        node.getTryBody()->accept(*this);
    } catch (const governance::GovernanceHardError&) {
        throw;  // uncatchable — propagate to main
    } catch (NaabError& e) {
        // Phase 4.1: Error propagation - exception caught

        // Execute catch block with error bound to catch variable
        auto catch_env = std::make_shared<Environment>(current_env_);
        auto prev_env = current_env_;
        current_env_ = catch_env;

        // Bind the error value to the catch variable
        auto* catch_clause = node.getCatchClause();

        // Issue #6 Fix: Ensure error object has structured properties
        NaabVal error_val = e.getValue();
        if (error_val.isNull()) {
            // Create structured error object if getValue() returned null
            std::unordered_map<std::string, NaabVal> error_dict;
            error_dict["message"] = NaabVal::makeString(e.getMessage());
            error_dict["type"] = NaabVal::makeString(NaabError::errorTypeToString(e.getType()));
            error_val = NaabVal::makeDict(std::move(error_dict));
        }
        current_env_->define(catch_clause->error_name, error_val);

        // BUG-AB + REFACTOR-2 + V-TAINT-THROW: Scoped catch variable taint
        bool catch_var_was_tainted = false;
        if (governance_ && governance_->isActive()) {
            catch_var_was_tainted = governance_->isTainted(catch_clause->error_name);
            // V-TAINT-THROW: propagate taint from thrown value to catch variable
            if (e.isTainted()) {
                governance_->markTainted(catch_clause->error_name);
            } else {
                governance_->clearTaint(catch_clause->error_name);
            }
        }

        // BUG-1: Lambda to restore catch variable taint on ALL exit paths (happy + throw)
        auto restore_catch_taint = [&]() {
            if (governance_ && governance_->isActive()) {
                if (catch_var_was_tainted) governance_->markTainted(catch_clause->error_name);
                else governance_->clearTaint(catch_clause->error_name);
            }
        };

        try {
            // Execute catch body - successfully handled if no exception
            catch_clause->body->accept(*this);
        } catch (const governance::GovernanceHardError&) {
            restore_catch_taint();
            current_env_ = prev_env;
            throw;  // uncatchable — propagate to main
        } catch (NaabError&) {
            // BUG-1: Restore taint before propagating exception
            restore_catch_taint();
            current_env_ = prev_env;
            throw;
        } catch (const std::exception& std_error) {
            // BUG-1: Restore taint before propagating exception
            restore_catch_taint();
            current_env_ = prev_env;
            throw createError(std_error.what(), ErrorType::RUNTIME_ERROR);
        }

        // BUG-1: Restore on happy path too
        restore_catch_taint();
        current_env_ = prev_env;

    } catch (const std::exception& std_error) {
        // Convert std::exception to NaabError and execute catch block
        // This handles polyglot exceptions (Python, JavaScript, etc.)

        auto catch_env = std::make_shared<Environment>(current_env_);
        auto prev_env = current_env_;
        current_env_ = catch_env;

        // Create structured error object from std::exception
        // Issue #6 Fix: Create dict with "message" property for polyglot exceptions
        std::unordered_map<std::string, NaabVal> error_dict;
        error_dict["message"] = NaabVal::makeString(std::string(std_error.what()));
        error_dict["type"] = NaabVal::makeString(std::string("PolyglotError"));
        auto error_value = NaabVal::makeDict(std::move(error_dict));

        // Bind the error value to the catch variable
        auto* catch_clause = node.getCatchClause();
        current_env_->define(catch_clause->error_name, error_value);

        // Finding D fix + BUG-AB + REFACTOR-2: Scoped catch variable taint.
        // Save the pre-existing taint state for the catch variable name (almost always false
        // since this is a new variable scoped to the catch block), then mark it TAINTED.
        // The polyglot exception message (std_error.what()) is untrusted external input:
        // it may contain API keys, tokens, or other sensitive data from the foreign runtime.
        // Taint it so downstream sinks (http.post, file.write, etc.) block exfiltration.
        // The restore lambda below clears the taint when the catch block exits, so the
        // taint does not escape the catch scope.
        bool catch_var_was_tainted2 = false;
        if (governance_ && governance_->isActive()) {
            catch_var_was_tainted2 = governance_->isTainted(catch_clause->error_name);
            governance_->markTainted(catch_clause->error_name);  // Finding D: was clearTaint
        }

        // BUG-1: Lambda to restore catch variable taint on ALL exit paths (happy + throw)
        auto restore_catch_taint2 = [&]() {
            if (governance_ && governance_->isActive()) {
                if (catch_var_was_tainted2) governance_->markTainted(catch_clause->error_name);
                else governance_->clearTaint(catch_clause->error_name);
            }
        };

        try {
            // Execute catch body
            catch_clause->body->accept(*this);
        } catch (const governance::GovernanceHardError&) {
            restore_catch_taint2();
            current_env_ = prev_env;
            throw;  // uncatchable — propagate to main
        } catch (NaabError&) {
            // BUG-1: Restore taint before propagating exception
            restore_catch_taint2();
            current_env_ = prev_env;
            throw;
        } catch (const std::exception& nested_error) {
            // BUG-1: Restore taint before propagating exception
            restore_catch_taint2();
            current_env_ = prev_env;
            throw createError(nested_error.what(), ErrorType::RUNTIME_ERROR);
        }

        // BUG-1: Restore on happy path too
        restore_catch_taint2();

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
        result_ = NaabVal::makeNull();

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
    auto val = eval(*node.getExpr());
    // V-TAINT-THROW: propagate taint across throw/catch
    NaabError err(val);
    if (governance_ && governance_->isActive()) {
        err.setTainted(checkRhsTainted(node.getExpr()));
    }
    throw err;
}
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
NaabVal Interpreter::copyValue(NaabVal nval) {
    // Inline types are trivially copied (no heap allocation)
    if (nval.isNull()) return NaabVal::makeNull();
    if (nval.isInt()) return NaabVal::makeInt(nval.asInt());
    if (nval.isDouble()) return NaabVal::makeDouble(nval.asDouble());
    if (nval.isBool()) return NaabVal::makeBool(nval.asBool());

    // Heap types: use NaabVal type checks directly
    if (nval.isString()) {
        return NaabVal::makeString(nval.asString());
    } else if (nval.isList()) {
        const auto& list = nval.asListConst();
        std::vector<NaabVal> new_list;
        for (const auto& item : list) {
            new_list.push_back(copyValue(item));
        }
        return NaabVal::makeList(std::move(new_list));
    } else if (nval.isDict()) {
        const auto& dict = nval.asDictConst();
        std::unordered_map<std::string, NaabVal> new_dict;
        for (const auto& [key, val] : dict) {
            new_dict[key] = copyValue(val);
        }
        return NaabVal::makeDict(std::move(new_dict));
    } else if (nval.isStructVal()) {
        const auto& struct_val = nval.asStructConst();
        auto new_struct = std::make_shared<StructValue>(struct_val->type_name, struct_val->definition);
        for (size_t i = 0; i < struct_val->field_values.size(); i++) {
            new_struct->field_values[i] = copyValue(struct_val->field_values[i]);
        }
        return NaabVal::makeStruct(new_struct);
    }

    // Functions, blocks, python objects: not deep copied, share reference
    return nval;
}

// Parallel polyglot execution: Capture variables for thread-safe parallel execution
// Type system methods (inferValueType, inferTypeBindings, substituteType,
// monomorphizeStruct, valueMatchesType, etc.) moved to type_system.cpp

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
    std::vector<NaabVal> extra_roots;
    if (!result_.isNull()) {
        extra_roots.push_back(result_);
    }

    // Build extra environments: always include global_env_ if root is not global
    std::vector<std::shared_ptr<Environment>> extra_envs;
    if (root_env != global_env_ && global_env_) {
        extra_envs.push_back(global_env_);
    }

    // BUG-10 fix: Include all saved caller environments from the call stack
    // During nested function calls, caller envs are saved as C++ locals (saved_env)
    // and are invisible to the GC. env_stack_ makes them visible as GC roots.
    for (const auto& env_on_stack : env_stack_) {
        if (env_on_stack) {
            extra_envs.push_back(env_on_stack);
        }
    }

    // Run mark-and-sweep cycle detection with complete root set
    size_t collected = cycle_detector_->detectAndCollect(root_env, extra_roots, extra_envs);

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

void Interpreter::registerValue(NaabVal value) {
    // Phase J: No-op — handle table is iterated directly by GC.
    // Heap values are automatically tracked via NaabVal's handle table.
    (void)value;
}

void Interpreter::trackAllocation() {
    if (!gc_enabled_ || !cycle_detector_ || gc_suspended_) {
        return;
    }

    allocation_count_++;

    // Register the newly created value for global tracking (complete GC)
    // Only track heap-allocated values (NaabVal inline types don't need GC)
    if (result_.isHeap()) {
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

// Debug module support: return all variables in current scope
std::unordered_map<std::string, NaabVal> Interpreter::getCurrentScopeVariables() const {
    std::unordered_map<std::string, NaabVal> result;
    if (current_env_) {
        auto names = current_env_->getAllNames();
        for (const auto& name : names) {
            auto val = current_env_->get(name);
            if (!val.isNull()) result[name] = val;
        }
    }
    return result;
}

// Debug module support: return call stack as array of strings
std::vector<std::string> Interpreter::getCallStackInfo() const {
    std::vector<std::string> result;
    for (const auto& frame : call_stack_) {
        result.push_back(frame.toString());
    }
    return result;
}

} // namespace interpreter
} // namespace naab
