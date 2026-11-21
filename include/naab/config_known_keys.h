#pragma once
// The govern.json paths the loader reads, generated from the loader itself
// (tools/config_known_keys.py -> src/runtime/governance_known_keys.inc).
// A path's segments are joined by '\x1f'; '*' stands for any key of an open
// map (agents.<name>, capabilities.functions.<fn>, field names), '[]' for an
// array element. One copy, read by the nested unknown-key warning and by the
// loader's type check (checkRestrictionTypes, A111), so the two cannot
// disagree on what is a setting and what is a user's name.
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace naab {
namespace governance {

struct KnownConfigSchema {
    std::unordered_set<std::string> known;
    std::unordered_map<std::string, std::vector<std::string>> children;
    // The known paths that extend each candidate by `seg`: the key itself, and
    // '*' where the candidate is an open map.
    std::vector<std::string> step(const std::vector<std::string>& cands,
                                  const std::string& seg) const;
};

const KnownConfigSchema& knownConfigSchema();

// A comment or annotation key the loader does not read: `_...`, `rationale`,
// `..._rationale`.
bool isConfigAnnotationKey(const std::string& k);

}  // namespace governance
}  // namespace naab
