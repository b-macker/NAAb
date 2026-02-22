// Test Library Detection Feature
// Verifies BlockEnricher::detectLibraries() works correctly

#include "naab/block_enricher.h"
#include <fmt/core.h>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
    fmt::print("================================================================\n");
    fmt::print("  Library Detection Test\n");
    fmt::print("================================================================\n\n");

    naab::tools::BlockEnricher enricher;

    // Test 1: LLVM/Clang block
    fmt::print("Test 1: LLVM/Clang Block (BLOCK-CPP-23886)\n");
    fmt::print("-------------------------------------------\n");

    // Read the block
    std::ifstream file("blocks/library/c++/BLOCK-CPP-23886.json");
    if (!file.is_open()) {
        fmt::print("ERROR: Could not open BLOCK-CPP-23886.json\n");
        return 1;
    }

    json block_json;
    file >> block_json;
    file.close();

    std::string code = block_json["code"].get<std::string>();

    // Detect libraries
    auto libraries = enricher.detectLibraries(code);

    fmt::print("Code snippet (first 200 chars):\n");
    fmt::print("{}\n\n", code.substr(0, 200));

    fmt::print("Detected libraries ({}): ", libraries.size());
    for (const auto& lib : libraries) {
        fmt::print("{} ", lib);
    }
    fmt::print("\n\n");

    // Verify expected libraries
    bool found_clang = false;
    bool found_llvm = false;
    for (const auto& lib : libraries) {
        if (lib == "clang") found_clang = true;
        if (lib == "llvm") found_llvm = true;
    }

    if (found_clang && found_llvm) {
        fmt::print("✅ SUCCESS: Detected both clang and llvm\n");
    } else {
        fmt::print("❌ FAILURE: Missing libraries\n");
        fmt::print("   clang: {}\n", found_clang ? "✅" : "❌");
        fmt::print("   llvm: {}\n", found_llvm ? "✅" : "❌");
    }

    fmt::print("\n");

    // Test 2: spdlog block
    fmt::print("Test 2: spdlog Block (BLOCK-CPP-00004)\n");
    fmt::print("---------------------------------------\n");

    std::ifstream file2("blocks/library/c++/BLOCK-CPP-00004.json");
    if (file2.is_open()) {
        json block2_json;
        file2 >> block2_json;
        file2.close();

        std::string code2 = block2_json["code"].get<std::string>();
        auto libraries2 = enricher.detectLibraries(code2);

        fmt::print("Detected libraries ({}): ", libraries2.size());
        for (const auto& lib : libraries2) {
            fmt::print("{} ", lib);
        }
        fmt::print("\n");

        bool found_spdlog = false;
        for (const auto& lib : libraries2) {
            if (lib == "spdlog") found_spdlog = true;
        }

        if (found_spdlog) {
            fmt::print("✅ SUCCESS: Detected spdlog\n");
        } else {
            fmt::print("❌ FAILURE: Did not detect spdlog\n");
        }
    } else {
        fmt::print("⚠️  Could not open BLOCK-CPP-00004.json\n");
    }

    fmt::print("\n================================================================\n");
    fmt::print("Library detection test complete!\n");
    fmt::print("================================================================\n");

    return 0;
}
