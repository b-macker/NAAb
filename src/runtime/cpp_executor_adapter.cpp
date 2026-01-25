// NAAb C++ Executor Adapter Implementation
// Adapts CppExecutor to the Executor interface

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/cpp_executor_adapter.h"
#include "naab/block_enricher.h"
#include "naab/subprocess_helpers.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace naab {
namespace runtime {

CppExecutorAdapter::CppExecutorAdapter()
    : block_counter_(0) {
    fmt::print("[CPP ADAPTER] C++ executor adapter initialized\n");
}

bool CppExecutorAdapter::execute(const std::string& code) {
    // Check if code has main() function - if so, compile and execute as program
    if (code.find("int main(") != std::string::npos ||
        code.find("int main (") != std::string::npos) {

        fmt::print("[CPP ADAPTER] Detected main() - compiling as executable\n");

        // Create temp source and binary files
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        std::filesystem::path temp_cpp = temp_dir / "naab_temp_cpp.cpp";
        std::filesystem::path temp_bin = temp_dir / "naab_temp_cpp";

        // Write code to temp file
        std::ofstream ofs(temp_cpp);
        if (!ofs.is_open()) {
            fmt::print("[ERROR] Failed to create temp C++ source file\n");
            return false;
        }
        ofs << code;
        ofs.close();

        // Compile as executable
        std::string compile_stdout, compile_stderr;
        int compile_exit = execute_subprocess_with_pipes(
            "g++",
            {temp_cpp.string(), "-o", temp_bin.string(), "-std=c++17"},
            compile_stdout,
            compile_stderr,
            nullptr
        );

        if (compile_exit != 0) {
            fmt::print("[ERROR] C++ compilation failed:\n{}\n", compile_stderr);
            std::filesystem::remove(temp_cpp);
            return false;
        }

        // Execute the binary
        std::string exec_stdout, exec_stderr;
        int exec_exit = execute_subprocess_with_pipes(
            temp_bin.string(),
            {},
            exec_stdout,
            exec_stderr,
            nullptr
        );

        // Store output in buffer
        captured_output_ = exec_stdout;
        if (!exec_stderr.empty()) {
            captured_output_ += "\n[C++ stderr]: " + exec_stderr;
        }

        // Cleanup
        std::filesystem::remove(temp_cpp);
        std::filesystem::remove(temp_bin);

        bool success = (exec_exit == 0);
        if (success) {
            fmt::print("[SUCCESS] C++ program executed (exit code {})\n", exec_exit);
        } else {
            fmt::print("[ERROR] C++ program failed with code {}\n", exec_exit);
        }

        return success;
    }

    // Otherwise, wrap in main() with headers and execute
    fmt::print("[CPP ADAPTER] Wrapping C++ code for execution\n");

    // Phase 2.3: Ensure statements have semicolons
    std::string processed_code = code;

    // Split into lines and add semicolons if needed
    std::vector<std::string> lines;
    std::istringstream stream(processed_code);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r");
        size_t end = line.find_last_not_of(" \t\r");
        if (start != std::string::npos && end != std::string::npos) {
            std::string trimmed = line.substr(start, end - start + 1);
            // Add semicolon if line doesn't have one and isn't empty
            if (!trimmed.empty() && trimmed.back() != ';' && trimmed.back() != '{' && trimmed.back() != '}') {
                lines.push_back("    " + trimmed + ";");
            } else {
                lines.push_back("    " + trimmed);
            }
        } else if (!line.empty()) {
            lines.push_back(line);
        }
    }

    std::string code_with_semicolons;
    for (const auto& l : lines) {
        code_with_semicolons += l + "\n";
    }

    // Wrap code in main() function with common headers
    std::string wrapped_code =
        "#include <iostream>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "#include <map>\n"
        "#include <set>\n"
        "#include <algorithm>\n"
        "int main() {\n"
        + code_with_semicolons +
        "    return 0;\n"
        "}\n";

    // Create temp source and binary files
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_cpp = temp_dir / "naab_temp_cpp_exec.cpp";
    std::filesystem::path temp_bin = temp_dir / "naab_temp_cpp_exec";

    // Write wrapped code to temp file
    std::ofstream ofs(temp_cpp);
    if (!ofs.is_open()) {
        fmt::print("[ERROR] Failed to create temp C++ source file\n");
        return false;
    }
    ofs << wrapped_code;
    ofs.close();

    // Compile as executable
    std::string compile_stdout, compile_stderr;
    int compile_exit = execute_subprocess_with_pipes(
        "g++",
        {temp_cpp.string(), "-o", temp_bin.string(), "-std=c++17"},
        compile_stdout,
        compile_stderr,
        nullptr
    );

    if (compile_exit != 0) {
        fmt::print("[ERROR] C++ compilation failed:\n{}\n", compile_stderr);
        std::filesystem::remove(temp_cpp);
        return false;
    }

    // Execute the binary
    std::string exec_stdout, exec_stderr;
    int exec_exit = execute_subprocess_with_pipes(
        temp_bin.string(),
        {},
        exec_stdout,
        exec_stderr,
        nullptr
    );

    // Print output immediately
    if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
    if (!exec_stderr.empty()) fmt::print("[C++ stderr]: {}", exec_stderr);

    // Store output in buffer
    captured_output_ = exec_stdout;
    if (!exec_stderr.empty()) {
        captured_output_ += "\n[C++ stderr]: " + exec_stderr;
    }

    // Cleanup
    std::filesystem::remove(temp_cpp);
    std::filesystem::remove(temp_bin);

    bool success = (exec_exit == 0);
    if (success) {
        fmt::print("[CPP ADAPTER] C++ code executed successfully\n");
    } else {
        fmt::print("[ERROR] C++ execution failed with code {}\n", exec_exit);
    }

    return success;
}

// Phase 2.3: Execute code and return the result value
std::shared_ptr<interpreter::Value> CppExecutorAdapter::executeWithReturn(
    const std::string& code) {

    // For C++ with main(), compile and execute
    if (code.find("int main(") != std::string::npos ||
        code.find("int main (") != std::string::npos) {

        fmt::print("[CPP ADAPTER] Compiling C++ with return value capture\n");

        // Phase 3.3.1: Check cache
        std::string cached_binary_main = cache_.getCachedBinary("cpp", code);
        std::filesystem::path temp_bin_main;

        if (!cached_binary_main.empty()) {
            // Cache hit
            fmt::print("[CPP ADAPTER] Using cached binary\n");
            temp_bin_main = cached_binary_main;
        } else {
            // Cache miss - compile
            fmt::print("[CPP ADAPTER] Compiling (cache miss)\n");

            std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
            std::filesystem::path temp_cpp = temp_dir / "naab_temp_cpp_ret.cpp";
            temp_bin_main = temp_dir / "naab_temp_cpp_ret";

            std::ofstream ofs(temp_cpp);
            if (!ofs.is_open()) {
                fmt::print("[ERROR] Failed to create temp C++ file\n");
                return std::make_shared<interpreter::Value>();
            }
            ofs << code;
            ofs.close();

            // Compile
            std::string compile_stdout, compile_stderr;
            int compile_exit = execute_subprocess_with_pipes(
                "g++",
                {temp_cpp.string(), "-o", temp_bin_main.string(), "-std=c++17"},
                compile_stdout, compile_stderr, nullptr
            );

            if (compile_exit != 0) {
                fmt::print("[ERROR] C++ compilation failed:\n{}\n", compile_stderr);
                std::filesystem::remove(temp_cpp);
                return std::make_shared<interpreter::Value>();
            }

            // Store in cache
            cache_.storeBinary("cpp", code, temp_bin_main.string(), temp_cpp.string());

            // Cleanup temp source
            std::filesystem::remove(temp_cpp);
        }

        // Execute
        std::string exec_stdout, exec_stderr;
        int exec_exit = execute_subprocess_with_pipes(
            temp_bin_main.string(), {}, exec_stdout, exec_stderr, nullptr
        );

        // Print output
        if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
        if (!exec_stderr.empty()) fmt::print("[C++ stderr]: {}", exec_stderr);

        // Phase 3.3.1: Only cleanup if not cached
        if (cached_binary_main.empty()) {
            std::filesystem::remove(temp_bin_main);
        }

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
    }

    // For expression blocks (no main), check if it's a statement or expression
    std::string trimmed = code;
    size_t start = trimmed.find_first_not_of(" \t\n\r");
    size_t end = trimmed.find_last_not_of(" \t\n\r;");
    if (start != std::string::npos && end != std::string::npos) {
        trimmed = trimmed.substr(start, end - start + 1);
    }

    // Phase 2.3: For multi-line code, check if LAST line is an expression
    // Don't reject entire block just because earlier lines have assignments
    bool is_multiline = (trimmed.find('\n') != std::string::npos);
    std::string check_for_statement = trimmed;

    if (is_multiline) {
        // Extract last non-empty line for checking
        auto lines_vec = std::vector<std::string>{};
        std::istringstream ss(trimmed);
        std::string line;
        while (std::getline(ss, line)) {
            size_t s = line.find_first_not_of(" \t\r");
            if (s != std::string::npos) {
                lines_vec.push_back(line);
            }
        }
        if (!lines_vec.empty()) {
            check_for_statement = lines_vec.back();
        }
    }

    // Check if this is a statement (not an expression that returns a value)
    // Statements: std::cout, assignments, declarations, etc.
    bool is_statement = (check_for_statement.find("std::cout") != std::string::npos ||
                         check_for_statement.find("std::cerr") != std::string::npos ||
                         check_for_statement.find("printf") != std::string::npos ||
                         check_for_statement.find("=") != std::string::npos ||  // Assignment
                         check_for_statement.find("for") == 0 ||
                         check_for_statement.find("while") == 0 ||
                         check_for_statement.find("if") == 0);

    // If it's a statement, execute it without trying to capture return value
    if (is_statement && check_for_statement.find("return") != 0) {
        fmt::print("[CPP ADAPTER] Detected statement (not expression), executing without return\n");
        execute(code);
        return std::make_shared<interpreter::Value>();  // Return null/void
    }

    fmt::print("[CPP ADAPTER] Wrapping C++ expression for return value\n");

    // Strip leading/trailing whitespace and "return" keyword if present
    std::string expr = code;
    // Trim whitespace
    start = expr.find_first_not_of(" \t\n\r");
    end = expr.find_last_not_of(" \t\n\r;");
    if (start != std::string::npos && end != std::string::npos) {
        expr = expr.substr(start, end - start + 1);
    }

    // Remove "return" keyword if present
    if (expr.substr(0, 6) == "return") {
        expr = expr.substr(6);
        // Trim whitespace after "return"
        start = expr.find_first_not_of(" \t\n\r");
        if (start != std::string::npos) {
            expr = expr.substr(start);
        }
    }

    // Remove trailing semicolon if present
    if (!expr.empty() && expr.back() == ';') {
        expr.pop_back();
    }

    // Phase 2.3: Multi-line code support
    // Check if expression contains newlines (multi-statement code)
    std::string wrapped_code;
    if (expr.find('\n') != std::string::npos) {
        // Multi-line code: wrap in main() and print last statement
        // Find the last non-empty line
        std::vector<std::string> lines;
        std::istringstream stream(expr);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }

        int last_line_idx = -1;
        for (int i = lines.size() - 1; i >= 0; i--) {
            std::string trimmed = lines[i];
            size_t s = trimmed.find_first_not_of(" \t\r");
            if (s != std::string::npos) {
                last_line_idx = i;
                break;
            }
        }

        wrapped_code =
            "#include <iostream>\n"
            "#include <string>\n"
            "#include <vector>\n"
            "#include <map>\n"
            "int main() {\n";

        // Add all lines except the last
        for (size_t i = 0; i < lines.size(); i++) {
            if (static_cast<int>(i) == last_line_idx) {
                // Print the last expression
                wrapped_code += "    std::cout << (" + lines[i] + ");\n";
            } else {
                wrapped_code += "    " + lines[i] + "\n";
            }
        }

        wrapped_code +=
            "    return 0;\n"
            "}\n";
    } else {
        // Single-line expression: wrap and print
        wrapped_code =
            "#include <iostream>\n"
            "#include <string>\n"
            "#include <vector>\n"
            "#include <map>\n"
            "int main() {\n"
            "    auto result = (" + expr + ");\n"
            "    std::cout << result;\n"
            "    return 0;\n"
            "}\n";
    }

    // Phase 3.3.1: Check cache before compiling
    std::string cached_binary = cache_.getCachedBinary("cpp", wrapped_code);
    std::filesystem::path temp_bin;

    if (!cached_binary.empty()) {
        // Cache hit - use cached binary
        fmt::print("[CPP ADAPTER] Using cached binary\n");
        temp_bin = cached_binary;
    } else {
        // Cache miss - compile and cache
        fmt::print("[CPP ADAPTER] Compiling C++ code (cache miss)\n");

        std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
        std::filesystem::path temp_cpp = temp_dir / "naab_temp_cpp_expr.cpp";
        temp_bin = temp_dir / "naab_temp_cpp_expr";

        std::ofstream ofs(temp_cpp);
        if (!ofs.is_open()) {
            fmt::print("[ERROR] Failed to create temp C++ file\n");
            return std::make_shared<interpreter::Value>();
        }
        ofs << wrapped_code;
        ofs.close();

        // Compile
        std::string compile_stdout, compile_stderr;
        int compile_exit = execute_subprocess_with_pipes(
            "g++",
            {temp_cpp.string(), "-o", temp_bin.string(), "-std=c++17"},
            compile_stdout, compile_stderr, nullptr
        );

        if (compile_exit != 0) {
            fmt::print("[ERROR] C++ compilation failed:\n{}\n", compile_stderr);
            std::filesystem::remove(temp_cpp);
            return std::make_shared<interpreter::Value>();
        }

        // Store in cache
        cache_.storeBinary("cpp", wrapped_code, temp_bin.string(), temp_cpp.string());

        // Cleanup temp source (binary will be cached)
        std::filesystem::remove(temp_cpp);
    }

    // Execute
    std::string exec_stdout, exec_stderr;
    int exec_exit = execute_subprocess_with_pipes(
        temp_bin.string(), {}, exec_stdout, exec_stderr, nullptr
    );

    // Phase 3.3.1: Only cleanup if not using cached binary
    if (cached_binary.empty()) {
        // We compiled a temp binary, clean it up
        std::filesystem::remove(temp_bin);
    }
    // temp_cpp was already removed above if it was created

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
}

std::shared_ptr<interpreter::Value> CppExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (current_block_id_.empty()) {
        throw std::runtime_error("No C++ block loaded. Call execute() first.");
    }

    fmt::print("[CPP ADAPTER] Calling function: {}\n", function_name);

    // Call function in the current block
    return executor_.callFunction(current_block_id_, function_name, args);
}

bool CppExecutorAdapter::isInitialized() const {
    // C++ executor is always initialized (no runtime needed)
    return true;
}

std::string CppExecutorAdapter::getCapturedOutput() {
    // Return captured output from inline main() execution
    std::string output = captured_output_;
    captured_output_.clear();  // Clear after retrieval
    return output;
}

} // namespace runtime
} // namespace naab
