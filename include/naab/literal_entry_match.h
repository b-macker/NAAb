#pragma once
// A config entry naming a function or command, matched against code as
// literal text, case-insensitively: regex metacharacters are escaped, so
// "curl |" or "Function(" mean exactly that, and an entry with a space must
// end at whitespace or the end of the code ("rm -rf /" does not match
// "rm -rf /tmp/safe").
//
// One copy for the lists that name things this way:
// languages.per_language.<lang>.banned_functions (checkBannedFunctions) and
// restrictions.dangerous_calls.blocklist_extra (checkDangerousCall), so the
// two cannot come to read one entry differently. Throws std::regex_error only
// if the escaped entry still fails to compile.
//
// The language's reserved word (A184): case is ignored because PHP's function
// names are case-insensitive and an alias can be spelled in any case, but
// JavaScript and TypeScript are case-sensitive, and there "Function(" -- the
// constructor -- also matched the keyword in every anonymous function,
// `function (x) {...}`, so a project that banned the constructor refused
// ordinary code. A match whose leading word is spelled exactly `function` is
// the keyword and is skipped, and the search goes on past it -- A91's rule for
// restrictions.dangerous_calls, which kept a copy of its own. An entry that
// itself starts with the exact word `function` names the keyword, and is
// honoured as written.
#include <string>

#include "naab/language_descriptors.h"
#include "naab/rx.h"

namespace naab {
namespace governance {

namespace literal_entry_detail {
// The identifier the text starts with ([A-Za-z0-9_$]*), as written.
inline std::string leadingWord(const std::string& s) {
    size_t n = 0;
    while (n < s.size()) {
        const char c = s[n];
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '$';
        if (!word) break;
        ++n;
    }
    return s.substr(0, n);
}
// The reserved word a case-insensitive match can be spelled as in `language`,
// or "" when case-insensitive matching is that language's own rule.
inline std::string reservedWordFor(const std::string& language) {
    const std::string lang = naab::lang::canonicalLanguage(language);
    if (lang == "javascript" || lang == "typescript") return "function";
    return "";
}
}  // namespace literal_entry_detail

inline bool literalEntryMatches(const std::string& code, const std::string& entry,
                                const std::string& language) {
    std::string escaped;
    for (char c : entry) {
        if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos) escaped += '\\';
        escaped += c;
    }
    if (entry.find(' ') != std::string::npos) escaped += "(?:\\s|$)";
    naab::rx::regex re(escaped, naab::rx::regex::icase);
    const std::string keyword = literal_entry_detail::reservedWordFor(language);
    if (keyword.empty() || literal_entry_detail::leadingWord(entry) == keyword)
        return naab::rx::regex_search(code, re);
    for (naab::rx::sregex_iterator it(code.begin(), code.end(), re), end; it != end; ++it) {
        if (literal_entry_detail::leadingWord((*it)[0].str()) == keyword) continue;
        return true;
    }
    return false;
}

}  // namespace governance
}  // namespace naab
