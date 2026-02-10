#include "naab/generic_subprocess_executor.h"
#include "naab/interpreter.h" // For Value definition
#include "naab/subprocess_helpers.h" // For execute_subprocess_with_pipes
#include "naab/temp_file_guard.h"   // For TempFileGuard
#include "naab/sandbox.h" // For security sandbox
#include "naab/audit_logger.h" // For security audit logging
#include <climits>      // For INT_MIN, INT_MAX
#include <cstdio>       // For popen, pclose
#include <array>        // For std::array
#include <stdexcept>    // For std::runtime_error
#include <fmt/core.h>   // For fmt::print
#include <fstream>      // For temporary file I/O
#include <sstream>      // For string stream
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
    // GenericSubprocessExecutor initialized (silent)
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

        std::string command_line = format_command(command_template_, temp_file_path.string());
        bool success = runCommand(command_line);

        // temp_file_guard's destructor will handle deletion
        return success;
    }
}

// Phase 2.3: Execute and return stdout as value
std::shared_ptr<interpreter::Value> GenericSubprocessExecutor::executeWithReturn(
    const std::string& code) {

    std::string stdout_output;
    std::string stderr_output;
    int exit_code;

    if (file_extension_.empty()) {
        // Run directly from code string
        std::string command_line = format_command(command_template_, code);

        // Parse command for subprocess execution
        std::istringstream iss(command_line);
        std::string cmd;
        iss >> cmd;
        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }

        exit_code = execute_subprocess_with_pipes(
            cmd, args, stdout_output, stderr_output, nullptr);

    } else {
        // Write to temp file and run
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        std::filesystem::path temp_file_path = temp_dir / ("naab_temp_" + language_id_ + file_extension_);

        int counter = 0;
        while (std::filesystem::exists(temp_file_path)) {
            temp_file_path = temp_dir / (
                "naab_temp_" + language_id_ + "_" + std::to_string(counter++) + file_extension_
            );
            if (counter > 1000) {
                throw std::runtime_error("Failed to find unique temp file name");
            }
        }

        TempFileGuard temp_file_guard(temp_file_path);

        // Phase 2.3: Multi-line support - add language-specific wrapping
        std::string wrapped_code = code;

        // Go-specific wrapping: if no package main, wrap in it
        if (language_id_ == "go" && code.find("package main") == std::string::npos) {
            // Check if multi-line
            if (code.find('\n') != std::string::npos) {
                // Multi-line: wrap in main() and print last expression
                std::vector<std::string> lines;
                std::istringstream stream(code);
                std::string line;
                while (std::getline(stream, line)) {
                    lines.push_back(line);
                }

                // Find last non-empty line
                int last_line_idx = -1;
                for (int i = lines.size() - 1; i >= 0; i--) {
                    std::string trimmed = lines[i];
                    size_t s = trimmed.find_first_not_of(" \t\r");
                    if (s != std::string::npos) {
                        last_line_idx = i;
                        break;
                    }
                }

                wrapped_code = "package main\nimport \"fmt\"\nfunc main() {\n";
                for (size_t i = 0; i < lines.size(); i++) {
                    if (static_cast<int>(i) == last_line_idx) {
                        // Last line: print it
                        std::string trimmed = lines[i];
                        size_t s = trimmed.find_first_not_of(" \t\r");
                        if (s != std::string::npos) {
                            trimmed = trimmed.substr(s);
                            wrapped_code += "\tfmt.Println(" + trimmed + ")\n";
                        }
                    } else {
                        wrapped_code += "\t" + lines[i] + "\n";
                    }
                }
                wrapped_code += "}\n";
            } else {
                // Single-line expression
                std::string expr = code;
                size_t s = expr.find_first_not_of(" \t\r");
                if (s != std::string::npos) {
                    expr = expr.substr(s);
                }
                wrapped_code = "package main\nimport \"fmt\"\nfunc main() {\n\tfmt.Println(" + expr + ")\n}\n";
            }
        }

        // TypeScript-specific wrapping: wrap expressions in console.log()
        if ((language_id_ == "typescript" || language_id_ == "ts")) {
            // Check if multi-line
            if (code.find('\n') != std::string::npos) {
                // Multi-line: wrap last expression in console.log()
                std::vector<std::string> lines;
                std::istringstream stream(code);
                std::string line;
                while (std::getline(stream, line)) {
                    lines.push_back(line);
                }

                // Find last non-empty line
                int last_line_idx = -1;
                for (int i = lines.size() - 1; i >= 0; i--) {
                    std::string trimmed = lines[i];
                    size_t s = trimmed.find_first_not_of(" \t\r");
                    if (s != std::string::npos) {
                        last_line_idx = i;
                        break;
                    }
                }

                wrapped_code = "";
                for (size_t i = 0; i < lines.size(); i++) {
                    if (static_cast<int>(i) == last_line_idx) {
                        // Last line: wrap in console.log()
                        std::string trimmed = lines[i];
                        size_t s = trimmed.find_first_not_of(" \t\r");
                        if (s != std::string::npos) {
                            trimmed = trimmed.substr(s);
                            // Remove trailing semicolon if present
                            if (!trimmed.empty() && trimmed.back() == ';') {
                                trimmed.pop_back();
                            }
                            wrapped_code += "console.log(" + trimmed + ");\n";
                        }
                    } else {
                        wrapped_code += lines[i] + "\n";
                    }
                }
            } else {
                // Single-line expression: wrap in console.log()
                std::string expr = code;
                size_t s = expr.find_first_not_of(" \t\r");
                if (s != std::string::npos) {
                    expr = expr.substr(s);
                }
                // Remove trailing semicolon if present
                if (!expr.empty() && expr.back() == ';') {
                    expr.pop_back();
                }
                wrapped_code = "console.log(" + expr + ");\n";
            }
        }

        std::ofstream ofs(temp_file_path);
        if (!ofs.is_open()) {
            throw std::runtime_error("Failed to create temp file");
        }
        ofs << wrapped_code;
        ofs.close();

        std::string command_line = format_command(command_template_, temp_file_path.string());

        // Parse command for subprocess execution
        std::istringstream iss(command_line);
        std::string cmd;
        iss >> cmd;
        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }

        exit_code = execute_subprocess_with_pipes(
            cmd, args, stdout_output, stderr_output, nullptr);
    }

    // Print output (side effects preserved)
    if (!stdout_output.empty()) {
        fmt::print("{}", stdout_output);
    }
    if (!stderr_output.empty()) {
        fmt::print("[{} stderr]: {}", language_id_, stderr_output);
    }

    // Trim trailing newline
    std::string result = stdout_output;
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }

    // Try to parse as number
    // Try double first to avoid int overflow for large numbers
    if (!result.empty()) {
        try {
            size_t pos;
            double d = std::stod(result, &pos);
            if (pos == result.size()) {
                // Check if it's actually an integer that fits in int range
                if (d == static_cast<int>(d) && d >= INT_MIN && d <= INT_MAX) {
                    return std::make_shared<naab::interpreter::Value>(static_cast<int>(d));
                }
                // Return as double if too large or has decimal part
                return std::make_shared<naab::interpreter::Value>(d);
            }
        } catch (...) {}
    }

    // Return as string
    return std::make_shared<naab::interpreter::Value>(result);
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
        // Command executed (silent)
    } else {
        fmt::print("[ERROR] GenericSubprocessExecutor-{} command failed with code {} (captured stderr: \'{}\')\n", language_id_, exit_code, stderr_buffer_local);
    }
    return success;
}

} // namespace runtime
} // namespace naab
