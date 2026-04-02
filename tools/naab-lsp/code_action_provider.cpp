#include "code_action_provider.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace naab {
namespace lsp {

// ============================================================================
// CodeAction::toJson
// ============================================================================

json CodeAction::toJson() const {
    json j = {
        {"title", title},
        {"kind", kind}
    };

    if (has_edit) {
        j["edit"] = {
            {"changes", {
                {edit_uri, json::array({
                    {
                        {"range", edit_range.toJson()},
                        {"newText", edit_new_text}
                    }
                })}
            }}
        };
    }

    return j;
}

// ============================================================================
// Helpers
// ============================================================================

std::string CodeActionProvider::getLine(const Document& doc, int line) const {
    return doc.getLineText(line);
}

Range CodeActionProvider::lineRange(const Document& doc, int line) const {
    std::string text = doc.getLineText(line);
    return Range{
        Position{line, 0},
        Position{line, static_cast<int>(text.length())}
    };
}

// Strip trailing whitespace from a string
static std::string rtrim(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

// ============================================================================
// getCodeActions — dispatch to per-code fix generators
// ============================================================================

std::vector<CodeAction> CodeActionProvider::getCodeActions(
    const Document& doc,
    const Range& range,
    const std::vector<Diagnostic>& context_diagnostics)
{
    std::vector<CodeAction> actions;

    for (const auto& diag : context_diagnostics) {
        // Only handle diagnostics that overlap with the requested range
        if (diag.range.start.line > range.end.line ||
            diag.range.end.line < range.start.line) {
            continue;
        }

        std::string code = diag.code;
        // Normalize: lowercase
        std::transform(code.begin(), code.end(), code.begin(), ::tolower);

        if (code.find("placeholder") != std::string::npos ||
            code.find("todo") != std::string::npos ||
            code.find("fixme") != std::string::npos) {
            auto fixes = fixForPlaceholder(doc, diag);
            actions.insert(actions.end(), fixes.begin(), fixes.end());
        } else if (code.find("debug") != std::string::npos ||
                   code.find("print") != std::string::npos ||
                   code.find("console") != std::string::npos ||
                   code.find("artifact") != std::string::npos) {
            auto fixes = fixForDebugArtifact(doc, diag);
            actions.insert(actions.end(), fixes.begin(), fixes.end());
        } else if (code.find("secret") != std::string::npos ||
                   code.find("hardcoded") != std::string::npos ||
                   code.find("credential") != std::string::npos ||
                   code.find("password") != std::string::npos ||
                   code.find("token") != std::string::npos ||
                   code.find("api-key") != std::string::npos) {
            auto fixes = fixForSecret(doc, diag);
            actions.insert(actions.end(), fixes.begin(), fixes.end());
        }
    }

    return actions;
}

// ============================================================================
// fixForPlaceholder — remove or stub out TODO/FIXME/placeholder comments
// ============================================================================

std::vector<CodeAction> CodeActionProvider::fixForPlaceholder(
    const Document& doc, const Diagnostic& diag)
{
    std::vector<CodeAction> actions;
    int line = diag.range.start.line;
    std::string line_text = getLine(doc, line);

    // Action 1: Delete the placeholder line entirely
    CodeAction remove_action;
    remove_action.title = "Remove placeholder comment";
    remove_action.kind = "quickfix";
    remove_action.has_edit = true;
    remove_action.edit_uri = doc.getUri();
    // Delete whole line including newline by extending to start of next line
    remove_action.edit_range = Range{
        Position{line, 0},
        Position{line + 1, 0}
    };
    remove_action.edit_new_text = "";
    actions.push_back(remove_action);

    // Action 2: Replace placeholder with a proper comment stub
    // Detect indentation
    size_t indent_len = 0;
    while (indent_len < line_text.size() && (line_text[indent_len] == ' ' || line_text[indent_len] == '\t')) {
        ++indent_len;
    }
    std::string indent = line_text.substr(0, indent_len);

    CodeAction stub_action;
    stub_action.title = "Replace with implementation stub";
    stub_action.kind = "quickfix";
    stub_action.has_edit = true;
    stub_action.edit_uri = doc.getUri();
    stub_action.edit_range = lineRange(doc, line);
    stub_action.edit_new_text = indent + "// TODO: Implement this";
    actions.push_back(stub_action);

    return actions;
}

// ============================================================================
// fixForDebugArtifact — remove debug print/log statements
// ============================================================================

std::vector<CodeAction> CodeActionProvider::fixForDebugArtifact(
    const Document& doc, const Diagnostic& diag)
{
    std::vector<CodeAction> actions;
    int line = diag.range.start.line;
    std::string line_text = getLine(doc, line);

    // Action: Delete the debug statement line
    CodeAction remove_action;
    remove_action.title = "Remove debug statement";
    remove_action.kind = "quickfix";
    remove_action.has_edit = true;
    remove_action.edit_uri = doc.getUri();
    remove_action.edit_range = Range{
        Position{line, 0},
        Position{line + 1, 0}
    };
    remove_action.edit_new_text = "";
    actions.push_back(remove_action);

    // Action: Comment out the debug statement
    size_t indent_len = 0;
    while (indent_len < line_text.size() && (line_text[indent_len] == ' ' || line_text[indent_len] == '\t')) {
        ++indent_len;
    }
    std::string indent = line_text.substr(0, indent_len);
    std::string rest = rtrim(line_text.substr(indent_len));

    CodeAction comment_action;
    comment_action.title = "Comment out debug statement";
    comment_action.kind = "quickfix";
    comment_action.has_edit = true;
    comment_action.edit_uri = doc.getUri();
    comment_action.edit_range = lineRange(doc, line);
    comment_action.edit_new_text = indent + "// " + rest;
    actions.push_back(comment_action);

    return actions;
}

// ============================================================================
// fixForSecret — replace hardcoded secret with env variable reference
// ============================================================================

std::vector<CodeAction> CodeActionProvider::fixForSecret(
    const Document& doc, const Diagnostic& diag)
{
    std::vector<CodeAction> actions;
    int line = diag.range.start.line;
    std::string line_text = getLine(doc, line);

    // Detect indentation
    size_t indent_len = 0;
    while (indent_len < line_text.size() && (line_text[indent_len] == ' ' || line_text[indent_len] == '\t')) {
        ++indent_len;
    }
    std::string indent = line_text.substr(0, indent_len);

    // Try to extract variable name from the line (e.g., "let token = \"abc123\"")
    // Look for pattern: let/const <name> = "..."
    std::string var_name = "SECRET";
    size_t eq_pos = line_text.find('=');
    if (eq_pos != std::string::npos) {
        std::string lhs = rtrim(line_text.substr(indent_len, eq_pos - indent_len));
        // Strip "let " or "const " prefix
        for (const auto& kw : {"let ", "const ", "var "}) {
            if (lhs.size() > strlen(kw) && lhs.substr(0, strlen(kw)) == kw) {
                lhs = lhs.substr(strlen(kw));
                break;
            }
        }
        // Trim whitespace
        size_t start = lhs.find_first_not_of(" \t");
        size_t end = lhs.find_last_not_of(" \t");
        if (start != std::string::npos) {
            var_name = lhs.substr(start, end - start + 1);
            // Uppercase for env var name
            std::transform(var_name.begin(), var_name.end(), var_name.begin(), ::toupper);
        }
    }

    // Action: Replace with env.get_var() call
    CodeAction env_action;
    env_action.title = "Replace with environment variable";
    env_action.kind = "quickfix";
    env_action.has_edit = true;
    env_action.edit_uri = doc.getUri();
    env_action.edit_range = lineRange(doc, line);

    // Preserve original variable name (lowercase) in the replacement
    std::string lower_var = var_name;
    std::transform(lower_var.begin(), lower_var.end(), lower_var.begin(), ::tolower);
    env_action.edit_new_text = indent + "let " + lower_var + " = env.get_var(\"" + var_name + "\")";
    actions.push_back(env_action);

    // Action: Remove the line entirely
    CodeAction remove_action;
    remove_action.title = "Remove hardcoded secret";
    remove_action.kind = "quickfix";
    remove_action.has_edit = true;
    remove_action.edit_uri = doc.getUri();
    remove_action.edit_range = Range{
        Position{line, 0},
        Position{line + 1, 0}
    };
    remove_action.edit_new_text = "";
    actions.push_back(remove_action);

    return actions;
}

} // namespace lsp
} // namespace naab
