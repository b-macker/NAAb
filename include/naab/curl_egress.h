// Shared libcurl egress settings -- one implementation for every outbound
// connection the engine makes, so the copies cannot drift.
//
// The engine opens connections in six places: an agent call
// (agent_provider.cpp), the telemetry webhook (telemetry_forwarder.cpp), a
// guidance source (governance_guidance.cpp), the program's http module
// (http_impl.cpp), a URL module import (module_resolver.cpp) and the package
// manager (package_manager.cpp, CLI-only: no program runs in that process).
// The first five take their proxy from the load-time snapshot through
// applySnapshotProxy(); the three the operator owns also take their CA bundle
// through applySnapshotCa(). The two the program drives (http module, URL
// import) refuse a private, loopback or link-local address on the socket
// actually opened through refusePrivateOpenSocket().

#pragma once

#include <curl/curl.h>
#include <cstdint>
#include <string>

namespace naab {
namespace net {

// Sets CURLOPT_PROXY and CURLOPT_NOPROXY from the load-time snapshot for this
// URL's scheme -- both always, even to "" (direct / no exclusions), which is
// what stops libcurl reading the LIVE environment that env.set can change. An
// unset CURLOPT_NOPROXY alone is enough for libcurl to read no_proxy live.
// Returns the proxy set ("" = direct). With a redirect, the first URL's proxy
// applies to every hop.
std::string applySnapshotProxy(CURL* curl, const std::string& url);

// Sets CURLOPT_CAINFO from the load-time snapshot: SSL_CERT_FILE, else
// CURL_CA_BUNDLE, exactly as given (a missing file fails TLS -- the operator's
// setting is not replaced by a different trust store); with neither, the
// Termux bundle where that file exists (F32: never a path from one machine);
// else libcurl's own default.
void applySnapshotCa(CURL* curl);

// Private, loopback, link-local or "this network" address (host byte order /
// 16 network-order bytes). IPv4-mapped IPv6 is judged as IPv4.
bool isPrivateIPv4(uint32_t ip);
bool isPrivateIPv6(const unsigned char addr[16]);

// CURLOPT_OPENSOCKETFUNCTION: refuses a connection to a private address. libcurl
// calls it with the RESOLVED address once per socket -- the first request and
// every redirect hop -- so neither a DNS name, an alternate IP spelling (127.1,
// 2130706433, 0177.0.0.1) nor a redirect reaches a private address (#229,
// F43/F47/A11). Through a proxy the socket is the PROXY's, so a proxy on a
// private address is refused too.
curl_socket_t refusePrivateOpenSocket(void* clientp, curlsocktype purpose,
                                      struct curl_sockaddr* address);

} // namespace net
} // namespace naab
