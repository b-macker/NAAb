#ifndef NAAB_BLOCK_LOADER_H
#define NAAB_BLOCK_LOADER_H

// NAAb Block Loader - Load blocks from SQLite registry
// Enables cross-language block assembly

#include <string>
#include <memory>
#include <map>
#include <unordered_map>
#include <vector>

namespace naab {

// Forward declaration
namespace versioning {
    struct SemanticVersion;
}

namespace runtime {

// Block metadata from registry
struct BlockMetadata {
    // Core identification
    std::string block_id;
    std::string name;
    std::string language;
    std::string category;
    std::string subcategory;
    std::string file_path;
    std::string code_hash;
    int token_count;
    int times_used;

    // Versioning and lifecycle
    std::string version;                  // Block version (e.g., "1.2.3")
    std::string min_runtime_version;      // Minimum NAAb runtime version (e.g., ">=0.1.0")
    bool deprecated;                      // Deprecation flag
    std::string deprecated_reason;        // Why deprecated
    std::string replacement_block_id;     // Suggested replacement block
    std::vector<std::string> tags;
    std::vector<std::string> dependencies;
    bool is_active;

    // AI-powered discovery fields (Phase 1.4)
    std::string description;              // Full description of block functionality
    std::string short_desc;               // Brief one-line summary
    std::string input_types;              // Expected input types (e.g., "string, number")
    std::string output_type;              // Return type (e.g., "string", "object")
    std::vector<std::string> keywords;    // Search keywords for AI discovery
    std::vector<std::string> use_cases;   // Example use cases and scenarios
    std::vector<std::string> related_blocks; // Related block IDs for suggestions

    // Performance and quality metrics
    double avg_execution_ms;              // Average execution time in milliseconds
    int max_memory_mb;                    // Maximum memory usage in MB
    std::string performance_tier;         // Performance classification (fast/medium/slow)
    int success_rate_percent;             // Reliability metric (0-100)
    int avg_tokens_saved;                 // Average AI tokens saved by using this block

    // Quality assurance
    int test_coverage_percent;            // Test coverage percentage (0-100)
    bool security_audited;                // Security audit status
    std::string stability;                // Stability level (stable/beta/experimental)

    // Version helpers
    versioning::SemanticVersion getSemanticVersion() const;
    bool isCompatibleWithRuntime() const;
};

// Block loader - reads from SQLite registry
class BlockLoader {
public:
    explicit BlockLoader(const std::string& db_path);
    ~BlockLoader();

    // Query blocks
    BlockMetadata getBlock(const std::string& block_id);
    std::vector<BlockMetadata> searchBlocks(const std::string& query);
    std::vector<BlockMetadata> getBlocksByLanguage(const std::string& language);
    int getTotalBlocks() const;

    // Load block source code
    std::string loadBlockCode(const std::string& block_id);

    // Statistics
    void recordBlockUsage(const std::string& block_id, int tokens_saved);
    void recordBlockPair(const std::string& block1_id, const std::string& block2_id);
    std::vector<BlockMetadata> getTopBlocksByUsage(int limit = 10);
    std::vector<std::pair<std::string, std::string>> getTopCombinations(int limit = 10);
    std::map<std::string, int> getLanguageStats();
    long long getTotalTokensSaved();

    // Version checking (static methods)
    static bool checkBlockCompatibility(const BlockMetadata& block);
    static void warnDeprecated(const BlockMetadata& block);
    static std::string formatDeprecationWarning(const BlockMetadata& block);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace runtime
} // namespace naab

#endif // NAAB_BLOCK_LOADER_H
