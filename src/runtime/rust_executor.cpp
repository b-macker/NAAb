// NAAb Rust Executor Implementation
// Loads and executes Rust blocks via dlopen and FFI

#include "naab/rust_executor.h"
#include "naab/rust_ffi.h"
#include <dlfcn.h>
#include <fmt/core.h>
#include <stdexcept>
#include <regex>

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

std::shared_ptr<interpreter::Value> RustExecutor::execute(
    const std::string& code,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Parse URI: rust://path/to/lib.so::function_name
    std::string lib_path, func_name;
    parseRustURI(code, lib_path, func_name);

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
        throw std::runtime_error("Rust function returned null");
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

} // namespace runtime
} // namespace naab
