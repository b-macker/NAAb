// NAAb JavaScript Executor Implementation
// Executes JavaScript blocks using QuickJS engine

#include "naab/js_executor.h"
#include "naab/interpreter.h"
#include "naab/resource_limits.h"
#include "naab/audit_logger.h"
#include "naab/sandbox.h"
#include <fmt/core.h>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <atomic>

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

    fmt::print("[JS] JavaScript runtime initialized with timeout support\n");
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
    fmt::print("[JS] JavaScript runtime shut down\n");
}

bool JsExecutor::execute(const std::string& code) {
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

    fmt::print("[JS] Executing JavaScript code ({} bytes)\n", code.size());

    // Set up timeout mechanism (30 seconds)
    timeout_triggered_ = false;
    std::thread timeout_thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        timeout_triggered_ = true;
    });
    timeout_thread.detach();

    // Evaluate code in global context
    JSValue result = JS_Eval(ctx_, code.c_str(), code.length(),
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
    fmt::print("[SUCCESS] JavaScript code executed\n");
    return true;
}

std::shared_ptr<interpreter::Value> JsExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (!isInitialized()) {
        throw std::runtime_error("JavaScript runtime not initialized");
    }

    fmt::print("[CALL] Calling JavaScript function: {}\n", function_name);

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
        std::string error = getLastError();
        JS_FreeValue(ctx_, func);
        JS_FreeValue(ctx_, global);

        // Check if it was a timeout
        if (error.find("interrupted") != std::string::npos) {
            security::AuditLogger::logTimeout("JavaScript function: " + function_name, 30);
        }

        throw std::runtime_error(fmt::format(
            "JavaScript function '{}' threw exception: {}", function_name, error));
    }

    // Convert result
    auto naab_result = fromJSValue(ctx_, result);

    // Cleanup
    JS_FreeValue(ctx_, result);
    JS_FreeValue(ctx_, func);
    JS_FreeValue(ctx_, global);

    fmt::print("[RESULT] {} returned: {}\n",
               function_name, naab_result->toString());

    return naab_result;
}

std::shared_ptr<interpreter::Value> JsExecutor::evaluate(
    const std::string& expression) {

    if (!isInitialized()) {
        throw std::runtime_error("JavaScript runtime not initialized");
    }

    // Evaluate expression
    JSValue result = JS_Eval(ctx_, expression.c_str(), expression.length(),
                              "<eval>", JS_EVAL_TYPE_GLOBAL);

    // Check for errors
    if (JS_IsException(result)) {
        std::string error = getLastError();
        JS_FreeValue(ctx_, result);
        throw std::runtime_error(fmt::format(
            "JavaScript evaluation failed: {}", error));
    }

    // Convert result
    auto naab_result = fromJSValue(ctx_, result);
    JS_FreeValue(ctx_, result);

    return naab_result;
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

    // Number (check int first, then double)
    if (JS_IsNumber(val)) {
        int32_t i;
        if (JS_ToInt32(ctx, &i, val) == 0) {
            // Check if it's actually an integer
            double d;
            JS_ToFloat64(ctx, &d, val);
            if (d == static_cast<double>(i)) {
                return std::make_shared<interpreter::Value>(i);
            } else {
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

} // namespace runtime
} // namespace naab
