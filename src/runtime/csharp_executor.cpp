#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/csharp_executor.h"
#include "naab/subprocess_helpers.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <fmt/core.h>

namespace naab {
namespace runtime {

CSharpExecutor::CSharpExecutor() {
    fmt::print("[INFO] CSharpExecutor initialized\n");
}

bool CSharpExecutor::execute(const std::string& code) {
    // Create temp directory
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_cs = temp_dir / "naab_temp_csharp.cs";
    std::filesystem::path temp_exe = temp_dir / "naab_temp_csharp.exe";

    try {
        // Write code to temp file
        std::ofstream ofs(temp_cs);
        if (!ofs.is_open()) {
            fmt::print("[ERROR] Failed to create temp C# file\n");
            return false;
        }
        ofs << code;
        ofs.close();

        // Compile with mcs
        std::string compile_stdout, compile_stderr;
        std::vector<std::string> compile_args = {
            temp_cs.string(),
            "-out:" + temp_exe.string()
        };

        int compile_exit = execute_subprocess_with_pipes(
            "mcs", compile_args, compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            fmt::print("[ERROR] C# compilation failed (exit code {})\n", compile_exit);
            stderr_buffer_.append(compile_stderr);
            // Clean up
            std::filesystem::remove(temp_cs);
            return false;
        }

        // Execute with mono
        std::string exec_stdout, exec_stderr;
        std::vector<std::string> exec_args = {temp_exe.string()};

        int exec_exit = execute_subprocess_with_pipes(
            "mono", exec_args, exec_stdout, exec_stderr, nullptr
        );

        // Capture output
        stdout_buffer_.append(exec_stdout);
        if (!exec_stderr.empty()) {
            stderr_buffer_.append(exec_stderr);
        }

        // Clean up
        std::filesystem::remove(temp_cs);
        std::filesystem::remove(temp_exe);

        if (exec_exit == 0) {
            fmt::print("[SUCCESS] C# program executed (exit code 0)\n");
        } else {
            fmt::print("[ERROR] C# program failed (exit code {})\n", exec_exit);
        }

        return (exec_exit == 0);

    } catch (const std::exception& e) {
        fmt::print("[ERROR] C# execution failed: {}\n", e.what());
        // Clean up on error
        std::filesystem::remove(temp_cs);
        std::filesystem::remove(temp_exe);
        return false;
    }
}

// Phase 2.3: Execute and return stdout as value
std::shared_ptr<interpreter::Value> CSharpExecutor::executeWithReturn(
    const std::string& code) {

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_cs = temp_dir / "naab_temp_cs_ret.cs";
    std::filesystem::path temp_exe = temp_dir / "naab_temp_cs_ret.exe";

    try {
        // Phase 2.3: Multi-line support - wrap code if needed
        std::string csharp_code = code;

        // If no class definition, wrap in Main method
        if (code.find("class ") == std::string::npos &&
            code.find("static void Main") == std::string::npos) {

            // Check if multi-line
            if (code.find('\n') != std::string::npos) {
                // Multi-line: wrap in Main() and print last expression
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

                csharp_code = "using System;\nclass Program {\n    static void Main() {\n";
                for (size_t i = 0; i < lines.size(); i++) {
                    if (static_cast<int>(i) == last_line_idx) {
                        // Last line: print it
                        std::string trimmed = lines[i];
                        size_t s = trimmed.find_first_not_of(" \t\r");
                        if (s != std::string::npos) {
                            trimmed = trimmed.substr(s);
                            // Remove trailing semicolon if present
                            if (!trimmed.empty() && trimmed.back() == ';') {
                                trimmed.pop_back();
                            }
                            csharp_code += "        Console.WriteLine(" + trimmed + ");\n";
                        }
                    } else {
                        csharp_code += "        " + lines[i] + "\n";
                    }
                }
                csharp_code += "    }\n}\n";
            } else {
                // Single-line expression
                std::string expr = code;
                size_t s = expr.find_first_not_of(" \t\r");
                if (s != std::string::npos) {
                    expr = expr.substr(s);
                }
                // Remove trailing semicolon
                if (!expr.empty() && expr.back() == ';') {
                    expr.pop_back();
                }
                csharp_code = "using System;\nclass Program {\n    static void Main() {\n        Console.WriteLine(" + expr + ");\n    }\n}\n";
            }
        }

        std::ofstream ofs(temp_cs);
        if (!ofs.is_open()) {
            return std::make_shared<interpreter::Value>();
        }
        ofs << csharp_code;
        ofs.close();

        // Compile
        std::string compile_stdout, compile_stderr;
        int compile_exit = execute_subprocess_with_pipes(
            "mcs", {temp_cs.string(), "-out:" + temp_exe.string()},
            compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            std::filesystem::remove(temp_cs);
            return std::make_shared<interpreter::Value>();
        }

        // Execute
        std::string exec_stdout, exec_stderr;
        int exec_exit = execute_subprocess_with_pipes(
            "mono", {temp_exe.string()},
            exec_stdout, exec_stderr, nullptr
        );

        // Print output
        if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
        if (!exec_stderr.empty()) fmt::print("[C# stderr]: {}", exec_stderr);

        // Cleanup
        std::filesystem::remove(temp_cs);
        std::filesystem::remove(temp_exe);

        // Trim trailing newline
        std::string result = exec_stdout;
        if (!result.empty() && result.back() == '\n') result.pop_back();

        // Try to parse as number
        if (!result.empty()) {
            try {
                size_t pos;
                int i = std::stoi(result, &pos);
                if (pos == result.size()) return std::make_shared<interpreter::Value>(i);
            } catch (...) {}

            try {
                size_t pos;
                double d = std::stod(result, &pos);
                if (pos == result.size()) return std::make_shared<interpreter::Value>(d);
            } catch (...) {}
        }

        // Return as string
        return std::make_shared<interpreter::Value>(result);

    } catch (const std::exception& e) {
        std::filesystem::remove(temp_cs);
        std::filesystem::remove(temp_exe);
        return std::make_shared<interpreter::Value>();
    }
}

std::shared_ptr<interpreter::Value> CSharpExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "exec" && !args.empty()) {
        if (auto* str_val = std::get_if<std::string>(&args[0]->data)) {
            bool success = execute(*str_val);
            return std::make_shared<interpreter::Value>(success);
        }
    }

    throw std::runtime_error("CSharpExecutor only supports 'exec(code_string)'");
}

std::string CSharpExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string errors = stderr_buffer_.getAndClear();
    if (!errors.empty()) {
        output += "\n[C# stderr]: " + errors;
    }
    return output;
}

} // namespace runtime
} // namespace naab
