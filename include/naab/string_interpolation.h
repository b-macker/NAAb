// String interpolation: one splitter for every consumer.
//
// "${expr}" inside a string literal is evaluated as a NAAb expression. Four
// places used to scan for it independently -- the tree-walker, the VM compiler
// and both taint scanners -- so the evaluator and the taint tracker could
// disagree about what counts as interpolation. They all call
// splitInterpolation() now.
//
// Escape: \${ in a string literal is a literal "${" (JS template literals,
// shell ${VAR}, templating syntax). The lexer encodes it as kEscapedDollar,
// which is not '$', so no scanner can mistake it for interpolation; the
// splitter decodes it back to '$' in the text it returns. A backslash-dollar
// NOT followed by '{' keeps its old meaning (the backslash is kept), so shell
// text such as "echo \$HOME" is unchanged.

#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace naab {
namespace interp {

// U+E000 (Private Use Area): stands for a literal '$' written as \${.
inline constexpr const char* kEscapedDollar = "\xEE\x80\x80";

struct InterpSegment {
    bool is_expr;       // true: text is a NAAb expression from ${...}
    std::string text;   // literal text (escapes decoded) or expression source
};

inline std::string decodeLiteralText(const std::string& s) {
    static const std::string marker(kEscapedDollar);
    if (s.find(marker) == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, marker.size(), marker) == 0) {
            out += '$';
            i += marker.size();
        } else {
            out += s[i++];
        }
    }
    return out;
}

inline std::vector<InterpSegment> splitInterpolation(const std::string& raw) {
    std::vector<InterpSegment> segments;
    std::string text;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
            if (!text.empty()) {
                segments.push_back({false, decodeLiteralText(text)});
                text.clear();
            }
            i += 2;  // skip ${
            int depth = 1;
            std::string expr;
            while (i < raw.size() && depth > 0) {
                if (raw[i] == '{') depth++;
                else if (raw[i] == '}') {
                    depth--;
                    if (depth == 0) break;
                }
                expr += raw[i++];
            }
            if (i < raw.size()) i++;  // skip closing }
            segments.push_back({true, expr});
        } else {
            text += raw[i++];
        }
    }
    if (!text.empty()) segments.push_back({false, decodeLiteralText(text)});
    return segments;
}

inline bool hasInterpolation(const std::string& raw) {
    return raw.find("${") != std::string::npos;
}

// True when splitInterpolation() would return exactly `raw` as one text
// segment: no ${...} and no \${ escape. Lets hot paths skip the copy.
inline bool isPlainLiteral(const std::string& raw) {
    return !hasInterpolation(raw) && raw.find(kEscapedDollar) == std::string::npos;
}

// Identifiers referenced by the ${...} expressions in a literal. Used by the
// taint scanners (tree-walker and VM), which must see exactly the expressions
// the evaluator will run -- an escaped \${x} is literal text, not a use of x.
inline std::vector<std::string> interpolatedIdentifiers(const std::string& raw) {
    std::vector<std::string> ids;
    for (const auto& seg : splitInterpolation(raw)) {
        if (!seg.is_expr) continue;
        std::string word;
        for (char c : seg.text) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                word += c;
            } else {
                if (!word.empty()) ids.push_back(word);
                word.clear();
            }
        }
        if (!word.empty()) ids.push_back(word);
    }
    return ids;
}

// What the text inside ${...} most likely was, when it is not NAAb.
inline std::string foreignSyntaxGuess(const std::string& expr) {
    auto has = [&](const char* s) { return expr.find(s) != std::string::npos; };
    if (has("=>") || has("Math.") || has("console.") || has(".toFixed(") ||
        has(".replace(/") || has(".map(") || has(".join(") || has("`") ||
        has("===") || has("!==")) {
        return "JavaScript";
    }
    if (has(":-") || has(":=") || has("#") || has("%") || has("//") || has("[@]")) {
        return "shell";
    }
    bool all_caps = !expr.empty();
    bool any_letter = false;
    for (char c : expr) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalpha(u)) { any_letter = true; if (std::islower(u)) all_caps = false; }
        else if (!std::isdigit(u) && c != '_') all_caps = false;
    }
    if (all_caps && any_letter) return "a shell or environment variable";
    return "";
}

// A parse error inside ${...} used to surface as "line 1, column N" -- the
// position within the extracted expression, with no file -- followed by a
// hint written for a different mistake. Report the enclosing string instead.
inline std::string formatInterpolationError(const std::string& expr,
                                            const std::string& inner_error,
                                            const std::string& file,
                                            int line, int column) {
    std::string first = inner_error.substr(0, inner_error.find("\n\n"));
    // The inner parser only saw the extracted expression, so its "line 1,
    // column N" is a position inside ${...}. Say that, not a file position.
    const std::string prefix = "Parse error at line 1, column ";
    if (first.compare(0, prefix.size(), prefix) == 0) {
        size_t colon = first.find(':', prefix.size());
        if (colon != std::string::npos) {
            first = "at character " + first.substr(prefix.size(), colon - prefix.size()) +
                    " of the expression:" + first.substr(colon + 1);
        }
    }
    std::string where = file.empty() ? "" : file;
    if (line > 0) {
        where += (where.empty() ? "line " : ":") + std::to_string(line);
        if (column > 0) where += ":" + std::to_string(column);
    }
    std::string guess = foreignSyntaxGuess(expr);

    std::string msg = "String interpolation error: the text inside ${...} is not a NAAb expression\n\n";
    if (!where.empty()) msg += "  In the string at " + where + "\n";
    msg += "  Interpolated: ${" + expr + "}\n";
    msg += "  Parser said:  " + first + "\n\n";
    msg += "  Help:\n";
    msg += "  - NAAb evaluates every ${...} in a string literal as NAAb code\n";
    if (!guess.empty()) {
        msg += "  - This looks like " + guess + " text, not NAAb\n";
    }
    msg += "  - To keep a literal ${ in the string, escape it as \\${\n";
    if (guess == "JavaScript") {
        msg += "  - Better: write JavaScript in a <<javascript ... >> block, not in a NAAb string\n";
    }
    msg += "\n  Example:\n";
    msg += "    x Wrong: \"const w = `${a / b}`\"\n";
    msg += "    v Right: \"const w = `\\${a / b}`\"\n";
    return msg;
}

// Appended to "Undefined variable 'NAME'" when NAME is ALL_CAPS: in practice
// that is almost always shell or env syntax that NAAb interpolated.
inline std::string undefinedCapsHint(const std::string& name) {
    return "\n\n  '" + name + "' looks like a shell or environment variable.\n"
           "  NAAb evaluates ${...} inside every string, so \"echo ${" + name + "}\"\n"
           "  reads a NAAb variable named " + name + ".\n"
           "    Literal text for a shell or template:  \"echo \\${" + name + "}\"\n"
           "    Environment variable in NAAb:          env.get(\"" + name + "\")";
}

inline bool isAllCapsName(const std::string& name) {
    if (name.size() < 2 || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    bool any_letter = false;
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalpha(u)) { any_letter = true; if (std::islower(u)) return false; }
        else if (!std::isdigit(u) && c != '_') return false;
    }
    return any_letter;
}

} // namespace interp
} // namespace naab
