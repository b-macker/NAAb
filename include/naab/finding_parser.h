// NAAb Finding Parser — Shared FINDING/VERDICT line extraction
// Used by both stdlib/governance_impl.cpp and runtime/agent_review.cpp
// No external dependencies (pure string parsing)

#pragma once
#include <string>
#include <vector>

namespace naab {
namespace runtime {

struct ParsedFinding {
    std::string category;
    std::string description;
};

struct ParsedVerdict {
    std::string category;
    bool confirmed;
    std::string reason;
};

// Parse FINDING|category|description lines from agent output.
// Handles markdown code blocks, preamble, delimiter variations (|, :, -)
std::vector<ParsedFinding> parseFindings(const std::string& text);

// Parse VERDICT|CONFIRMED|category|reason and VERDICT|FALSE_POSITIVE|category|reason
// from analyst output. Handles mid-line markers (VERDICT may not be at line start).
std::vector<ParsedVerdict> parseVerdicts(const std::string& text);

} // namespace runtime
} // namespace naab
