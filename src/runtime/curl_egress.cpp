// Shared libcurl egress settings -- see include/naab/curl_egress.h.

#include "naab/curl_egress.h"
#include "naab/net_proxy_snapshot.h"

#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

namespace naab {
namespace net {

std::string applySnapshotProxy(CURL* curl, const std::string& url) {
    const ProxyCaSnapshot& px = proxyCaSnapshot();
    std::string proxy = proxyForUrl(px, url);
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());   // "" = direct
    // ALWAYS set, even to "": libcurl reads no_proxy / NO_PROXY from the live
    // environment whenever CURLOPT_NOPROXY is unset -- even with CURLOPT_PROXY
    // set explicitly -- so a program's env.set("no_proxy", "*") took an agent
    // call and http.get off the operator's proxy (measured; EO-06).
    curl_easy_setopt(curl, CURLOPT_NOPROXY, px.no_proxy.c_str());
    return proxy;
}

void applySnapshotCa(CURL* curl) {
    const std::string ca = caInfoFromSnapshot(proxyCaSnapshot());
    if (!ca.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
        return;
    }
    // On Termux (Android) the system curl CA bundle is not at the standard
    // path. F32: this used to be pinned UNCONDITIONALLY -- the author's platform
    // compiled in as the default everywhere, which fails TLS on an ordinary
    // Linux box. Used only where the file exists.
    std::error_code ec;
    static const char* kTermuxCa = "/data/data/com.termux/files/usr/etc/tls/cert.pem";
    if (std::filesystem::exists(kTermuxCa, ec) && !ec) curl_easy_setopt(curl, CURLOPT_CAINFO, kTermuxCa);
}

bool isPrivateIPv4(uint32_t ip) {
    if ((ip >> 24) == 127) return true;                 // 127.0.0.0/8 loopback
    if ((ip >> 24) == 10) return true;                  // 10.0.0.0/8 RFC1918
    if ((ip >> 20) == (172 << 4 | 1)) return true;      // 172.16.0.0/12 RFC1918
    if ((ip >> 16) == (192 << 8 | 168)) return true;    // 192.168.0.0/16 RFC1918
    if ((ip >> 16) == (169 << 8 | 254)) return true;    // 169.254.0.0/16 link-local
    if ((ip >> 24) == 0) return true;                   // 0.0.0.0/8 "this" network
    return false;
}

bool isPrivateIPv6(const unsigned char a[16]) {
    static const unsigned char loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (std::memcmp(a, loopback, 16) == 0) return true;
    static const unsigned char unspec[16] = {0};
    if (std::memcmp(a, unspec, 16) == 0) return true;
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return true;    // fe80::/10
    if ((a[0] & 0xfe) == 0xfc) return true;                    // fc00::/7
    static const unsigned char v4mapped_prefix[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    if (std::memcmp(a, v4mapped_prefix, 12) == 0) {
        const uint32_t ip = (static_cast<uint32_t>(a[12]) << 24) | (static_cast<uint32_t>(a[13]) << 16) |
                            (static_cast<uint32_t>(a[14]) << 8) | static_cast<uint32_t>(a[15]);
        return isPrivateIPv4(ip);
    }
    return false;
}

curl_socket_t refusePrivateOpenSocket(void* /*clientp*/, curlsocktype purpose,
                                      struct curl_sockaddr* address) {
    if (!address) return CURL_SOCKET_BAD;
    if (purpose == CURLSOCKTYPE_IPCXN) {
        if (address->family == AF_INET) {
            const auto* sin = reinterpret_cast<const struct sockaddr_in*>(&address->addr);
            if (isPrivateIPv4(ntohl(sin->sin_addr.s_addr))) return CURL_SOCKET_BAD;
        } else if (address->family == AF_INET6) {
            const auto* sin6 = reinterpret_cast<const struct sockaddr_in6*>(&address->addr);
            if (isPrivateIPv6(reinterpret_cast<const unsigned char*>(&sin6->sin6_addr))) return CURL_SOCKET_BAD;
        }
    }
    curl_socket_t sock = ::socket(address->family, address->socktype, address->protocol);
    return sock == CURL_SOCKET_BAD ? CURL_SOCKET_BAD : sock;
}

} // namespace net
} // namespace naab
