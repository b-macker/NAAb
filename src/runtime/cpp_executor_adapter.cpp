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
#include <thread>
#include <regex>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace naab {
namespace runtime {

// Helper: Get temp directory with Termux fallback
static std::filesystem::path getSafeTempDir() {
    try {
        auto dir = std::filesystem::temp_directory_path();
        if (std::filesystem::is_directory(dir)) return dir;
    } catch (...) {}
    // Termux fallback
    auto fallback = std::filesystem::path("/data/data/com.termux/files/home/.cache/naab");
    std::filesystem::create_directories(fallback);
    return fallback;
}

// Helper: Add hints for common C++ polyglot compilation errors
// Truncate C++ compiler output to show only error lines (not verbose notes)
static std::string truncateCppErrors(const std::string& stderr_output, size_t max_lines = 20) {
    std::string result;
    std::istringstream stream(stderr_output);
    std::string line;
    size_t error_lines = 0;
    size_t total_lines = 0;
    bool in_note = false;

    while (std::getline(stream, line)) {
        total_lines++;
        // Show error and warning lines, skip verbose "note:" lines
        if (line.find("error:") != std::string::npos ||
            line.find("warning:") != std::string::npos) {
            in_note = false;
            if (error_lines < max_lines) {
                result += line + "\n";
                error_lines++;
            }
        } else if (line.find("note:") != std::string::npos) {
            in_note = true;
            // Skip notes - they're too verbose
        } else if (!in_note && error_lines < max_lines) {
            // Show context lines (source code snippets) but not note context
            result += line + "\n";
            error_lines++;
        }
    }

    if (total_lines > error_lines + 5) {
        result += fmt::format("  ({} lines of compiler notes hidden)\n", total_lines - error_lines);
    }

    return result;
}

static std::string addCppCompileHints(const std::string& stderr_output) {
    std::string hints;

    // Detect quote/escaping issues
    if (stderr_output.find("expected expression") != std::string::npos &&
        (stderr_output.find("\\\"") != std::string::npos || stderr_output.find("<<") != std::string::npos)) {
        hints += "\n  Hint: Quote escaping issue in C++ polyglot block.\n"
                 "  Escaped quotes (\\\" ) can get mangled inside <<cpp ... >> blocks.\n"
                 "  Workarounds:\n"
                 "    1. Use C++ raw strings:  R\"(\"key\": \"value\")\" \n"
                 "    2. Use a quote variable:  std::string q = \"\\\"\"; then q + \"key\" + q\n"
                 "    3. Build JSON with char:  char q = '\"'; ss << q << \"key\" << q;\n";
    }

    // Detect return type mismatch (common: returning string from int function)
    if (stderr_output.find("no viable conversion") != std::string::npos &&
        stderr_output.find("return type") != std::string::npos) {
        hints += "\n  Hint: Don't use 'return' in <<cpp>> blocks.\n"
                 "  The last expression is automatically the return value.\n"
                 "  Instead of:  return result;\n"
                 "  Just write:  result\n";
    }

    // Detect return statement issues (polyglot blocks use last expression, not return)
    if (stderr_output.find("return") != std::string::npos &&
        stderr_output.find("void function") != std::string::npos) {
        hints += "\n  Hint: Don't use 'return' in <<cpp>> blocks.\n"
                 "  The last expression is automatically the return value.\n"
                 "  Instead of:  return result;\n"
                 "  Just write:  result\n";
    }

    // Detect lambda capture issues
    if (stderr_output.find("cannot be implicitly captured") != std::string::npos ||
        stderr_output.find("capture-default") != std::string::npos) {
        hints += "\n  Hint: Lambda needs to capture outer variables.\n"
                 "  Change [] to [&] for capture-by-reference:\n"
                 "    auto fn = [&](args) { ... };  // captures all outer vars\n";
    }

    return hints;
}

// Initialize static temp file counter (kept for ABI compatibility; no longer used for naming)
std::atomic<int> CppExecutorAdapter::temp_file_counter_(0);

// V-RCE-005/V-RCE-006: pre-compilation scanner — reject dangerous compile-time directives.
// V-RCE-006 hardens the original scanner to catch angle-bracket includes and line-splice
// obfuscation that bypassed the R16 regex.
static bool isCppSourceSafe(const std::string& code, std::string& reason) {
    // V-RCE-008: Strip CRLF (\r) so line-splicing works for Windows line endings,
    // then collapse line continuations, then strip C-style block comments.
    std::string processed;
    processed.reserve(code.size());
    // Step 1: strip \r (CRLF → LF)
    for (char c : code) {
        if (c != '\r') processed += c;
    }
    // Step 2: collapse line continuations (\<newline>)
    std::string spliced;
    spliced.reserve(processed.size());
    for (size_t i = 0; i < processed.size(); i++) {
        if (processed[i] == '\\' && i + 1 < processed.size() && processed[i + 1] == '\n') {
            i++;  // drop both the backslash and the newline
        } else {
            spliced += processed[i];
        }
    }
    // Step 3: strip C-style block comments (/* ... */)
    processed.clear();
    for (size_t i = 0; i < spliced.size(); i++) {
        if (i + 1 < spliced.size() && spliced[i] == '/' && spliced[i + 1] == '*') {
            i += 2;
            while (i + 1 < spliced.size() && !(spliced[i] == '*' && spliced[i + 1] == '/')) i++;
            if (i + 1 < spliced.size()) i++;  // skip closing */
        } else {
            processed += spliced[i];
        }
    }

    // Reject absolute-path double-quote #include (e.g. #include "/etc/shadow")
    std::regex abs_include_dq(R"(#\s*include\s*"/)");
    if (std::regex_search(processed, abs_include_dq)) {
        reason = "absolute-path #include (double-quote) is not permitted in polyglot C++ blocks";
        return false;
    }
    // V-RCE-006: also reject angle-bracket absolute #include (e.g. #include </etc/shadow>)
    std::regex abs_include_ab(R"(#\s*include\s*</)");
    if (std::regex_search(processed, abs_include_ab)) {
        reason = "absolute-path #include (angle-bracket) is not permitted in polyglot C++ blocks";
        return false;
    }
    // V-RCE-014: Reject relative path traversal in #include (e.g. #include "../../../etc/shadow")
    // Blocks ".." anywhere in double-quote or angle-bracket include paths.
    // g++ runs without containment, so traversal from mkdtemp dir leaks file contents
    // via compiler errors or successful compilation.
    std::regex traversal_include_dq(R"(#\s*include\s*"[^"]*\.\.)");
    if (std::regex_search(processed, traversal_include_dq)) {
        reason = "path traversal in #include is not permitted in polyglot C++ blocks";
        return false;
    }
    std::regex traversal_include_ab(R"(#\s*include\s*<[^>]*\.\.)");
    if (std::regex_search(processed, traversal_include_ab)) {
        reason = "path traversal in #include is not permitted in polyglot C++ blocks";
        return false;
    }
    // Reject #pragma GCC plugin — executes a native shared library at compile time
    std::regex plugin_pragma(R"(#\s*pragma\s+GCC\s+plugin)");
    if (std::regex_search(processed, plugin_pragma)) {
        reason = "#pragma GCC plugin is not permitted in polyglot C++ blocks";
        return false;
    }
    // V-RCE-013: Reject macro-based #include evasion.
    // Block #include with a macro identifier (not a string/angle literal).
    std::regex macro_include(R"(#\s*include\s+[A-Za-z_][A-Za-z_0-9]*)");
    if (std::regex_search(processed, macro_include)) {
        reason = "#include with macro expansion is not permitted in polyglot C++ blocks";
        return false;
    }
    // Block #define whose value contains a path-like include target (absolute or traversal)
    std::regex define_path(R"(#\s*define\s+\w+\s+[<"]/)");
    if (std::regex_search(processed, define_path)) {
        reason = "#define with absolute path value is not permitted in polyglot C++ blocks";
        return false;
    }
    std::regex define_traversal(R"(#\s*define\s+\w+\s+[<"][^>"]*\.\.)");
    if (std::regex_search(processed, define_traversal)) {
        reason = "#define with path traversal value is not permitted in polyglot C++ blocks";
        return false;
    }
    return true;
}

CppExecutorAdapter::CppExecutorAdapter()
    : block_counter_(0) {
    // C++ executor adapter initialized (silent)
}

bool CppExecutorAdapter::execute(const std::string& code) {
    // Default to INLINE_CODE for backwards compatibility
    return execute(code, CppExecutionMode::INLINE_CODE);
}

bool CppExecutorAdapter::execute(const std::string& code, CppExecutionMode mode) {
    // V-RCE-005: pre-compilation source scanner — applied to all compilation modes
    std::string unsafe_reason;
    if (!isCppSourceSafe(code, unsafe_reason)) {
        throw std::runtime_error("[Security] C++ polyglot block rejected: " + unsafe_reason);
    }

    // For BLOCK_LIBRARY mode, compile to shared library
    if (mode == CppExecutionMode::BLOCK_LIBRARY) {
        // Generate unique block ID based on code hash (so each block gets its own cache)
        size_t code_hash = std::hash<std::string>{}(code);
        std::string block_id = "BLOCK_LIB_" + std::to_string(code_hash);
        current_block_id_ = block_id;

        // Compile to shared library (no wrapping)
        bool compiled = executor_.compileBlock(block_id, code);
        if (!compiled) {
            fmt::print("[ERROR] Failed to compile C++ block library\n");
            return false;
        }

        // Block compiled successfully
        return true;
    }

    // INLINE_CODE mode: use wrapping logic below
    // Check if code has main() function - if so, compile and execute as program
    if (code.find("int main(") != std::string::npos ||
        code.find("int main (") != std::string::npos) {

        // Detected main() - compiling as executable (silent)

        // V-RCE-004: mkdtemp creates an exclusive, unpredictable temp directory.
        // Predictable /tmp names allow symlink pre-creation attacks; mkdtemp eliminates this.
        std::string tmpl1 = (getSafeTempDir() / "naab_cpp_XXXXXX").string();
        char* raw_dir1 = mkdtemp(tmpl1.data());
        if (!raw_dir1) {
            fmt::print("[ERROR] Failed to create secure temp directory\n");
            return false;
        }
        chmod(raw_dir1, 0700);
        std::filesystem::path compile_dir1(raw_dir1);
        std::filesystem::path temp_cpp = compile_dir1 / "src.cpp";
        std::filesystem::path temp_bin = compile_dir1 / "bin";

        // Write code to temp file
        std::ofstream ofs(temp_cpp);
        if (!ofs.is_open()) {
            std::filesystem::remove_all(compile_dir1);
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
            fmt::print("[ERROR] C++ compilation failed:\n{}{}\n", truncateCppErrors(compile_stderr), addCppCompileHints(compile_stderr));
            std::filesystem::remove_all(compile_dir1);
            return false;
        }

        // Execute the binary
        std::string exec_stdout, exec_stderr;
        auto containment = SubprocessContainment::fromCurrentSandbox(temp_bin.string());
        int exec_exit = execute_subprocess_with_pipes(
            temp_bin.string(),
            {},
            exec_stdout,
            exec_stderr,
            nullptr,
            &containment
        );

        // Store output in buffer
        captured_output_ = exec_stdout;
        if (!exec_stderr.empty()) {
            captured_output_ += "\n[C++ stderr]: " + exec_stderr;
        }

        // Cleanup secure compile dir
        std::filesystem::remove_all(compile_dir1);

        bool success = (exec_exit == 0);
        if (success) {
            // C++ program executed (silent)
        } else {
            fmt::print("[ERROR] C++ program failed with code {}\n", exec_exit);
        }

        return success;
    }

    // Otherwise, wrap in main() with headers and execute
    // Wrapping C++ code for execution (silent)

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

    // V-RCE-004: mkdtemp for exclusive, unpredictable temp directory (wrapped code path)
    std::string tmpl2 = (getSafeTempDir() / "naab_cpp_XXXXXX").string();
    char* raw_dir2 = mkdtemp(tmpl2.data());
    if (!raw_dir2) {
        fmt::print("[ERROR] Failed to create secure temp directory\n");
        return false;
    }
    chmod(raw_dir2, 0700);
    std::filesystem::path compile_dir2(raw_dir2);
    std::filesystem::path temp_cpp = compile_dir2 / "src.cpp";
    std::filesystem::path temp_bin = compile_dir2 / "bin";

    // Write wrapped code to temp file
    std::ofstream ofs(temp_cpp);
    if (!ofs.is_open()) {
        std::filesystem::remove_all(compile_dir2);
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
        fmt::print("[ERROR] C++ compilation failed:\n{}{}\n", truncateCppErrors(compile_stderr), addCppCompileHints(compile_stderr));
        std::filesystem::remove_all(compile_dir2);
        return false;
    }

    // Execute the binary
    std::string exec_stdout, exec_stderr;
    auto containment = SubprocessContainment::fromCurrentSandbox(temp_bin.string());
    int exec_exit = execute_subprocess_with_pipes(
        temp_bin.string(),
        {},
        exec_stdout,
        exec_stderr,
        nullptr,
        &containment
    );

    // Print output immediately
    if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
    if (!exec_stderr.empty()) fmt::print("[C++ stderr]: {}", exec_stderr);

    // Store output in buffer
    captured_output_ = exec_stdout;
    if (!exec_stderr.empty()) {
        captured_output_ += "\n[C++ stderr]: " + exec_stderr;
    }

    // Cleanup secure compile dir
    std::filesystem::remove_all(compile_dir2);

    bool success = (exec_exit == 0);
    if (success) {
        // C++ code executed successfully (silent)
    } else {
        fmt::print("[ERROR] C++ execution failed with code {}\n", exec_exit);
    }

    return success;
}

// Phase 2.3: Execute code and return the result value
interpreter::NaabVal CppExecutorAdapter::executeWithReturn(
    const std::string& code) {

    // V-RCE-005: pre-compilation source scanner
    std::string unsafe_reason_r;
    if (!isCppSourceSafe(code, unsafe_reason_r)) {
        throw std::runtime_error("[Security] C++ polyglot block rejected: " + unsafe_reason_r);
    }

    // For C++ with main(), compile and execute
    if (code.find("int main(") != std::string::npos ||
        code.find("int main (") != std::string::npos) {

        // Compiling C++ with return value capture (silent)

        // Phase 3.3.1: Check cache
        std::string cached_binary_main = cache_.getCachedBinary("cpp", code);
        std::filesystem::path temp_bin_main;

        if (!cached_binary_main.empty()) {
            // Cache hit
            // Using cached binary (silent)
            temp_bin_main = cached_binary_main;
        } else {
            // Cache miss - compile
            // Compiling (cache miss) (silent)

            // V-RCE-004: mkdtemp for exclusive, unpredictable temp directory
            std::string tmpl3 = (getSafeTempDir() / "naab_cpp_XXXXXX").string();
            char* raw_dir3 = mkdtemp(tmpl3.data());
            if (!raw_dir3) {
                fmt::print("[ERROR] Failed to create secure temp directory\n");
                return interpreter::NaabVal::makeNull();
            }
            chmod(raw_dir3, 0700);
            std::filesystem::path compile_dir3(raw_dir3);
            std::filesystem::path temp_cpp = compile_dir3 / "src.cpp";
            temp_bin_main = compile_dir3 / "bin";

            std::ofstream ofs(temp_cpp);
            if (!ofs.is_open()) {
                std::filesystem::remove_all(compile_dir3);
                fmt::print("[ERROR] Failed to create temp C++ file\n");
                return interpreter::NaabVal::makeNull();
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
                fmt::print("[ERROR] C++ compilation failed:\n{}{}\n", truncateCppErrors(compile_stderr), addCppCompileHints(compile_stderr));
                std::filesystem::remove_all(compile_dir3);
                return interpreter::NaabVal::makeNull();
            }

            // Store in cache (binary stays in compile_dir3; dir persists for cache lifetime)
            cache_.storeBinary("cpp", code, temp_bin_main.string(), temp_cpp.string());

            // Remove source only; binary remains in the private dir for cache use
            std::filesystem::remove(temp_cpp);
        }

        // Execute
        std::string exec_stdout, exec_stderr;
        auto containment = SubprocessContainment::fromCurrentSandbox(temp_bin_main.string());
        int exec_exit = execute_subprocess_with_pipes(
            temp_bin_main.string(), {}, exec_stdout, exec_stderr, nullptr, &containment
        );

        if (exec_exit != 0) {
            std::string error_msg = "C++ code execution failed with exit code " + std::to_string(exec_exit);
            if (!exec_stderr.empty()) error_msg += "\n  stderr: " + exec_stderr;
            throw std::runtime_error(error_msg);
        }

        // Buffer only log output (strip last line = return value)
        if (!exec_stdout.empty()) {
            auto last_nl = exec_stdout.rfind('\n', exec_stdout.size() - 2);
            if (last_nl != std::string::npos) {
                captured_output_ += exec_stdout.substr(0, last_nl + 1);
            }
        }
        if (!exec_stderr.empty()) fmt::print("[C++ stderr]: {}", exec_stderr);

        // Phase 3.3.1: Only cleanup if not cached
        if (cached_binary_main.empty()) {
            std::filesystem::remove(temp_bin_main);
        }

        // Trim trailing whitespace/newlines (C++ often adds trailing newlines via endl)
        std::string result = exec_stdout;
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t')) {
            result.pop_back();
        }

        // Try to parse as number
        if (!result.empty()) {
            try {
                size_t pos;
                int i = std::stoi(result, &pos);
                if (pos == result.size()) return interpreter::NaabVal::makeInt(i);
            } catch (...) {}

            try {
                size_t pos;
                double d = std::stod(result, &pos);
                if (pos == result.size()) return interpreter::NaabVal::makeDouble(d);
            } catch (...) {}
        }

        // Return as string
        return interpreter::NaabVal::makeString(result);
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
        // Detected statement (not expression), executing without return (silent)
        execute(code);
        return interpreter::NaabVal::makeNull();  // Return null/void
    }

    // Wrapping C++ expression for return value (silent)

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

        // Extract #include and using directives to top-level
        std::vector<std::string> includes;
        std::vector<std::string> code_lines;

        for (const auto& line : lines) {
            std::string trimmed = line;
            size_t trim_start = trimmed.find_first_not_of(" \t\r");
            if (trim_start != std::string::npos) {
                trimmed = trimmed.substr(trim_start);
            }

            // Check if line is #include or using directive
            if (trimmed.find("#include") == 0 || trimmed.find("using namespace") == 0) {
                includes.push_back(line);
            } else {
                code_lines.push_back(line);
            }
        }

        // Start with default includes + extracted includes
        wrapped_code =
            "#include <iostream>\n"
            "#include <string>\n"
            "#include <vector>\n"
            "#include <map>\n";

        // Add user's extracted includes
        for (const auto& inc : includes) {
            wrapped_code += inc + "\n";
        }

        wrapped_code += "int main() {\n";

        // Find last non-empty code line
        int last_code_idx = -1;
        for (int i = static_cast<int>(code_lines.size()) - 1; i >= 0; i--) {
            size_t idx = static_cast<size_t>(i);
            std::string trimmed = code_lines[idx];
            size_t s = trimmed.find_first_not_of(" \t\r");
            if (s != std::string::npos) {
                last_code_idx = i;
                break;
            }
        }

        // Add all code lines
        for (size_t i = 0; i < code_lines.size(); i++) {
            if (static_cast<int>(i) == last_code_idx) {
                // Check if last line is a statement (has semicolon) or expression
                std::string trimmed_last = code_lines[i];
                size_t trim_start = trimmed_last.find_first_not_of(" \t\r");
                if (trim_start != std::string::npos) {
                    trimmed_last = trimmed_last.substr(trim_start);
                }

                // If last line has semicolon or starts with statement keyword, treat as statement
                bool is_statement = (trimmed_last.find(';') != std::string::npos) ||
                                   (trimmed_last.find("std::cout") == 0) ||
                                   (trimmed_last.find("std::cerr") == 0) ||
                                   (trimmed_last.find("printf") == 0) ||
                                   (trimmed_last.find("return") == 0) ||
                                   (trimmed_last.find("if") == 0) ||
                                   (trimmed_last.find("for") == 0) ||
                                   (trimmed_last.find("while") == 0);

                if (is_statement) {
                    // Last line is a statement - add it with semicolon if needed
                    std::string stmt_line = code_lines[i];
                    size_t stmt_end = stmt_line.find_last_not_of(" \t\r");
                    if (stmt_end != std::string::npos && stmt_line[stmt_end] != ';' &&
                        stmt_line[stmt_end] != '{' && stmt_line[stmt_end] != '}') {
                        stmt_line += ";";
                    }
                    wrapped_code += "    " + stmt_line + "\n";
                } else {
                    // Last line is an expression - print it
                    wrapped_code += "    std::cout << (" + code_lines[i] + ");\n";
                }
            } else {
                wrapped_code += "    " + code_lines[i] + "\n";
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
        // Using cached binary (silent)
        temp_bin = cached_binary;
    } else {
        // Cache miss - compile and cache
        // Compiling C++ code (cache miss) (silent)

        // V-RCE-004: mkdtemp for exclusive, unpredictable temp directory (expression path)
        std::string tmpl4 = (getSafeTempDir() / "naab_cpp_XXXXXX").string();
        char* raw_dir4 = mkdtemp(tmpl4.data());
        if (!raw_dir4) {
            fmt::print("[ERROR] Failed to create secure temp directory\n");
            return interpreter::NaabVal::makeNull();
        }
        chmod(raw_dir4, 0700);
        std::filesystem::path compile_dir4(raw_dir4);
        std::filesystem::path temp_cpp = compile_dir4 / "src.cpp";
        temp_bin = compile_dir4 / "bin";

        std::ofstream ofs(temp_cpp);
        if (!ofs.is_open()) {
            std::filesystem::remove_all(compile_dir4);
            fmt::print("[ERROR] Failed to create temp C++ file\n");
            return interpreter::NaabVal::makeNull();
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
            fmt::print("[ERROR] C++ compilation failed:\n{}{}\n", truncateCppErrors(compile_stderr), addCppCompileHints(compile_stderr));
            std::filesystem::remove_all(compile_dir4);
            return interpreter::NaabVal::makeNull();
        }

        // Store in cache (binary stays in compile_dir4; dir persists for cache lifetime)
        cache_.storeBinary("cpp", wrapped_code, temp_bin.string(), temp_cpp.string());

        // Remove source only; binary remains in private dir for cache use
        std::filesystem::remove(temp_cpp);
    }

    // Execute
    std::string exec_stdout, exec_stderr;
    auto containment = SubprocessContainment::fromCurrentSandbox(temp_bin.string());
    int exec_exit = execute_subprocess_with_pipes(
        temp_bin.string(), {}, exec_stdout, exec_stderr, nullptr, &containment
    );

    if (exec_exit != 0) {
        std::string error_msg = "C++ code execution failed with exit code " + std::to_string(exec_exit);
        if (!exec_stderr.empty()) error_msg += "\n  stderr: " + exec_stderr;
        throw std::runtime_error(error_msg);
    }

    // Phase 3.3.1: Only cleanup if not using cached binary
    if (cached_binary.empty()) {
        // We compiled a temp binary, clean it up
        std::filesystem::remove(temp_bin);
    }
    // temp_cpp was already removed above if it was created

    // Trim trailing whitespace/newlines (C++ often adds trailing newlines via endl)
    std::string result = exec_stdout;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }

    // Try to parse as number
    if (!result.empty()) {
        try {
            size_t pos;
            int i = std::stoi(result, &pos);
            if (pos == result.size()) return interpreter::NaabVal::makeInt(i);
        } catch (...) {}

        try {
            size_t pos;
            double d = std::stod(result, &pos);
            if (pos == result.size()) return interpreter::NaabVal::makeDouble(d);
        } catch (...) {}
    }

    // Return as string
    return interpreter::NaabVal::makeString(result);
}

interpreter::NaabVal CppExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<interpreter::NaabVal>& args) {

    if (current_block_id_.empty()) {
        throw std::runtime_error("No C++ block loaded. Call execute() first.");
    }

    // Calling function (silent)

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
