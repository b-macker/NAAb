#ifndef NAAB_RUST_EXECUTOR_H
#define NAAB_RUST_EXECUTOR_H

#include "naab/language_registry.h"
#include "naab/rust_ffi.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace naab {
namespace interpreter {
    class Value;  // Forward declaration
}

namespace runtime {

/**
 * RustExecutor: Executes Rust blocks via FFI
 *
 * URI Format: rust://path/to/lib.so::function_name
 *
 * Example:
 *   rust:///usr/local/lib/my_blocks.so::process_data
 *   rust://./libs/custom.so::transform
 *
 * The Rust library must export functions with signature:
 *   extern "C" NaabRustValue* function_name(NaabRustValue** args, size_t arg_count)
 */
class RustExecutor : public Executor {
public:
    RustExecutor();
    ~RustExecutor() override;

    // Executor interface implementation
    bool execute(const std::string& code) override;
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    ) override;
    bool isInitialized() const override;
    std::string getLanguage() const override;

    /**
     * Execute a Rust block function directly (non-Executor interface)
     *
     * @param code URI in format "rust://path/to/lib.so::function_name"
     * @param args Arguments to pass to Rust function
     * @return Result from Rust function as C++ Value
     * @throws std::runtime_error on dlopen/dlsym failures or missing functions
     */
    std::shared_ptr<interpreter::Value> executeBlock(
        const std::string& code,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    );

private:
    /**
     * Parse Rust block URI into library path and function name
     *
     * @param uri URI string (e.g., "rust://./libs/custom.so::process")
     * @param lib_path Output: path to .so file
     * @param func_name Output: function name to call
     * @throws std::runtime_error on invalid URI format
     */
    void parseRustURI(const std::string& uri, std::string& lib_path, std::string& func_name);

    /**
     * Load a Rust library with dlopen
     *
     * @param lib_path Path to .so file
     * @return dlopen handle
     * @throws std::runtime_error on dlopen failure
     */
    void* loadLibrary(const std::string& lib_path);

    /**
     * Get function pointer from loaded library
     *
     * @param lib_handle dlopen handle
     * @param func_name Function name
     * @return Function pointer
     * @throws std::runtime_error on dlsym failure
     */
    NaabRustBlockFn getFunction(void* lib_handle, const std::string& func_name);

    // Cache of loaded libraries: path -> dlopen handle
    std::unordered_map<std::string, void*> library_cache_;

    // Cache of resolved functions: "path::function" -> function pointer
    std::unordered_map<std::string, NaabRustBlockFn> function_cache_;
};

} // namespace runtime
} // namespace naab

#endif // NAAB_RUST_EXECUTOR_H
