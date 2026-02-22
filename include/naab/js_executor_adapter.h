#pragma once

// NAAb JavaScript Executor Adapter
// Adapts JsExecutor to the Executor interface for the language registry

#include "naab/language_registry.h"
#include "naab/js_executor.h"
#include <memory>
#include <string>

namespace naab {
namespace runtime {

// Adapter class that wraps JsExecutor for the language registry
class JsExecutorAdapter : public Executor {
public:
    JsExecutorAdapter();
    ~JsExecutorAdapter() override = default;

    // Execute code and store in runtime context
    bool execute(const std::string& code) override;

    // Execute code with specified execution mode (for block library support)
    bool execute(const std::string& code, JsExecutionMode mode);

    // Phase 2.3: Execute code and return the result value
    std::shared_ptr<interpreter::Value> executeWithReturn(
        const std::string& code) override;

    // Call a function in the executor
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    ) override;

    // Check if executor is initialized
    bool isInitialized() const override;

    // Get language name
    std::string getLanguage() const override { return "javascript"; }

    // Get captured output
    std::string getCapturedOutput() override;

private:
    JsExecutor executor_;
};

} // namespace runtime
} // namespace naab

