// language_descriptors.cpp -- the language table. See language_descriptors.h.
#include "naab/language_descriptors.h"

#include <algorithm>
#include <cctype>

namespace naab {
namespace lang {

namespace {

std::string lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

const BlockComment kCBlock{"/*", "*/", false};

// Parse-only checks run through python3, which every supported platform's
// test environment has. SQL is checked by Python's sqlite3 module -- SQLite,
// the same engine NAAb's own SQL executor embeds.
const char* const kPyParse =
    "import ast,sys; ast.parse(open(sys.argv[1], encoding='utf-8').read())";
const char* const kPySqlite =
    "import sqlite3,sys; sqlite3.connect(':memory:').executescript("
    "open(sys.argv[1], encoding='utf-8').read())";

std::vector<LanguageDescriptor> buildTable() {
    // Every executor registered in src/cli/main.cpp (registerExecutor) must
    // resolve here, under its canonical name or an alias.
    return {
        // canonical     aliases        line comments  block comments            gov-only
        //   extension  prelude   syntax checks (parse only, never run)
        {"python",     {"py"},              {"#"},             {},                        false,
         ".py",   "",        {{"python3", "-c", kPyParse, "{file}"}}},
        {"javascript", {"js", "node"},      {"//"},            {kCBlock},                 false,
         ".js",   "",        {{"node", "--check", "{file}"}}},
        {"typescript", {"ts"},              {"//"},            {kCBlock},                 false,
         ".ts",   "",        {{"tsc", "--noEmit", "--skipLibCheck", "{file}"}}},
        {"shell",      {"sh", "bash"},      {"#"},             {},                        false,
         ".sh",   "",        {{"bash", "-n", "{file}"}}},
        {"ruby",       {"rb"},              {"#"},             {{"=begin", "=end", true}}, false,
         ".rb",   "",        {{"ruby", "-c", "{file}"}}},
        {"go",         {"golang"},          {"//"},            {kCBlock},                 false,
         ".go",   "package main\n", {{"gofmt", "-e", "{file}"}}},
        {"cpp",        {"c++"},             {"//"},            {kCBlock},                 false,
         ".cpp",  "",        {{"g++", "-fsyntax-only", "-x", "c++", "{file}"},
                              {"clang++", "-fsyntax-only", "-x", "c++", "{file}"}}},
        {"csharp",     {"cs"},              {"//"},            {kCBlock},                 false,
         ".cs",   "",        {}},
        {"rust",       {},                  {"//"},            {kCBlock},                 false,
         ".rs",   "",        {{"rustc", "--crate-type=lib", "--emit=metadata", "--out-dir", "{dir}", "{file}"}}},
        {"nim",        {},                  {"#"},             {{"#[", "]#", false}},     false,
         ".nim",  "",        {{"nim", "check", "--hints:off", "{file}"}}},
        {"php",        {},                  {"//", "#"},       {kCBlock},                 false,
         ".php",  "<?php\n", {{"php", "-l", "{file}"}}},
        {"julia",      {},                  {"#"},             {{"#=", "=#", false}},     false,
         ".jl",   "",        {{"julia", "--startup-file=no", "{file}"}}},
        {"zig",        {},                  {"//"},            {},                        false,
         ".zig",  "",        {{"zig", "ast-check", "{file}"}}},
        {"sql",        {"sqlite"},          {"--"},            {kCBlock},                 false,
         ".sql",  "",        {{"python3", "-c", kPySqlite, "{file}"}}},
        // NAAb itself: the host language, not a polyglot block, so it is
        // governance-only here. Its text-level checks (checkNaabFunctionBody)
        // read this entry; its parse-tree checks (taint, contracts, function
        // capabilities) never read the table at all. Comment forms are the
        // lexer's (Lexer::skipComment): #, // and /* */. naab-lang is its own
        // judge in langconform verify.
        {"naab",       {},                  {"#", "//"},       {kCBlock},                 true,
         ".naab", "",        {{"naab-lang", "{file}", "--no-governance"}}},
        // Known to governance (naab-gov check, the C API) with no executor.
        {"lua",        {},                  {"--"},            {{"--[[", "]]", false}},   true,
         ".lua",  "",        {{"luac", "-p", "{file}"}}},
        {"haskell",    {},                  {"--"},            {{"{-", "-}", false}},     true,
         ".hs",   "main = return ()\n", {{"ghc", "-fno-code", "{file}"}}},
        {"ada",        {},                  {"--"},            {},                        true,
         ".adb",  "",        {}},
    };
}

bool atLineStart(const std::string& code, size_t i) {
    while (i > 0) {
        char c = code[i - 1];
        if (c == '\n') return true;
        if (c != ' ' && c != '\t') return false;
        --i;
    }
    return true;
}

}  // namespace

const std::vector<LanguageDescriptor>& allLanguages() {
    static const std::vector<LanguageDescriptor> table = buildTable();
    return table;
}

const LanguageDescriptor* findLanguage(const std::string& name) {
    const std::string n = lower(name);
    for (const auto& d : allLanguages()) {
        if (d.canonical == n) return &d;
        if (std::find(d.aliases.begin(), d.aliases.end(), n) != d.aliases.end()) return &d;
    }
    return nullptr;
}

std::string canonicalLanguage(const std::string& name) {
    const LanguageDescriptor* d = findLanguage(name);
    return d ? d->canonical : lower(name);
}

std::string stripComments(const std::string& code, const std::string& language) {
    const LanguageDescriptor* d = findLanguage(language);
    if (!d || (d->line_comments.empty() && d->block_comments.empty())) return code;

    std::string result;
    result.reserve(code.size());
    for (size_t i = 0; i < code.size(); ++i) {
        bool handled = false;

        // Block comments first: replaced with spaces through the closer,
        // newlines kept so line numbers in later findings stay right. An
        // unterminated block runs to the end of the code.
        for (const auto& b : d->block_comments) {
            if (code.compare(i, b.open.size(), b.open) != 0) continue;
            if (b.line_start_only && !atLineStart(code, i)) continue;
            result.append(b.open.size(), ' ');
            i += b.open.size();
            while (i < code.size()) {
                if (code.compare(i, b.close.size(), b.close) == 0) {
                    result.append(b.close.size(), ' ');
                    i += b.close.size() - 1;  // the outer loop does +1
                    break;
                }
                result += (code[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            handled = true;
            break;
        }
        if (handled) continue;

        // Line comments: the rest of the line becomes spaces.
        for (const auto& m : d->line_comments) {
            if (code.compare(i, m.size(), m) != 0) continue;
            while (i < code.size() && code[i] != '\n') {
                result += ' ';
                ++i;
            }
            if (i < code.size()) result += '\n';
            handled = true;
            break;
        }
        if (handled) continue;

        result += code[i];
    }
    return result;
}

namespace {
bool patternsReadOpener(const std::string& open) {
    return open == "#" || open == "//" || open == "/*";
}
}  // namespace

std::string markCommentsForScan(const std::string& code, const std::string& language) {
    const LanguageDescriptor* d = findLanguage(language);
    if (!d) return code;
    bool needed = false;
    for (const auto& m : d->line_comments) needed = needed || !patternsReadOpener(m);
    for (const auto& b : d->block_comments) needed = needed || !patternsReadOpener(b.open);
    if (!needed) return code;

    std::string result;
    result.reserve(code.size() + 16);
    for (size_t i = 0; i < code.size(); ++i) {
        bool handled = false;
        for (const auto& b : d->block_comments) {
            if (code.compare(i, b.open.size(), b.open) != 0) continue;
            if (b.line_start_only && !atLineStart(code, i)) continue;
            const bool rewrite = !patternsReadOpener(b.open);
            // A block the patterns already read (/* */) is copied unchanged;
            // a rewritten one gets `#` + padding for its opener, `# ` at the
            // start of every further line, and spaces for its closer.
            if (rewrite) {
                result += '#';
                result.append(b.open.size() - 1, ' ');
            } else {
                result += b.open;
            }
            i += b.open.size();
            while (i < code.size()) {
                if (code.compare(i, b.close.size(), b.close) == 0) {
                    if (rewrite) result.append(b.close.size(), ' ');
                    else result += b.close;
                    i += b.close.size() - 1;
                    break;
                }
                result += code[i];
                if (rewrite && code[i] == '\n') result += "# ";
                ++i;
            }
            handled = true;
            break;
        }
        if (handled) continue;
        for (const auto& m : d->line_comments) {
            if (code.compare(i, m.size(), m) != 0) continue;
            if (patternsReadOpener(m)) {
                result += m;
            } else {
                result += '#';
                result.append(m.size() - 1, ' ');
            }
            i += m.size();
            while (i < code.size() && code[i] != '\n') result += code[i++];
            if (i < code.size()) result += '\n';
            handled = true;
            break;
        }
        if (handled) continue;
        result += code[i];
    }
    return result;
}

}  // namespace lang
}  // namespace naab
