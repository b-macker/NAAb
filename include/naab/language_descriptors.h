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

struct LanguageDescriptor {
    std::string canonical;
    std::vector<std::string> aliases;
    std::vector<std::string> line_comments;
    std::vector<BlockComment> block_comments;
    // True when no executor runs this language: it is known to governance
    // (naab-gov check, the C API) but cannot be a NAAb block.
    bool governance_only = false;
};

// Every descriptor, in a fixed order.
const std::vector<LanguageDescriptor>& allLanguages();

// The descriptor for a canonical name or alias, case-insensitive; nullptr
// when the name is unknown.
const LanguageDescriptor* findLanguage(const std::string& name);

// The canonical name for `name`, or `name` lower-cased when it is unknown.
std::string canonicalLanguage(const std::string& name);

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
