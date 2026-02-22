#pragma once

// NAAb Shell Executor
// Executes shell commands and scripts via subprocess

#include "naab/language_registry.h"
#include "naab/output_buffer.h"
#include <string>
#include <vector>
#include <memory>

namespace naab {
namespace runtime {

class ShellExecutor : public Executor {
public:
    ShellExecutor();
    ~ShellExecutor() override;

    // Executor interface implementations
    bool execute(const std::string& code) override;
    std::shared_ptr<interpreter::Value> executeWithReturn(
        const std::string& code) override;
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;
    std::string getCapturedOutput() override;
    bool isInitialized() const override { return true; } // Shell is always "initialized" if executable exists
    std::string getLanguage() const override { return "sh"; } // Default language ID for shell


private:
    OutputBuffer stdout_buffer_;
    OutputBuffer stderr_buffer_;

    // Helper to run command and capture output
    bool runCommand(const std::string& command);
};

} // namespace runtime
} // namespace naab

