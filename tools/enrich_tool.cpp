// Block Enrichment Tool
// Command-line tool to enrich blocks

#include "naab/block_enricher.h"
#include <fmt/core.h>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace naab::tools;

BlockMetadata loadBlockFromJSON(const std::string& json_path) {
    std::ifstream file(json_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + json_path);
    }

    json j;
    file >> j;

    BlockMetadata meta;
    meta.id = j.value("id", "");
    meta.language = j.value("language", "");
    meta.code = j.value("code", "");
    meta.source_file = j.value("source_file", "");

    // Handle null source_line gracefully
    if (j.contains("source_line") && !j["source_line"].is_null()) {
        meta.source_line = j["source_line"].get<int>();
    } else {
        meta.source_line = 0;
    }

    meta.validation_status = j.value("validation_status", "pending");

    return meta;
}

void saveEnrichedBlock(const BlockMetadata& meta, const std::string& output_path) {
    json j;
    j["id"] = meta.id;
    j["language"] = meta.language;
    j["code"] = meta.code;
    j["source_file"] = meta.source_file;
    j["source_line"] = meta.source_line;
    j["validation_status"] = meta.validation_status;

    std::ofstream file(output_path);
    file << j.dump(2);
}

int main(int argc, char** argv) {
    fmt::print("=== NAAb Block Enricher Tool ===\n\n");

    if (argc < 2) {
        fmt::print("Usage: {} <block_json_file> [output_file]\n", argv[0]);
        fmt::print("\nExample:\n");
        fmt::print("  {} /path/to/BLOCK-CPP-00001.json\n", argv[0]);
        return 1;
    }

    std::string input_path = argv[1];
    std::string output_path = (argc >= 3) ? argv[2] : input_path;

    try {
        // Load block
        fmt::print("Loading block from: {}\n", input_path);
        auto block = loadBlockFromJSON(input_path);

        fmt::print("  ID: {}\n", block.id);
        fmt::print("  Language: {}\n", block.language);
        fmt::print("  Source: {}:{}\n", block.source_file, block.source_line);
        fmt::print("  Status: {}\n\n", block.validation_status);

        // Enrich block
        BlockEnricher enricher;
        auto enriched = enricher.enrichBlock(block);

        // Save result
        fmt::print("\nSaving enriched block to: {}\n", output_path);
        saveEnrichedBlock(enriched, output_path);

        if (enriched.validation_status == "validated") {
            fmt::print("\n✓ Block enriched successfully\n");
            fmt::print("\nEnriched code preview (first 300 chars):\n");
            fmt::print("{}\n", enriched.code.substr(0, 300));
            if (enriched.code.length() > 300) {
                fmt::print("... ({} more chars)\n", enriched.code.length() - 300);
            }
            return 0;
        } else {
            fmt::print("\n✗ Block enrichment failed\n");
            return 1;
        }

    } catch (const std::exception& e) {
        fmt::print("\nERROR: {}\n", e.what());
        return 1;
    }
}
