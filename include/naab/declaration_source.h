// The source text the per-function governance checks read, taken from the
// text an engine PARSED. One copy, used by both engines.
//
// The checks (placeholders, contracts, oversimplification, complexity floor,
// intent, ...) are handed the text of one declaration, and every engine used
// to get it by re-reading its current file from disk and counting braces from
// the AST line -- four hand-written copies (two tree-walker visitors, two VM
// compiler visitors) that a fix reached one at a time. Measured at a1e49f3d
// (docs/open-investigations.md A45, tests/governance_v4/test_declaration_source.sh):
//
//   - REST /execute names every request "<cwd>/api-request.naab", a file that
//     does not exist, so the tree-walker read nothing and ran none of these
//     checks on submitted code; a file planted under that name decided the
//     verdict instead. libnaab's Context::eval ("<embedded>") is the same.
//   - The tree-walker counted braces inside strings (11146ceb taught only the
//     VM to skip them), so `let s = "}"` ended the body and the rest of the
//     function went unchecked.
//   - The VM searched up to five lines back for "fn NAME(" (7d4fda0f, VM only)
//     and, when that exact text was absent -- `fn stub (x)` -- started five
//     lines early anyway, so the checks read the PREVIOUS function's body.
//
// What remains on disk: an engine given no text (setSourceText() not called,
// or a file the interpreter never parsed) still reads the file, exactly as
// before, so nothing that worked stops working.

#pragma once

#include <cctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace naab {
namespace source {

// `text` split into lines the way std::getline splits a file: no '\n' kept,
// and a final newline does not make an empty last line ("" -> no lines).
inline std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

// Parsed source of one file: the whole text (for the whole-source checks) and
// its lines (for the per-declaration ones).
struct ParsedSource {
    std::string text;
    std::vector<std::string> lines;
    explicit ParsedSource(std::string t) : text(std::move(t)), lines(splitLines(text)) {}
};

// The pre-A45 behaviour, kept as the fallback when an engine has no parsed
// text for a file: read it from disk. A file that cannot be opened gives null.
inline std::shared_ptr<const ParsedSource> readSourceFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return nullptr;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return std::make_shared<const ParsedSource>(std::move(text));
}

inline bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Does `ln` declare function `name`: the word `fn`, whitespace, then `name`
// as a whole word -- whatever follows it (`(`, a space, `<`).
inline bool declaresFunction(const std::string& ln, const std::string& name) {
    if (name.empty()) return false;
    for (size_t p = ln.find("fn"); p != std::string::npos; p = ln.find("fn", p + 1)) {
        if (p > 0 && isIdentChar(ln[p - 1])) continue;
        size_t q = p + 2;
        if (q >= ln.size() || (ln[q] != ' ' && ln[q] != '\t')) continue;
        while (q < ln.size() && (ln[q] == ' ' || ln[q] == '\t')) ++q;
        if (ln.compare(q, name.size(), name) != 0) continue;
        size_t r = q + name.size();
        if (r < ln.size() && isIdentChar(ln[r])) continue;
        return true;
    }
    return false;
}

// The text of the declaration at 1-based AST `line`: whole lines from the one
// that declares it through the one holding its closing brace. For a function
// (`name` non-empty) the declaring line is looked for up to five lines above
// `line`, because an AST line can sit below `fn` (7d4fda0f); when no line in
// that window declares it, `line` itself is the start. Braces inside "..." or
// '...' strings and after a // comment do not count; a string may span lines.
// For a main block pass an empty `name`. Out of range -> "".
inline std::string declarationText(const std::vector<std::string>& lines, int line,
                                   const std::string& name) {
    if (line <= 0 || line > static_cast<int>(lines.size())) return "";
    size_t start = static_cast<size_t>(line - 1);
    for (size_t back = 0; back < 5 && back <= static_cast<size_t>(line - 1); ++back) {
        if (declaresFunction(lines[static_cast<size_t>(line - 1) - back], name)) {
            start = static_cast<size_t>(line - 1) - back;
            break;
        }
    }
    std::string body;
    int depth = 0;
    bool found_start = false;
    bool in_string = false;
    char quote = 0;
    for (size_t i = start; i < lines.size(); ++i) {
        body += lines[i];
        body += '\n';
        const std::string& ln = lines[i];
        for (size_t j = 0; j < ln.size(); ++j) {
            char c = ln[j];
            if (in_string) {
                if (c == '\\' && j + 1 < ln.size()) { ++j; continue; }
                if (c == quote) in_string = false;
                continue;
            }
            if (c == '"' || c == '\'') { in_string = true; quote = c; continue; }
            if (c == '/' && j + 1 < ln.size() && ln[j + 1] == '/') break;
            if (c == '{') { depth++; found_start = true; }
            else if (c == '}') depth--;
        }
        if (found_start && depth <= 0) break;
    }
    return body;
}

}  // namespace source
}  // namespace naab
