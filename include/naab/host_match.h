#pragma once
// One host-policy matcher, shared by the sandbox (connections the PROGRAM
// makes, Sandbox::canConnect) and the guidance connectors (connections the
// ENGINE makes on the operator's behalf, GovernanceEngine::networkPolicyDenial).
// F10: one matcher for both lists, so an operator never has to know which list
// a name is on to predict how it matches. Case-insensitive per RFC 1035, with
// subdomain matching on a dot boundary: "example.com" covers "api.example.com"
// but never "notexample.com". An empty entry matches nothing.

#include <cctype>
#include <string>
#include <vector>

namespace naab {
namespace net {

inline bool hostEqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

inline bool hostMatchesPattern(const std::string& host, const std::string& pattern) {
    if (pattern.empty()) return false;
    return hostEqualsIgnoreCase(host, pattern) ||
           (host.size() > pattern.size() &&
            host[host.size() - pattern.size() - 1] == '.' &&
            hostEqualsIgnoreCase(host.substr(host.size() - pattern.size()), pattern));
}

inline bool hostMatchesAny(const std::string& host, const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) if (hostMatchesPattern(host, p)) return true;
    return false;
}

}  // namespace net
}  // namespace naab
