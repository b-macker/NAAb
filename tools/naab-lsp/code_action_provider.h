#pragma once

#include "document_manager.h"
#include <vector>
#include <string>

namespace naab {
namespace lsp {

// A single code action (quick fix or refactor)
struct CodeAction {
    std::string title;
    std::string kind;  // "quickfix", "refactor", etc.

    // If non-empty, this is a text edit to apply
    bool has_edit = false;
    std::string edit_uri;
    Range edit_range;
    std::string edit_new_text;

    json toJson() const;
};

class CodeActionProvider {
public:
    // Get code actions applicable to [range] in [doc], given [context_diagnostics].
    // Each action corresponds to a quick fix for a known diagnostic code.
    std::vector<CodeAction> getCodeActions(
        const Document& doc,
        const Range& range,
        const std::vector<Diagnostic>& context_diagnostics
    );

private:
    // Per-diagnostic-code fix generators
    std::vector<CodeAction> fixForPlaceholder(const Document& doc, const Diagnostic& diag);
    std::vector<CodeAction> fixForDebugArtifact(const Document& doc, const Diagnostic& diag);
    std::vector<CodeAction> fixForSecret(const Document& doc, const Diagnostic& diag);

    // Get full line text and range for a given 0-based line number
    std::string getLine(const Document& doc, int line) const;
    Range lineRange(const Document& doc, int line) const;
};

} // namespace lsp
} // namespace naab
