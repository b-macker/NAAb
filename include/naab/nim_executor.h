#pragma once

#include "naab/language_registry.h"
#include "naab/output_buffer.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace naab {
namespace interpreter {
    class Value;
}

namespace runtime {

/**
 * NimExecutor: Executes Nim blocks via subprocess compilation
 *
 * Compiles Nim code to native binaries and executes them.
 * Similar to Go/Rust executors but for Nim language.
 *
 * Example usage in NAAb:
 *   let result = <<nim
 *   proc greet(name: string): string =
 *     "Hello, " & name & "!"
 *   echo greet("World")
 *   >>
 */
class NimExecutor : public Executor {
public:
    NimExecutor();
    ~NimExecutor() override = default;

    // Executor interface implementation
    bool execute(const std::string& code) override;
    std::shared_ptr<interpreter::Value> executeWithReturn(
        const std::string& code) override;
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    ) override;
    bool isInitialized() const override { return true; }
    std::string getLanguage() const override { return "nim"; }
    std::string getCapturedOutput() override;

private:
    OutputBuffer stdout_buffer_;
    OutputBuffer stderr_buffer_;

    // Thread-safe temp file counter for parallel execution
    static std::atomic<int> temp_file_counter_;

    /**
     * Wrap code in proper Nim structure if needed
     *
     * @param code Raw Nim code
     * @param for_return If true, wrap last expression with echo for return value
     * @return Wrapped Nim code ready for compilation
     */
    std::string wrapNimCode(const std::string& code, bool for_return = false);
};

} // namespace runtime
} // namespace naab
