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

std::vector<LanguageDescriptor> buildTable() {
    // Every executor registered in src/cli/main.cpp (registerExecutor) must
    // resolve here, under its canonical name or an alias.
    return {
        {"python",     {"py"},              {"#"},  {},        false},
        {"javascript", {"js", "node"},      {"//"}, {kCBlock}, false},
        {"typescript", {"ts"},              {},     {},        false},
        {"shell",      {"sh", "bash"},      {"#"},  {},        false},
        {"ruby",       {"rb"},              {"#"},  {},        false},
        {"go",         {"golang"},          {"//"}, {kCBlock}, false},
        {"cpp",        {"c++"},             {"//"}, {kCBlock}, false},
        {"csharp",     {"cs"},              {"//"}, {kCBlock}, false},
        {"rust",       {},                  {"//"}, {kCBlock}, false},
        {"nim",        {},                  {"#"},  {},        false},
        {"php",        {},                  {},     {},        false},
        {"julia",      {},                  {},     {},        false},
        {"zig",        {},                  {},     {},        false},
        {"sql",        {"sqlite"},          {"--"}, {},        false},
        // Known to governance (naab-gov check, the C API) with no executor.
        {"lua",        {},                  {"--"}, {},        true},
        {"haskell",    {},                  {"--"}, {},        true},
        {"ada",        {},                  {"--"}, {},        true},
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

}  // namespace lang
}  // namespace naab
