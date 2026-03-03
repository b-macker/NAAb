#include "naab/interpreter.h"
#include "naab/nim_executor.h"
#include "naab/subprocess_helpers.h"
#include "naab/sandbox.h"
#include "naab/audit_logger.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <fmt/core.h>

namespace naab {
namespace runtime {

std::atomic<int> NimExecutor::temp_file_counter_(0);

NimExecutor::NimExecutor() {}

std::string NimExecutor::wrapNimCode(const std::string& code, bool for_return) {
    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    if (!for_return) {
        // For execute(): return code as-is (Nim doesn't need wrapper for side effects)
        return code;
    }

    // For executeWithReturn(): need to ensure last expression is echoed

    // Check if code already has echo statements
    bool already_echoes = (code.find("echo ") != std::string::npos ||
                          code.find("echo(") != std::string::npos);

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

        if (already_echoes) {
            return expr;
        }
        return "echo " + expr;
    }

    // Multi-line: wrap last expression with echo if needed
    if (last_line_idx >= 0 && !already_echoes) {
        std::string wrapped;
        for (size_t i = 0; i < lines.size(); i++) {
            if (static_cast<int>(i) == last_line_idx) {
                std::string trimmed = lines[i];
                size_t s = trimmed.find_first_not_of(" \t\r");
                if (s != std::string::npos) {
                    trimmed = trimmed.substr(s);
                    // Check if it's a statement (not an expression)
                    if (trimmed.find("var ") == 0 || trimmed.find("let ") == 0 ||
                        trimmed.find("const ") == 0 || trimmed.find("proc ") == 0 ||
                        trimmed.find("func ") == 0 || trimmed.find("if ") == 0 ||
                        trimmed.find("for ") == 0 || trimmed.find("while ") == 0 ||
                        trimmed.find("import ") == 0 || trimmed.find("echo ") == 0) {
                        // It's a statement, don't wrap
                        wrapped += lines[i] + "\n";
                    } else {
                        // It's an expression, wrap with echo
                        wrapped += "echo " + trimmed + "\n";
                    }
                } else {
                    wrapped += lines[i] + "\n";
                }
            } else {
                wrapped += lines[i] + "\n";
            }
        }
        return wrapped;
    }

    return code;
}

bool NimExecutor::execute(const std::string& code) {
    // Check sandbox permissions
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Nim execution denied\n");
        sandbox->logViolation("execute_nim", code, "BLOCK_CALL capability required");
        throw std::runtime_error("Nim execution denied by sandbox");
    }

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    auto thread_id = std::this_thread::get_id();
    int counter = temp_file_counter_.fetch_add(1);
    std::ostringstream filename_base;
    filename_base << "naab_nim_" << thread_id << "_" << counter;

    std::filesystem::path temp_nim = temp_dir / (filename_base.str() + ".nim");
    std::filesystem::path temp_bin = temp_dir / (filename_base.str() + "_bin");

    try {
        std::string nim_code = wrapNimCode(code, false);

        std::ofstream ofs(temp_nim);
        if (!ofs.is_open()) {
            fmt::print("[ERROR] Failed to create temp Nim file\n");
            return false;
        }
        ofs << nim_code;
        ofs.close();

        // Compile with nim c
        std::string compile_stdout, compile_stderr;
        std::vector<std::string> compile_args = {
            "c", "--hints:off", "-o:" + temp_bin.string(), temp_nim.string()
        };

        int compile_exit = execute_subprocess_with_pipes(
            "nim", compile_args, compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            fmt::print("[ERROR] Nim compilation failed (exit code {})\n", compile_exit);
            stderr_buffer_.append(compile_stderr);
            std::filesystem::remove(temp_nim);
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
        std::filesystem::remove(temp_nim);
        std::filesystem::remove(temp_bin);

        if (exec_exit != 0) {
            fmt::print("[ERROR] Nim program failed (exit code {})\n", exec_exit);
        }

        return (exec_exit == 0);

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Nim execution failed: {}\n", e.what());
        std::filesystem::remove(temp_nim);
        std::filesystem::remove(temp_bin);
        return false;
    }
}

std::shared_ptr<interpreter::Value> NimExecutor::executeWithReturn(
    const std::string& code) {

    // Check sandbox permissions
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Nim execution denied\n");
        sandbox->logViolation("execute_nim_return", code, "BLOCK_CALL capability required");
        throw std::runtime_error("Nim execution denied by sandbox");
    }

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    auto thread_id = std::this_thread::get_id();
    int counter = temp_file_counter_.fetch_add(1);
    std::ostringstream filename_base;
    filename_base << "naab_nim_" << thread_id << "_" << counter;

    std::filesystem::path temp_nim = temp_dir / (filename_base.str() + "_ret.nim");
    std::filesystem::path temp_bin = temp_dir / (filename_base.str() + "_ret_bin");

    try {
        std::string nim_code = wrapNimCode(code, true);

        std::ofstream ofs(temp_nim);
        if (!ofs.is_open()) {
            return std::make_shared<interpreter::Value>();
        }
        ofs << nim_code;
        ofs.close();

        // Compile
        std::string compile_stdout, compile_stderr;
        int compile_exit = execute_subprocess_with_pipes(
            "nim", {"c", "--hints:off", "-o:" + temp_bin.string(), temp_nim.string()},
            compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            std::string error_msg = compile_stderr;
            std::filesystem::remove(temp_nim);

            // Add helpful hints for common Nim compilation errors
            if (error_msg.find("undeclared identifier") != std::string::npos ||
                (error_msg.find("'") != std::string::npos && error_msg.find("' is undeclared") != std::string::npos)) {
                fmt::print("\n  Hint: Nim variable or identifier not defined.\n"
                           "  - Check spelling and imports\n"
                           "  - Nim is case-sensitive\n"
                           "  - Use 'var' for mutable variables, 'let' for immutable, 'const' for compile-time\n"
                           "  - Ensure the module is imported if using stdlib functions\n\n");
            } else if (error_msg.find("type mismatch") != std::string::npos) {
                fmt::print("\n  Hint: Nim type error.\n"
                           "  - Nim has strict static typing\n"
                           "  - Use explicit type conversions: int(), float(), $\n"
                           "  - Example: let x: int = 5\n"
                           "  - Use $ operator to convert to string: echo $myInt\n\n");
            } else if (error_msg.find("could not import") != std::string::npos ||
                       error_msg.find("cannot open") != std::string::npos) {
                fmt::print("\n  Hint: Nim module not found.\n"
                           "  - Standard library modules: strutils, sequtils, os, math, etc.\n"
                           "  - Install packages: nimble install <package>\n"
                           "  - Check module name spelling (case-sensitive)\n"
                           "  - List stdlib modules: https://nim-lang.org/docs/lib.html\n\n");
            } else if (error_msg.find("expression expected") != std::string::npos ||
                       error_msg.find("invalid indentation") != std::string::npos) {
                fmt::print("\n  Hint: Nim syntax error.\n"
                           "  - Nim uses significant indentation (like Python)\n"
                           "  - Use 2 or 4 spaces consistently (not tabs)\n"
                           "  - Blocks must be indented under proc/if/for/etc\n"
                           "  - Check for missing colons after proc/if statements\n\n");
            } else if (error_msg.find("undeclared field") != std::string::npos) {
                fmt::print("\n  Hint: Nim object field not found.\n"
                           "  - Check field name spelling\n"
                           "  - Fields are case-sensitive\n"
                           "  - Use object.field syntax\n\n");
            }

            throw std::runtime_error(
                "Nim compilation failed:\n" + error_msg +
                "\n  Code preview:\n    " + nim_code.substr(0, std::min(nim_code.size(), size_t(200))));
        }

        // Execute
        std::string exec_stdout, exec_stderr;
        execute_subprocess_with_pipes(
            temp_bin.string(), {},
            exec_stdout, exec_stderr, nullptr
        );

        // Print output
        if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
        if (!exec_stderr.empty()) fmt::print("[Nim stderr]: {}", exec_stderr);

        // Cleanup
        std::filesystem::remove(temp_nim);
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
        std::filesystem::remove(temp_nim);
        std::filesystem::remove(temp_bin);
        throw;
    }
}

std::shared_ptr<interpreter::Value> NimExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "exec" && !args.empty()) {
        if (auto* str_val = std::get_if<std::string>(&args[0]->data)) {
            bool success = execute(*str_val);
            return std::make_shared<interpreter::Value>(success);
        }
    }

    throw std::runtime_error("NimExecutor only supports 'exec(code_string)'");
}

std::string NimExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string errors = stderr_buffer_.getAndClear();
    if (!errors.empty()) {
        output += "\n[Nim stderr]: " + errors;
    }
    return output;
}

} // namespace runtime
} // namespace naab
