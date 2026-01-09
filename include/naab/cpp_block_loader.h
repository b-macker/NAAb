#ifndef NAAB_CPP_BLOCK_LOADER_H
#define NAAB_CPP_BLOCK_LOADER_H

// NAAb C++ Block Loader
// Dynamically loads C++ blocks (.so files) and manages their lifecycle

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

namespace naab {
namespace interpreter {
    class Value;
}

namespace runtime {

// Forward declaration
struct CppBlockHandle;

/**
 * C++ Block Loader - Manages dynamic loading of C++ blocks
 */
class CppBlockLoader {
public:
    CppBlockLoader();
    ~CppBlockLoader();

    /**
     * Load a C++ block from .so file
     * @param block_id - Unique block identifier (e.g., "BLOCK-JSON")
     * @param so_path - Path to .so file
     * @return true on success, false on error
     */
    bool loadBlock(const std::string& block_id, const std::string& so_path);

    /**
     * Check if a block is loaded
     */
    bool isBlockLoaded(const std::string& block_id) const;

    /**
     * Get list of functions exported by a block
     */
    std::vector<std::string> getBlockFunctions(const std::string& block_id) const;

    /**
     * Call a function in a loaded block
     * @param block_id - Block identifier
     * @param func_name - Function name
     * @param args - Arguments as NAAb Values
     * @return Result as NAAb Value, or nullptr on error
     */
    std::shared_ptr<interpreter::Value> callBlockFunction(
        const std::string& block_id,
        const std::string& func_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    );

    /**
     * Unload a block
     */
    void unloadBlock(const std::string& block_id);

    /**
     * Unload all blocks
     */
    void unloadAll();

    /**
     * Get last error message
     */
    std::string getLastError() const { return last_error_; }

private:
    std::unordered_map<std::string, std::unique_ptr<CppBlockHandle>> blocks_;
    std::string last_error_;

    void setError(const std::string& error);
};

} // namespace runtime
} // namespace naab

#endif // NAAB_CPP_BLOCK_LOADER_H
