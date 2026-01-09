#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <map>
#include "naab/block_loader.h"

namespace naab {
namespace runtime {

/**
 * SearchQuery - Structured search query with filters
 */
struct SearchQuery {
    std::string query;                    // Full-text search query
    std::optional<std::string> language;  // Filter by language (cpp, javascript, python, etc.)
    std::optional<std::string> category;  // Filter by category
    std::optional<std::string> performance_tier;  // Filter by performance (fast/medium/slow)
    int min_success_rate = 0;            // Minimum success rate percentage (0-100)
    int limit = 20;                      // Maximum results to return
    int offset = 0;                      // Result offset for pagination
};

/**
 * SearchResult - Single search result with relevance score
 */
struct SearchResult {
    BlockMetadata metadata;  // Complete block metadata
    double relevance_score;  // Relevance score (0.0 - 1.0)
    double popularity_score; // Popularity score based on usage
    double quality_score;    // Quality score based on metrics
    double final_score;      // Combined weighted score
    std::string snippet;     // Code snippet preview
};

/**
 * BlockSearchIndex - SQLite-based full-text search index for blocks
 *
 * Phase 1.5: Provides fast (<100ms) semantic search across all 24,488 blocks
 * using SQLite FTS5 (Full-Text Search) engine.
 */
class BlockSearchIndex {
public:
    /**
     * Constructor - Opens or creates SQLite database
     *
     * @param db_path Path to SQLite database file
     */
    explicit BlockSearchIndex(const std::string& db_path);

    /**
     * Destructor - Closes database connection
     */
    ~BlockSearchIndex();

    /**
     * Build index from blocks directory
     *
     * Scans blocks directory, extracts metadata, and populates SQLite database
     * with FTS5 index for fast full-text search.
     *
     * @param blocks_path Path to blocks library directory
     * @return Number of blocks indexed
     */
    int buildIndex(const std::string& blocks_path);

    /**
     * Search blocks with full-text query
     *
     * Performs FTS5 search with relevance ranking, filters, and pagination.
     * Target latency: <100ms for 24,488 blocks
     *
     * @param query Search query with filters
     * @return Vector of search results sorted by relevance
     */
    std::vector<SearchResult> search(const SearchQuery& query);

    /**
     * Get block by ID
     *
     * @param block_id Block identifier
     * @return BlockMetadata if found, nullopt otherwise
     */
    std::optional<BlockMetadata> getBlock(const std::string& block_id);

    /**
     * Get total number of indexed blocks
     *
     * @return Count of blocks in index
     */
    int getBlockCount() const;

    /**
     * Get index statistics
     *
     * @return Map of statistic name to value
     */
    std::map<std::string, int> getStatistics() const;

    /**
     * Record block usage
     *
     * Updates times_used counter for popularity ranking
     *
     * @param block_id Block identifier
     */
    void recordUsage(const std::string& block_id);

    /**
     * Clear all data and rebuild schema
     */
    void clearIndex();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;  // PIMPL pattern to hide SQLite details
};

} // namespace runtime
} // namespace naab
