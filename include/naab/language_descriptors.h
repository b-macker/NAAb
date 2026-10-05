// language_descriptors.h -- the ONE place NAAb knows facts about a language.
//
// WHY. Per-language knowledge was written out at every site that needed it:
// 540 language-name comparisons across 21 files on 2026-10-05, three separate
// normalizeLanguage() functions (two of which chose OPPOSITE canonical names
// for C#), and a comment stripper that knew neither `sqlite` (an alias of
// `sql`, same executor) nor any comment form of julia, php, typescript or zig.
// A fix applied at one site never reached the others, so the same block got a
// different governance answer depending on how its language was spelled.
//
// WHAT BELONGS HERE. Facts expressible as data: the canonical name, every
// alias, and the comment syntax. A site that needs a language fact asks this
// table instead of comparing strings. What cannot be data (Python's audit
// hook, SQL's bound parameters) stays in that language's own code.
//
// ADDING A LANGUAGE. Add one entry. tests/self-audit/test_langconform.sh asks
// the binary for every registered name and fails when one has no entry here,
// and generates its probes from these entries, so a new language is covered
// by the conformance matrix the day it is registered.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace naab {
namespace lang {

struct BlockComment {
    std::string open;
    std::string close;
    // The opener only counts at the start of a line (after optional
    // indentation) -- Ruby's =begin/=end.
    bool line_start_only = false;
};

// A line-comment marker and the context in which it actually begins a comment.
// Matching a bare marker anywhere was the #294 regression: shell `#` fired
// inside ${#a[@]}, $#, a#b, so a stray `#` ate a real string's opening quote
// and the code after that string was stripped as if it were string contents
// -- hidden from every check. The constructors keep the plain `{"#"}` / `{"//"}`
// table spelling working; the context flags are set only where a language needs
// them.
struct LineComment {
    std::string marker;
    // shell `#`: a comment only at a token boundary (start of input, or after
    // whitespace/newline or a shell operator). Mid-token it is literal.
    bool require_token_boundary = false;
    // PHP 8 `#`: a comment unless followed by one of these ("[" -> attribute).
    std::vector<std::string> not_if_followed_by;
    // Ruby `#`: not a comment when preceded by `?` (a character literal, ?#).
    bool not_after_question = false;

    LineComment(const char* m) : marker(m) {}
    LineComment(std::string m) : marker(std::move(m)) {}
    LineComment(std::string m, bool boundary,
                std::vector<std::string> followed = {}, bool after_q = false)
        : marker(std::move(m)), require_token_boundary(boundary),
          not_if_followed_by(std::move(followed)), not_after_question(after_q) {}
};

struct LanguageDescriptor {
    std::string canonical;
    std::vector<std::string> aliases;
    std::vector<LineComment> line_comments;
    std::vector<BlockComment> block_comments;
    // True when no executor runs this language: it is known to governance
    // (naab-gov check, the C API) but cannot be a NAAb block.
    bool governance_only = false;

    // --- Toolchain: how the language's OWN tools judge a file -------------
    // Used by tools/langconform verify to prove the facts above against the
    // real compiler/interpreter rather than trusting this table.
    std::string extension;  // source file extension, with the dot
    std::string prelude;    // text a file needs before any code (php: "<?php")
    // Commands that parse a file WITHOUT running it, tried in order; the
    // first whose program is installed is used. "{file}" is the source file,
    // "{dir}" a scratch directory. Exit 0 = the file is valid syntax.
    std::vector<std::vector<std::string>> syntax_checks;

    // Aliases that are ALSO a separate runtime. `node` is JavaScript for every
    // check that reads the code (per_language, rules, comment syntax), but it
    // is its own executor -- a Node.js subprocess with filesystem and process
    // reach, where `javascript` is in-process QuickJS with neither. An
    // allow/block list is a decision about what may RUN, so it needs the
    // runtime, not just the language: allowing QuickJS must not admit Node,
    // and `blocked: ["node"]` must block Node without blocking QuickJS.
    // Every name here must also be in `aliases`.
    std::vector<std::string> runtime_variants;
};

// Does the line comment `lc` actually BEGIN at code[i]? Checks the marker AND
// its context flags (token boundary, not-after-?, not-if-followed-by). The one
// place that answers this, shared by the string stripper, stripComments() and
// markCommentsForScan() so they cannot disagree.
bool lineCommentBegins(const std::string& code, size_t i, const LineComment& lc);

// Every descriptor, in a fixed order.
const std::vector<LanguageDescriptor>& allLanguages();

// The descriptor for a canonical name or alias, case-insensitive; nullptr
// when the name is unknown.
const LanguageDescriptor* findLanguage(const std::string& name);

// The canonical name for `name`, or `name` lower-cased when it is unknown.
std::string canonicalLanguage(const std::string& name);

// The name of the RUNTIME `name` selects: the alias itself when it is one of
// its language's runtime_variants (`node`), otherwise canonicalLanguage().
// Use this, not canonicalLanguage(), wherever a language name decides what may
// run: languages.allowed/blocked, codegen and per-agent language lists,
// runtime pins, and executor lookup.
std::string runtimeLanguage(const std::string& name);

// The one matching rule for allow/block lists of runtime names (entries
// already passed through runtimeLanguage()). A block list blocks a runtime
// when it names the runtime OR the runtime's language, so blocking
// `javascript` still blocks `node`. An allow list admits a runtime only when
// it names that runtime, so allowing `javascript` does not admit `node`.
// Both directions resolve toward refusing.
template <class List>
bool languageListBlocks(const List& blocked, const std::string& runtime) {
    const std::string family = canonicalLanguage(runtime);
    for (const auto& b : blocked)
        if (b == runtime || b == family) return true;
    return false;
}
template <class List>
bool languageListAdmits(const List& allowed, const std::string& runtime) {
    for (const auto& a : allowed)
        if (a == runtime) return true;
    return false;
}

// Replace every comment in `code` with spaces, keeping newlines, using the
// language's own comment syntax. Call it AFTER string literals are stripped,
// so a comment marker inside a string is never read as a comment. An unknown
// language has no comment syntax: nothing is stripped, so every check still
// sees the whole text -- the strict direction.
std::string stripComments(const std::string& code, const std::string& language);

// For the checks that look for markers INSIDE comments (temporary code,
// oversimplification, incomplete logic). Their patterns are written against
// `#`, `//` and C-style `/* */` comments, so a marker in a `--`, `#=`, `#[`,
// `=begin`, `--[[` or `{-` comment was invisible to them. This rewrites only
// THOSE openers to `#` (padded to the same width) and marks each further line
// of such a block with `#`; code, and comments the patterns already read, are
// left byte-for-byte unchanged. For a language whose comments are all `#`,
// `//` or `/* */`, the result IS the input -- so this can add findings in
// the languages the patterns could not read, and change nothing elsewhere.
std::string markCommentsForScan(const std::string& code, const std::string& language);

}  // namespace lang
}  // namespace naab
