// NAAb Documentation Generator
// Extracts documentation from NAAb source files and generates markdown

#include "naab/doc_generator.h"
#include <fmt/core.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

void printUsage(const char* program_name) {
    fmt::print("NAAb Documentation Generator v0.1.0\n\n");
    fmt::print("Usage:\n");
    fmt::print("  {} <file.naab> [file2.naab ...] [options]\n\n", program_name);
    fmt::print("Options:\n");
    fmt::print("  --output, -o <dir>     Output directory for generated docs (default: docs/)\n");
    fmt::print("  --catalog, -c          Generate a catalog index of all modules\n");
    fmt::print("  --help, -h             Show this help message\n\n");
    fmt::print("Examples:\n");
    fmt::print("  {} examples/math.naab\n", program_name);
    fmt::print("  {} examples/*.naab --output api-docs/\n", program_name);
    fmt::print("  {} src/**/*.naab --catalog --output docs/\n\n", program_name);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    // Parse arguments
    std::vector<std::string> input_files;
    std::string output_dir = "docs";
    bool generate_catalog = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--output" || arg == "-o") {
            if (i + 1 < argc) {
                output_dir = argv[++i];
            } else {
                fmt::print(stderr, "Error: --output requires a directory argument\n");
                return 1;
            }
        } else if (arg == "--catalog" || arg == "-c") {
            generate_catalog = true;
        } else if (arg[0] == '-') {
            fmt::print(stderr, "Error: Unknown option: {}\n", arg);
            return 1;
        } else {
            input_files.push_back(arg);
        }
    }

    if (input_files.empty()) {
        fmt::print(stderr, "Error: No input files specified\n");
        printUsage(argv[0]);
        return 1;
    }

    // Create output directory
    try {
        if (!fs::exists(output_dir)) {
            fs::create_directories(output_dir);
            fmt::print("Created output directory: {}\n", output_dir);
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "Error creating output directory: {}\n", e.what());
        return 1;
    }

    // Process files
    naab::doc::DocGenerator generator;
    std::vector<naab::doc::ModuleDoc> all_modules;
    int success_count = 0;
    int error_count = 0;

    fmt::print("\nGenerating documentation...\n\n");

    for (const auto& filepath : input_files) {
        try {
            // Check if file exists
            if (!fs::exists(filepath)) {
                fmt::print(stderr, "Warning: File not found: {}\n", filepath);
                error_count++;
                continue;
            }

            fmt::print("Processing: {}\n", filepath);

            // Parse file
            auto module = generator.parseFile(filepath);
            all_modules.push_back(module);

            // Generate markdown
            std::string markdown = generator.generateMarkdown(module);

            // Determine output filename
            fs::path input_path(filepath);
            std::string base_name = input_path.stem().string();
            std::string output_filename = base_name + ".md";
            fs::path output_path = fs::path(output_dir) / output_filename;

            // Write output
            std::ofstream out_file(output_path);
            if (!out_file.is_open()) {
                fmt::print(stderr, "Error: Failed to open output file: {}\n",
                          output_path.string());
                error_count++;
                continue;
            }

            out_file << markdown;
            out_file.close();

            fmt::print("  → Generated: {}\n", output_path.string());
            fmt::print("  → Functions documented: {}\n", module.functions.size());
            success_count++;

        } catch (const std::exception& e) {
            fmt::print(stderr, "Error processing {}: {}\n", filepath, e.what());
            error_count++;
        }
    }

    // Generate catalog if requested
    if (generate_catalog && !all_modules.empty()) {
        fmt::print("\nGenerating catalog...\n");
        std::string catalog = generator.generateCatalog(all_modules);

        fs::path catalog_path = fs::path(output_dir) / "API_CATALOG.md";
        std::ofstream catalog_file(catalog_path);
        if (catalog_file.is_open()) {
            catalog_file << catalog;
            catalog_file.close();
            fmt::print("  → Generated: {}\n", catalog_path.string());
        } else {
            fmt::print(stderr, "Error: Failed to write catalog file\n");
            error_count++;
        }
    }

    // Summary
    fmt::print("\n");
    fmt::print("════════════════════════════════════════\n");
    fmt::print("Documentation generation complete!\n");
    fmt::print("════════════════════════════════════════\n");
    fmt::print("  Files processed: {}\n", success_count);
    if (error_count > 0) {
        fmt::print("  Errors: {}\n", error_count);
    }
    fmt::print("  Output directory: {}\n", output_dir);
    fmt::print("\n");

    return (error_count > 0) ? 1 : 0;
}
