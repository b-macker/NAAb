#include "naab/generic_subprocess_executor.h"
#include "naab/interpreter.h" // For Value definition
#include "naab/subprocess_helpers.h" // For execute_subprocess_with_pipes
#include "naab/temp_file_guard.h"   // For TempFileGuard
#include "naab/sandbox.h" // For security sandbox
#include "naab/audit_logger.h" // For security audit logging
#include <cstdio>       // For popen, pclose
#include <array>        // For std::array
#include <stdexcept>    // For std::runtime_error
#include <fmt/core.h>   // For fmt::print
#include <fstream>      // For temporary file I/O
#include <string_view>  // For string_view
#include <filesystem>   // For std::filesystem::path and temp_directory_path

namespace naab {
namespace runtime {

// Helper to unescape string literals (e.g. convert "\\n" to actual newline)
std::string unescape_string_literal(const std::string& s) {
    std::string unescaped;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\\' && i + 1 < s.length()) {
            switch (s[i+1]) {
                case 'n': unescaped += '\n'; ++i; break;
                case 't': unescaped += '\t'; ++i; break;
                case 'r': unescaped += '\r'; ++i; break;
                case '\\': unescaped += '\\'; ++i; break;
                case '"': unescaped += '"'; ++i; break;
                // Add other escape sequences as needed
                default: unescaped += s[i]; // Not a recognized escape, keep '\'
            }
        } else {
            unescaped += s[i];
        }
    }
    return unescaped;
}
// Helper to replace {} in command template with actual value
std::string format_command(const std::string& command_template, const std::string& value) {
    std::string command = command_template;
    // Replace ALL occurrences of {} with the value
    size_t pos = 0;
    while ((pos = command.find("{}", pos)) != std::string::npos) {
        command.replace(pos, 2, value);
        pos += value.length();
    }
    return command;
}

GenericSubprocessExecutor::GenericSubprocessExecutor(
    std::string language_id, std::string command_template, std::string file_extension)
    : language_id_(std::move(language_id)),
      command_template_(std::move(command_template)),
      file_extension_(std::move(file_extension)) {
    fmt::print("[GenericSubprocessExecutor] Initialized for language \'{}\' with template \'{}\'\n",
               language_id_, command_template_);
}

GenericSubprocessExecutor::~GenericSubprocessExecutor() {
    // Cleanup any lingering temp files if necessary (though unlikely with proper usage)
}

bool naab::runtime::GenericSubprocessExecutor::execute(const std::string& code) {
    // Check sandbox permissions for command execution
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: {} execution denied\n", language_id_);
        sandbox->logViolation("execute" + language_id_, code, "BLOCK_CALL capability required");
        throw std::runtime_error(fmt::format("{} execution denied by sandbox", language_id_));
    }

    // Don't unescape for inline polyglot code - preserve raw text with escape sequences
    std::string processed_code = code;

    if (file_extension_.empty()) {
        // Run directly from code string (e.g., "ruby -e '{}'")
        std::string command_line = format_command(command_template_, processed_code);
        return runCommand(command_line);
    } else {
        // Write to temp file, then run (e.g., "go run {}")

        // Generate a unique temporary file path
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        std::filesystem::path temp_file_path = temp_dir / ("naab_temp_" + language_id_ + file_extension_);

        int counter = 0;
        while (std::filesystem::exists(temp_file_path)) {
            temp_file_path = temp_dir / (
                "naab_temp_" + language_id_ + "_" + std::to_string(counter++) + file_extension_
            );
            if (counter > 1000) { // Safety break
                fmt::print("[ERROR] Failed to find unique temp file name for {} execution\n", language_id_);
                return false;
            }
        }

        // Create TempFileGuard, which ensures deletion on scope exit
        TempFileGuard temp_file_guard(temp_file_path);

        // Write code to temp file
        std::ofstream ofs(temp_file_path);
        if (!ofs.is_open()) {
            fmt::print("[ERROR] Failed to create temp file for {} execution: '{}'\n", language_id_, temp_file_path.string());
            return false;
        }
        ofs << processed_code;
        ofs.close();

        fmt::print("[GenericSubprocessExecutor-{}] Created temp file: '{}'\n", language_id_, temp_file_path.string());

        std::string command_line = format_command(command_template_, temp_file_path.string());
        bool success = runCommand(command_line);

        // temp_file_guard's destructor will handle deletion
        return success;
    }
}

std::shared_ptr<interpreter::Value> GenericSubprocessExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // This executor primarily supports the "exec" function for running code strings.
    // If other function names are needed, they would map to specific CLI calls.
    if (function_name == "exec" && !args.empty()) {
        if (auto* str_val = std::get_if<std::string>(&args[0]->data)) {
            bool success = execute(*str_val);
            // Return boolean result of execution success
            return std::make_shared<naab::interpreter::Value>(success);
        }
        // If argument is not a string, try converting to string (e.g. integer)
        if (auto* int_val = std::get_if<int>(&args[0]->data)) {
            bool success = execute(std::to_string(*int_val));
            return std::make_shared<naab::interpreter::Value>(success);
        }
        // Fallback for unsupported argument types
        throw std::runtime_error(fmt::format(
            "GenericSubprocessExecutor for {} only supports 'exec(string_code)' or 'exec(number)'",
            language_id_));
    }

    throw std::runtime_error(fmt::format(
        "GenericSubprocessExecutor for {} only supports 'exec(code_string)'",
        language_id_));
}

std::string naab::runtime::GenericSubprocessExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string error_output = stderr_buffer_.getAndClear();
    if (!error_output.empty()) {
        output += "\n[" + language_id_ + " stderr]: " + error_output;
    }
    return output;
}

bool naab::runtime::GenericSubprocessExecutor::runCommand(const std::string& command_line) {
    fmt::print("[GenericSubprocessExecutor-{}] Executing: \'{}\'\n", language_id_, command_line);

    // Parse the command string into command and arguments
    std::istringstream iss(command_line);
    std::string cmd_path;
    iss >> cmd_path;
    std::vector<std::string> args;
    std::string arg;
    while (iss >> arg) {
        args.push_back(arg);
    }

    std::string stdout_buffer_local;
    std::string stderr_buffer_local;

    int exit_code = execute_subprocess_with_pipes(
        cmd_path, args, stdout_buffer_local, stderr_buffer_local, nullptr // Explicitly pass nullptr for env
    );

    // Append to internal buffers
    stdout_buffer_.append(stdout_buffer_local);
    stderr_buffer_.append(stderr_buffer_local);

    bool success = (exit_code == 0);

    if (success) {
        fmt::print("[SUCCESS] GenericSubprocessExecutor-{} command executed (exit code {})\n", language_id_, exit_code);
    } else {
        fmt::print("[ERROR] GenericSubprocessExecutor-{} command failed with code {} (captured stderr: \'{}\')\n", language_id_, exit_code, stderr_buffer_local);
    }
    return success;
}

} // namespace runtime
} // namespace naab
