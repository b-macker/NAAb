#ifndef NAAB_CPP_EXECUTOR_ADAPTER_H
#define NAAB_CPP_EXECUTOR_ADAPTER_H

// NAAb C++ Executor Adapter
// Adapts CppExecutor to the Executor interface for the language registry

#include "naab/language_registry.h"
#include "naab/cpp_executor.h"
#include "naab/inline_code_cache.h"  // Phase 3.3.1
#include <atomic>
#include <memory>
#include <string>

namespace naab {
namespace runtime {

// Adapter class that wraps CppExecutor for the language registry
class CppExecutorAdapter : public Executor {
public:
    CppExecutorAdapter();
    ~CppExecutorAdapter() override = default;

    // Execute code and store in runtime context
    bool execute(const std::string& code) override;

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
    std::string getLanguage() const override { return "cpp"; }

    // Get captured output
    std::string getCapturedOutput() override;

private:
    CppExecutor executor_;
    std::string current_block_id_;
    int block_counter_;
    std::string captured_output_;  // For inline main() execution
    InlineCodeCache cache_;  // Phase 3.3.1: Content-based caching

    // Thread-safe temp file counter for parallel execution
    static std::atomic<int> temp_file_counter_;
};

} // namespace runtime
} // namespace naab

#endif // NAAB_CPP_EXECUTOR_ADAPTER_H
