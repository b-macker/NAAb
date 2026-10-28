#pragma once
// The labels of a governance message, shared by the writer and its readers.
//
// GovernanceEngine::formatError() writes a block:
//
//   Governance error: <what> [HARD-MANDATORY]       (or "Governance warning:")
//
//     At: <location>
//     Rule: <rule> = <value>
//
// and the LSP (tools/naab-lsp) reads it back out of `naab-lang --lint-only`
// output to build a diagnostic. The two used to spell the labels separately:
// 7ab3fa77 renamed "Rule (govern.json): " to "Rule: " in the writer (a config
// leak fix) and the LSP kept looking for the old label, so every governance
// diagnostic's code read "governance" from then on (A43(d)). Both now use
// these constants and parsers.

#include <string>
#include <string_view>

namespace naab {
namespace governance {
namespace message {

inline constexpr std::string_view kErrorHeader   = "Governance error: ";
inline constexpr std::string_view kWarningHeader = "Governance warning: ";
inline constexpr std::string_view kAtLabel       = "  At: ";
inline constexpr std::string_view kRuleLabel     = "  Rule: ";

// Where a block's header starts on this output line, or npos. The CLI prints a
// fatal block behind "Error: ", so the header may follow that prefix.
inline std::string::size_type headerStart(std::string_view line) {
    for (std::string_view lead : {std::string_view(""), std::string_view("Error: ")}) {
        if (line.substr(0, lead.size()) != lead) continue;
        std::string_view rest = line.substr(lead.size());
        if (rest.substr(0, kErrorHeader.size()) == kErrorHeader ||
            rest.substr(0, kWarningHeader.size()) == kWarningHeader) {
            return lead.size();
        }
    }
    return std::string::npos;
}

// The rule NAME from a "Rule:" line's text: the key path before " = <value>".
// The value is configuration (often redacted) and is not part of the name.
inline std::string ruleName(std::string_view rule_text) {
    auto end = rule_text.find(" = ");
    if (end != std::string_view::npos) rule_text = rule_text.substr(0, end);
    while (!rule_text.empty() && (rule_text.back() == ' ' || rule_text.back() == '\r')) {
        rule_text.remove_suffix(1);
    }
    return std::string(rule_text);
}

}  // namespace message
}  // namespace governance
}  // namespace naab
