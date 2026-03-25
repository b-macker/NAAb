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

class GoExecutor : public Executor {
public:
    GoExecutor();
    ~GoExecutor() override = default;

    bool execute(const std::string& code) override;
    interpreter::NaabVal executeWithReturn(
        const std::string& code) override;
    interpreter::NaabVal callFunction(
        const std::string& function_name,
        const std::vector<interpreter::NaabVal>& args
    ) override;
    bool isInitialized() const override { return true; }
    std::string getLanguage() const override { return "go"; }
    std::string getCapturedOutput() override;

private:
    OutputBuffer stdout_buffer_;
    OutputBuffer stderr_buffer_;

    // Thread-safe temp file counter for parallel execution
    static std::atomic<int> temp_file_counter_;

    // Wrap code in package main if needed
    std::string wrapGoCode(const std::string& code, bool for_return = false);
};

} // namespace runtime
} // namespace naab
