// NAAb loopback URL check -- the one place that decides whether a plain-http
// URL points at this machine.
//
// Per-agent api_base is https-only, except plain http to loopback (local test
// stubs). Both the config loader and the HTTP client used to decide that with
// a string-PREFIX match: rfind("http://127.0.0.1", 0) == 0. A prefix is not a
// host. "http://127.0.0.1@192.0.2.2:8080/" starts with it -- the part before
// '@' is userinfo -- and the request, API-key header included, went over
// plaintext HTTP to 192.0.2.2. "http://localhost.example.com/" passes the
// same way.
//
// This parses the authority and accepts only an exact loopback host, with an
// optional numeric port: no userinfo, no other characters. Anything it cannot
// classify is not loopback, so the caller falls back to requiring https.

#pragma once

#include <cctype>
#include <string>

namespace naab {
namespace net {

inline bool isLoopbackHttpUrl(const std::string& url) {
    static const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;

    // Authority ends at the first '/', '?' or '#'.
    const size_t start = scheme.size();
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    const std::string authority = url.substr(start, end - start);
    if (authority.empty()) return false;

    std::string host, port;
    if (authority[0] == '[') {                        // [::1] or [::1]:port
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        host = authority.substr(0, close + 1);
        const std::string rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest[0] != ':') return false;
            port = rest.substr(1);
            if (port.empty()) return false;
        }
    } else {
        const size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) return false;
        }
    }
    // Exact hosts only. Rejecting everything else also rejects userinfo ('@'),
    // percent-encoding and look-alikes such as "localhost.example.com".
    if (host != "127.0.0.1" && host != "localhost" && host != "[::1]") return false;
    for (char c : port) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

} // namespace net
} // namespace naab
