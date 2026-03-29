#pragma once

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

// Execution mode for C++ code
enum class CppExecutionMode {
    INLINE_CODE,      // User-written inline code - wrap in main()
    BLOCK_LIBRARY     // Pre-compiled block library - compile to shared library
};

// Adapter class that wraps CppExecutor for the language registry
class CppExecutorAdapter : public Executor {
public:
    CppExecutorAdapter();
    ~CppExecutorAdapter() override = default;

    // Execute code and store in runtime context
    bool execute(const std::string& code) override;
    bool execute(const std::string& code, CppExecutionMode mode);

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
    std::string getLanguage() const override { return "cpp"; }

    // Get captured output
    std::string getCapturedOutput() override;

    // Set active block ID (for multi-block scenarios where blocks share an executor)
    void setCurrentBlockId(const std::string& block_id) { current_block_id_ = block_id; }
    const std::string& getCurrentBlockId() const { return current_block_id_; }

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

