// The agent and telemetry curl sites must build their proxy/CA from the
// LOAD-TIME snapshot, not the live environment, so a program's env.set cannot
// repoint a key-bearing TLS session. These tests pin that property and the
// per-scheme proxy selection. The snapshot itself is captured once per process
// (std::call_once), so to test capture-then-mutate without depending on that
// global's state we build ProxyCaSnapshot values directly and also exercise
// the real global once.

#include "naab/net_proxy_snapshot.h"
#include <gtest/gtest.h>
#include <cstdlib>

using naab::net::ProxyCaSnapshot;
using naab::net::proxyForUrl;
using naab::net::caInfoFromSnapshot;

TEST(NetProxySnapshot, HttpsUrlPrefersHttpsProxyThenAllProxy) {
    ProxyCaSnapshot s;
    s.https_proxy = "http://ok-https:8080";
    s.all_proxy   = "http://ok-all:8080";
    s.http_proxy_lc = "http://ok-http:8080";
    EXPECT_EQ(proxyForUrl(s, "https://api.example.com/v1"), "http://ok-https:8080");

    ProxyCaSnapshot s2;              // no https_proxy -> falls back to all_proxy
    s2.all_proxy = "http://ok-all:8080";
    EXPECT_EQ(proxyForUrl(s2, "https://api.example.com/v1"), "http://ok-all:8080");

    ProxyCaSnapshot s3;              // nothing set -> "" (direct, env ignored)
    EXPECT_EQ(proxyForUrl(s3, "https://api.example.com/v1"), "");
}

TEST(NetProxySnapshot, HttpUrlPrefersHttpProxyThenAllProxy) {
    ProxyCaSnapshot s;
    s.http_proxy_lc = "http://ok-http:8080";
    s.all_proxy     = "http://ok-all:8080";
    EXPECT_EQ(proxyForUrl(s, "http://127.0.0.1:1234/"), "http://ok-http:8080");

    ProxyCaSnapshot s2;
    s2.all_proxy = "http://ok-all:8080";
    EXPECT_EQ(proxyForUrl(s2, "http://127.0.0.1:1234/"), "http://ok-all:8080");
}

TEST(NetProxySnapshot, CaInfoPrefersSslCertFileThenCurlCaBundle) {
    ProxyCaSnapshot s;
    s.ssl_cert_file  = "/etc/ssl/real.pem";
    s.curl_ca_bundle = "/etc/ssl/other.pem";
    EXPECT_EQ(caInfoFromSnapshot(s), "/etc/ssl/real.pem");

    ProxyCaSnapshot s2;
    s2.curl_ca_bundle = "/etc/ssl/other.pem";
    EXPECT_EQ(caInfoFromSnapshot(s2), "/etc/ssl/other.pem");

    ProxyCaSnapshot s3;
    EXPECT_EQ(caInfoFromSnapshot(s3), "");   // "" -> libcurl default, not ""-CAINFO
}

// The load-bearing property: once captured, a later env change (what a NAAb
// program's env.set does) does NOT alter the snapshot the curl sites read.
TEST(NetProxySnapshot, SnapshotIsImmuneToLaterEnvChange) {
    // Set a distinctive CA before the (process-once) capture.
    setenv("SSL_CERT_FILE", "/load-time/ca.pem", 1);
    unsetenv("CURL_CA_BUNDLE");
    naab::net::captureProxyCaSnapshotOnce();      // first call in this process wins

    const ProxyCaSnapshot& snap = naab::net::proxyCaSnapshot();
    const std::string ca_at_load = caInfoFromSnapshot(snap);

    // A program "env.set"s a CA it controls, after load.
    setenv("SSL_CERT_FILE", "/attacker/ca.pem", 1);

    // The snapshot the curl sites use is unchanged: still the load-time value,
    // never the attacker's. (If the capture had not run before this test in
    // this process, ca_at_load is whatever the env held at first capture; the
    // assertion that matters is that the post-change read equals the captured
    // one, i.e. live env no longer leaks in.)
    EXPECT_EQ(caInfoFromSnapshot(naab::net::proxyCaSnapshot()), ca_at_load);
    EXPECT_NE(caInfoFromSnapshot(naab::net::proxyCaSnapshot()), "/attacker/ca.pem");
}
