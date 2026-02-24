#include "naab/interpreter.h"
#include "naab/go_executor.h"
#include "naab/subprocess_helpers.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <fmt/core.h>

namespace naab {
namespace runtime {

std::atomic<int> GoExecutor::temp_file_counter_(0);

GoExecutor::GoExecutor() {}

std::string GoExecutor::wrapGoCode(const std::string& code, bool for_return) {
    // If code already has package main, return as-is
    if (code.find("package main") != std::string::npos) {
        return code;
    }

    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    if (!for_return) {
        // For execute(): wrap in package main + func main
        std::string wrapped = "package main\nimport \"fmt\"\nfunc main() {\n\t_ = fmt.Sprintf(\"\")\n";
        for (const auto& l : lines) {
            wrapped += "\t" + l + "\n";
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
        return "package main\nimport \"fmt\"\nfunc main() {\n\tfmt.Println(" + expr + ")\n}\n";
    }

    // Multi-line: wrap in main, print last expression
    std::string wrapped = "package main\nimport \"fmt\"\nfunc main() {\n";
    for (size_t i = 0; i < lines.size(); i++) {
        if (static_cast<int>(i) == last_line_idx) {
            std::string trimmed = lines[i];
            size_t s = trimmed.find_first_not_of(" \t\r");
            if (s != std::string::npos) {
                trimmed = trimmed.substr(s);
                wrapped += "\tfmt.Println(" + trimmed + ")\n";
            }
        } else {
            wrapped += "\t" + lines[i] + "\n";
        }
    }
    wrapped += "}\n";
    return wrapped;
}

bool GoExecutor::execute(const std::string& code) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    auto thread_id = std::this_thread::get_id();
    int counter = temp_file_counter_.fetch_add(1);
    std::ostringstream filename_base;
    filename_base << "naab_go_" << thread_id << "_" << counter;

    std::filesystem::path temp_go = temp_dir / (filename_base.str() + ".go");
    std::filesystem::path temp_bin = temp_dir / (filename_base.str() + "_bin");

    try {
        std::string go_code = wrapGoCode(code, false);

        std::ofstream ofs(temp_go);
        if (!ofs.is_open()) {
            fmt::print("[ERROR] Failed to create temp Go file\n");
            return false;
        }
        ofs << go_code;
        ofs.close();

        // Compile with go build
        std::string compile_stdout, compile_stderr;
        std::vector<std::string> compile_args = {
            "build", "-o", temp_bin.string(), temp_go.string()
        };

        int compile_exit = execute_subprocess_with_pipes(
            "go", compile_args, compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            fmt::print("[ERROR] Go compilation failed (exit code {})\n", compile_exit);
            stderr_buffer_.append(compile_stderr);
            std::filesystem::remove(temp_go);
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
        std::filesystem::remove(temp_go);
        std::filesystem::remove(temp_bin);

        if (exec_exit != 0) {
            fmt::print("[ERROR] Go program failed (exit code {})\n", exec_exit);
        }

        return (exec_exit == 0);

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Go execution failed: {}\n", e.what());
        std::filesystem::remove(temp_go);
        std::filesystem::remove(temp_bin);
        return false;
    }
}

std::shared_ptr<interpreter::Value> GoExecutor::executeWithReturn(
    const std::string& code) {

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    auto thread_id = std::this_thread::get_id();
    int counter = temp_file_counter_.fetch_add(1);
    std::ostringstream filename_base;
    filename_base << "naab_go_" << thread_id << "_" << counter;

    std::filesystem::path temp_go = temp_dir / (filename_base.str() + "_ret.go");
    std::filesystem::path temp_bin = temp_dir / (filename_base.str() + "_ret_bin");

    try {
        std::string go_code = wrapGoCode(code, true);

        std::ofstream ofs(temp_go);
        if (!ofs.is_open()) {
            return std::make_shared<interpreter::Value>();
        }
        ofs << go_code;
        ofs.close();

        // Compile
        std::string compile_stdout, compile_stderr;
        int compile_exit = execute_subprocess_with_pipes(
            "go", {"build", "-o", temp_bin.string(), temp_go.string()},
            compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            std::string error_msg = compile_stderr;
            std::filesystem::remove(temp_go);
            throw std::runtime_error(
                "Go compilation failed:\n" + error_msg +
                "\n  Code preview:\n    " + go_code.substr(0, std::min(go_code.size(), size_t(200))));
        }

        // Execute
        std::string exec_stdout, exec_stderr;
        execute_subprocess_with_pipes(
            temp_bin.string(), {},
            exec_stdout, exec_stderr, nullptr
        );

        // Print output
        if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
        if (!exec_stderr.empty()) fmt::print("[Go stderr]: {}", exec_stderr);

        // Cleanup
        std::filesystem::remove(temp_go);
        std::filesystem::remove(temp_bin);

        // Trim trailing whitespace/newlines
        std::string result = exec_stdout;
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t')) {
            result.pop_back();
        }

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
        std::filesystem::remove(temp_go);
        std::filesystem::remove(temp_bin);
        throw;
    }
}

std::shared_ptr<interpreter::Value> GoExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "exec" && !args.empty()) {
        if (auto* str_val = std::get_if<std::string>(&args[0]->data)) {
            bool success = execute(*str_val);
            return std::make_shared<interpreter::Value>(success);
        }
    }

    throw std::runtime_error("GoExecutor only supports 'exec(code_string)'");
}

std::string GoExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string errors = stderr_buffer_.getAndClear();
    if (!errors.empty()) {
        output += "\n[Go stderr]: " + errors;
    }
    return output;
}

} // namespace runtime
} // namespace naab
