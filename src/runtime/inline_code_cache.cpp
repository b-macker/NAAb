//
// NAAb Inline Code Cache Implementation - Phase 3.3.1
//

#include "naab/inline_code_cache.h"
#include "naab/paths.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

namespace naab {
namespace runtime {

namespace fs = std::filesystem;

// ============================================================================
// InlineCodeCache Implementation
// ============================================================================

InlineCodeCache::InlineCodeCache() {
    cache_root_ = naab::paths::cache_dir();

    try {
        if (!fs::exists(cache_root_)) {
            fs::create_directories(cache_root_);
            // Created inline code cache (silent)
        }

        // Load existing metadata
        loadMetadata();

    } catch (const std::exception& e) {
        // Failed to create cache directory (silent - will use temp files as fallback)
    }
}

InlineCodeCache::~InlineCodeCache() {
    // Save metadata on shutdown
    saveMetadata();
}

std::string InlineCodeCache::hashCode(const std::string& code) const {
    // Simple but effective hash using std::hash + length + first/last chars
    // This avoids expensive crypto hashing while being collision-resistant for cache purposes

    std::hash<std::string> hasher;
    size_t hash1 = hasher(code);

    // Mix in code length and content from different positions
    size_t hash2 = code.length();
    if (!code.empty()) {
        hash2 ^= (size_t)code[0] << 16;
        hash2 ^= (size_t)code[code.length() / 2] << 8;
        hash2 ^= (size_t)code[code.length() - 1];
    }

    // Combine hashes
    size_t final_hash = hash1 ^ (hash2 << 1);

    // Convert to hex string
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << final_hash;
    return oss.str();
}

bool InlineCodeCache::isCached(const std::string& language, const std::string& code) const {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    std::string hash = hashCode(code);
    std::string cache_key = language + ":" + hash;

    if (entries_.find(cache_key) == entries_.end()) {
        return false;
    }

    // Verify binary actually exists on disk
    const auto& entry = entries_.at(cache_key);
    return fs::exists(entry.binary_path);
}

std::string InlineCodeCache::getCachedBinary(const std::string& language, const std::string& code) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    std::string hash = hashCode(code);
    std::string cache_key = language + ":" + hash;

    auto it = entries_.find(cache_key);
    if (it == entries_.end()) {
        return "";
    }

    // Update access metadata
    it->second.last_access = std::chrono::system_clock::now();
    it->second.access_count++;

    // Verify binary exists
    if (!fs::exists(it->second.binary_path)) {
        // Warning: cached binary missing (silent)
        entries_.erase(it);
        return "";
    }

    // Cache hit (silent)

    return it->second.binary_path.string();
}

void InlineCodeCache::storeBinary(
    const std::string& language,
    const std::string& code,
    const std::string& binary_path,
    const std::string& source_path) {

    std::lock_guard<std::mutex> lock(cache_mutex_);

    std::string hash = hashCode(code);
    std::string cache_key = language + ":" + hash;

    // Create language-specific cache directory
    std::string lang_dir = getCacheDir(language);

    // Destination paths
    std::string cached_binary = getBinaryPath(language, hash);
    std::string cached_source = getSourcePath(language, hash);

    try {
        // Copy binary to cache
        if (fs::exists(binary_path)) {
            fs::copy_file(binary_path, cached_binary, fs::copy_options::overwrite_existing);
        }

        // Copy source to cache
        if (fs::exists(source_path)) {
            fs::copy_file(source_path, cached_source, fs::copy_options::overwrite_existing);
        }

        // Create metadata entry
        CacheEntry entry;
        entry.hash = hash;
        entry.language = language;
        entry.binary_path = cached_binary;
        entry.source_path = cached_source;
        entry.created = std::chrono::system_clock::now();
        entry.last_access = entry.created;
        entry.access_count = 1;
        entry.code_size = code.length();

        entries_[cache_key] = entry;

        // Stored binary in cache (silent)

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to cache binary: {}\n", e.what());
    }
}

std::string InlineCodeCache::getCacheDir(const std::string& language) const {
    std::string lang_dir = (cache_root_ / language).string();

    if (!fs::exists(lang_dir)) {
        fs::create_directories(lang_dir);
    }

    return lang_dir;
}

std::string InlineCodeCache::getBinaryPath(const std::string& language, const std::string& hash) const {
    std::string lang_dir = getCacheDir(language);

    // Language-specific extension
    std::string ext;
    if (language == "cpp" || language == "c++") ext = ".so";
    else if (language == "rust") ext = ".so";
    else if (language == "go") ext = "";  // Go produces executables
    else if (language == "csharp" || language == "cs") ext = ".exe";
    else ext = ".bin";

    return lang_dir + "/" + hash + ext;
}

std::string InlineCodeCache::getSourcePath(const std::string& language, const std::string& hash) const {
    std::string lang_dir = getCacheDir(language);

    // Language-specific extension
    std::string ext;
    if (language == "cpp" || language == "c++") ext = ".cpp";
    else if (language == "rust") ext = ".rs";
    else if (language == "go") ext = ".go";
    else if (language == "csharp" || language == "cs") ext = ".cs";
    else ext = ".src";

    return lang_dir + "/" + hash + ext;
}

void InlineCodeCache::cleanCache(size_t max_size_mb) {
    std::lock_guard<std::mutex> lock(cache_mutex_);

    size_t current_size = getCacheSize();
    size_t max_size_bytes = max_size_mb * 1024 * 1024;

    if (current_size <= max_size_bytes) {
        return; // Cache is within limits
    }

    // Cleaning cache (silent)

    // Sort entries by LRU (least recently used first)
    auto lru_entries = sortByLRU();

    // Remove entries until we're under the limit
    size_t removed_count = 0;
    size_t removed_bytes = 0;

    for (const auto& entry : lru_entries) {
        if (current_size - removed_bytes <= max_size_bytes) {
            break;
        }

        // Get file sizes before removing
        size_t binary_size = 0;
        size_t source_size = 0;

        if (fs::exists(entry.binary_path)) {
            binary_size = fs::file_size(entry.binary_path);
        }
        if (fs::exists(entry.source_path)) {
            source_size = fs::file_size(entry.source_path);
        }

        // Remove entry
        std::string cache_key = entry.language + ":" + entry.hash;
        removeEntry(cache_key);

        removed_bytes += binary_size + source_size;
        removed_count++;
    }

    // Removed entries from cache (silent)
}

std::vector<CacheEntry> InlineCodeCache::sortByLRU() const {
    std::vector<CacheEntry> sorted;
    sorted.reserve(entries_.size());

    for (const auto& pair : entries_) {
        sorted.push_back(pair.second);
    }

    // Sort by last access time (oldest first)
    std::sort(sorted.begin(), sorted.end(),
        [](const CacheEntry& a, const CacheEntry& b) {
            return a.last_access < b.last_access;
        });

    return sorted;
}

void InlineCodeCache::removeEntry(const std::string& cache_key) {
    auto it = entries_.find(cache_key);
    if (it == entries_.end()) {
        return;
    }

    const auto& entry = it->second;

    // Remove files
    try {
        if (fs::exists(entry.binary_path)) {
            fs::remove(entry.binary_path);
        }
        if (fs::exists(entry.source_path)) {
            fs::remove(entry.source_path);
        }
    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to remove cache files: {}\n", e.what());
    }

    // Remove from map
    entries_.erase(it);
}

size_t InlineCodeCache::getCacheSize() const {
    size_t total_size = 0;

    for (const auto& pair : entries_) {
        const auto& entry = pair.second;

        if (fs::exists(entry.binary_path)) {
            total_size += fs::file_size(entry.binary_path);
        }
        if (fs::exists(entry.source_path)) {
            total_size += fs::file_size(entry.source_path);
        }
    }

    return total_size;
}

size_t InlineCodeCache::getEntryCount() const {
    return entries_.size();
}

void InlineCodeCache::printStats() const {
    size_t total_size = getCacheSize();
    size_t total_accesses = 0;

    for (const auto& pair : entries_) {
        total_accesses += pair.second.access_count;
    }

    fmt::print("\n[CACHE STATS]\n");
    fmt::print("  Entries: {}\n", entries_.size());
    fmt::print("  Total size: {:.2f} MB\n", total_size / (1024.0 * 1024.0));
    fmt::print("  Total accesses: {}\n", total_accesses);
    if (!entries_.empty()) {
        fmt::print("  Avg accesses/entry: {:.1f}\n",
                   (double)total_accesses / entries_.size());
    }
}

void InlineCodeCache::loadMetadata() {
    std::string metadata_path = getMetadataPath();

    if (!fs::exists(metadata_path)) {
        return; // No metadata file yet
    }

    try {
        std::ifstream file(metadata_path);
        std::string line;

        while (std::getline(file, line)) {
            // Simple format: language:hash|binary_path|source_path|access_count|last_access_epoch
            std::istringstream iss(line);
            std::string cache_key, binary_path, source_path;
            size_t access_count;
            long long last_access_epoch;

            if (std::getline(iss, cache_key, '|') &&
                std::getline(iss, binary_path, '|') &&
                std::getline(iss, source_path, '|') &&
                (iss >> access_count) && iss.ignore(1) &&
                (iss >> last_access_epoch)) {

                // Extract language and hash from cache_key
                size_t colon_pos = cache_key.find(':');
                if (colon_pos == std::string::npos) continue;

                std::string language = cache_key.substr(0, colon_pos);
                std::string hash = cache_key.substr(colon_pos + 1);

                // Create entry
                CacheEntry entry;
                entry.hash = hash;
                entry.language = language;
                entry.binary_path = binary_path;
                entry.source_path = source_path;
                entry.access_count = access_count;
                entry.last_access = std::chrono::system_clock::from_time_t(last_access_epoch);
                entry.created = entry.last_access;

                // Only add if files still exist
                if (fs::exists(entry.binary_path)) {
                    entries_[cache_key] = entry;
                }
            }
        }

        // Loaded cached entries (silent)

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to load cache metadata: {}\n", e.what());
    }
}

void InlineCodeCache::saveMetadata() {
    std::string metadata_path = getMetadataPath();

    try {
        std::ofstream file(metadata_path);

        for (const auto& pair : entries_) {
            const auto& entry = pair.second;

            // Simple format: language:hash|binary_path|source_path|access_count|last_access_epoch
            auto last_access_epoch = std::chrono::system_clock::to_time_t(entry.last_access);

            file << pair.first << "|"
                 << entry.binary_path.string() << "|"
                 << entry.source_path.string() << "|"
                 << entry.access_count << "|"
                 << last_access_epoch << "\n";
        }

        // Saved metadata (silent)

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to save cache metadata: {}\n", e.what());
    }
}

std::string InlineCodeCache::getMetadataPath() const {
    return (cache_root_ / "metadata.txt").string();
}

} // namespace runtime
} // namespace naab
