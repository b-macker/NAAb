#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include "naab/block_loader.h"

namespace naab {
namespace runtime {

/**
 * BlockRegistry - Singleton registry for discovering and loading blocks from filesystem
 *
 * Phase 8: Scans BLOCKS_PATH directory to discover available blocks and provides
 * lookup by block ID. Resolves the primary blocker from Phase 7e.
 */
class BlockRegistry {
public:
    /**
     * Get singleton instance
     */
    static BlockRegistry& instance();

    /**
     * Initialize registry by scanning the blocks directory
     *
     * @param blocks_path Absolute path to blocks library directory
     *                    Expected: $HOME/.naab/blocks/library/
     */
    void initialize(const std::string& blocks_path);

    /**
     * Check if registry has been initialized
     */
    bool isInitialized() const { return initialized_; }

    /**
     * Lookup block metadata by ID
     *
     * @param block_id Block identifier (e.g., "BLOCK-CPP-MATH")
     * @return BlockMetadata if found, std::nullopt otherwise
     */
    std::optional<BlockMetadata> getBlock(const std::string& block_id) const;

    /**
     * Get block source code
     *
     * @param block_id Block identifier
     * @return Source code as string, empty if not found
     */
    std::string getBlockSource(const std::string& block_id) const;

    /**
     * List all available block IDs
     *
     * @return Vector of block IDs sorted alphabetically
     */
    std::vector<std::string> listBlocks() const;

    /**
     * List blocks by language
     *
     * @param language Language name (cpp, javascript, python, etc.)
     * @return Vector of block IDs for that language
     */
    std::vector<std::string> listBlocksByLanguage(const std::string& language) const;

    /**
     * Get total block count
     */
    size_t blockCount() const { return blocks_.size(); }

    /**
     * Get list of supported languages (languages with at least one block)
     */
    std::vector<std::string> supportedLanguages() const;

    /**
     * Get blocks path
     */
    std::string getBlocksPath() const { return blocks_path_; }

private:
    BlockRegistry() = default;
    BlockRegistry(const BlockRegistry&) = delete;
    BlockRegistry& operator=(const BlockRegistry&) = delete;

    /**
     * Scan directory for blocks
     *
     * @param base_path Base directory to scan
     */
    void scanDirectory(const std::string& base_path);

    /**
     * Scan language-specific subdirectory
     *
     * @param lang_dir Language directory (e.g., "cpp/", "javascript/")
     * @param language Language name
     */
    void scanLanguageDirectory(const std::string& lang_dir, const std::string& language);

    /**
     * Extract block ID from filename
     *
     * @param filename File name (e.g., "BLOCK-CPP-MATH.cpp")
     * @return Block ID without extension (e.g., "BLOCK-CPP-MATH")
     */
    std::string extractBlockId(const std::string& filename) const;

    /**
     * Detect language from file extension
     *
     * @param filename File name
     * @return Language name or empty string
     */
    std::string detectLanguageFromExtension(const std::string& filename) const;

    /**
     * Read file contents
     *
     * @param file_path Absolute path to file
     * @return File contents as string
     */
    std::string readFile(const std::string& file_path) const;

    /**
     * Try to load metadata cache from .block_cache.json
     * @return true if cache was loaded successfully
     */
    bool loadCache(const std::string& base_path);

    /**
     * Save metadata cache to .block_cache.json
     */
    void saveCache(const std::string& base_path) const;

    bool initialized_ = false;
    std::string blocks_path_;
    std::unordered_map<std::string, BlockMetadata> blocks_;

    // Cache for block source code to avoid repeated filesystem reads
    mutable std::unordered_map<std::string, std::string> source_cache_;
};

} // namespace runtime
} // namespace naab
