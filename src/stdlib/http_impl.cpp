// NAAb HTTP Module - Real Implementation using libcurl
// Provides HTTP GET, POST, PUT, DELETE with headers and timeout support

#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include "naab/sandbox.h"
#include "naab/utils/string_utils.h"
#include <curl/curl.h>
#include <fmt/core.h>
#include <stdexcept>
#include <sstream>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <cstring>

namespace naab {
namespace stdlib {

// ============================================================================
// HTTP Module Implementation using libcurl
// ============================================================================

// M-13: SSRF protection — block requests to private/reserved IP ranges
static bool isPrivateHost(const std::string& host) {
    // Check common private hostnames
    if (host == "localhost" || host == "localhost.localdomain") return true;

    // Try parsing as IPv4
    struct in_addr addr4;
    if (inet_pton(AF_INET, host.c_str(), &addr4) == 1) {
        uint32_t ip = ntohl(addr4.s_addr);
        // 127.0.0.0/8 — loopback
        if ((ip >> 24) == 127) return true;
        // 10.0.0.0/8 — RFC1918
        if ((ip >> 24) == 10) return true;
        // 172.16.0.0/12 — RFC1918
        if ((ip >> 20) == (172 << 4 | 1)) return true;
        // 192.168.0.0/16 — RFC1918
        if ((ip >> 16) == (192 << 8 | 168)) return true;
        // 169.254.0.0/16 — link-local (cloud metadata)
        if ((ip >> 16) == (169 << 8 | 254)) return true;
        // 0.0.0.0/8 — "this" network
        if ((ip >> 24) == 0) return true;
        return false;
    }

    // Try parsing as IPv6
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, host.c_str(), &addr6) == 1) {
        // ::1 — loopback
        static const uint8_t loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (std::memcmp(&addr6, loopback, 16) == 0) return true;
        // :: — unspecified
        static const uint8_t unspec[16] = {0};
        if (std::memcmp(&addr6, unspec, 16) == 0) return true;
        // fe80::/10 — link-local
        if (addr6.s6_addr[0] == 0xfe && (addr6.s6_addr[1] & 0xc0) == 0x80) return true;
        // fc00::/7 — unique local
        if ((addr6.s6_addr[0] & 0xfe) == 0xfc) return true;
        // ::ffff:0:0/96 — IPv4-mapped (check the embedded IPv4)
        static const uint8_t v4mapped_prefix[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        if (std::memcmp(&addr6, v4mapped_prefix, 12) == 0) {
            uint32_t ip = (static_cast<uint32_t>(addr6.s6_addr[12]) << 24) |
                          (static_cast<uint32_t>(addr6.s6_addr[13]) << 16) |
                          (static_cast<uint32_t>(addr6.s6_addr[14]) << 8) |
                           static_cast<uint32_t>(addr6.s6_addr[15]);
            if ((ip >> 24) == 127 || (ip >> 24) == 10 ||
                (ip >> 20) == (172 << 4 | 1) ||
                (ip >> 16) == (192 << 8 | 168) ||
                (ip >> 16) == (169 << 8 | 254) ||
                (ip >> 24) == 0) return true;
        }
        return false;
    }

    return false;
}

// V-DOS-010: Maximum HTTP response size (25 MB)
static constexpr size_t MAX_HTTP_RESPONSE_BYTES = 25 * 1024 * 1024;

// Bounded write sink — aborts transfer if response exceeds MAX_HTTP_RESPONSE_BYTES
struct BoundedResponseSink {
    std::string* buffer;
    size_t max_size;
};

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* sink = static_cast<BoundedResponseSink*>(userp);
    if (sink->buffer->size() + total_size > sink->max_size) {
        return 0;  // Signal curl to abort (CURLE_WRITE_ERROR)
    }
    sink->buffer->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// Callback for capturing response headers
static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    auto* headers = static_cast<std::unordered_map<std::string, std::string>*>(userdata);

    std::string header_line(buffer, total_size);

    // Parse "Key: Value" format
    size_t colon_pos = header_line.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header_line.substr(0, colon_pos);
        std::string value = header_line.substr(colon_pos + 1);

        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        if (!key.empty()) {
            (*headers)[key] = value;
        }
    }

    return total_size;
}

// Helper: Perform HTTP request with libcurl
interpreter::NaabVal performRequest(
    const std::string& method,
    const std::string& url,
    const std::string& body = "",
    const std::unordered_map<std::string, std::string>& headers = {},
    int timeout_ms = 30000) {

    // Security: Check sandbox permissions for network access
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox) {
        // Block dangerous URL schemes
        if (url.size() >= 7 && (url.substr(0, 7) == "file://" ||
            url.substr(0, 9) == "gopher://" || url.substr(0, 7) == "dict://")) {
            throw std::runtime_error(
                "Security: URL scheme not allowed: " + url + "\n\n"
                "  Only http:// and https:// URLs are permitted.\n"
            );
        }

        // Extract host and port from URL for sandbox check
        std::string host;
        int port = 0;
        size_t scheme_end = url.find("://");
        if (scheme_end != std::string::npos) {
            size_t host_start = scheme_end + 3;
            size_t at_pos = url.find('@', host_start);
            size_t slash_pos = url.find('/', host_start);
            if (at_pos != std::string::npos &&
                (slash_pos == std::string::npos || at_pos < slash_pos)) {
                host_start = at_pos + 1;
            }
            size_t host_end = url.find_first_of(":/", host_start);
            if (host_end == std::string::npos) host_end = url.size();
            host = url.substr(host_start, host_end - host_start);

            if (host_end < url.size() && url[host_end] == ':') {
                size_t port_end = url.find('/', host_end);
                if (port_end == std::string::npos) port_end = url.size();
                try {
                    port = std::stoi(url.substr(host_end + 1, port_end - host_end - 1));
                    if (port < 1 || port > 65535) {
                        throw std::runtime_error(
                            "HTTP error: port number out of range\n\n"
                            "  Got: " + std::to_string(port) + "\n"
                            "  Help:\n  - Port must be between 1 and 65535\n");
                    }
                } catch (const std::runtime_error&) {
                    throw;
                } catch (...) {
                    throw std::runtime_error(
                        "HTTP error: invalid port in URL\n\n"
                        "  Help:\n  - Port must be a number between 1 and 65535\n");
                }
            } else {
                port = (url.substr(0, 5) == "https") ? 443 : 80;
            }
        }

        // M-13: SSRF defense-in-depth — block private/reserved IPs
        if (!host.empty() && isPrivateHost(host)) {
            sandbox->logViolation("http." + method, url, "SSRF: private/reserved IP blocked");
            throw std::runtime_error(
                "Security: HTTP request to private network denied\n\n"
                "  URL: " + url + "\n"
                "  Host: " + host + "\n\n"
                "  Requests to private, loopback, and link-local addresses are blocked\n"
                "  to prevent server-side request forgery (SSRF).\n"
            );
        }

        if (!host.empty() && !sandbox->canConnect(host, port)) {
            sandbox->logViolation("http." + method, url, "NET_CONNECT capability required");
            throw std::runtime_error(
                "Security: HTTP request denied by sandbox\n\n"
                "  URL: " + url + "\n"
                "  Host: " + host + "\n"
                "  Method: " + method + "\n\n"
                "  The current sandbox level does not permit network connections.\n"
                "  The project owner can adjust the sandbox level in the project configuration.\n"
            );
        }
    }

    // Initialize curl with RAII cleanup
    struct CurlCleanup {
        CURL* curl;
        ~CurlCleanup() { if (curl) curl_easy_cleanup(curl); }
    };
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }
    CurlCleanup curl_guard{curl};

    // Response buffers
    std::string response_body;
    std::unordered_map<std::string, std::string> response_headers;
    long response_code = 0;

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Set method
    if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "DELETE") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (method == "HEAD") {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (method == "PATCH") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }

    // Set write callback with bounded sink (V-DOS-010)
    BoundedResponseSink sink{&response_body, MAX_HTTP_RESPONSE_BYTES};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

    // Set header callback
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

    // Set timeout (in milliseconds)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

    // V-SSRF-001: Restrict protocols to http/https only — prevents redirect-based
    // SSRF to file://, gopher://, dict://, etc.
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

    // SSL/TLS settings
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // Set User-Agent (many APIs require this)
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NAAb/1.0 (https://github.com/naab-lang)");

    // Set custom headers
    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    // Get response code
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    // Cleanup
    if (header_list) {
        curl_slist_free_all(header_list);
    }

    // Check for errors (curl_guard handles cleanup on any exit path)
    if (res != CURLE_OK) {
        throw std::runtime_error(fmt::format(
            "HTTP request failed: {} ({})",
            curl_easy_strerror(res),
            static_cast<int>(res)
        ));
    }

    // Build response object
    std::unordered_map<std::string, interpreter::NaabVal> response;
    response["status"] = interpreter::NaabVal::makeInt(static_cast<int>(response_code));
    response["body"] = interpreter::NaabVal::makeString(response_body);
    response["ok"] = interpreter::NaabVal::makeBool(response_code >= 200 && response_code < 300);

    // Convert headers map to Value map
    std::unordered_map<std::string, interpreter::NaabVal> headers_value_map;
    for (const auto& [key, value] : response_headers) {
        headers_value_map[key] = interpreter::NaabVal::makeString(value);
    }
    response["headers"] = interpreter::NaabVal::makeDict(std::move(headers_value_map));

    return interpreter::NaabVal::makeDict(std::move(response));
}

bool HTTPModule::hasFunction(const std::string& name) const {
    return name == "get" || name == "post" || name == "put" || name == "delete"
        || name == "head" || name == "patch";
}

interpreter::NaabVal HTTPModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "get") {
        return get(args);
    } else if (function_name == "post") {
        return post(args);
    } else if (function_name == "put") {
        return put(args);
    } else if (function_name == "delete") {
        return del(args);
    } else if (function_name == "head") {
        return head(args);
    } else if (function_name == "patch") {
        return patch(args);
    }

    // Common LLM mistakes
    if (function_name == "fetch" || function_name == "request" || function_name == "call") {
        throw std::runtime_error(
            "Unknown http function: " + function_name + "\n\n"
            "  Use the specific HTTP method:\n"
            "    http.get(url)             // GET request\n"
            "    http.post(url, body)      // POST request\n"
            "    http.put(url, body)       // PUT request\n"
            "    http.delete(url)          // DELETE request\n"
        );
    }

    // Fuzzy matching for typos
    static const std::vector<std::string> FUNCTIONS = {
        "get", "post", "put", "delete", "head", "patch"
    };
    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown http function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }
    throw std::runtime_error(oss.str());
}

interpreter::NaabVal HTTPModule::get(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("http.get() requires URL argument");
    }

    std::string url = args[0].toString();

    // Optional: headers dict
    std::unordered_map<std::string, std::string> headers;
    if (args.size() >= 2) {
        if (args[1].isDict()) {
            for (const auto& [k, v] : args[1].asDictConst()) {
                headers[k] = v.toString();
            }
        }
    }

    // Optional: timeout
    int timeout_ms = 30000;  // 30 seconds default
    if (args.size() >= 3) {
        timeout_ms = args[2].toInt();
    }

    return performRequest("GET", url, "", headers, timeout_ms);
}

interpreter::NaabVal HTTPModule::post(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("http.post() requires URL and data arguments");
    }

    std::string url = args[0].toString();
    std::string data = args[1].toString();

    // Optional: headers
    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";  // Default to JSON
    if (args.size() >= 3) {
        if (args[2].isDict()) {
            for (const auto& [k, v] : args[2].asDictConst()) {
                headers[k] = v.toString();
            }
        }
    }

    // Optional: timeout
    int timeout_ms = 30000;
    if (args.size() >= 4) {
        timeout_ms = args[3].toInt();
    }

    return performRequest("POST", url, data, headers, timeout_ms);
}

interpreter::NaabVal HTTPModule::put(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("http.put() requires URL and data arguments");
    }

    std::string url = args[0].toString();
    std::string data = args[1].toString();

    // Optional: headers
    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    if (args.size() >= 3) {
        if (args[2].isDict()) {
            for (const auto& [k, v] : args[2].asDictConst()) {
                headers[k] = v.toString();
            }
        }
    }

    // Optional: timeout
    int timeout_ms = 30000;
    if (args.size() >= 4) {
        timeout_ms = args[3].toInt();
    }

    return performRequest("PUT", url, data, headers, timeout_ms);
}

interpreter::NaabVal HTTPModule::del(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("http.delete() requires URL argument");
    }

    std::string url = args[0].toString();

    // Optional: headers
    std::unordered_map<std::string, std::string> headers;
    if (args.size() >= 2) {
        if (args[1].isDict()) {
            for (const auto& [k, v] : args[1].asDictConst()) {
                headers[k] = v.toString();
            }
        }
    }

    // Optional: timeout
    int timeout_ms = 30000;
    if (args.size() >= 3) {
        timeout_ms = args[2].toInt();
    }

    return performRequest("DELETE", url, "", headers, timeout_ms);
}

interpreter::NaabVal HTTPModule::head(
    std::vector<interpreter::NaabVal>& args) {

    if (args.empty()) {
        throw std::runtime_error("http.head() requires URL argument");
    }

    std::string url = args[0].toString();

    std::unordered_map<std::string, std::string> headers;
    if (args.size() >= 2) {
        if (args[1].isDict()) {
            for (const auto& [k, v] : args[1].asDictConst()) {
                headers[k] = v.toString();
            }
        }
    }

    int timeout_ms = 30000;
    if (args.size() >= 3) {
        timeout_ms = args[2].toInt();
    }

    return performRequest("HEAD", url, "", headers, timeout_ms);
}

interpreter::NaabVal HTTPModule::patch(
    std::vector<interpreter::NaabVal>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("http.patch() requires URL and data arguments");
    }

    std::string url = args[0].toString();
    std::string data = args[1].toString();

    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    if (args.size() >= 3) {
        if (args[2].isDict()) {
            for (const auto& [k, v] : args[2].asDictConst()) {
                headers[k] = v.toString();
            }
        }
    }

    int timeout_ms = 30000;
    if (args.size() >= 4) {
        timeout_ms = args[3].toInt();
    }

    return performRequest("PATCH", url, data, headers, timeout_ms);
}

} // namespace stdlib
} // namespace naab
