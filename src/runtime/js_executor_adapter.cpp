// NAAb JavaScript Executor Adapter Implementation
// Adapts JsExecutor to the Executor interface

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/js_executor_adapter.h"
#include <fmt/core.h>
#include <iostream>

namespace naab {
namespace runtime {

namespace {
// The last executeWithReturn() on this thread: which adapter ran it, and how
// it ended. Read back by getCapturedOutput()/getLastExitCode() on the same
// thread, right after the call. Members would be shared by every thread
// calling the one registered adapter.
struct LastCall {
    const JsExecutorAdapter* adapter = nullptr;
    int exit_code = 0;
    std::string error_text;
};
thread_local LastCall t_last_call;
}  // namespace

JsExecutorAdapter::JsExecutorAdapter() {
    // JavaScript executor adapter initialized (silent)
}

template <class F>
auto JsExecutorAdapter::onThisThread(F&& f) {
    if (std::this_thread::get_id() == owner_) return f(executor_);
    JsExecutor fresh;
    return f(fresh);
}

bool JsExecutorAdapter::execute(const std::string& code) {
    // Executing JavaScript code (silent)
    return onThisThread([&](JsExecutor& js) { return js.execute(code); });
}

bool JsExecutorAdapter::execute(const std::string& code, JsExecutionMode mode) {
    // Executing JavaScript code with mode (silent)
    return onThisThread([&](JsExecutor& js) { return js.execute(code, mode); });
}

// Phase 2.3: Execute code and return result value
interpreter::NaabVal JsExecutorAdapter::executeWithReturn(
    const std::string& code) {
    // Executing JavaScript code with return (silent)
    // Reset exit code and stderr at start of each call
    t_last_call = LastCall{this, 0, ""};

    try {
        return onThisThread([&](JsExecutor& js) { return js.evaluate(code); });
    } catch (const std::exception& e) {
        std::string error_msg = e.what();
        fmt::print("[JS Error] {}\n", error_msg);

        // Set exit code to non-zero on error
        t_last_call.exit_code = 1;
        t_last_call.error_text = error_msg;

        // Helper hints for common JS-in-polyglot errors
        if (error_msg.find("is not defined") != std::string::npos) {
            fmt::print("\n  Hint: JavaScript variable not defined.\n"
                       "  - Bound variables from NAAb use <<javascript[var1, var2] syntax\n"
                       "  - Check spelling and that the variable is listed in the binding list\n"
                       "  - Built-in objects like JSON, Math, Array are always available\n\n");
        } else if (error_msg.find("SyntaxError") != std::string::npos) {
            fmt::print("\n  Hint: JavaScript syntax error in polyglot block.\n"
                       "  - NAAb wraps multi-line JS in an IIFE for return value capture\n"
                       "  - Avoid trailing comments (// ...) on the last expression line\n"
                       "  - For complex JS, consider using an external .js file via <<sh: node script.js\n\n");
        } else if (error_msg.find("is not a function") != std::string::npos) {
            fmt::print("\n  Hint: Value is not callable.\n"
                       "  - Check that bound variables have the expected type\n"
                       "  - NAAb arrays become JS arrays, dicts become objects\n"
                       "  - NAAb strings are passed as JS strings\n\n");
        }

        throw std::runtime_error(std::string("JavaScript execution error: ") + error_msg);
    }
}

interpreter::NaabVal JsExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<interpreter::NaabVal>& args) {

    // Calling function (silent)
    return onThisThread([&](JsExecutor& js) { return js.callFunction(function_name, args); });
}

bool JsExecutorAdapter::isInitialized() const {
    return executor_.isInitialized();
}

std::string JsExecutorAdapter::getCapturedOutput() {
    // Return captured error output from this thread's last execution
    return t_last_call.adapter == this ? t_last_call.error_text : std::string();
}

int JsExecutorAdapter::getLastExitCode() const {
    // Return this thread's last exit code (0 for success, 1 for error)
    return t_last_call.adapter == this ? t_last_call.exit_code : 0;
}

std::string JsExecutorAdapter::getRuntimeVersion() const {
#ifdef NAAB_QUICKJS_VERSION
    return std::string("QuickJS ") + NAAB_QUICKJS_VERSION;
#else
    return "";
#endif
}

} // namespace runtime
} // namespace naab
