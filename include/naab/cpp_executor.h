#pragma once

// NAAb C++ Block Executor
// Compiles and executes C++ blocks via dynamic loading

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "naab/type_marshaller.h"

// Include libffi if available
#ifdef HAVE_LIBFFI
#include <ffi.h>
#endif

namespace naab {
namespace interpreter {
    class Value;  // Forward declaration
}

namespace runtime {

// Function signature descriptor
struct FunctionSignature {
    std::string return_type;           // "int", "double", "void", "string", etc.
    std::vector<std::string> param_types;  // Parameter types in order

    FunctionSignature() = default;
    FunctionSignature(const std::string& ret, const std::vector<std::string>& params)
        : return_type(ret), param_types(params) {}
};

// Represents a compiled C++ block
struct CompiledBlock {
    std::string block_id;
    std::string so_path;        // Path to compiled .so file
    void* handle;               // dlopen handle
    std::string entry_point;    // Function name to call
    bool is_loaded;
    std::unordered_map<std::string, FunctionSignature> function_signatures;

    CompiledBlock() : handle(nullptr), is_loaded(false) {}
    ~CompiledBlock();
};

// C++ Block Executor - compiles and executes C++ blocks
class CppExecutor {
public:
    CppExecutor();
    ~CppExecutor();

    // Compile a C++ block to shared library
    bool compileBlock(
        const std::string& block_id,
        const std::string& code,
        const std::string& entry_point = "execute",
        const std::vector<std::string>& dependencies = {}
    );

    // Execute a compiled C++ block (legacy - calls default entry point)
    std::shared_ptr<interpreter::Value> executeBlock(
        const std::string& block_id,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    );

    // Call a specific function in a compiled block with type marshalling
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& block_id,
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    );

    // Check if block is already compiled
    bool isCompiled(const std::string& block_id) const;

    // Get cache directory
    std::string getCacheDir() const { return cache_dir_; }

    // Clear compilation cache
    void clearCache();

    // Register a function signature for dynamic calling
    void registerFunctionSignature(
        const std::string& block_id,
        const std::string& function_name,
        const FunctionSignature& signature
    );

private:
    std::string cache_dir_;
    std::unordered_map<std::string, std::shared_ptr<CompiledBlock>> compiled_blocks_;
    TypeMarshaller marshaller_;

    // Helper methods
    std::string getSourcePath(const std::string& block_id) const;
    std::string getLibraryPath(const std::string& block_id) const;
    std::string wrapFragmentIfNeeded(const std::string& code);
    bool compileToSharedLibrary(
        const std::string& source_path,
        const std::string& so_path,
        const std::vector<std::string>& dependencies = {}
    );
    bool loadCompiledBlock(const std::string& block_id);
    std::string buildLibraryFlags(const std::vector<std::string>& dependencies) const;

#ifdef HAVE_LIBFFI
    // libffi-based dynamic function calling
    std::shared_ptr<interpreter::Value> callWithFFI(
        void* func_ptr,
        const FunctionSignature& signature,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    );

    ffi_type* mapTypeToFFI(const std::string& type_name);
#endif
};

} // namespace runtime
} // namespace naab

