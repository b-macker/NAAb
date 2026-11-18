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
#include <string>

#include "naab/rx.h"

namespace naab {
namespace governance {

inline bool literalEntryMatches(const std::string& code, const std::string& entry) {
    std::string escaped;
    for (char c : entry) {
        if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos) escaped += '\\';
        escaped += c;
    }
    if (entry.find(' ') != std::string::npos) escaped += "(?:\\s|$)";
    naab::rx::regex re(escaped, naab::rx::regex::icase);
    return naab::rx::regex_search(code, re);
}

}  // namespace governance
}  // namespace naab
