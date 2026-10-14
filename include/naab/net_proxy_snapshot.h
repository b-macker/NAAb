// NAAb proxy / CA snapshot -- the outbound-TLS settings an agent call, the
// telemetry webhook, a guidance source and the program's own http requests use
// are fixed WHEN THE ENGINE LOADS, not read live.
//
// agent_provider.cpp and telemetry_forwarder.cpp build https requests that
// carry the operator's API key and data off the machine. libcurl, left to
// itself, reads the proxy from the live environment (http_proxy / https_proxy /
// all_proxy), and both call sites used to read the CA bundle (SSL_CERT_FILE /
// CURL_CA_BUNDLE) with std::getenv at call time. A NAAb program's env.set
// writes the REAL process environment, so a program could choose the proxy and
// the trust store of the operator's connections. Measured end to end before
// #313's fix (tests/security/test_operator_egress_env.sh, with the pre-#313
// call sites): a program that set *_proxy to a closed port broke the agent
// call and the webhook; one that pointed SSL_CERT_FILE at a missing file broke
// the https agent call. The same mechanism let a program route its OWN http
// requests through a proxy host the network policy never lists (the host
// lists check the URL's host, the socket goes to the proxy).
//
// The values come from the one load-time environment snapshot
// (env_snapshot.h), captured before any program statement runs. Every curl
// site sets CURLOPT_PROXY / CURLOPT_NOPROXY explicitly from it -- even to ""
// (direct), which also stops libcurl reading the live proxy environment -- and
// the operator-owned sites set CURLOPT_CAINFO from it too.
//
// The rules follow libcurl's own: an http URL uses lower-case http_proxy only
// (libcurl ignores HTTP_PROXY because a CGI host sets it from a request
// header), https_proxy / all_proxy / no_proxy in either case with lower case
// preferred, an empty value counts as unset.
//
// This does NOT gate agent calls on capabilities.network.enabled (owner's
// decision 2026-10-13): that switch governs what the PROGRAM does; agent
// endpoints are operator-declared in govern.json, like the hook-style command
// guidance source.

#pragma once

#include "naab/env_snapshot.h"

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

// One variable from the load-time snapshot, lower-case spelling first.
inline std::string snapshotEnvPreferLower(const char* lower, const char* upper) {
    std::string v = naab::snapshotEnv(lower);
    if (v.empty()) v = naab::snapshotEnv(upper);
    return v;
}

// Derive the proxy/CA settings from the load-time environment snapshot,
// exactly once. Called from the config load path, so the snapshot predates
// any env.set a program could run.
inline void captureProxyCaSnapshotOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        naab::captureEnvironmentSnapshotOnce();
        ProxyCaSnapshot& s = proxyCaSnapshotMutable();
        s.http_proxy_lc   = naab::snapshotEnv("http_proxy");   // lower case only, as libcurl
        s.https_proxy     = snapshotEnvPreferLower("https_proxy", "HTTPS_PROXY");
        s.all_proxy       = snapshotEnvPreferLower("all_proxy", "ALL_PROXY");
        s.no_proxy        = snapshotEnvPreferLower("no_proxy", "NO_PROXY");
        s.ssl_cert_file   = naab::snapshotEnv("SSL_CERT_FILE");
        s.curl_ca_bundle  = naab::snapshotEnv("CURL_CA_BUNDLE");
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
