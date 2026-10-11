#pragma once
// The project path policy's decision, shared by the filesystem gate
// (GovernanceEngine::checkPathAccessImpl, governance_engine.cpp) and the
// mid-run reload ratchet (checkRatchetViolation, governance_config.cpp). The
// ratchet asks THIS function whether a reload widens access, so it cannot
// judge a path list by a different rule than the gate that enforces it.
//
// Pure: no engine state. Entries are resolved by the caller's `resolve`
// (relative entries are project-relative; see checkPathAccessImpl).

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace naab {
namespace governance {

// ---------------------------------------------------------------------------
// Path policy precedence, decided in one place.
//
// An allowed entry and a blocked entry can both match the same path, and which
// one wins used to be answered twice in this file with two different answers.
// The capabilities layer let ANY allowed entry cancel EVERY blocked entry; the
// agent-role overlay applied its blocked list first and unconditionally.
//
// The capabilities answer was the wrong one. Its comment read "specific allow
// beats broad deny", but nothing compared specificity, so allowed_paths ["."]
// cancelled a blocked entry naming a single file. That silently voided
// addGovernanceProtectedPaths(), the mechanism that stops a NAAb program from
// rewriting its own govern.json: measured, a project allowing "." could read
// its govern.json, and the same config with allowed_paths empty could not.
//
// The rule is now longest matching prefix wins, and a tie denies. A blocked
// entry naming a file beats an allowed entry naming its directory, while the
// documented "allowed ./data beats blocked /" pattern still works because
// ./data resolves to the longer prefix.
//
// The agent-role overlay keeps deny-first, which is strictly stricter than
// longest-prefix. That asymmetry is deliberate: a role narrows the project
// policy and must never widen it. It is expressed here as a mode rather than
// left to the order in which two loops happen to be written.
// ---------------------------------------------------------------------------

enum class PathPrecedence {
    LongestPrefixWins,  // capabilities.filesystem
    DenyWins,           // agents.<name> overlay
};

struct PathMatch {
    bool matched = false;
    std::string rule;        // the configured entry that matched
    std::size_t specificity = 0;  // length of that entry once resolved
};

struct PathVerdict {
    enum class Result { Allow, NotAllowlisted, Blocked };
    Result result = Result::Allow;
    std::string rule;  // the blocked entry, when result is Blocked
};

// V-GOV-022: prefix match with a directory boundary, so /data/safe does not
// match /data/safe_malicious.
inline bool pathPrefixMatch(const std::string& path, const std::string& prefix) {
    // An empty entry names no path, so it matches nothing.
    //
    // This guard is not hypothetical: weakly_canonical("") resolves to "" here,
    // so a `""` in either list reached prefix.back() below, which is undefined
    // on an empty string. Measured on this platform the UB read as "matches
    // everything" -- allowed_paths [""] granted every path and blocked_paths
    // [""] denied every path. Defining it costs one direction: blocked_paths
    // [""] no longer blocks everything. That was never a specified behaviour to
    // rely on, and no reader of a config writes an empty string meaning "all".
    if (prefix.empty()) return false;
    if (path.find(prefix) != 0) return false;
    return path.size() == prefix.size() ||
           prefix.back() == '/' ||
           path[prefix.size()] == '/';
}

inline PathMatch longestPathMatch(const std::string& canonical_path,
                           const std::vector<std::string>& list,
                           const std::function<std::string(const std::string&)>& resolve) {
    PathMatch best;
    for (const auto& entry : list) {
        const std::string resolved = resolve(entry);
        if (!pathPrefixMatch(canonical_path, resolved)) continue;
        if (best.matched && resolved.size() <= best.specificity) continue;
        best.matched = true;
        best.rule = entry;
        best.specificity = resolved.size();
    }
    return best;
}

inline PathVerdict decidePathAccess(const std::string& canonical_path,
                             const std::vector<std::string>& allowed,
                             const std::vector<std::string>& blocked,
                             PathPrecedence precedence,
                             const std::function<std::string(const std::string&)>& resolve) {
    const PathMatch allow_match = longestPathMatch(canonical_path, allowed, resolve);
    const PathMatch block_match = longestPathMatch(canonical_path, blocked, resolve);

    if (precedence == PathPrecedence::DenyWins) {
        if (block_match.matched) return {PathVerdict::Result::Blocked, block_match.rule};
        if (!allowed.empty() && !allow_match.matched)
            return {PathVerdict::Result::NotAllowlisted, ""};
        return {PathVerdict::Result::Allow, ""};
    }

    // A non-empty allowed list is an allowlist: no match is a denial whatever
    // the blocked list says.
    if (!allowed.empty() && !allow_match.matched)
        return {PathVerdict::Result::NotAllowlisted, ""};

    // >= so that an equally specific blocked entry wins. Ties deny.
    if (block_match.matched &&
        (!allow_match.matched || block_match.specificity >= allow_match.specificity))
        return {PathVerdict::Result::Blocked, block_match.rule};

    return {PathVerdict::Result::Allow, ""};
}


}  // namespace governance
}  // namespace naab
