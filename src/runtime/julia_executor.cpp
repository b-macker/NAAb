#include "naab/interpreter.h"
#include "naab/julia_executor.h"
#include "naab/subprocess_helpers.h"
#include "naab/sandbox.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <fmt/core.h>

namespace naab {
namespace runtime {

std::atomic<int> JuliaExecutor::temp_file_counter_(0);

JuliaExecutor::JuliaExecutor() {}

std::string JuliaExecutor::wrapJuliaCode(const std::string& code, bool for_return) {
    if (!for_return) {
        // For execute(): return code as-is (Julia doesn't need wrapper)
        return code;
    }

    // For executeWithReturn(): need to ensure last expression is printed

    // Check if code already has println/print statements
    bool already_prints = (code.find("println(") != std::string::npos ||
                          code.find("print(") != std::string::npos);

    if (already_prints) {
        return code;
    }

    // Split into lines to find last expression
    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

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
        // Single-line expression: wrap with println
        std::string expr = code;
        size_t s = expr.find_first_not_of(" \t\r");
        if (s != std::string::npos) {
            expr = expr.substr(s);
        }
        return "println(" + expr + ")";
    }

    // Multi-line: wrap last expression with println if needed
    if (last_line_idx >= 0) {
        std::string wrapped;
        for (size_t i = 0; i < lines.size(); i++) {
            if (static_cast<int>(i) == last_line_idx) {
                std::string trimmed = lines[i];
                size_t s = trimmed.find_first_not_of(" \t\r");
                if (s != std::string::npos) {
                    trimmed = trimmed.substr(s);
                    // Check if it's a statement (not an expression)
                    if (trimmed.find("function ") == 0 || trimmed.find("using ") == 0 ||
                        trimmed.find("import ") == 0 || trimmed.find("if ") == 0 ||
                        trimmed.find("for ") == 0 || trimmed.find("while ") == 0 ||
                        trimmed.find("end") == 0 || trimmed.find("println(") != std::string::npos) {
                        // It's a statement, don't wrap
                        wrapped += lines[i] + "\n";
                    } else {
                        // It's an expression, wrap with println
                        wrapped += "println(" + trimmed + ")\n";
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

bool JuliaExecutor::execute(const std::string& code) {
    // Check sandbox permissions
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Julia execution denied\n");
        sandbox->logViolation("execute_julia", code, "BLOCK_CALL capability required");
        throw std::runtime_error("Julia execution denied by sandbox");
    }

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    auto thread_id = std::this_thread::get_id();
    int counter = temp_file_counter_.fetch_add(1);
    std::ostringstream filename_base;
    filename_base << "naab_julia_" << thread_id << "_" << counter;

    std::filesystem::path temp_jl = temp_dir / (filename_base.str() + ".jl");

    try {
        std::string julia_code = wrapJuliaCode(code, false);

        std::ofstream ofs(temp_jl);
        if (!ofs.is_open()) {
            fmt::print("[ERROR] Failed to create temp Julia file\n");
            return false;
        }
        ofs << julia_code;
        ofs.close();

        // Execute with julia
        std::string exec_stdout, exec_stderr;
        std::vector<std::string> exec_args = {temp_jl.string()};

        int exec_exit = execute_subprocess_with_pipes(
            "julia", exec_args, exec_stdout, exec_stderr, nullptr
        );

        stdout_buffer_.append(exec_stdout);
        if (!exec_stderr.empty()) {
            stderr_buffer_.append(exec_stderr);
        }

        // Clean up
        std::filesystem::remove(temp_jl);

        if (exec_exit != 0) {
            fmt::print("[ERROR] Julia program failed (exit code {})\n", exec_exit);
        }

        return (exec_exit == 0);

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Julia execution failed: {}\n", e.what());
        std::filesystem::remove(temp_jl);
        return false;
    }
}

std::shared_ptr<interpreter::Value> JuliaExecutor::executeWithReturn(
    const std::string& code) {

    // Check sandbox permissions
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Julia execution denied\n");
        sandbox->logViolation("execute_julia_return", code, "BLOCK_CALL capability required");
        throw std::runtime_error("Julia execution denied by sandbox");
    }

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    auto thread_id = std::this_thread::get_id();
    int counter = temp_file_counter_.fetch_add(1);
    std::ostringstream filename_base;
    filename_base << "naab_julia_" << thread_id << "_" << counter;

    std::filesystem::path temp_jl = temp_dir / (filename_base.str() + "_ret.jl");

    try {
        std::string julia_code = wrapJuliaCode(code, true);

        std::ofstream ofs(temp_jl);
        if (!ofs.is_open()) {
            return std::make_shared<interpreter::Value>();
        }
        ofs << julia_code;
        ofs.close();

        // Execute
        std::string exec_stdout, exec_stderr;
        int exec_exit = execute_subprocess_with_pipes(
            "julia", {temp_jl.string()},
            exec_stdout, exec_stderr, nullptr
        );

        if (exec_exit != 0) {
            std::string error_msg = exec_stderr;
            std::filesystem::remove(temp_jl);

            // Add helpful hints for common Julia errors
            if (error_msg.find("UndefVarError") != std::string::npos ||
                error_msg.find("not defined") != std::string::npos) {
                fmt::print("\n  Hint: Julia variable not defined.\n"
                           "  - Check spelling and imports\n"
                           "  - Julia is case-sensitive\n"
                           "  - Use 'using' to import packages: using Statistics\n"
                           "  - Variables must be defined before use\n\n");
            } else if (error_msg.find("MethodError") != std::string::npos ||
                       error_msg.find("no method matching") != std::string::npos) {
                fmt::print("\n  Hint: Julia method error.\n"
                           "  - Check function argument types\n"
                           "  - Julia uses multiple dispatch - methods are matched by types\n"
                           "  - Use convert() for explicit type conversion\n"
                           "  - Example: convert(Float64, 5) converts int to float\n\n");
            } else if (error_msg.find("LoadError") != std::string::npos ||
                       error_msg.find("ArgumentError") != std::string::npos) {
                fmt::print("\n  Hint: Julia package or argument error.\n"
                           "  - Install packages: using Pkg; Pkg.add(\"PackageName\")\n"
                           "  - Standard library: Statistics, LinearAlgebra, Dates, etc.\n"
                           "  - Check function arguments match expected types\n\n");
            } else if (error_msg.find("syntax") != std::string::npos) {
                fmt::print("\n  Hint: Julia syntax error.\n"
                           "  - Julia uses 'end' to close blocks (not braces)\n"
                           "  - Use '=' for assignment, '==' for comparison\n"
                           "  - Arrays use 1-based indexing: arr[1] (not arr[0])\n"
                           "  - Use 'function' keyword to define functions\n\n");
            } else if (error_msg.find("BoundsError") != std::string::npos) {
                fmt::print("\n  Hint: Julia array index out of bounds.\n"
                           "  - Julia uses 1-based indexing\n"
                           "  - Use length() to get array size\n"
                           "  - Use eachindex() to iterate safely\n\n");
            }

            throw std::runtime_error(
                "Julia execution failed:\n" + error_msg +
                "\n  Code preview:\n    " + julia_code.substr(0, std::min(julia_code.size(), size_t(200))));
        }

        // Print output
        if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
        if (!exec_stderr.empty()) fmt::print("[Julia stderr]: {}", exec_stderr);

        // Cleanup
        std::filesystem::remove(temp_jl);

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
        std::filesystem::remove(temp_jl);
        throw;
    }
}

std::shared_ptr<interpreter::Value> JuliaExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "exec" && !args.empty()) {
        if (auto* str_val = std::get_if<std::string>(&args[0]->data)) {
            bool success = execute(*str_val);
            return std::make_shared<interpreter::Value>(success);
        }
    }

    throw std::runtime_error("JuliaExecutor only supports 'exec(code_string)'");
}

std::string JuliaExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string errors = stderr_buffer_.getAndClear();
    if (!errors.empty()) {
        output += "\n[Julia stderr]: " + errors;
    }
    return output;
}

} // namespace runtime
} // namespace naab
