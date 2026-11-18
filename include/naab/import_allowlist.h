#pragma once
// The import allowlist: restrictions.imports.mode "allowlist" with
// restrictions.imports.allowed.<lang>, and
// languages.per_language.<lang>.imports.mode "allowlist" with its
// imports.allowed. Both were parsed and read by nothing (A175); the owner
// decided (2026-11-18) that the allowlist is STATIC: every import a block
// writes must be on the list, an import whose module name is computed is
// refused because it cannot be checked, and a config that sets an allowlist
// for a language whose imports cannot be listed is refused at load.
//
// Static means the text of the block, as the import blocklist reads it
// (comments included, so a module named only in a comment counts). It is not
// containment: code can reach a module already loaded without importing it
// (A7), and an import assembled at run time inside exec()/eval() is not an
// import statement. The run-time import hook in embedded Python enforces the
// BLOCKED list only.
#include <string>
#include <vector>

namespace naab {
namespace governance {

// The languages whose written imports checkImports() can list (by their
// canonical name). Anything else under an allowlist refuses the config.
inline bool importsListable(const std::string& canonical_language) {
    return canonical_language == "python" || canonical_language == "javascript" ||
           canonical_language == "typescript" || canonical_language == "go" ||
           canonical_language == "ruby";
}

// The separator a module name nests by: python "os" admits "os.path"; a
// JavaScript, Go or Ruby path "lodash" admits "lodash/fp".
inline char importNameSeparator(const std::string& canonical_language) {
    return canonical_language == "python" ? '.' : '/';
}

// Is `name` on the list: equal to an entry, or nested under one.
inline bool importAdmitted(const std::string& name, const std::vector<std::string>& allowed,
                           char separator) {
    for (const auto& entry : allowed) {
        if (entry.empty()) continue;
        if (name == entry) return true;
        if (name.size() > entry.size() && name.compare(0, entry.size(), entry) == 0 &&
            name[entry.size()] == separator)
            return true;
    }
    return false;
}

}  // namespace governance
}  // namespace naab
