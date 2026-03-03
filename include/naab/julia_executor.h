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
 * JuliaExecutor: Executes Julia code via subprocess
 *
 * Julia is a high-performance dynamic language for scientific computing.
 * JIT-compiled via LLVM for near-native performance with Python-like syntax.
 *
 * Example usage in NAAb:
 *   let result = <<julia
 *   using Statistics
 *   data = [1, 2, 3, 4, 5]
 *   mean(data)
 *   >>
 */
class JuliaExecutor : public Executor {
public:
    JuliaExecutor();
    ~JuliaExecutor() override = default;

    // Executor interface implementation
    bool execute(const std::string& code) override;
    std::shared_ptr<interpreter::Value> executeWithReturn(
        const std::string& code) override;
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    ) override;
    bool isInitialized() const override { return true; }
    std::string getLanguage() const override { return "julia"; }
    std::string getCapturedOutput() override;

private:
    OutputBuffer stdout_buffer_;
    OutputBuffer stderr_buffer_;

    // Thread-safe temp file counter for parallel execution
    static std::atomic<int> temp_file_counter_;

    /**
     * Wrap code to ensure output if needed
     *
     * @param code Raw Julia code
     * @param for_return If true, ensure last expression is printed
     * @return Wrapped Julia code
     */
    std::string wrapJuliaCode(const std::string& code, bool for_return = false);
};

} // namespace runtime
} // namespace naab
