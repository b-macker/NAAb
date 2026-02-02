// NAAb Tamper-Evident Log Verification Tool
// Phase 1 Item 8: Tamper-Evident Logging
//
// Usage: naab-verify-audit <log-file> [--hmac-key <key>]

#include "naab/tamper_evident_logger.h"
#include <iostream>
#include <fmt/core.h>
#include <fmt/color.h>

using namespace naab::security;

void printUsage(const char* program_name) {
    fmt::print("Usage: {} <log-file> [options]\n\n", program_name);
    fmt::print("Options:\n");
    fmt::print("  --hmac-key <key>    Verify HMAC signatures with provided key\n");
    fmt::print("  --verbose, -v       Show detailed verification output\n");
    fmt::print("  --help, -h          Show this help message\n\n");
    fmt::print("Examples:\n");
    fmt::print("  {} ~/.naab/logs/security_tamper_evident.log\n", program_name);
    fmt::print("  {} audit.log --hmac-key secret-key\n", program_name);
    fmt::print("  {} audit.log --verbose\n\n", program_name);
}

void printHeader() {
    fmt::print(fmt::emphasis::bold | fg(fmt::color::cyan),
               "╔══════════════════════════════════════════════════════════════╗\n");
    fmt::print(fmt::emphasis::bold | fg(fmt::color::cyan),
               "║  NAAb Tamper-Evident Log Verification Tool                  ║\n");
    fmt::print(fmt::emphasis::bold | fg(fmt::color::cyan),
               "║  Phase 1 Item 8: Cryptographic Integrity Verification       ║\n");
    fmt::print(fmt::emphasis::bold | fg(fmt::color::cyan),
               "╚══════════════════════════════════════════════════════════════╝\n\n");
}

void printVerificationResult(const VerificationResult& result, bool verbose) {
    fmt::print(fmt::emphasis::bold, "\n═══════════════════════════════════════════════════════════════\n");
    fmt::print(fmt::emphasis::bold, "  Verification Results\n");
    fmt::print(fmt::emphasis::bold, "═══════════════════════════════════════════════════════════════\n\n");

    // Summary
    fmt::print("Total Entries:     {}\n", result.total_entries);
    fmt::print("Verified Entries:  {}\n", result.verified_entries);

    if (result.is_valid) {
        fmt::print(fg(fmt::color::green) | fmt::emphasis::bold,
                   "Status:            ✓ VALID\n\n");
        fmt::print(fg(fmt::color::green),
                   "All entries verified successfully!\n");
        fmt::print("The log chain is intact and has not been tampered with.\n");
    } else {
        fmt::print(fg(fmt::color::red) | fmt::emphasis::bold,
                   "Status:            ✗ TAMPERED\n\n");
        fmt::print(fg(fmt::color::red) | fmt::emphasis::bold,
                   "WARNING: Log tampering detected!\n");
    }

    // Tampered entries
    if (!result.tampered_sequences.empty()) {
        fmt::print("\n");
        fmt::print(fg(fmt::color::red) | fmt::emphasis::bold,
                   "Tampered Entries ({}):\n", result.tampered_sequences.size());
        for (auto seq : result.tampered_sequences) {
            fmt::print(fg(fmt::color::red), "  ✗ Sequence {}\n", seq);
        }
    }

    // Missing entries
    if (!result.missing_sequences.empty()) {
        fmt::print("\n");
        fmt::print(fg(fmt::color::yellow) | fmt::emphasis::bold,
                   "Missing Entries ({}):\n", result.missing_sequences.size());
        for (auto seq : result.missing_sequences) {
            fmt::print(fg(fmt::color::yellow), "  ⚠ Sequence {}\n", seq);
        }
    }

    // Detailed errors
    if (verbose && !result.errors.empty()) {
        fmt::print("\n");
        fmt::print(fmt::emphasis::bold, "Detailed Errors:\n");
        for (const auto& error : result.errors) {
            fmt::print(fg(fmt::color::red), "  • {}\n", error);
        }
    }

    fmt::print("\n");
    fmt::print(fmt::emphasis::bold, "═══════════════════════════════════════════════════════════════\n");
}

void printProgressBar(uint64_t current, uint64_t total) {
    if (total == 0) return;

    int bar_width = 50;
    float progress = static_cast<float>(current) / total;
    int pos = static_cast<int>(bar_width * progress);

    fmt::print("\r[");
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) fmt::print("=");
        else if (i == pos) fmt::print(">");
        else fmt::print(" ");
    }
    fmt::print("] {}/{} ({:.1f}%)", current, total, progress * 100.0);
    std::cout.flush();
}

int main(int argc, char* argv[]) {
    // Parse arguments
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string log_file;
    std::string hmac_key;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--hmac-key") {
            if (i + 1 < argc) {
                hmac_key = argv[++i];
            } else {
                fmt::print(fg(fmt::color::red), "Error: --hmac-key requires an argument\n");
                return 1;
            }
        } else if (arg[0] != '-') {
            log_file = arg;
        } else {
            fmt::print(fg(fmt::color::red), "Error: Unknown option: {}\n", arg);
            printUsage(argv[0]);
            return 1;
        }
    }

    if (log_file.empty()) {
        fmt::print(fg(fmt::color::red), "Error: No log file specified\n");
        printUsage(argv[0]);
        return 1;
    }

    // Print header
    printHeader();

    // Verify log file exists
    if (!std::filesystem::exists(log_file)) {
        fmt::print(fg(fmt::color::red) | fmt::emphasis::bold,
                   "Error: Log file not found: {}\n", log_file);
        return 1;
    }

    fmt::print("Log File: {}\n", log_file);

    // Get file size
    auto file_size = std::filesystem::file_size(log_file);
    fmt::print("File Size: {:.2f} KB\n", file_size / 1024.0);

    if (!hmac_key.empty()) {
        fmt::print("HMAC Verification: Enabled\n");
    }
    fmt::print("\n");

    // Create temporary logger instance for verification
    fmt::print("Verifying log integrity...\n\n");

    try {
        // Create a temporary TamperEvidenceLogger to use its verification method
        // Note: We can't directly instantiate because the log already exists
        // So we'll read and verify manually (this is a simplified approach)

        // For now, we'll use the VerificationResult structure directly
        // In production, we'd have a standalone verification function

        // Count total entries first
        std::ifstream count_file(log_file);
        std::string line;
        uint64_t total_entries = 0;
        while (std::getline(count_file, line)) {
            if (!line.empty()) total_entries++;
        }
        count_file.close();

        fmt::print("Total entries to verify: {}\n\n", total_entries);

        // Create verification instance
        // Note: This is a workaround - in production we'd have a read-only verifier
        TamperEvidenceLogger temp_logger(log_file);

        // Perform verification
        VerificationResult result;
        if (!hmac_key.empty()) {
            result = temp_logger.verifyIntegrity(hmac_key);
        } else {
            result = temp_logger.verifyIntegrity();
        }

        // Print results
        printVerificationResult(result, verbose);

        // Return exit code
        return result.is_valid ? 0 : 2;

    } catch (const std::exception& e) {
        fmt::print(fg(fmt::color::red) | fmt::emphasis::bold,
                   "\nError during verification: {}\n", e.what());
        return 3;
    }
}
