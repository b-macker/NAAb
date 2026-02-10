// NAAb JavaScript Executor Implementation
// Executes JavaScript blocks using QuickJS engine

#include "naab/js_executor.h"
#include "naab/interpreter.h"
#include "naab/resource_limits.h"
#include "naab/audit_logger.h"
#include "naab/sandbox.h"
#include "naab/stack_tracer.h"  // Phase 4.2.3: Cross-language stack traces
#include "naab/limits.h"  // Week 1, Task 1.2: Input size caps
#include <climits>      // For INT32_MIN, INT32_MAX
#include <fmt/core.h>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <atomic>
#include <sstream>  // Phase 4.2.3: For parsing JS stack traces

// Include QuickJS headers
extern "C" {
#include "quickjs.h"
#include "quickjs-libc.h"
}

namespace naab {
namespace runtime {

// Forward declarations of static helper functions
static JSValue toJSValue(JSContext* ctx, const std::shared_ptr<interpreter::Value>& val);
static std::shared_ptr<interpreter::Value> fromJSValue(JSContext* ctx, JSValue val);

JsExecutor::JsExecutor() : rt_(nullptr), ctx_(nullptr), timeout_triggered_(false) {
    // Create JavaScript runtime
    rt_ = JS_NewRuntime();
    if (!rt_) {
        throw std::runtime_error("Failed to create JavaScript runtime");
    }

    // Install interrupt handler for timeout support
    JS_SetInterruptHandler(rt_, interruptHandler, this);

    // Create JavaScript context
    ctx_ = JS_NewContext(rt_);
    if (!ctx_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
        throw std::runtime_error("Failed to create JavaScript context");
    }

    // Initialize standard library
    js_std_add_helpers(ctx_, 0, nullptr);

    // JavaScript runtime initialized with timeout support (silent)
}

JsExecutor::~JsExecutor() {
    if (ctx_) {
        JS_FreeContext(ctx_);
        ctx_ = nullptr;
    }
    if (rt_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
    }
    // JavaScript runtime shut down (silent)
}

bool JsExecutor::execute(const std::string& code) {
    // Default to INLINE_CODE for backwards compatibility
    return execute(code, JsExecutionMode::INLINE_CODE);
}

bool JsExecutor::execute(const std::string& code, JsExecutionMode mode) {
    // Week 1, Task 1.2: Check polyglot block size
    try {
        limits::checkPolyglotBlockSize(code.size(), "JavaScript");
    } catch (const limits::InputSizeException& e) {
        fmt::print("[ERROR] {}\n", e.what());
        return false;
    }

    if (!isInitialized()) {
        fmt::print("[ERROR] JavaScript runtime not initialized\n");
        return false;
    }

    // Check sandbox permissions for code execution
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: JavaScript execution denied\n");
        sandbox->logViolation("executeJavaScript", "<code>", "BLOCK_CALL capability required");
        return false;
    }

    // Choose wrapping strategy based on execution mode
    std::string code_to_execute;

    if (mode == JsExecutionMode::INLINE_CODE) {
        // Wrap code in IIFE to isolate variable scope between inline blocks
        // This prevents variable redeclaration errors when using const/let
        code_to_execute = "(function() {\n" + code + "\n})();";
    } else {
        // BLOCK_LIBRARY mode: Execute directly in global scope
        // Functions will be defined globally and accessible via callFunction()
        code_to_execute = code;
    }

    // Set up timeout mechanism (30 seconds)
    timeout_triggered_ = false;
    std::thread timeout_thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        timeout_triggered_ = true;
    });
    timeout_thread.detach();

    // Evaluate code
    JSValue result = JS_Eval(ctx_, code_to_execute.c_str(), code_to_execute.length(),
                              "<naab-block>", JS_EVAL_TYPE_GLOBAL);

    // Clear timeout flag
    timeout_triggered_ = false;

    // Check for errors
    if (JS_IsException(result)) {
        std::string error = getLastError();
        fmt::print("[ERROR] JavaScript execution failed: {}\n", error);
        JS_FreeValue(ctx_, result);

        // Check if it was a timeout
        if (error.find("interrupted") != std::string::npos) {
            security::AuditLogger::logTimeout("JavaScript execution", 30);
        }
        return false;
    }

    JS_FreeValue(ctx_, result);
    // JavaScript code executed (silent)
    return true;
}

std::shared_ptr<interpreter::Value> JsExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (!isInitialized()) {
        throw std::runtime_error("JavaScript runtime not initialized");
    }

    // Phase 4.2.3: Push stack frame for cross-language tracing
    error::ScopedStackFrame stack_frame("javascript", function_name, "<javascript>", 0);

    // Get global object
    JSValue global = JS_GetGlobalObject(ctx_);

    // Get function from global object
    JSValue func = JS_GetPropertyStr(ctx_, global, function_name.c_str());

    if (JS_IsUndefined(func) || !JS_IsFunction(ctx_, func)) {
        JS_FreeValue(ctx_, func);
        JS_FreeValue(ctx_, global);
        throw std::runtime_error(fmt::format(
            "Function '{}' not found or not a function", function_name));
    }

    // Convert arguments
    std::vector<JSValue> js_args;
    for (const auto& arg : args) {
        js_args.push_back(toJSValue(ctx_, arg));
    }

    // Set up timeout mechanism (30 seconds)
    timeout_triggered_ = false;
    std::thread timeout_thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        timeout_triggered_ = true;
    });
    timeout_thread.detach();

    // Call function
    JSValue result = JS_Call(ctx_, func, global,
                             js_args.size(), js_args.data());

    // Clear timeout flag
    timeout_triggered_ = false;

    // Free arguments
    for (auto& js_arg : js_args) {
        JS_FreeValue(ctx_, js_arg);
    }

    // Check for errors
    if (JS_IsException(result)) {
        // Phase 4.2.3: Extract JavaScript stack trace and add to unified trace
        extractJavaScriptStackTrace();

        std::string error = getLastError();
        JS_FreeValue(ctx_, func);
        JS_FreeValue(ctx_, global);

        // Check if it was a timeout
        if (error.find("interrupted") != std::string::npos) {
            security::AuditLogger::logTimeout("JavaScript function: " + function_name, 30);
        }

        // Re-throw with enriched stack trace
        throw std::runtime_error(fmt::format(
            "JavaScript function '{}' threw exception: {}\n{}",
            function_name, error, error::StackTracer::formatTrace()));
    }

    // Convert result
    auto naab_result = fromJSValue(ctx_, result);

    // Cleanup
    JS_FreeValue(ctx_, result);
    JS_FreeValue(ctx_, func);
    JS_FreeValue(ctx_, global);

    return naab_result;
}

std::shared_ptr<interpreter::Value> JsExecutor::evaluate(
    const std::string& expression) {

    if (!isInitialized()) {
        throw std::runtime_error("JavaScript runtime not initialized");
    }

    // Enterprise Security: Install signal handlers for resource limits (once)
    if (!security::ResourceLimiter::isInitialized()) {
        security::ResourceLimiter::installSignalHandlers();
    }

    // Enterprise Security: Get limits from sandbox config (if active)
    unsigned int timeout = 30;  // Default: 30 seconds
    size_t memory_limit = 512;  // Default: 512MB
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox) {
        timeout = sandbox->getConfig().max_cpu_seconds;
        memory_limit = sandbox->getConfig().max_memory_mb;
    }

    // Enterprise Security: Apply resource limits BEFORE executing JavaScript code
    try {
        security::ResourceLimiter::setMemoryLimit(memory_limit);
    } catch (const std::exception& e) {
        // Memory limit may fail on some systems, log but continue
    }
    security::ScopedTimeout scoped_timeout(timeout);

    // Phase 2.3: Fixed multi-line code handling
    // Strategy: Just evaluate the code directly - QuickJS eval returns the last expression value

    std::string code = expression;

    // Trim leading/trailing whitespace
    size_t first = code.find_first_not_of(" \t\n\r");
    size_t last = code.find_last_not_of(" \t\n\r");
    if (first != std::string::npos && last != std::string::npos) {
        code = code.substr(first, last - first + 1);
    }

    // In JavaScript, when you eval a sequence of statements, the value is the value of
    // the last expression statement. We wrap in parens to ensure we get a value back.
    // For multi-line code with declarations, we use a different approach:
    // wrap in an IIFE and evaluate as a module/script

    // Check if code contains newlines (multi-statement)
    if (code.find('\n') != std::string::npos) {
        // Multi-line code: wrap in IIFE to get last expression value
        // For a simple expression like [42], just wrap it in an IIFE
        // For multiple statements, wrap in IIFE and use eval to get last value

        // Check if code is a simple expression (no semicolons, no 'let'/'const'/'var'/'function')
        bool is_simple_expr = (code.find(';') == std::string::npos &&
                               code.find("let ") == std::string::npos &&
                               code.find("const ") == std::string::npos &&
                               code.find("var ") == std::string::npos &&
                               code.find("function ") == std::string::npos);

        std::string wrapped;
        if (is_simple_expr) {
            // Simple expression: wrap directly in IIFE without eval
            wrapped = "(function() { return (" + code + "); })()";
        } else {
            // Complex code with statements: use eval approach
            // Escape backticks and backslashes for template literal
            std::string escaped_code = code;
            std::string result_str;
            for (char c : escaped_code) {
                if (c == '`') result_str += "\\`";
                else if (c == '\\') result_str += "\\\\";
                else if (c == '$') result_str += "\\$";
                else result_str += c;
            }
            wrapped = "(function() { return eval(`" + result_str + "`); })()";
        }

        JSValue result = JS_Eval(ctx_, wrapped.c_str(), wrapped.length(),
                                  "<eval>", JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(result)) {
            std::string error = getLastError();
            JS_FreeValue(ctx_, result);
            throw std::runtime_error(fmt::format(
                "JavaScript evaluation failed: {}", error));
        }

        auto naab_result = fromJSValue(ctx_, result);
        JS_FreeValue(ctx_, result);
        return naab_result;
    } else {
        // Single-line expression: wrap in parens and evaluate
        std::string wrapped_expr = "(" + code + ")";

        JSValue result = JS_Eval(ctx_, wrapped_expr.c_str(), wrapped_expr.length(),
                                  "<eval>", JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(result)) {
            std::string error = getLastError();
            JS_FreeValue(ctx_, result);
            throw std::runtime_error(fmt::format(
                "JavaScript evaluation failed: {}", error));
        }

        auto naab_result = fromJSValue(ctx_, result);
        JS_FreeValue(ctx_, result);
        return naab_result;
    }
}

// Interrupt handler for QuickJS timeout support
int JsExecutor::interruptHandler(JSRuntime* rt, void* opaque) {
    (void)rt;  // Unused
    JsExecutor* executor = static_cast<JsExecutor*>(opaque);

    // Return 1 to interrupt execution if timeout triggered
    return executor->timeout_triggered_ ? 1 : 0;
}

// Static helper: Convert NAAb Value to JSValue
static JSValue toJSValue(JSContext* ctx, const std::shared_ptr<interpreter::Value>& val) {
    if (!val) {
        return JS_UNDEFINED;
    }

    // Check type using std::holds_alternative
    if (std::holds_alternative<int>(val->data)) {
        return JS_NewInt32(ctx, std::get<int>(val->data));
    } else if (std::holds_alternative<double>(val->data)) {
        return JS_NewFloat64(ctx, std::get<double>(val->data));
    } else if (std::holds_alternative<bool>(val->data)) {
        return JS_NewBool(ctx, std::get<bool>(val->data));
    } else if (std::holds_alternative<std::string>(val->data)) {
        const auto& str = std::get<std::string>(val->data);
        return JS_NewString(ctx, str.c_str());
    } else if (std::holds_alternative<std::monostate>(val->data)) {
        return JS_NULL;
    } else if (std::holds_alternative<std::vector<std::shared_ptr<interpreter::Value>>>(val->data)) {
        // Array
        const auto& vec = std::get<std::vector<std::shared_ptr<interpreter::Value>>>(val->data);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < vec.size(); i++) {
            JSValue elem = toJSValue(ctx, vec[i]);
            JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), elem);
        }
        return arr;
    } else if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(val->data)) {
        // Dictionary/Object
        const auto& map = std::get<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(val->data);
        JSValue obj = JS_NewObject(ctx);
        for (const auto& [key, value] : map) {
            JSValue prop_val = toJSValue(ctx, value);
            JS_SetPropertyStr(ctx, obj, key.c_str(), prop_val);
        }
        return obj;
    } else {
        // Unsupported type - return undefined
        fmt::print("[WARN] Unsupported type for JavaScript conversion\n");
        return JS_UNDEFINED;
    }
}

// Static helper: Convert JSValue to NAAb Value
static std::shared_ptr<interpreter::Value> fromJSValue(JSContext* ctx, JSValue val) {
    // Null or undefined
    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        return std::make_shared<interpreter::Value>();
    }

    // Boolean
    if (JS_IsBool(val)) {
        int32_t b = JS_ToBool(ctx, val);
        return std::make_shared<interpreter::Value>(b != 0);
    }

    // Number (always get as double first, then check if it fits in int)
    if (JS_IsNumber(val)) {
        double d;
        if (JS_ToFloat64(ctx, &d, val) == 0) {
            // Check if it fits in int32 range and is actually an integer
            if (d >= INT32_MIN && d <= INT32_MAX && d == static_cast<int32_t>(d)) {
                return std::make_shared<interpreter::Value>(static_cast<int>(d));
            } else {
                // Too large or has decimal part, return as double
                return std::make_shared<interpreter::Value>(d);
            }
        }
    }

    // String
    if (JS_IsString(val)) {
        const char* str = JS_ToCString(ctx, val);
        if (str) {
            auto result = std::make_shared<interpreter::Value>(std::string(str));
            JS_FreeCString(ctx, str);
            return result;
        }
    }

    // Array
    if (JS_IsArray(ctx, val)) {
        // Get array length
        JSValue length_val = JS_GetPropertyStr(ctx, val, "length");
        uint32_t length = 0;
        if (JS_IsNumber(length_val)) {
            JS_ToUint32(ctx, &length, length_val);
        }
        JS_FreeValue(ctx, length_val);

        // Convert each element recursively
        std::vector<std::shared_ptr<interpreter::Value>> naab_array;
        for (uint32_t i = 0; i < length; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, val, i);
            naab_array.push_back(fromJSValue(ctx, elem));
            JS_FreeValue(ctx, elem);
        }

        return std::make_shared<interpreter::Value>(naab_array);
    }

    // Object (but not array)
    if (JS_IsObject(val)) {
        std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> naab_dict;

        // Get property names
        JSPropertyEnum* props = nullptr;
        uint32_t prop_count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            // Iterate through properties
            for (uint32_t i = 0; i < prop_count; i++) {
                JSAtom atom = props[i].atom;
                const char* key = JS_AtomToCString(ctx, atom);
                if (key) {
                    JSValue prop_val = JS_GetProperty(ctx, val, atom);
                    naab_dict[std::string(key)] = fromJSValue(ctx, prop_val);
                    JS_FreeValue(ctx, prop_val);
                    JS_FreeCString(ctx, key);
                }
            }

            // Free property array
            for (uint32_t i = 0; i < prop_count; i++) {
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }

        return std::make_shared<interpreter::Value>(naab_dict);
    }

    // Unsupported type
    fmt::print("[WARN] Unsupported JavaScript type, returning null\n");
    return std::make_shared<interpreter::Value>();
}

std::string JsExecutor::getLastError() {
    JSValue exception = JS_GetException(ctx_);

    if (JS_IsNull(exception) || JS_IsUndefined(exception)) {
        return "Unknown error";
    }

    const char* error_str = JS_ToCString(ctx_, exception);
    std::string result = error_str ? error_str : "Unknown error";

    if (error_str) {
        JS_FreeCString(ctx_, error_str);
    }

    JS_FreeValue(ctx_, exception);

    return result;
}

// ============================================================================
// Phase 4.2.3: JavaScript Traceback Extraction
// ============================================================================

void JsExecutor::extractJavaScriptStackTrace() {
    try {
        // Get the exception object
        JSValue exception = JS_GetException(ctx_);

        if (JS_IsNull(exception) || JS_IsUndefined(exception)) {
            return;
        }

        // Get the "stack" property from the exception
        JSValue stack_val = JS_GetPropertyStr(ctx_, exception, "stack");

        if (!JS_IsString(stack_val)) {
            JS_FreeValue(ctx_, stack_val);
            JS_FreeValue(ctx_, exception);
            return;
        }

        // Convert stack trace to C string
        const char* stack_str = JS_ToCString(ctx_, stack_val);
        if (!stack_str) {
            JS_FreeValue(ctx_, stack_val);
            JS_FreeValue(ctx_, exception);
            return;
        }

        std::string stack_trace(stack_str);
        JS_FreeCString(ctx_, stack_str);

        // Parse QuickJS stack trace format
        // QuickJS format: "    at functionName (filename:line)\n"
        std::istringstream ss(stack_trace);
        std::string line;

        while (std::getline(ss, line)) {
            // Skip empty lines
            if (line.empty() || line.find("at ") == std::string::npos) {
                continue;
            }

            // Extract function name and location
            size_t at_pos = line.find("at ");
            if (at_pos == std::string::npos) {
                continue;
            }

            // Extract function name
            size_t func_start = at_pos + 3;
            size_t func_end = line.find(" (", func_start);
            if (func_end == std::string::npos) {
                func_end = line.find("\n", func_start);
                if (func_end == std::string::npos) {
                    func_end = line.length();
                }
            }

            std::string function_name = line.substr(func_start, func_end - func_start);

            // Trim whitespace from function name
            size_t first = function_name.find_first_not_of(" \t");
            size_t last = function_name.find_last_not_of(" \t");
            if (first != std::string::npos && last != std::string::npos) {
                function_name = function_name.substr(first, last - first + 1);
            }

            // Extract filename and line number
            std::string filename = "<javascript>";
            size_t line_number = 0;

            size_t paren_start = line.find('(', func_end);
            size_t paren_end = line.find(')', paren_start);
            if (paren_start != std::string::npos && paren_end != std::string::npos) {
                std::string location = line.substr(paren_start + 1, paren_end - paren_start - 1);

                // Parse "filename:line"
                size_t colon_pos = location.rfind(':');
                if (colon_pos != std::string::npos) {
                    filename = location.substr(0, colon_pos);
                    try {
                        line_number = std::stoull(location.substr(colon_pos + 1));
                    } catch (...) {
                        line_number = 0;
                    }
                }
            }

            // Add JavaScript frame to stack trace
            error::StackFrame js_frame("javascript", function_name, filename, line_number);
            error::StackTracer::pushFrame(js_frame);

            fmt::print("[TRACE] JavaScript frame: {} ({}:{})\n",
                function_name, filename, line_number);
        }

        // Cleanup
        JS_FreeValue(ctx_, stack_val);
        JS_FreeValue(ctx_, exception);

    } catch (const std::exception& ex) {
        fmt::print("[WARN] Failed to extract JavaScript traceback: {}\n", ex.what());
    }
}

} // namespace runtime
} // namespace naab
