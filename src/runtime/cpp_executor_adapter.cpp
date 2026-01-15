// NAAb C++ Executor Adapter Implementation
// Adapts CppExecutor to the Executor interface

#include "naab/cpp_executor_adapter.h"
#include "naab/block_enricher.h"
#include "naab/subprocess_helpers.h"
#include <fmt/core.h>
#include <fstream>
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

    // Otherwise, compile as library (original behavior)
    current_block_id_ = fmt::format("CPP-BLOCK-{}", ++block_counter_);

    fmt::print("[CPP ADAPTER] Compiling C++ block: {}\n", current_block_id_);

    // Detect required libraries from code
    tools::BlockEnricher enricher;
    std::vector<std::string> libraries = enricher.detectLibraries(code);

    if (!libraries.empty()) {
        fmt::print("[CPP ADAPTER] Detected libraries:");
        for (const auto& lib : libraries) {
            fmt::print(" {}", lib);
        }
        fmt::print("\n");
    }

    // Compile the block with detected libraries
    return executor_.compileBlock(current_block_id_, code, "execute", libraries);
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
