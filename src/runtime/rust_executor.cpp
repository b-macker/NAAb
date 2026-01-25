// NAAb Rust Executor Implementation
// Loads and executes Rust blocks via dlopen and FFI

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/rust_executor.h"
#include "naab/rust_ffi.h"
#include "naab/stack_tracer.h"  // Phase 4.2.4: Cross-language stack traces
#include "naab/subprocess_helpers.h"  // For execute_subprocess_with_pipes
#include <dlfcn.h>
#include <fmt/core.h>
#include <stdexcept>
#include <regex>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace naab {
namespace runtime {

// Forward declarations for FFI conversion helpers
std::shared_ptr<interpreter::Value> ffiToValue(NaabRustValue* ffi_val);
NaabRustValue* valueToFfi(const std::shared_ptr<interpreter::Value>& val);

RustExecutor::RustExecutor() {
    fmt::print("[INFO] RustExecutor initialized\n");
}

RustExecutor::~RustExecutor() {
    // Clean up loaded libraries
    for (const auto& [path, handle] : library_cache_) {
        if (handle) {
            dlclose(handle);
            fmt::print("[INFO] Unloaded Rust library: {}\n", path);
        }
    }
}

// Executor interface: execute code (store for later call)
bool RustExecutor::execute(const std::string& code) {
    // For inline Rust code, compile and execute immediately

    // Create temp source file
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_rs = temp_dir / "naab_temp_rust.rs";
    std::filesystem::path temp_bin = temp_dir / "naab_temp_rust";

    // Write code to temp file
    std::ofstream ofs(temp_rs);
    if (!ofs.is_open()) {
        fmt::print("[ERROR] Failed to create temp Rust source file\n");
        return false;
    }
    ofs << code;
    ofs.close();

    // Compile with rustc
    std::string compile_cmd = fmt::format("rustc {} -o {}", temp_rs.string(), temp_bin.string());
    fmt::print("[INFO] Compiling Rust code: {}\n", compile_cmd);

    std::string compile_stdout, compile_stderr;
    int compile_exit = execute_subprocess_with_pipes(
        "rustc",
        {temp_rs.string(), "-o", temp_bin.string()},
        compile_stdout,
        compile_stderr,
        nullptr
    );

    if (compile_exit != 0) {
        fmt::print("[ERROR] Rust compilation failed:\n{}\n", compile_stderr);
        std::filesystem::remove(temp_rs);
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
    stdout_buffer_.append(exec_stdout);
    if (!exec_stderr.empty()) {
        stderr_buffer_.append(exec_stderr);
    }

    // Cleanup
    std::filesystem::remove(temp_rs);
    std::filesystem::remove(temp_bin);

    bool success = (exec_exit == 0);
    if (success) {
        fmt::print("[SUCCESS] Rust program executed (exit code {})\n", exec_exit);
    } else {
        fmt::print("[ERROR] Rust program failed with code {}\n", exec_exit);
    }

    return success;
}

// Phase 2.3: Execute and return stdout as value
std::shared_ptr<interpreter::Value> RustExecutor::executeWithReturn(
    const std::string& code) {

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_rs = temp_dir / "naab_temp_rust_ret.rs";
    std::filesystem::path temp_bin = temp_dir / "naab_temp_rust_ret";

    // Phase 2.3: Multi-line support - check if code needs wrapping
    std::string rust_code = code;

    // If code contains main(), use as-is
    if (code.find("fn main()") == std::string::npos &&
        code.find("fn main (") == std::string::npos) {

        // No main() - need to wrap
        // Check if multi-line
        if (code.find('\n') != std::string::npos) {
            // Multi-line: wrap in main() and print last expression
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

            rust_code = "fn main() {\n";
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
                        rust_code += "    println!(\"{}\", " + trimmed + ");\n";
                    }
                } else {
                    rust_code += "    " + lines[i] + "\n";
                }
            }
            rust_code += "}\n";
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
            rust_code = "fn main() {\n    println!(\"{}\", " + expr + ");\n}\n";
        }
    }

    std::ofstream ofs(temp_rs);
    if (!ofs.is_open()) {
        return std::make_shared<interpreter::Value>();
    }
    ofs << rust_code;
    ofs.close();

    // Compile
    std::string compile_stdout, compile_stderr;
    int compile_exit = execute_subprocess_with_pipes(
        "rustc", {temp_rs.string(), "-o", temp_bin.string()},
        compile_stdout, compile_stderr, nullptr
    );

    if (compile_exit != 0) {
        std::filesystem::remove(temp_rs);
        return std::make_shared<interpreter::Value>();
    }

    // Execute
    std::string exec_stdout, exec_stderr;
    int exec_exit = execute_subprocess_with_pipes(
        temp_bin.string(), {},
        exec_stdout, exec_stderr, nullptr
    );

    // Print output
    if (!exec_stdout.empty()) fmt::print("{}", exec_stdout);
    if (!exec_stderr.empty()) fmt::print("[Rust stderr]: {}", exec_stderr);

    // Cleanup
    std::filesystem::remove(temp_rs);
    std::filesystem::remove(temp_bin);

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

// Executor interface: call a function
std::shared_ptr<interpreter::Value> RustExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // For Rust, function_name should be the full URI
    // rust://path/to/lib.so::function_name
    return executeBlock(function_name, args);
}

// Executor interface: check if initialized
bool RustExecutor::isInitialized() const {
    return true;  // Rust executor is always ready
}

// Executor interface: get language name
std::string RustExecutor::getLanguage() const {
    return "rust";
}

// Direct execution method
std::shared_ptr<interpreter::Value> RustExecutor::executeBlock(
    const std::string& code,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Parse URI: rust://path/to/lib.so::function_name
    std::string lib_path, func_name;
    parseRustURI(code, lib_path, func_name);

    // Phase 4.2.4: Push stack frame for cross-language tracing
    error::ScopedStackFrame stack_frame("rust", func_name, "<rust>", 0);

    // Check function cache first
    std::string cache_key = lib_path + "::" + func_name;
    NaabRustBlockFn func = nullptr;

    auto cache_it = function_cache_.find(cache_key);
    if (cache_it != function_cache_.end()) {
        func = cache_it->second;
        fmt::print("[INFO] Using cached Rust function: {}\n", cache_key);
    } else {
        // Load library and get function
        void* lib_handle = loadLibrary(lib_path);
        func = getFunction(lib_handle, func_name);

        // Cache for future calls
        function_cache_[cache_key] = func;
        fmt::print("[INFO] Cached Rust function: {}\n", cache_key);
    }

    // Convert C++ arguments to FFI
    std::vector<NaabRustValue*> ffi_args;
    ffi_args.reserve(args.size());
    for (const auto& arg : args) {
        ffi_args.push_back(valueToFfi(arg));
    }

    // Call Rust function
    NaabRustValue* ffi_result = nullptr;
    try {
        ffi_result = func(ffi_args.data(), ffi_args.size());
    } catch (...) {
        // Clean up arguments on exception
        for (auto* ffi_arg : ffi_args) {
            naab_rust_value_free(ffi_arg);
        }
        throw;
    }

    // Clean up arguments
    for (auto* ffi_arg : ffi_args) {
        naab_rust_value_free(ffi_arg);
    }

    // Convert result back to C++ Value
    if (!ffi_result) {
        // Phase 4.2.4: Extract Rust error and add to unified trace
        extractRustError();

        // Re-throw with enriched stack trace
        throw std::runtime_error(fmt::format(
            "Rust function '{}' returned null (error occurred)\n{}",
            func_name, error::StackTracer::formatTrace()));
    }

    auto result = ffiToValue(ffi_result);
    naab_rust_value_free(ffi_result);

    return result;
}

void RustExecutor::parseRustURI(const std::string& uri,
                                std::string& lib_path,
                                std::string& func_name) {
    // Expected format: rust://path/to/lib.so::function_name
    // Example: rust://./libs/custom.so::process_data

    std::regex uri_regex(R"(^rust://([^:]+)::([^:]+)$)");
    std::smatch matches;

    if (!std::regex_match(uri, matches, uri_regex)) {
        throw std::runtime_error(
            "Invalid Rust block URI format. Expected: rust://path/to/lib.so::function_name. Got: " + uri
        );
    }

    lib_path = matches[1].str();
    func_name = matches[2].str();

    fmt::print("[INFO] Parsed Rust URI: lib='{}', func='{}'\n", lib_path, func_name);
}

void* RustExecutor::loadLibrary(const std::string& lib_path) {
    // Check cache first
    auto cache_it = library_cache_.find(lib_path);
    if (cache_it != library_cache_.end()) {
        fmt::print("[INFO] Using cached Rust library: {}\n", lib_path);
        return cache_it->second;
    }

    // Load with dlopen
    void* handle = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        const char* error = dlerror();
        throw std::runtime_error(
            "Failed to load Rust library '" + lib_path + "': " +
            (error ? std::string(error) : "unknown error")
        );
    }

    // Cache the handle
    library_cache_[lib_path] = handle;
    fmt::print("[INFO] Loaded Rust library: {}\n", lib_path);

    return handle;
}

NaabRustBlockFn RustExecutor::getFunction(void* lib_handle, const std::string& func_name) {
    if (!lib_handle) {
        throw std::runtime_error("Cannot get function from null library handle");
    }

    // Clear any existing error
    dlerror();

    // Get function pointer
    void* func_ptr = dlsym(lib_handle, func_name.c_str());

    // Check for errors
    const char* error = dlerror();
    if (error) {
        throw std::runtime_error(
            "Failed to get function '" + func_name + "': " + std::string(error)
        );
    }

    if (!func_ptr) {
        throw std::runtime_error(
            "Function '" + func_name + "' not found in library (dlsym returned null)"
        );
    }

    // Cast to function pointer type
    NaabRustBlockFn func = reinterpret_cast<NaabRustBlockFn>(func_ptr);
    fmt::print("[INFO] Resolved Rust function: {}\n", func_name);

    return func;
}

// ============================================================================
// Phase 4.2.4: Rust Error Extraction
// ============================================================================

void RustExecutor::extractRustError() {
    try {
        // Get last error from Rust FFI
        NaabRustError* rust_error = naab_rust_get_last_error();

        if (!rust_error) {
            // No error information available
            return;
        }

        // Extract error details
        std::string error_message = rust_error->message ? rust_error->message : "Unknown Rust error";
        std::string error_file = rust_error->file ? rust_error->file : "<unknown>";
        uint32_t error_line = rust_error->line;

        // Add Rust frame to stack trace
        error::StackFrame rust_frame("rust", error_message, error_file, error_line);
        error::StackTracer::pushFrame(rust_frame);

        fmt::print("[TRACE] Rust frame: {} ({}:{})\n",
            error_message, error_file, error_line);

        // Free error structure
        naab_rust_error_free(rust_error);

    } catch (const std::exception& ex) {
        fmt::print("[WARN] Failed to extract Rust error: {}\n", ex.what());
    }
}

std::string RustExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string errors = stderr_buffer_.getAndClear();
    if (!errors.empty()) {
        output += "\n[Rust stderr]: " + errors;
    }
    return output;
}

} // namespace runtime
} // namespace naab
