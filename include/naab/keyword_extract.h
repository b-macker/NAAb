#pragma once

// Shared keyword extraction for CDD semantic signals and agent governance.
// Single implementation used by both src/stdlib/agent_impl.cpp (mandate,
// instruction, response, tool-result keywords) and
// src/runtime/behavioral_sequence.cpp (plan steps, config-change re-derivation).
// The two consumers MUST stay behaviorally identical — that is the point of
// this header. Keep it free of nlohmann/json and other heavy includes.

#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace naab {
namespace keywords {

// Stop words: English function words >3 chars + LLM response boilerplate.
// No programming terms — those are domain-relevant for coding assistants.
inline const std::unordered_set<std::string>& stopWords() {
    static const std::unordered_set<std::string> kStopWords = {
        "that", "this", "with", "from", "have", "your", "will", "also",
        "each", "more", "like", "just", "some", "when", "then",
        "into", "here", "been", "both", "want", "used", "them", "than",
        "what", "were", "they", "does", "done", "very", "much", "most",
        "over", "such", "should", "would", "could", "about",
        "other", "their", "there", "which", "these", "those", "being",
        "after", "before",
        "sure", "great", "lets", "following", "below",
        "approach", "solution", "need", "look"
    };
    return kStopWords;
}

// Code stop words: language syntax and boilerplate tokens that carry no
// topical signal. Deliberately EXCLUDES topical words that double as
// syntax ("class", "main", "pass", "case") — for a coding agent those are
// domain-relevant and may legitimately appear in an English mandate.
inline const std::unordered_set<std::string>& codeStopWords() {
    static const std::unordered_set<std::string> kCodeStopWords = {
        "return", "returns", "self", "print", "import", "elif", "else",
        "true", "false", "none", "null", "while", "break", "continue",
        "raise", "except", "finally", "yield", "lambda", "assert",
        "global", "nonlocal", "async", "await", "args", "kwargs", "init",
        "void", "const", "static", "public", "private", "protected",
        "function", "struct", "enum", "typeof", "instanceof",
        "catch", "throw"
    };
    return kCodeStopWords;
}

namespace detail {

inline void emitCandidate(const std::string& lower,
                          std::unordered_set<std::string>& out) {
    if (lower.size() > 3 && !stopWords().count(lower) &&
        !codeStopWords().count(lower)) {
        out.insert(lower);
    }
}

// Flush one raw (case-preserved) alphanumeric token: emit the whole token
// lowercased, plus its camelCase / digit-boundary components when the token
// splits into two or more parts. Splitting code identifiers into their
// component words is what lets code responses share a token space with
// English mandate keywords (getHistory -> history, TodoItem -> todo, item).
inline void flushToken(const std::string& raw,
                       std::unordered_set<std::string>& out) {
    if (raw.empty()) return;

    std::string whole;
    whole.reserve(raw.size());
    for (char c : raw) {
        whole += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    emitCandidate(whole, out);

    auto up = [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; };
    auto lo = [](char c) { return std::islower(static_cast<unsigned char>(c)) != 0; };
    auto dg = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    auto al = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };

    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 1; i < raw.size(); ++i) {
        const char p = raw[i - 1];
        const char c = raw[i];
        const bool boundary =
            ((lo(p) || dg(p)) && up(c)) ||                          // getHistory, sha1Sum
            (up(p) && up(c) && i + 1 < raw.size() && lo(raw[i + 1])) ||  // HTTPServer
            (al(p) && dg(c)) ||                                     // sha256
            (dg(p) && al(c));                                       // 2fast
        if (boundary) {
            parts.push_back(raw.substr(start, i - start));
            start = i;
        }
    }
    if (parts.empty()) return;  // no boundaries: whole token already emitted
    parts.push_back(raw.substr(start));

    for (auto& part : parts) {
        for (char& c : part) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        emitCandidate(part, out);
    }
}

}  // namespace detail

// Extract >3-char lowercase keywords from text (stop words filtered).
// Tokens are maximal alphanumeric runs; every other character delimits
// (so snake_case identifiers split into their component words). Code-aware:
// camelCase/PascalCase and letter-digit boundaries additionally emit their
// component words, and language syntax tokens are filtered (codeStopWords).
// Symmetric by construction — mandate, instruction, and response keywords
// all pass through this same function, so overlap ratios stay meaningful.
inline void extractKeywords(const std::string& text,
                            std::unordered_set<std::string>& out) {
    std::string current;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current += c;
        } else {
            detail::flushToken(current, out);
            current.clear();
        }
    }
    detail::flushToken(current, out);
}

}  // namespace keywords
}  // namespace naab
