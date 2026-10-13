// NAAb proxy / CA snapshot -- the outbound-TLS settings an agent call and the
// telemetry webhook use are fixed WHEN THE ENGINE LOADS, not read live.
//
// agent_provider.cpp and telemetry_forwarder.cpp build https requests that
// carry the operator's API key and data off the machine. libcurl, left to
// itself, reads the proxy from the live environment (http_proxy / https_proxy /
// all_proxy), and both call sites read the CA bundle (SSL_CERT_FILE /
// CURL_CA_BUNDLE) with std::getenv at call time. A NAAb program's
// env.set writes the REAL process environment (env writes default-allowed), so
// a program could repoint the agent's proxy to one it controls, or point the
// agent's trust store at a CA it controls -- in either case the key-bearing
// TLS session is the program's to intercept. Traced in the code and confirmed
// in-process: env.set changes what std::getenv returns in the same process.
//
// This captures those values ONCE, at engine load, before any program
// statement runs (std::call_once, invoked from loadFromJson -- the one path
// every load and the reload pass through; the first, startup, call wins and a
// later reload is a no-op, which is correct because a reload happens after
// user code). Both call sites then set CURLOPT_PROXY / CURLOPT_NOPROXY /
// CURLOPT_CAINFO explicitly from the snapshot, which also stops libcurl reading
// the live proxy env. A program's later env.set cannot move either.
//
// This does NOT gate agent calls on capabilities.network.enabled (owner's
// decision 2026-10-13): that switch governs what the PROGRAM does; agent
// endpoints are operator-declared in govern.json, like the hook-style command
// guidance source.

#pragma once

#include <cstdlib>
#include <mutex>
#include <string>

namespace naab {
namespace net {

struct ProxyCaSnapshot {
    bool captured = false;
    // Proxy env, both cases. libcurl treats http_proxy as lowercase-only but
    // reads either case for https_proxy / all_proxy; we keep all and resolve
    // per scheme in proxyForUrl().
    std::string http_proxy_lc;   // "http_proxy"
    std::string https_proxy;     // https_proxy / HTTPS_PROXY (lowercase wins)
    std::string all_proxy;       // all_proxy / ALL_PROXY  (lowercase wins)
    std::string no_proxy;        // no_proxy / NO_PROXY    (lowercase wins)
    std::string ssl_cert_file;   // SSL_CERT_FILE
    std::string curl_ca_bundle;  // CURL_CA_BUNDLE
};

inline ProxyCaSnapshot& proxyCaSnapshotMutable() {
    static ProxyCaSnapshot snap;   // one instance process-wide (inline fn)
    return snap;
}

// Read one env var, preferring the lowercase spelling, then uppercase.
inline std::string readEnvPreferLower(const char* lower, const char* upper) {
    const char* v = std::getenv(lower);
    if (!v || !v[0]) v = std::getenv(upper);
    return (v && v[0]) ? std::string(v) : std::string();
}

// Capture the proxy/CA environment exactly once. Call from the config load
// path so the values predate any env.set a program could run.
inline void captureProxyCaSnapshotOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        ProxyCaSnapshot& s = proxyCaSnapshotMutable();
        const char* hp = std::getenv("http_proxy");   // lowercase-only by convention
        s.http_proxy_lc   = (hp && hp[0]) ? std::string(hp) : std::string();
        s.https_proxy     = readEnvPreferLower("https_proxy", "HTTPS_PROXY");
        s.all_proxy       = readEnvPreferLower("all_proxy", "ALL_PROXY");
        s.no_proxy        = readEnvPreferLower("no_proxy", "NO_PROXY");
        const char* ca = std::getenv("SSL_CERT_FILE");
        s.ssl_cert_file   = (ca && ca[0]) ? std::string(ca) : std::string();
        const char* cb = std::getenv("CURL_CA_BUNDLE");
        s.curl_ca_bundle  = (cb && cb[0]) ? std::string(cb) : std::string();
        s.captured = true;
    });
}

inline const ProxyCaSnapshot& proxyCaSnapshot() {
    // Backstop only: the engine's load path is the real guarantee that this
    // runs before user code. If some path reached a curl site without ever
    // loading a config, capture now rather than read live env on every call.
    captureProxyCaSnapshotOnce();
    return proxyCaSnapshotMutable();
}

// The proxy libcurl would pick for this URL's scheme, from the SNAPSHOT.
// https URL -> https_proxy else all_proxy; anything else -> http_proxy else
// all_proxy. Empty means "no proxy" (we set CURLOPT_PROXY to "" so libcurl does
// not then consult the live env).
inline std::string proxyForUrl(const ProxyCaSnapshot& s, const std::string& url) {
    const bool is_https = url.compare(0, 8, "https://") == 0;
    if (is_https) return !s.https_proxy.empty() ? s.https_proxy : s.all_proxy;
    return !s.http_proxy_lc.empty() ? s.http_proxy_lc : s.all_proxy;
}

// The CA bundle path from the snapshot: SSL_CERT_FILE, else CURL_CA_BUNDLE,
// else "" (libcurl's own default). The Termux fallback stays at the call site
// because it depends on std::filesystem::exists, which this header avoids.
inline std::string caInfoFromSnapshot(const ProxyCaSnapshot& s) {
    if (!s.ssl_cert_file.empty()) return s.ssl_cert_file;
    if (!s.curl_ca_bundle.empty()) return s.curl_ca_bundle;
    return std::string();
}

} // namespace net
} // namespace naab
