// NAAb C++ Block Loader Implementation
// Dynamically loads C++ blocks using dlopen/dlsym

#include "naab/cpp_block_loader.h"
#include "naab/cpp_block_interface.h"
#include "naab/interpreter.h"
#include <dlfcn.h>
#include <fmt/core.h>
#include <cstring>

namespace naab {
namespace runtime {

// Internal structure for loaded block
struct CppBlockHandle {
    void* lib_handle;
    std::string block_id;
    std::string version;
    std::vector<std::string> functions;

    // Function pointers from the block
    const char* (*get_id)();
    const char* (*get_version)();
    const char* (*get_functions)();
    int (*init)();
    void (*cleanup)();
    int (*call)(const char*, int, void**, void**, char*);

    CppBlockHandle() : lib_handle(nullptr),
                       get_id(nullptr),
                       get_version(nullptr),
                       get_functions(nullptr),
                       init(nullptr),
                       cleanup(nullptr),
                       call(nullptr) {}

    ~CppBlockHandle() {
        if (cleanup) {
            cleanup();
        }
        if (lib_handle) {
            dlclose(lib_handle);
        }
    }
};

// Helper: Split comma-separated string
static std::vector<std::string> split(const std::string& str, char delim) {
    std::vector<std::string> result;
    size_t start = 0;
    size_t end = str.find(delim);

    while (end != std::string::npos) {
        result.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delim, start);
    }

    if (start < str.length()) {
        result.push_back(str.substr(start));
    }

    return result;
}

CppBlockLoader::CppBlockLoader() {
}

CppBlockLoader::~CppBlockLoader() {
    unloadAll();
}

void CppBlockLoader::setError(const std::string& error) {
    last_error_ = error;
    fmt::print("[ERROR] CppBlockLoader: {}\n", error);
}

bool CppBlockLoader::loadBlock(const std::string& block_id, const std::string& so_path) {
    // Check if already loaded
    if (blocks_.find(block_id) != blocks_.end()) {
        setError(fmt::format("Block {} already loaded", block_id));
        return false;
    }

    auto handle = std::make_unique<CppBlockHandle>();

    // Load shared library
    handle->lib_handle = dlopen(so_path.c_str(), RTLD_LAZY);
    if (!handle->lib_handle) {
        setError(fmt::format("Failed to dlopen {}: {}", so_path, dlerror()));
        return false;
    }

    // Clear any existing error
    dlerror();

    // Load required functions
    handle->get_id = (const char* (*)())dlsym(handle->lib_handle, "naab_block_id");
    handle->get_version = (const char* (*)())dlsym(handle->lib_handle, "naab_block_version");
    handle->get_functions = (const char* (*)())dlsym(handle->lib_handle, "naab_block_functions");
    handle->init = (int (*)())dlsym(handle->lib_handle, "naab_block_init");
    handle->cleanup = (void (*)())dlsym(handle->lib_handle, "naab_block_cleanup");
    handle->call = (int (*)(const char*, int, void**, void**, char*))dlsym(handle->lib_handle, "naab_block_call");

    const char* dl_error = dlerror();
    if (dl_error) {
        setError(fmt::format("Failed to load symbols from {}: {}", so_path, dl_error));
        return false;
    }

    // Verify required functions exist
    if (!handle->get_id || !handle->get_version || !handle->get_functions || !handle->call) {
        setError(fmt::format("Block {} missing required functions", so_path));
        return false;
    }

    // Get block metadata
    handle->block_id = handle->get_id();
    handle->version = handle->get_version();

    // Verify block ID matches
    if (handle->block_id != block_id) {
        setError(fmt::format("Block ID mismatch: expected {}, got {}", block_id, handle->block_id));
        return false;
    }

    // Parse exported functions
    const char* funcs_str = handle->get_functions();
    if (funcs_str) {
        handle->functions = split(std::string(funcs_str), ',');
    }

    // Initialize block if init function exists
    if (handle->init) {
        int result = handle->init();
        if (result != 0) {
            setError(fmt::format("Block {} initialization failed: code {}", block_id, result));
            return false;
        }
    }

    fmt::print("[INFO] Loaded C++ block: {} v{} ({} functions)\n",
               handle->block_id, handle->version, handle->functions.size());

    blocks_[block_id] = std::move(handle);
    return true;
}

bool CppBlockLoader::isBlockLoaded(const std::string& block_id) const {
    return blocks_.find(block_id) != blocks_.end();
}

std::vector<std::string> CppBlockLoader::getBlockFunctions(const std::string& block_id) const {
    auto it = blocks_.find(block_id);
    if (it == blocks_.end()) {
        return {};
    }
    return it->second->functions;
}

std::shared_ptr<interpreter::Value> CppBlockLoader::callBlockFunction(
    const std::string& block_id,
    const std::string& func_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    auto it = blocks_.find(block_id);
    if (it == blocks_.end()) {
        setError(fmt::format("Block {} not loaded", block_id));
        return nullptr;
    }

    CppBlockHandle* handle = it->second.get();

    // Prepare arguments as void* array
    std::vector<void*> argv;
    for (const auto& arg : args) {
        argv.push_back(arg.get());
    }

    // Call the block function
    void* result = nullptr;
    char error_msg[512] = {0};

    int status = handle->call(
        func_name.c_str(),
        argv.size(),
        argv.data(),
        &result,
        error_msg
    );

    if (status != 0) {
        setError(fmt::format("Block function {}:{} failed: {}",
                           block_id, func_name, error_msg));
        return nullptr;
    }

    // Wrap result as shared_ptr
    // Note: The block creates a new Value, we take ownership
    if (result) {
        return std::shared_ptr<interpreter::Value>(
            static_cast<interpreter::Value*>(result)
        );
    }

    return std::make_shared<interpreter::Value>();
}

void CppBlockLoader::unloadBlock(const std::string& block_id) {
    auto it = blocks_.find(block_id);
    if (it != blocks_.end()) {
        fmt::print("[INFO] Unloading C++ block: {}\n", block_id);
        blocks_.erase(it);
    }
}

void CppBlockLoader::unloadAll() {
    if (!blocks_.empty()) {
        fmt::print("[INFO] Unloading {} C++ blocks\n", blocks_.size());
        blocks_.clear();
    }
}

} // namespace runtime
} // namespace naab
