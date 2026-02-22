// Test BlockRegistry functionality
#include "naab/block_registry.h"
#include <fmt/core.h>
#include <iostream>

int main() {
    fmt::print("=== BlockRegistry Test ===\n\n");

    // Initialize registry
    auto& registry = naab::runtime::BlockRegistry::instance();
    std::string blocks_path = "blocks/library/";

    fmt::print("Initializing BlockRegistry from: {}\n", blocks_path);
    registry.initialize(blocks_path);

    // Test 1: Check block count
    fmt::print("\n--- Test 1: Block Count ---\n");
    fmt::print("Total blocks found: {}\n", registry.blockCount());

    // Test 2: List supported languages
    fmt::print("\n--- Test 2: Supported Languages ---\n");
    auto langs = registry.supportedLanguages();
    for (const auto& lang : langs) {
        auto lang_blocks = registry.listBlocksByLanguage(lang);
        fmt::print("  {} : {} blocks\n", lang, lang_blocks.size());
    }

    // Test 3: List all blocks
    fmt::print("\n--- Test 3: All Blocks ---\n");
    auto all_blocks = registry.listBlocks();
    for (const auto& block_id : all_blocks) {
        fmt::print("  • {}\n", block_id);
    }

    // Test 4: Get specific block metadata
    fmt::print("\n--- Test 4: Block Metadata ---\n");
    auto cpp_math_opt = registry.getBlock("BLOCK-CPP-MATH");
    if (cpp_math_opt) {
        auto& meta = *cpp_math_opt;
        fmt::print("Block ID: {}\n", meta.block_id);
        fmt::print("Language: {}\n", meta.language);
        fmt::print("File path: {}\n", meta.file_path);
        fmt::print("Version: {}\n", meta.version);
    } else {
        fmt::print("[ERROR] BLOCK-CPP-MATH not found!\n");
    }

    // Test 5: Get block source code
    fmt::print("\n--- Test 5: Block Source Code ---\n");
    auto source = registry.getBlockSource("BLOCK-CPP-MATH");
    if (!source.empty()) {
        fmt::print("Source code loaded: {} bytes\n", source.size());
        fmt::print("First 200 chars:\n{}\n", source.substr(0, 200));
    } else {
        fmt::print("[ERROR] Failed to load source code!\n");
    }

    fmt::print("\n=== All Tests Complete ===\n");
    return 0;
}
