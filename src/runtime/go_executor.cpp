#include "naab/interpreter.h"
#include "naab/go_executor.h"
#include "naab/subprocess_helpers.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <fmt/core.h>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace naab {
namespace runtime {

std::atomic<int> GoExecutor::temp_file_counter_(0);

// Termux-aware temp directory
static std::filesystem::path getGoSafeTempDir() {
    try {
        auto dir = std::filesystem::temp_directory_path();
        if (std::filesystem::is_directory(dir)) return dir;
    } catch (...) {}
    auto fallback = std::filesystem::path("/data/data/com.termux/files/home/.cache/naab");
    std::filesystem::create_directories(fallback);
    return fallback;
}

GoExecutor::GoExecutor() {}

std::string GoExecutor::wrapGoCode(const std::string& code, bool for_return) {
    // If code already has package main, return as-is
    if (code.find("package main") != std::string::npos) {
        return code;
    }

    // If code has func main(), user wrote their own structure — just prepend package main
    if (code.find("func main()") != std::string::npos) {
        return "package main\n" + code;
    }

    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Build grouped import block based on packages used in code
    std::string imports = "import (\n\t\"fmt\"\n";
    if (code.find("time.") != std::string::npos) imports += "\t\"time\"\n";
    if (code.find("strings.") != std::string::npos) imports += "\t\"strings\"\n";
    if (code.find("strconv.") != std::string::npos) imports += "\t\"strconv\"\n";
    if (code.find("math.") != std::string::npos || code.find("math/") != std::string::npos) imports += "\t\"math\"\n";
    if (code.find("json.") != std::string::npos) imports += "\t\"encoding/json\"\n";
    if (code.find("sha256.") != std::string::npos) imports += "\t\"crypto/sha256\"\n";
    if (code.find("hex.") != std::string::npos) imports += "\t\"encoding/hex\"\n";
    if (code.find("regexp.") != std::string::npos) imports += "\t\"regexp\"\n";
    if (code.find("sort.") != std::string::npos) imports += "\t\"sort\"\n";
    imports += ")\n";

    // Helper: check if a line is a user import statement (skip when wrapping)
    auto isImportLine = [](const std::string& l) -> bool {
        auto pos = l.find_first_not_of(" \t");
        if (pos == std::string::npos) return false;
        return l.substr(pos, 7) == "import ";
    };

    if (!for_return) {
        // For execute(): wrap in package main + func main
        std::string wrapped = "package main\n" + imports + "func main() {\n\t_ = fmt.Sprintf(\"\")\n";
        for (const auto& l : lines) {
            if (isImportLine(l)) continue;  // Skip user imports — already auto-added
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
        return "package main\n" + imports + "func main() {\n\tfmt.Println(" + expr + ")\n}\n";
    }

    // Multi-line: wrap in main, print last expression
    std::string wrapped = "package main\n" + imports + "func main() {\n";
    for (size_t i = 0; i < lines.size(); i++) {
        if (static_cast<int>(i) == last_line_idx) {
            std::string trimmed = lines[i];
            size_t s = trimmed.find_first_not_of(" \t\r");
            if (s != std::string::npos) {
                trimmed = trimmed.substr(s);
                wrapped += "\tfmt.Println(" + trimmed + ")\n";
            }
        } else {
            if (isImportLine(lines[i])) continue;  // Skip user imports
            wrapped += "\t" + lines[i] + "\n";
        }
    }
    wrapped += "}\n";
    return wrapped;
}

bool GoExecutor::execute(const std::string& code) {
    // V-RCE-004: mkdtemp for exclusive, unpredictable temp directory
    std::string tmpl = (getGoSafeTempDir() / "naab_go_XXXXXX").string();
    char* raw_dir = mkdtemp(tmpl.data());
    if (!raw_dir) {
        fmt::print("[ERROR] Failed to create secure temp directory\n");
        return false;
    }
    chmod(raw_dir, 0700);
    std::filesystem::path compile_dir(raw_dir);
    std::filesystem::path temp_go = compile_dir / "src.go";
    std::filesystem::path temp_bin = compile_dir / "bin";

    try {
        std::string go_code = wrapGoCode(code, false);

        std::ofstream ofs(temp_go);
        if (!ofs.is_open()) {
            std::filesystem::remove_all(compile_dir);
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
            std::filesystem::remove_all(compile_dir);
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

        // Cleanup secure compile dir
        std::filesystem::remove_all(compile_dir);

        if (exec_exit != 0) {
            fmt::print("[ERROR] Go program failed (exit code {})\n", exec_exit);
        }

        return (exec_exit == 0);

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Go execution failed: {}\n", e.what());
        std::filesystem::remove_all(compile_dir);
        return false;
    }
}

interpreter::NaabVal GoExecutor::executeWithReturn(
    const std::string& code) {

    // V-RCE-004: mkdtemp for exclusive, unpredictable temp directory
    std::string tmpl_r = (getGoSafeTempDir() / "naab_go_XXXXXX").string();
    char* raw_dir_r = mkdtemp(tmpl_r.data());
    if (!raw_dir_r) {
        fmt::print("[ERROR] Failed to create secure temp directory\n");
        return interpreter::NaabVal::makeNull();
    }
    chmod(raw_dir_r, 0700);
    std::filesystem::path compile_dir_r(raw_dir_r);
    std::filesystem::path temp_go = compile_dir_r / "src.go";
    std::filesystem::path temp_bin = compile_dir_r / "bin";

    try {
        std::string go_code = wrapGoCode(code, true);

        std::ofstream ofs(temp_go);
        if (!ofs.is_open()) {
            std::filesystem::remove_all(compile_dir_r);
            return interpreter::NaabVal::makeNull();
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
            std::filesystem::remove_all(compile_dir_r);
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

        // Buffer only log output (strip last line = return value)
        // The return value is the last line of stdout (wrapGoCode wraps it in Println)
        if (!exec_stdout.empty()) {
            auto last_nl = exec_stdout.rfind('\n', exec_stdout.size() - 2);
            if (last_nl != std::string::npos) {
                stdout_buffer_.append(exec_stdout.substr(0, last_nl + 1));
            }
        }
        if (!exec_stderr.empty()) stderr_buffer_.append(exec_stderr);

        // Cleanup secure compile dir
        std::filesystem::remove_all(compile_dir_r);

        // Trim trailing whitespace/newlines
        std::string result = exec_stdout;
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t')) {
            result.pop_back();
        }

        // Empty result → null
        if (result.empty()) {
            return interpreter::NaabVal::makeNull();
        }

        // Polyglot output is always a string — no implicit type coercion
        return interpreter::NaabVal::makeString(result);

    } catch (const std::exception& e) {
        std::filesystem::remove_all(compile_dir_r);
        throw;
    }
}

interpreter::NaabVal GoExecutor::callFunction(
    const std::string& function_name,
    const std::vector<interpreter::NaabVal>& args) {

    if (function_name == "exec" && !args.empty()) {
        if (args[0].isString()) {
            bool success = execute(args[0].asString());
            return interpreter::NaabVal::makeBool(success);
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
