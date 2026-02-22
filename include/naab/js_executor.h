#pragma once

// NAAb JavaScript Block Executor
// Executes JavaScript blocks using QuickJS engine

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// Forward declare QuickJS types (opaque pointers only)
struct JSRuntime;
struct JSContext;

namespace naab {
namespace interpreter {
    class Value;  // Forward declaration
}

namespace runtime {

// Execution mode for JavaScript code
enum class JsExecutionMode {
    INLINE_CODE,      // User-written inline code - wrap in IIFE for isolation
    BLOCK_LIBRARY     // Pre-compiled block library - no wrapping, define in global scope
};

// JavaScript Block Executor - executes JavaScript blocks using QuickJS
class JsExecutor {
public:
    JsExecutor();
    ~JsExecutor();

    // Execute JavaScript code and store in runtime context
    bool execute(const std::string& code);

    // Execute JavaScript code with specified execution mode
    bool execute(const std::string& code, JsExecutionMode mode);

    // Call a JavaScript function
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    );

    // Evaluate JavaScript expression and return result
    std::shared_ptr<interpreter::Value> evaluate(const std::string& expression);

    // Check if runtime is initialized
    bool isInitialized() const { return rt_ != nullptr && ctx_ != nullptr; }

private:
    JSRuntime* rt_;
    JSContext* ctx_;
    bool timeout_triggered_;  // Flag for interrupt handler

    // Internal helper methods (implemented in .cpp, use QuickJS types internally)
    // Note: Type conversion uses JSValue internally - not exposed in header
    std::string getLastError();
    static int interruptHandler(JSRuntime* rt, void* opaque);

    // Phase 4.2.3: JavaScript traceback extraction
    void extractJavaScriptStackTrace();
};

} // namespace runtime
} // namespace naab

