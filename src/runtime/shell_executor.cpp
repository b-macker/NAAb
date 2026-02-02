// NAAb Shell Executor Implementation
// Executes shell commands using fork/execvp for separate stdout/stderr capture

#include "naab/shell_executor.h"
#include "naab/interpreter.h" // For Value definition
#include "naab/ast.h" // For ast::StructField and ast::Type
#include "naab/subprocess_helpers.h" // For execute_subprocess_with_pipes
#include "naab/sandbox.h" // For security sandbox
#include "naab/audit_logger.h" // For security audit logging
#include <cstdio>       // For popen, pclose, FILE
#include <array>        // For std::array
#include <stdexcept>    // For std::runtime_error
#include <fmt/core.h>   // For fmt::print
#include <sstream>      // For std::istringstream


namespace naab {
namespace runtime {

ShellExecutor::ShellExecutor() {
    fmt::print("[Shell] Shell executor initialized\n");
}

ShellExecutor::~ShellExecutor() {
}

bool ShellExecutor::execute(const std::string& code) {
    return runCommand(code);
}

// Phase 2.3: Execute command and return struct with {exit_code, stdout, stderr}
std::shared_ptr<naab::interpreter::Value> ShellExecutor::executeWithReturn(
    const std::string& code) {

    fmt::print("[Shell] Executing with return: \'{}\'\n", code);

    std::string stdout_output;
    std::string stderr_output;
    int exit_code;

    // Execute command and capture output
    // Always use shell to properly handle operators like &&, ||, |, ;, etc.
    // Check if command contains shell operators or special characters
    bool needs_shell = (code.find('\n') != std::string::npos ||
                       code.find("&&") != std::string::npos ||
                       code.find("||") != std::string::npos ||
                       code.find('|') != std::string::npos ||
                       code.find(';') != std::string::npos ||
                       code.find('>') != std::string::npos ||
                       code.find('<') != std::string::npos ||
                       code.find('&') != std::string::npos ||
                       code.find('$') != std::string::npos ||
                       code.find('`') != std::string::npos ||
                       code.find('\'') != std::string::npos ||
                       code.find('"') != std::string::npos);

    if (needs_shell) {
        // Use shell to interpret command
        std::vector<std::string> args = {"-c", code};
        exit_code = execute_subprocess_with_pipes(
            "sh", args, stdout_output, stderr_output, nullptr
        );
    } else {
        // Simple command - execute directly for better performance
        std::istringstream iss(code);
        std::string cmd_path;
        iss >> cmd_path;
        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }
        exit_code = execute_subprocess_with_pipes(
            cmd_path, args, stdout_output, stderr_output, nullptr
        );
    }

    // Print output (side effects preserved)
    if (!stdout_output.empty()) {
        fmt::print("{}", stdout_output);
    }
    if (!stderr_output.empty()) {
        fmt::print("[Shell stderr]: {}", stderr_output);
    }

    if (exit_code != 0) {
        fmt::print("[Shell] Command failed with exit code {}\n", exit_code);
    }

    // Trim trailing newlines from outputs
    auto trim_trailing_newline = [](std::string& str) {
        while (!str.empty() && str.back() == '\n') {
            str.pop_back();
        }
    };
    trim_trailing_newline(stdout_output);
    trim_trailing_newline(stderr_output);

    // Create ShellResult struct definition
    // struct ShellResult { exit_code: int, stdout: string, stderr: string }
    std::vector<ast::StructField> fields;
    fields.push_back(ast::StructField{"exit_code", ast::Type::makeInt(), std::nullopt});
    fields.push_back(ast::StructField{"stdout", ast::Type::makeString(), std::nullopt});
    fields.push_back(ast::StructField{"stderr", ast::Type::makeString(), std::nullopt});

    auto struct_def = std::make_shared<interpreter::StructDef>("ShellResult", std::move(fields));

    // Create struct instance
    auto struct_value = std::make_shared<interpreter::StructValue>("ShellResult", struct_def);

    // Set field values
    struct_value->field_values[0] = std::make_shared<interpreter::Value>(exit_code);
    struct_value->field_values[1] = std::make_shared<interpreter::Value>(stdout_output);
    struct_value->field_values[2] = std::make_shared<interpreter::Value>(stderr_output);

    // Return struct wrapped in Value
    return std::make_shared<interpreter::Value>(struct_value);
}

std::shared_ptr<naab::interpreter::Value> ShellExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<naab::interpreter::Value>>& args) {
    
    if (function_name == "exec" && !args.empty()) {
        // Handle string argument
        if (auto* str_val = std::get_if<std::string>(&args[0]->data)) {
            bool success = runCommand(*str_val);
            return std::make_shared<naab::interpreter::Value>(success);
        }
        // Handle integer argument (convert to string)
        if (auto* int_val = std::get_if<int>(&args[0]->data)) {
             bool success = runCommand(std::to_string(*int_val));
             return std::make_shared<naab::interpreter::Value>(success);
        }
    }
    
    throw std::runtime_error("Shell executor only supports 'exec(command_string)'");
}

std::string ShellExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string error_output = stderr_buffer_.getAndClear();
    if (!error_output.empty()) {
        output += "\n[Shell stderr]: " + error_output;
    }
    return output;
}

bool ShellExecutor::runCommand(const std::string& command) {
    // Check sandbox permissions for command execution
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Shell execution denied\n");
        sandbox->logViolation("executeShell", command, "BLOCK_CALL capability required");
        throw std::runtime_error("Shell execution denied by sandbox");
    }

    fmt::print("[Shell] Executing: \'{}\'\n", command);

    std::string stdout_buffer_local;
    std::string stderr_buffer_local;
    int exit_code;

    // Check if this is a multi-line script (contains newline)
    if (command.find('\n') != std::string::npos) {
        // Execute as a shell script using sh -c
        std::vector<std::string> args = {"-c", command};
        exit_code = execute_subprocess_with_pipes(
            "sh", args, stdout_buffer_local, stderr_buffer_local, nullptr
        );
    } else {
        // Parse the command string into command and arguments
        // For simplicity, assume the first word is the command and the rest are args
        std::istringstream iss(command);
        std::string cmd_path;
        iss >> cmd_path;
        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }

        exit_code = execute_subprocess_with_pipes(
            cmd_path, args, stdout_buffer_local, stderr_buffer_local, nullptr
        );
    }
    
    // Append to internal buffers
    stdout_buffer_.append(stdout_buffer_local);
    stderr_buffer_.append(stderr_buffer_local);

    bool success = (exit_code == 0);
    
    if (success) {
        fmt::print("[SUCCESS] Shell command executed (exit code {}\n)", exit_code);
    } else {
        fmt::print("[ERROR] Shell command failed with code {} (captured stderr: \'{}\')\n", exit_code, stderr_buffer_local);
    }
    return success;
}

} // namespace runtime
} // namespace naab
