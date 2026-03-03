#include "naab/interpreter.h"
#include "naab/zig_executor.h"
#include "naab/subprocess_helpers.h"
#include "naab/sandbox.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <fmt/core.h>

namespace naab {
namespace runtime {

std::atomic<int> ZigExecutor::temp_file_counter_(0);

ZigExecutor::ZigExecutor() {}

std::string ZigExecutor::wrapZigCode(const std::string& code, bool for_return) {
    // If code already has main function, return as-is
    if (code.find("pub fn main()") != std::string::npos) {
        return code;
    }

    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    if (!for_return) {
        // For execute(): wrap in main
        std::string wrapped = "const std = @import(\"std\");\n"
                              "pub fn main() !void {\n";
        for (const auto& l : lines) {
            wrapped += "    " + l + "\n";
        }
        wrapped += "}\n";
        return wrapped;
    }

    // For executeWithReturn(): wrap and print last expression
    // Find last non-empty line
    int last_line_idx = -1;
    for (int i = static_cast<int>(lines.size()) - 1; i >= 0; i--) {
        std::string trimmed = lines[i];
        size_t s = trimmed.find_first_not_of(" \t\r");
        if (s != std::string::npos) {
            last_line_idx = i;
            break;
        }
    }

    if (lines.size() <= 1) {
        // Single-line expression
        std::string expr = code;
        size_t s = expr.find_first_not_of(" \t\r");
        if (s != std::string::npos) {
            expr = expr.substr(s);
        }
        return "const std = @import(\"std\");\n"
               "pub fn main() !void {\n"
               "    const stdout = std.io.getStdOut().writer();\n"
               "    try stdout.print(\"{any}\", .{" + expr + "});\n"
               "}\n";
    }

    // Multi-line: wrap in main, print last expression
    std::string wrapped = "const std = @import(\"std\");\n"
                          "pub fn main() !void {\n"
                          "    const stdout = std.io.getStdOut().writer();\n";
    for (size_t i = 0; i < lines.size(); i++) {
        if (static_cast<int>(i) == last_line_idx) {
            std::string trimmed = lines[i];
            size_t s = trimmed.find_first_not_of(" \t\r");
            if (s != std::string::npos) {
                trimmed = trimmed.substr(s);
                wrapped += "    try stdout.print(\"{any}\", .{" + trimmed + "});\n";
            }
        } else {
            wrapped += "    " + lines[i] + "\n";
        }
    }
    wrapped += "}\n";
    return wrapped;
}

bool ZigExecutor::execute(const std::string& code) {
    // Check sandbox permissions
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Zig execution denied\n");
        sandbox->logViolation("execute_zig", code, "BLOCK_CALL capability required");
        throw std::runtime_error("Zig execution denied by sandbox");
    }

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    auto thread_id = std::this_thread::get_id();
    int counter = temp_file_counter_.fetch_add(1);
    std::ostringstream filename_base;
    filename_base << "naab_zig_" << thread_id << "_" << counter;

    std::filesystem::path temp_zig = temp_dir / (filename_base.str() + ".zig");
    std::filesystem::path temp_bin = temp_dir / (filename_base.str() + "_bin");

    try {
        std::string zig_code = wrapZigCode(code, false);

        std::ofstream ofs(temp_zig);
        if (!ofs.is_open()) {
            fmt::print("[ERROR] Failed to create temp Zig file\n");
            return false;
        }
        ofs << zig_code;
        ofs.close();

        // Compile with zig build-exe
        std::string compile_stdout, compile_stderr;
        std::vector<std::string> compile_args = {
            "build-exe", temp_zig.string(), "-femit-bin=" + temp_bin.string()
        };

        int compile_exit = execute_subprocess_with_pipes(
            "zig", compile_args, compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            fmt::print("[ERROR] Zig compilation failed (exit code {})\n", compile_exit);
            stderr_buffer_.append(compile_stderr);
            std::filesystem::remove(temp_zig);
            return false;
        }

        // Execute binary
        std::string exec_stdout, exec_stderr;
        std::vector<std::string> exec_args = {};

        int exec_exit = execute_subprocess_with_pipes(
            temp_bin.string(), exec_args, exec_stdout, exec_stderr, nullptr
        );

        stdout_buffer_.append(exec_stdout);
        if (!exec_stderr.empty()) {
            stderr_buffer_.append(exec_stderr);
        }

        // Clean up
        std::filesystem::remove(temp_zig);
        std::filesystem::remove(temp_bin);

        if (exec_exit != 0) {
            fmt::print("[ERROR] Zig program failed (exit code {})\n", exec_exit);
        }

        return (exec_exit == 0);

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Zig execution failed: {}\n", e.what());
        std::filesystem::remove(temp_zig);
        std::filesystem::remove(temp_bin);
        return false;
    }
}

std::shared_ptr<interpreter::Value> ZigExecutor::executeWithReturn(
    const std::string& code) {

    // Check sandbox permissions
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Zig execution denied\n");
        sandbox->logViolation("execute_zig_return", code, "BLOCK_CALL capability required");
        throw std::runtime_error("Zig execution denied by sandbox");
    }

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    auto thread_id = std::this_thread::get_id();
    int counter = temp_file_counter_.fetch_add(1);
    std::ostringstream filename_base;
    filename_base << "naab_zig_" << thread_id << "_" << counter;

    std::filesystem::path temp_zig = temp_dir / (filename_base.str() + "_ret.zig");
    std::filesystem::path temp_bin = temp_dir / (filename_base.str() + "_ret_bin");

    try {
        std::string zig_code = wrapZigCode(code, true);

        std::ofstream ofs(temp_zig);
        if (!ofs.is_open()) {
            return std::make_shared<interpreter::Value>();
        }
        ofs << zig_code;
        ofs.close();

        // Compile
        std::string compile_stdout, compile_stderr;
        int compile_exit = execute_subprocess_with_pipes(
            "zig", {"build-exe", temp_zig.string(), "-femit-bin=" + temp_bin.string()},
            compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            std::string error_msg = compile_stderr;
            std::filesystem::remove(temp_zig);

            // Add helpful hints for common Zig compilation errors
            if (error_msg.find("expected type") != std::string::npos ||
                error_msg.find("mismatched types") != std::string::npos) {
                fmt::print("\n  Hint: Zig type error.\n"
                           "  - Zig has strict compile-time type checking\n"
                           "  - Use explicit type annotations: const x: i32 = 5;\n"
                           "  - Use type coercion: @intCast(), @floatCast(), etc.\n"
                           "  - Check function return types match declarations\n\n");
            } else if (error_msg.find("use of undeclared identifier") != std::string::npos) {
                fmt::print("\n  Hint: Zig identifier not declared.\n"
                           "  - Check spelling and imports\n"
                           "  - Use const for compile-time constants, var for runtime variables\n"
                           "  - Import standard library: const std = @import(\"std\");\n\n");
            } else if (error_msg.find("unable to evaluate constant expression") != std::string::npos) {
                fmt::print("\n  Hint: Zig compile-time evaluation error.\n"
                           "  - const values must be known at compile time\n"
                           "  - Use var for runtime-computed values\n"
                           "  - comptime keyword forces compile-time evaluation\n\n");
            } else if (error_msg.find("error:") != std::string::npos && error_msg.find("not handled") != std::string::npos) {
                fmt::print("\n  Hint: Zig error not handled.\n"
                           "  - Zig requires explicit error handling with try or catch\n"
                           "  - Functions that can error use ! in signature: fn foo() !void\n"
                           "  - Use try before error-returning function calls\n\n");
            }

            throw std::runtime_error(
                "Zig compilation failed:\n" + error_msg +
                "\n  Code preview:\n    " + zig_code.substr(0, std::min(zig_code.size(), size_t(200))));
        }

        // Execute
        std::string exec_stdout, exec_stderr;
        execute_subprocess_with_pipes(
            temp_bin.string(), {},
            exec_stdout, exec_stderr, nullptr
        );

        // Print output
        if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
        if (!exec_stderr.empty()) fmt::print("[Zig stderr]: {}", exec_stderr);

        // Cleanup
        std::filesystem::remove(temp_zig);
        std::filesystem::remove(temp_bin);

        // Trim trailing whitespace/newlines
        std::string result = exec_stdout;
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                                    result.back() == ' ' || result.back() == '\t')) {
            result.pop_back();
        }

        // Try to parse as boolean
        if (result == "true") return std::make_shared<interpreter::Value>(true);
        if (result == "false") return std::make_shared<interpreter::Value>(false);

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

        return std::make_shared<interpreter::Value>(result);

    } catch (const std::exception& e) {
        std::filesystem::remove(temp_zig);
        std::filesystem::remove(temp_bin);
        throw;
    }
}

std::shared_ptr<interpreter::Value> ZigExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "exec" && !args.empty()) {
        if (auto* str_val = std::get_if<std::string>(&args[0]->data)) {
            bool success = execute(*str_val);
            return std::make_shared<interpreter::Value>(success);
        }
    }

    throw std::runtime_error("ZigExecutor only supports 'exec(code_string)'");
}

std::string ZigExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string errors = stderr_buffer_.getAndClear();
    if (!errors.empty()) {
        output += "\n[Zig stderr]: " + errors;
    }
    return output;
}

} // namespace runtime
} // namespace naab
