// NAAb Finding Parser — Shared FINDING/VERDICT line extraction
// Extracted from stdlib/governance_impl.cpp for reuse by agent review engine

#include "naab/finding_parser.h"
#include <sstream>

namespace naab {
namespace runtime {

static std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

// ============================================================================
// Parse FINDING lines from agent output
// ============================================================================

std::vector<ParsedFinding> parseFindings(const std::string& text) {
    std::vector<ParsedFinding> results;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmed = trimStr(line);
        if (trimmed.empty()) continue;

        // Skip markdown fences
        if (trimmed.rfind("```", 0) == 0) continue;

        // Must start with FINDING
        if (trimmed.rfind("FINDING", 0) != 0) continue;
        if (trimmed.size() < 9) continue;

        // Detect delimiter: |, :, or -
        char delim = trimmed[7];
        if (delim != '|' && delim != ':' && delim != '-') continue;

        // Split rest by same delimiter
        std::string rest = trimmed.substr(8);
        size_t sep = rest.find(delim);
        if (sep == std::string::npos) continue;

        std::string category = trimStr(rest.substr(0, sep));
        std::string description = trimStr(rest.substr(sep + 1));

        if (!category.empty() && !description.empty()) {
            results.push_back({category, description});
        }
    }
    return results;
}

// ============================================================================
// Parse VERDICT lines from analyst output
// ============================================================================

std::vector<ParsedVerdict> parseVerdicts(const std::string& text) {
    std::vector<ParsedVerdict> results;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmed = trimStr(line);
        if (trimmed.empty()) continue;

        // Look for VERDICT|CONFIRMED| or VERDICT|FALSE_POSITIVE| anywhere in line
        // (analysts sometimes prefix with FINDING text on same line)
        auto processMarker = [&](const std::string& marker, bool is_confirmed) {
            size_t pos = trimmed.find(marker);
            if (pos == std::string::npos) return false;

            std::string after = trimmed.substr(pos + marker.size());
            // Category is the next pipe-delimited field
            size_t pipe = after.find('|');
            std::string category, reason;
            if (pipe != std::string::npos) {
                category = trimStr(after.substr(0, pipe));
                reason = trimStr(after.substr(pipe + 1));
            } else {
                category = trimStr(after);
            }

            if (!category.empty()) {
                results.push_back({category, is_confirmed, reason});
            }
            return true;
        };

        if (processMarker("VERDICT|CONFIRMED|", true)) continue;
        processMarker("VERDICT|FALSE_POSITIVE|", false);
    }
    return results;
}

} // namespace runtime
} // namespace naab
