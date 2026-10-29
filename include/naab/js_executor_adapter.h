#pragma once

// NAAb JavaScript Executor Adapter
// Adapts JsExecutor to the Executor interface for the language registry

#include "naab/language_registry.h"
#include "naab/js_executor.h"
#include <memory>
#include <string>
#include <thread>

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
    interpreter::NaabVal executeWithReturn(
        const std::string& code) override;

    // Call a function in the executor
    interpreter::NaabVal callFunction(
        const std::string& function_name,
        const std::vector<interpreter::NaabVal>& args
    ) override;

    // Check if executor is initialized
    bool isInitialized() const override;

    // Get language name
    std::string getLanguage() const override { return "javascript"; }
    // runtime_versions pins: the QuickJS release linked in (in-process).
    std::string getRuntimeVersion() const override;
    bool runsInProcess() const override { return true; }

    // Get captured output
    std::string getCapturedOutput() override;

    // Get last execution exit code
    int getLastExitCode() const override;

private:
    // A QuickJS runtime belongs to the thread that made it: it records that
    // thread's stack as its limit, so on any other thread every evaluation
    // fails its stack check -- before even the error can be formatted, which
    // printed "Unknown error" for every <<javascript>> block in a VM async fn
    // (A61) -- and it is not safe to share between threads anyway. A call
    // from another thread runs on a fresh executor made on that thread for
    // that call, as the tree-walker's parallel pool does
    // (polyglot_async_executor.cpp). executor_ serves the owner thread only.
    // The last call's exit code and error text are kept per thread
    // (js_executor_adapter.cpp), so one thread's call never reports another's.
    template <class F>
    auto onThisThread(F&& f);

    JsExecutor executor_;
    std::thread::id owner_ = std::this_thread::get_id();
};

} // namespace runtime
} // namespace naab

