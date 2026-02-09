//
// NAAb Inline Code Cache - Phase 3.3.1
// Content-based caching for compiled inline code blocks
//

#pragma once

#include <string>
#include <filesystem>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace naab {
namespace runtime {

// Cache entry metadata
struct CacheEntry {
    std::string hash;
    std::string language;
    std::filesystem::path binary_path;
    std::filesystem::path source_path;
    std::chrono::system_clock::time_point last_access;
    std::chrono::system_clock::time_point created;
    size_t access_count = 0;
    size_t code_size = 0;
};

// Inline code cache manager
class InlineCodeCache {
public:
    InlineCodeCache();
    ~InlineCodeCache();

    // Hash code content for cache key
    std::string hashCode(const std::string& code) const;

    // Check if code is cached
    bool isCached(const std::string& language, const std::string& code) const;

    // Get cached binary path (returns empty string if not cached)
    std::string getCachedBinary(const std::string& language, const std::string& code);

    // Store compiled binary in cache
    void storeBinary(
        const std::string& language,
        const std::string& code,
        const std::string& binary_path,
        const std::string& source_path
    );

    // Get cache directory for language
    std::string getCacheDir(const std::string& language) const;

    // Get binary path for hash (creates if needed)
    std::string getBinaryPath(const std::string& language, const std::string& hash) const;

    // Get source path for hash
    std::string getSourcePath(const std::string& language, const std::string& hash) const;

    // Cache maintenance
    void cleanCache(size_t max_size_mb = 500); // Default 500MB max
    void loadMetadata();
    void saveMetadata();

    // Statistics
    size_t getCacheSize() const;
    size_t getEntryCount() const;
    void printStats() const;

private:
    std::filesystem::path cache_root_;
    std::unordered_map<std::string, CacheEntry> entries_; // hash -> entry

    // Thread-safe access to cache (mutable allows locking in const methods)
    mutable std::mutex cache_mutex_;

    // LRU cleanup helpers
    std::vector<CacheEntry> sortByLRU() const;
    void removeEntry(const std::string& hash);

    // Metadata file path
    std::string getMetadataPath() const;
};

} // namespace runtime
} // namespace naab
