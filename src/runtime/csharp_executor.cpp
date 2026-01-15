#include "naab/csharp_executor.h"
#include "naab/interpreter.h"
#include "naab/subprocess_helpers.h"
#include <filesystem>
#include <fstream>
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
