#pragma once

// NAAb Generic Subprocess Executor
// Executes code for various languages by invoking their command-line tools
// Supports languages that can be run from a file or directly from a string (via -e or similar)

#include "naab/language_registry.h" // For Executor base class
#include "naab/output_buffer.h"
#include <string>
#include <vector>
#include <memory>
#include <filesystem> // For temp file management

namespace naab {
namespace runtime {

class GenericSubprocessExecutor : public Executor {
public:
    // command_template: e.g., "ruby -e {}", "go run {}", "python {}"
    // file_extension: e.g., ".rb", ".go", ".py"
    // language_id: e.g., "ruby", "go", "rust"
    GenericSubprocessExecutor(std::string language_id, std::string command_template, std::string file_extension = "");
    ~GenericSubprocessExecutor() override;

    // Executor interface implementations
    bool execute(const std::string& code) override;
    std::shared_ptr<interpreter::Value> executeWithReturn(
        const std::string& code) override;
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;

    std::string getCapturedOutput() override;
    bool isInitialized() const override { return true; } // Assumed always ready if binary exists
    std::string getLanguage() const override { return language_id_; }

private:
    std::string language_id_;
    std::string command_template_; // Template for execution command
    std::string file_extension_;   // File extension for temp files, if needed

    OutputBuffer stdout_buffer_;
    OutputBuffer stderr_buffer_;

    // Helper to run command and capture output
    bool runCommand(const std::string& command_line);
    
    // Helper to create and clean up temporary files
    std::filesystem::path createTempFile(const std::string& code) const;
    void deleteTempFile(const std::filesystem::path& path) const;
};

} // namespace runtime
} // namespace naab

