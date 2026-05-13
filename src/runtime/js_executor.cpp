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
#include <cstring>  // For strlen

// Include QuickJS headers
extern "C" {
#include "quickjs.h"
#include "quickjs-libc.h"
}

namespace naab {
namespace runtime {

// Forward declarations of static helper functions
// V-RT-006: toJSValue takes a depth parameter (default 0) to enforce a 64-level limit.
static JSValue toJSValue(JSContext* ctx, const interpreter::NaabVal& val, int depth = 0);
static interpreter::NaabVal fromJSValue(JSContext* ctx, JSValue val);

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

interpreter::NaabVal JsExecutor::callFunction(
    const std::string& function_name,
    const std::vector<interpreter::NaabVal>& args) {

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

interpreter::NaabVal JsExecutor::evaluate(
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

    // Enterprise Security: Apply timeout before executing JavaScript code
    // NOTE: Process-wide RLIMIT_AS is NOT set here because it persists after
    // evaluation and breaks fork/exec for subsequent shell blocks.
    // Use JS_SetMemoryLimit() on the QuickJS runtime instead for JS-specific limits.
    if (rt_ && memory_limit > 0) {
        JS_SetMemoryLimit(rt_, memory_limit * 1024 * 1024);
    }
    security::ScopedTimeout scoped_timeout(timeout);

    // Phase 2.3: Fixed multi-line code handling
    // Strategy: Just evaluate the code directly - QuickJS eval returns the last expression value

    // Set up console.log capture so print output becomes the return value
    const char* capture_setup =
        "if (typeof __naab_captured === 'undefined') {"
        "  var __naab_captured = [];"
        "  var console = { log: function() {"
        "    __naab_captured.push(Array.prototype.slice.call(arguments).join(' '));"
        "  }, warn: function() {"
        "    __naab_captured.push(Array.prototype.slice.call(arguments).join(' '));"
        "  }, error: function() {"
        "    __naab_captured.push(Array.prototype.slice.call(arguments).join(' '));"
        "  }};"
        "} else {"
        "  __naab_captured = [];"
        "}";
    JSValue setup_result = JS_Eval(ctx_, capture_setup, strlen(capture_setup),
                                    "<capture-setup>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx_, setup_result);

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
            // Simple expression: use single-line wrapping (just parens, no IIFE)
            // This avoids any function call overhead that might be causing issues
            wrapped = "(" + code + ")";
        } else {
            // Complex code with statements: wrap in IIFE with explicit return
            // Split into lines, use all but last as setup, return last expression
            // This avoids eval() + template literal issues in QuickJS
            std::vector<std::string> js_lines;
            std::istringstream js_stream(code);
            std::string js_line;
            while (std::getline(js_stream, js_line)) {
                js_lines.push_back(js_line);
            }

            // Remove trailing empty lines
            while (!js_lines.empty()) {
                auto& back = js_lines.back();
                size_t first_char = back.find_first_not_of(" \t\r\n");
                if (first_char == std::string::npos) {
                    js_lines.pop_back();
                } else {
                    break;
                }
            }

            // Returns true if the trimmed string starts with a JS statement keyword
            // that cannot be used as a return expression in the IIFE wrapper.
            auto startsWithStmtKeyword = [](const std::string& s) -> bool {
                static const std::vector<std::string> kws = {
                    "for ", "for(", "while ", "while(", "do ", "do{",
                    "switch ", "switch(", "if ", "if(",
                    "return ", "throw ", "break", "continue",
                    "try ", "try{"
                };
                size_t start = s.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) return false;
                std::string trimmed = s.substr(start);
                for (const auto& kw : kws) {
                    if (trimmed.size() >= kw.size() &&
                        trimmed.compare(0, kw.size(), kw) == 0) return true;
                }
                return false;
            };

            if (js_lines.empty()) {
                wrapped = "undefined";
            } else {
                // Determine if the last expression is multi-line by checking
                // the last non-whitespace/non-semicolon char of the entire code.
                // If it's a closing bracket (}, ), ]), the final statement spans
                // multiple lines and taking only the last line would break it.
                // Example: JSON.stringify({\n  key: val\n}); — last line is "});"
                // which becomes "return (});" — a syntax error.
                char last_significant = 0;
                for (size_t ci = code.size(); ci > 0; ci--) {
                    char c = code[ci - 1];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ';') {
                        last_significant = c;
                        break;
                    }
                }

                bool is_multiline_last = (last_significant == '}' ||
                                          last_significant == ')' ||
                                          last_significant == ']');

                if (is_multiline_last) {
                    // The last expression spans multiple lines — use full code block
                    // as the return expression instead of just the last line.
                    // Scan backwards to find where the last top-level statement begins
                    // by tracking brace/bracket/paren depth from the end.
                    std::string full_code;
                    for (const auto& l : js_lines) {
                        full_code += l + "\n";
                    }
                    // Strip trailing whitespace and semicolon
                    size_t lp = full_code.find_last_not_of(" \t\r\n");
                    if (lp != std::string::npos) full_code = full_code.substr(0, lp + 1);
                    if (!full_code.empty() && full_code.back() == ';') full_code.pop_back();

                    // Find where the last top-level statement starts by scanning backwards
                    // with brace depth tracking. When depth returns to 0 and we hit a
                    // semicolon or newline at depth 0, that's the boundary.
                    int depth = 0;
                    size_t stmt_start = 0;
                    bool in_str = false;
                    char str_ch = 0;
                    for (size_t ci = full_code.size(); ci > 0; ci--) {
                        char c = full_code[ci - 1];
                        // Simple string tracking (backwards)
                        if (in_str) {
                            if (c == str_ch && (ci < 2 || full_code[ci - 2] != '\\')) {
                                in_str = false;
                            }
                            continue;
                        }
                        if (c == '"' || c == '\'') {
                            in_str = true;
                            str_ch = c;
                            continue;
                        }
                        if (c == '}' || c == ')' || c == ']') depth++;
                        else if (c == '{' || c == '(' || c == '[') depth--;

                        if (depth == 0 && c == ';') {
                            stmt_start = ci; // position after the semicolon
                            break;
                        }
                    }

                    if (stmt_start > 0) {
                        // Split into statements + last expression
                        std::string stmts_part = full_code.substr(0, stmt_start);
                        std::string expr_part = full_code.substr(stmt_start);
                        // Trim leading whitespace from expr_part
                        size_t fp = expr_part.find_first_not_of(" \t\r\n");
                        if (fp != std::string::npos) expr_part = expr_part.substr(fp);
                        // Strip trailing semicolon from expr_part
                        size_t ep = expr_part.find_last_not_of(" \t\r\n");
                        if (ep != std::string::npos) expr_part = expr_part.substr(0, ep + 1);
                        if (!expr_part.empty() && expr_part.back() == ';') expr_part.pop_back();

                        // If expr_part is a statement (for/while/etc), it can't be returned
                        if (startsWithStmtKeyword(expr_part)) {
                            stmts_part += "\n" + expr_part;
                            expr_part = "undefined";
                        }
                        wrapped = "(function() {\n" + stmts_part + "\nreturn (" + expr_part + ");\n})()";
                    } else {
                        // No semicolon found — entire block is one expression
                        // Check if it's actually a statement block
                        if (startsWithStmtKeyword(full_code)) {
                            wrapped = "(function() {\n" + full_code + "\nreturn (undefined);\n})()";
                        } else {
                            wrapped = "(function() {\nreturn (" + full_code + ");\n})()";
                        }
                    }
                } else {
                    // Last line is a simple expression — use existing last-line logic
                    std::string statements;
                    for (size_t i = 0; i + 1 < js_lines.size(); i++) {
                        statements += js_lines[i] + "\n";
                    }

                    // Trim last line for return
                    std::string last_expr = js_lines.back();
                    size_t first_pos = last_expr.find_first_not_of(" \t");
                    if (first_pos != std::string::npos) {
                        last_expr = last_expr.substr(first_pos);
                    }
                    // Strip line comments (// ...) before processing
                    // Be careful not to strip // inside string literals
                    bool in_str = false;
                    char str_ch = 0;
                    for (size_t ci = 0; ci + 1 < last_expr.size(); ci++) {
                        if (!in_str && (last_expr[ci] == '"' || last_expr[ci] == '\'')) {
                            in_str = true;
                            str_ch = last_expr[ci];
                        } else if (in_str && last_expr[ci] == str_ch && (ci == 0 || last_expr[ci-1] != '\\')) {
                            in_str = false;
                        } else if (!in_str && last_expr[ci] == '/' && last_expr[ci+1] == '/') {
                            last_expr = last_expr.substr(0, ci);
                            break;
                        }
                    }
                    // Remove trailing whitespace and semicolon
                    size_t last_pos = last_expr.find_last_not_of(" \t\r\n");
                    if (last_pos != std::string::npos) {
                        last_expr = last_expr.substr(0, last_pos + 1);
                    }
                    if (!last_expr.empty() && last_expr.back() == ';') {
                        last_expr.pop_back();
                    }

                    // If last_expr is a statement, move it to statements and return undefined
                    if (startsWithStmtKeyword(last_expr)) {
                        statements += last_expr + "\n";
                        last_expr = "undefined";
                    }
                    wrapped = "(function() {\n" + statements + "return (" + last_expr + ");\n})()";
                }
            }
        }

        JSValue result = JS_Eval(ctx_, wrapped.c_str(), wrapped.length(),
                                  "<eval>", JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(result)) {
            std::string error = getLastError();
            JS_FreeValue(ctx_, result);

            // Add code preview for SyntaxErrors to help debugging
            std::string hint;
            if (error.find("SyntaxError") != std::string::npos) {
                // Show first few lines of generated code
                std::string preview = wrapped.substr(0, 500);
                hint = fmt::format("\n\n  Generated JS code preview:\n    {}", preview);
                // Replace newlines with indented newlines for readability
                size_t pos = 0;
                while ((pos = hint.find('\n', pos + 1)) != std::string::npos) {
                    if (pos + 1 < hint.size() && hint[pos + 1] != ' ') {
                        hint.insert(pos + 1, "    ");
                    }
                }

                if (error.find("expecting ')'") != std::string::npos ||
                    error.find("expecting '}'") != std::string::npos ||
                    error.find("unexpected token") != std::string::npos) {
                    hint += "\n\n  Hint: JavaScript syntax error in polyglot block.\n"
                            "  - NAAb wraps multi-line JS in an IIFE for return value capture\n"
                            "  - If your last expression spans multiple lines (e.g. JSON.stringify({\\n"
                            "    ...\\n})), assign it to a variable and put the variable name on the\n"
                            "    last line:\n"
                            "      const result = JSON.stringify({ winner: x, margin: y });\n"
                            "      result\n"
                            "  - If your last line is a for/while/if statement (not an expression),\n"
                            "    put a result variable on the last line instead:\n"
                            "      let out = []; for (const x of items) { out.push(x); }\n"
                            "      out\n";
                }
            }

            throw std::runtime_error(fmt::format(
                "JavaScript evaluation failed: {}{}", error, hint));
        }

        auto naab_result = fromJSValue(ctx_, result);
        JS_FreeValue(ctx_, result);

        // If result is null/undefined, check for captured console.log output
        if (naab_result.isNull()) {
            const char* get_captured = "__naab_captured.join('\\n')";
            JSValue cap = JS_Eval(ctx_, get_captured, strlen(get_captured), "<capture>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsString(cap)) {
                const char* s = JS_ToCString(ctx_, cap);
                if (s && strlen(s) > 0) {
                    auto captured_result = interpreter::NaabVal::makeString(std::string(s));
                    JS_FreeCString(ctx_, s);
                    JS_FreeValue(ctx_, cap);
                    return captured_result;
                }
                if (s) JS_FreeCString(ctx_, s);
            }
            JS_FreeValue(ctx_, cap);
        }

        return naab_result;
    } else {
        // Single-line code: strip trailing semicolons, then evaluate
        std::string expr = code;
        // Remove trailing semicolons (they cause SyntaxError inside parens)
        while (!expr.empty() && expr.back() == ';') {
            expr.pop_back();
        }
        // Trim trailing whitespace
        size_t trail = expr.find_last_not_of(" \t\r");
        if (trail != std::string::npos) {
            expr = expr.substr(0, trail + 1);
        }

        // For statement-like calls (console.log, etc.), don't wrap in parens
        // Just eval directly - QuickJS returns the expression value
        std::string wrapped_expr;
        if (expr.find("console.") == 0 || expr.find("print(") == 0 ||
            expr.find("alert(") == 0) {
            wrapped_expr = expr;
        } else {
            wrapped_expr = "(" + expr + ")";
        }

        JSValue result = JS_Eval(ctx_, wrapped_expr.c_str(), wrapped_expr.length(),
                                  "<eval>", JS_EVAL_TYPE_GLOBAL);

        if (JS_IsException(result)) {
            std::string error = getLastError();
            JS_FreeValue(ctx_, result);

            // Add code preview for SyntaxErrors
            std::string hint;
            if (error.find("SyntaxError") != std::string::npos) {
                hint = fmt::format("\n\n  Generated JS code: {}", wrapped_expr.substr(0, 300));
            }

            throw std::runtime_error(fmt::format(
                "JavaScript evaluation failed: {}{}", error, hint));
        }

        auto naab_result = fromJSValue(ctx_, result);
        JS_FreeValue(ctx_, result);

        // If result is null/undefined, check for captured console.log output
        if (naab_result.isNull()) {
            const char* get_captured = "__naab_captured.join('\\n')";
            JSValue cap = JS_Eval(ctx_, get_captured, strlen(get_captured), "<capture>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsString(cap)) {
                const char* s = JS_ToCString(ctx_, cap);
                if (s && strlen(s) > 0) {
                    auto captured_result = interpreter::NaabVal::makeString(std::string(s));
                    JS_FreeCString(ctx_, s);
                    JS_FreeValue(ctx_, cap);
                    return captured_result;
                }
                if (s) JS_FreeCString(ctx_, s);
            }
            JS_FreeValue(ctx_, cap);
        }

        return naab_result;
    }
}

// Interrupt handler for QuickJS timeout support
int JsExecutor::interruptHandler(JSRuntime* rt, void* opaque) {
    (void)rt;  // Unused
    JsExecutor* executor = static_cast<JsExecutor*>(opaque);

    // V-RT-002 fix: also respect the process-level ResourceLimiter timeout so that
    // --timeout N / ScopedTimeout correctly interrupts JavaScript blocks.
    // executor->timeout_triggered_ is set by the internal 30s watchdog thread;
    // ResourceLimiter::isTimeoutTriggered() reflects the governance/CLI --timeout value.
    return (executor->timeout_triggered_ ||
            naab::security::ResourceLimiter::isTimeoutTriggered()) ? 1 : 0;
}

// Static helper: Convert NaabVal to JSValue
// V-RT-006: depth parameter prevents stack overflow on deeply-nested structures.
static JSValue toJSValue(JSContext* ctx, const interpreter::NaabVal& val, int depth) {
    if (depth > 64) {
        JS_ThrowRangeError(ctx,
            "NAAb marshalling error: nested structure exceeds maximum depth (64). "
            "Flatten the structure before passing it to a JS block.");
        return JS_EXCEPTION;
    }
    if (val.isNull()) {
        return JS_NULL;
    }

    if (val.isInt()) {
        return JS_NewInt32(ctx, val.asInt());
    } else if (val.isDouble()) {
        return JS_NewFloat64(ctx, val.asDouble());
    } else if (val.isBool()) {
        return JS_NewBool(ctx, val.asBool());
    } else if (val.isString()) {
        return JS_NewString(ctx, val.asString().c_str());
    } else if (val.isList()) {
        const auto& vec = val.asListConst();
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < vec.size(); i++) {
            JSValue elem = toJSValue(ctx, vec[i], depth + 1);
            if (JS_IsException(elem)) { JS_FreeValue(ctx, arr); return JS_EXCEPTION; }
            JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), elem);
        }
        return arr;
    } else if (val.isDict()) {
        const auto& map = val.asDictConst();
        JSValue obj = JS_NewObject(ctx);
        for (const auto& [key, value] : map) {
            JSValue prop_val = toJSValue(ctx, value, depth + 1);
            if (JS_IsException(prop_val)) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
            JS_SetPropertyStr(ctx, obj, key.c_str(), prop_val);
        }
        return obj;
    } else {
        fmt::print("[WARN] Unsupported type for JavaScript conversion\n");
        return JS_UNDEFINED;
    }
}

// Static helper: Convert JSValue to NaabVal
static interpreter::NaabVal fromJSValue(JSContext* ctx, JSValue val) {
    // Null or undefined
    if (JS_IsNull(val) || JS_IsUndefined(val)) {
        return interpreter::NaabVal::makeNull();
    }

    // Boolean
    if (JS_IsBool(val)) {
        int32_t b = JS_ToBool(ctx, val);
        return interpreter::NaabVal::makeBool(b != 0);
    }

    // Number (always get as double first, then check if it fits in int)
    if (JS_IsNumber(val)) {
        double d;
        if (JS_ToFloat64(ctx, &d, val) == 0) {
            if (d >= INT32_MIN && d <= INT32_MAX && d == static_cast<int32_t>(d)) {
                return interpreter::NaabVal::makeInt(static_cast<int>(d));
            } else {
                return interpreter::NaabVal::makeDouble(d);
            }
        }
    }

    // String
    if (JS_IsString(val)) {
        const char* str = JS_ToCString(ctx, val);
        if (str) {
            auto result = interpreter::NaabVal::makeString(std::string(str));
            JS_FreeCString(ctx, str);
            return result;
        }
    }

    // Array
    if (JS_IsArray(ctx, val)) {
        JSValue length_val = JS_GetPropertyStr(ctx, val, "length");

        if (JS_IsException(length_val)) {
            JS_FreeValue(ctx, length_val);
            return interpreter::NaabVal::makeNull();
        }

        uint32_t length = 0;
        if (JS_IsNumber(length_val)) {
            JS_ToUint32(ctx, &length, length_val);
        }
        JS_FreeValue(ctx, length_val);

        std::vector<interpreter::NaabVal> naab_array;
        for (uint32_t i = 0; i < length; i++) {
            JSValue elem = JS_GetPropertyUint32(ctx, val, i);

            if (JS_IsException(elem)) {
                JS_FreeValue(ctx, elem);
                naab_array.push_back(interpreter::NaabVal::makeNull());
                continue;
            }

            naab_array.push_back(fromJSValue(ctx, elem));
            JS_FreeValue(ctx, elem);
        }

        return interpreter::NaabVal::makeList(std::move(naab_array));
    }

    // Object (but not array)
    if (JS_IsObject(val)) {
        std::unordered_map<std::string, interpreter::NaabVal> naab_dict;

        JSPropertyEnum* props = nullptr;
        uint32_t prop_count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            return interpreter::NaabVal::makeNull();
        }

        for (uint32_t i = 0; i < prop_count; i++) {
            JSAtom atom = props[i].atom;
            const char* key = JS_AtomToCString(ctx, atom);
            if (!key) {
                continue;
            }

            JSValue prop_val = JS_GetProperty(ctx, val, atom);
            if (JS_IsException(prop_val)) {
                JS_FreeValue(ctx, prop_val);
                JS_FreeCString(ctx, key);
                continue;
            }

            naab_dict[std::string(key)] = fromJSValue(ctx, prop_val);
            JS_FreeValue(ctx, prop_val);
            JS_FreeCString(ctx, key);
        }

        for (uint32_t i = 0; i < prop_count; i++) {
            JS_FreeAtom(ctx, props[i].atom);
        }
        js_free(ctx, props);

        return interpreter::NaabVal::makeDict(std::move(naab_dict));
    }

    // Unsupported type - return null
    return interpreter::NaabVal::makeNull();
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
