#pragma once
// capabilities.env_vars per-variable lists, decided in one place. Used by the
// env gates (GovernanceEngine::checkEnvVarRead / checkEnvVarWrite) and by the
// mid-run reload ratchet (checkRatchetViolation), so a reload is judged by the
// same rule that enforces the lists.
//
// blocked_* is a HARD denial; a non-empty allowed_* is an allowlist whose miss
// is a SOFT denial. Names compare ASCII case-insensitively, as they always
// have in the gates.

#include <cctype>
#include <string>
#include <vector>

namespace naab {
namespace governance {

// Ordered by severity: Blocked (HARD) > NotAllowlisted (SOFT) > None.
enum class EnvListDenial { None = 0, NotAllowlisted = 1, Blocked = 2 };

inline bool envNameEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

inline EnvListDenial envListDenial(const std::vector<std::string>& blocked,
                                   const std::vector<std::string>& allowed,
                                   const std::string& name) {
    for (const auto& b : blocked)
        if (envNameEquals(name, b)) return EnvListDenial::Blocked;
    if (!allowed.empty()) {
        for (const auto& a : allowed)
            if (envNameEquals(name, a)) return EnvListDenial::None;
        return EnvListDenial::NotAllowlisted;
    }
    return EnvListDenial::None;
}

}  // namespace governance
}  // namespace naab
