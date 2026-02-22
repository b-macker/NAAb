// Test Block Search Index (Phase 1.5)
// Validates SQLite FTS5 search index creation and basic operations

#include "naab/block_search_index.h"
#include <iostream>
#include <cassert>

#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL: " << message << std::endl; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

#define ASSERT_GT(actual, expected, message) \
    do { \
        if ((actual) <= (expected)) { \
            std::cerr << "FAIL: " << message << std::endl; \
            std::cerr << "  Expected > " << (expected) << std::endl; \
            std::cerr << "  Actual: " << (actual) << std::endl; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

int main() {
    std::cout << "=== Testing Block Search Index (Phase 1.5) ===" << std::endl;
    std::cout << std::endl;

    // Test 1: Create search index database
    std::cout << "Test 1: Creating search index database..." << std::endl;
    try {
        naab::runtime::BlockSearchIndex index("/tmp/naab_search_test.db");
        std::cout << "  ✓ Database created successfully" << std::endl;

        // Test 2: Build index from test blocks
        std::cout << "Test 2: Building index from test blocks..." << std::endl;
        std::string test_blocks_path = "tests/fixtures/block-samples";
        int count = index.buildIndex(test_blocks_path);
        std::cout << "  ✓ Indexed " << count << " blocks" << std::endl;
        ASSERT_GT(count, 0, "Should index at least 1 block");

        // Test 3: Verify block count
        std::cout << "Test 3: Verifying block count..." << std::endl;
        int block_count = index.getBlockCount();
        std::cout << "  ✓ Block count: " << block_count << std::endl;
        ASSERT_GT(block_count, 0, "Block count should be > 0");

        // Test 4: Get block by ID
        std::cout << "Test 4: Getting block by ID..." << std::endl;
        auto block_opt = index.getBlock("TEST-ENHANCED-META");
        ASSERT(block_opt.has_value(), "Should find TEST-ENHANCED-META block");
        std::cout << "  ✓ Found block: " << block_opt->name << std::endl;
        std::cout << "  ✓ Description: " << block_opt->description.substr(0, 50) << "..." << std::endl;

        // Test 5: Search without query (list all)
        std::cout << "Test 5: Searching without query (list all)..." << std::endl;
        naab::runtime::SearchQuery query1;
        query1.query = "";
        query1.limit = 10;
        auto results1 = index.search(query1);
        std::cout << "  ✓ Found " << results1.size() << " blocks" << std::endl;
        ASSERT_GT(results1.size(), 0, "Should find at least one block");

        // Test 6: Search with text query
        std::cout << "Test 6: Searching with text query..." << std::endl;
        naab::runtime::SearchQuery query2;
        query2.query = "metadata";  // Should match our test blocks
        query2.limit = 10;
        auto results2 = index.search(query2);
        std::cout << "  ✓ Found " << results2.size() << " blocks matching 'metadata'" << std::endl;

        // Test 7: Search with language filter
        std::cout << "Test 7: Searching with language filter..." << std::endl;
        naab::runtime::SearchQuery query3;
        query3.query = "";
        query3.language = "javascript";
        query3.limit = 10;
        auto results3 = index.search(query3);
        std::cout << "  ✓ Found " << results3.size() << " JavaScript blocks" << std::endl;
        if (!results3.empty()) {
            ASSERT(results3[0].metadata.language == "javascript", "Should be JavaScript block");
        }

        // Test 8: Verify search result has scores
        std::cout << "Test 8: Verifying search result scores..." << std::endl;
        if (!results1.empty()) {
            auto& first_result = results1[0];
            std::cout << "  ✓ Relevance score: " << first_result.relevance_score << std::endl;
            std::cout << "  ✓ Quality score: " << first_result.quality_score << std::endl;
            std::cout << "  ✓ Popularity score: " << first_result.popularity_score << std::endl;
            std::cout << "  ✓ Final score: " << first_result.final_score << std::endl;
            std::cout << "  ✓ Snippet: " << first_result.snippet << std::endl;
        }

        // Test 9: Get statistics
        std::cout << "Test 9: Getting index statistics..." << std::endl;
        auto stats = index.getStatistics();
        std::cout << "  ✓ Total blocks: " << stats["total_blocks"] << std::endl;
        ASSERT_GT(stats["total_blocks"], 0, "Total blocks should be > 0");

        // Test 10: Record usage
        std::cout << "Test 10: Recording block usage..." << std::endl;
        if (block_opt.has_value()) {
            int initial_usage = block_opt->times_used;
            index.recordUsage("TEST-ENHANCED-META");
            auto updated_block = index.getBlock("TEST-ENHANCED-META");
            ASSERT(updated_block.has_value(), "Should still find block after usage recording");
            std::cout << "  ✓ Usage recorded (before: " << initial_usage
                     << ", after: " << updated_block->times_used << ")" << std::endl;
        }

        std::cout << std::endl;
        std::cout << "=== All 10 Search Index Tests Passed! ===" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << std::endl;
        return 1;
    }
}
