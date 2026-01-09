// Test Enhanced Block Metadata (Phase 1.4)
// Validates all 15 new metadata fields are correctly parsed from JSON

#include "naab/block_registry.h"
#include <iostream>
#include <cassert>
#include <cmath>

#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL: " << message << std::endl; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

#define ASSERT_EQ(actual, expected, message) \
    do { \
        if ((actual) != (expected)) { \
            std::cerr << "FAIL: " << message << std::endl; \
            std::cerr << "  Expected: " << (expected) << std::endl; \
            std::cerr << "  Actual: " << (actual) << std::endl; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

#define ASSERT_NEAR(actual, expected, epsilon, message) \
    do { \
        if (std::abs((actual) - (expected)) > (epsilon)) { \
            std::cerr << "FAIL: " << message << std::endl; \
            std::cerr << "  Expected: " << (expected) << std::endl; \
            std::cerr << "  Actual: " << (actual) << std::endl; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

int main() {
    std::cout << "=== Testing Enhanced Block Metadata (Phase 1.4) ===" << std::endl;
    std::cout << std::endl;

    // Initialize BlockRegistry with test blocks directory
    auto& registry = naab::runtime::BlockRegistry::instance();
    std::string test_blocks_path = "/storage/emulated/0/Download/.naab/naab_language/tests/test_blocks";

    registry.initialize(test_blocks_path);
    ASSERT(registry.isInitialized(), "BlockRegistry should be initialized");

    // Test 1: Basic metadata fields
    std::cout << "Test 1: Loading test block..." << std::endl;
    auto block_opt = registry.getBlock("TEST-ENHANCED-META");
    ASSERT(block_opt.has_value(), "Test block should be found");

    auto& block = block_opt.value();
    ASSERT_EQ(block.block_id, std::string("TEST-ENHANCED-META"), "Block ID should match");
    ASSERT_EQ(block.name, std::string("Test Enhanced Metadata Block"), "Block name should match");
    ASSERT_EQ(block.version, std::string("1.2.3"), "Version should match");
    std::cout << "  ✓ Basic fields correct" << std::endl;

    // Test 2: AI-powered discovery fields - string fields
    std::cout << "Test 2: AI-powered discovery string fields..." << std::endl;
    ASSERT(!block.description.empty(), "Description should not be empty");
    ASSERT(block.description.find("comprehensive test block") != std::string::npos,
           "Description should contain expected text");
    ASSERT_EQ(block.short_desc, std::string("Test block for enhanced metadata"),
              "Short description should match");
    ASSERT_EQ(block.input_types, std::string("string, number"), "Input types should match");
    ASSERT_EQ(block.output_type, std::string("object"), "Output type should match");
    std::cout << "  ✓ AI discovery string fields correct" << std::endl;

    // Test 3: AI-powered discovery fields - vector fields
    std::cout << "Test 3: AI-powered discovery vector fields..." << std::endl;
    ASSERT_EQ(block.keywords.size(), static_cast<size_t>(4), "Should have 4 keywords");
    ASSERT_EQ(block.keywords[0], std::string("test"), "First keyword should be 'test'");
    ASSERT_EQ(block.keywords[1], std::string("metadata"), "Second keyword should be 'metadata'");
    ASSERT_EQ(block.keywords[2], std::string("enhanced"), "Third keyword should be 'enhanced'");
    ASSERT_EQ(block.keywords[3], std::string("ai-discovery"), "Fourth keyword should be 'ai-discovery'");
    std::cout << "  ✓ Keywords parsed correctly" << std::endl;

    // Test 4: Use cases
    std::cout << "Test 4: Use cases..." << std::endl;
    ASSERT_EQ(block.use_cases.size(), static_cast<size_t>(3), "Should have 3 use cases");
    ASSERT_EQ(block.use_cases[0], std::string("Testing metadata parsing"),
              "First use case should match");
    ASSERT_EQ(block.use_cases[1], std::string("Validating JSON field extraction"),
              "Second use case should match");
    ASSERT_EQ(block.use_cases[2], std::string("AI-powered block discovery"),
              "Third use case should match");
    std::cout << "  ✓ Use cases parsed correctly" << std::endl;

    // Test 5: Related blocks
    std::cout << "Test 5: Related blocks..." << std::endl;
    ASSERT_EQ(block.related_blocks.size(), static_cast<size_t>(2), "Should have 2 related blocks");
    ASSERT_EQ(block.related_blocks[0], std::string("TEST-BASIC-META"),
              "First related block should match");
    ASSERT_EQ(block.related_blocks[1], std::string("TEST-SIMPLE-META"),
              "Second related block should match");
    std::cout << "  ✓ Related blocks parsed correctly" << std::endl;

    // Test 6: Performance metrics
    std::cout << "Test 6: Performance metrics..." << std::endl;
    ASSERT_NEAR(block.avg_execution_ms, 12.5, 0.01, "Average execution time should match");
    ASSERT_EQ(block.max_memory_mb, 8, "Max memory should match");
    ASSERT_EQ(block.performance_tier, std::string("fast"), "Performance tier should match");
    ASSERT_EQ(block.success_rate_percent, 98, "Success rate should match");
    ASSERT_EQ(block.avg_tokens_saved, 150, "Average tokens saved should match");
    std::cout << "  ✓ Performance metrics correct" << std::endl;

    // Test 7: Quality assurance fields
    std::cout << "Test 7: Quality assurance fields..." << std::endl;
    ASSERT_EQ(block.test_coverage_percent, 95, "Test coverage should match");
    ASSERT(block.security_audited, "Security audited should be true");
    ASSERT_EQ(block.stability, std::string("stable"), "Stability should match");
    std::cout << "  ✓ Quality assurance fields correct" << std::endl;

    // Test 8: Verify default values for blocks without enhanced metadata
    std::cout << "Test 8: Default values for non-enhanced blocks..." << std::endl;
    // This test would check that blocks without the new fields get proper defaults
    // For now, we just verify the test block doesn't have empty defaults
    ASSERT(!block.description.empty(), "Enhanced block should have non-empty description");
    ASSERT(block.avg_execution_ms > 0.0, "Enhanced block should have non-zero execution time");
    std::cout << "  ✓ Enhanced block has populated fields" << std::endl;

    // Test 9: Registry statistics
    std::cout << "Test 9: Registry statistics..." << std::endl;
    size_t block_count = registry.blockCount();
    ASSERT(block_count > 0, "Registry should have at least one block");
    std::cout << "  ✓ Registry has " << block_count << " block(s)" << std::endl;

    // Test 10: Block source code extraction
    std::cout << "Test 10: Block source code extraction..." << std::endl;
    std::string source = registry.getBlockSource("TEST-ENHANCED-META");
    ASSERT(!source.empty(), "Block source should not be empty");
    ASSERT(source.find("Enhanced metadata test") != std::string::npos,
           "Source should contain expected code");
    std::cout << "  ✓ Block source extracted correctly" << std::endl;

    std::cout << std::endl;
    std::cout << "=== All 10 Enhanced Metadata Tests Passed! ===" << std::endl;
    return 0;
}
