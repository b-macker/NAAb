// NAAb HTTP Module - Real Implementation using libcurl
// Provides HTTP GET, POST, PUT, DELETE with headers and timeout support

#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <curl/curl.h>
#include <fmt/core.h>
#include <stdexcept>
#include <sstream>

namespace naab {
namespace stdlib {

// ============================================================================
// HTTP Module Implementation using libcurl
// ============================================================================

// Callback for writing response data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), total_size);
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
std::shared_ptr<interpreter::Value> performRequest(
    const std::string& method,
    const std::string& url,
    const std::string& body = "",
    const std::unordered_map<std::string, std::string>& headers = {},
    int timeout_ms = 30000) {

    // Initialize curl
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

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
    }

    // Set write callback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    // Set header callback
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

    // Set timeout (in milliseconds)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

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

    // Check for errors
    if (res != CURLE_OK) {
        std::string error_msg = fmt::format(
            "HTTP request failed: {} ({})",
            curl_easy_strerror(res),
            static_cast<int>(res)
        );
        curl_easy_cleanup(curl);
        throw std::runtime_error(error_msg);
    }

    curl_easy_cleanup(curl);

    // Build response object
    std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> response;
    response["status"] = std::make_shared<interpreter::Value>(static_cast<int>(response_code));
    response["body"] = std::make_shared<interpreter::Value>(response_body);
    response["ok"] = std::make_shared<interpreter::Value>(response_code >= 200 && response_code < 300);

    // Convert headers map to Value map
    std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> headers_value_map;
    for (const auto& [key, value] : response_headers) {
        headers_value_map[key] = std::make_shared<interpreter::Value>(value);
    }
    response["headers"] = std::make_shared<interpreter::Value>(headers_value_map);

    return std::make_shared<interpreter::Value>(response);
}

bool HTTPModule::hasFunction(const std::string& name) const {
    return name == "get" || name == "post" || name == "put" || name == "delete";
}

std::shared_ptr<interpreter::Value> HTTPModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "get") {
        return get(args);
    } else if (function_name == "post") {
        return post(args);
    } else if (function_name == "put") {
        return put(args);
    } else if (function_name == "delete") {
        return del(args);
    }

    // Common LLM mistakes
    if (function_name == "fetch" || function_name == "request") {
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
        "get", "post", "put", "delete"
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

std::shared_ptr<interpreter::Value> HTTPModule::get(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        throw std::runtime_error("http.get() requires URL argument");
    }

    std::string url = args[0]->toString();

    // Optional: headers dict
    std::unordered_map<std::string, std::string> headers;
    if (args.size() >= 2) {
        // Parse headers from dict
        // TODO: Implement when dict access is available
    }

    // Optional: timeout
    int timeout_ms = 30000;  // 30 seconds default
    if (args.size() >= 3) {
        timeout_ms = args[2]->toInt();
    }

    return performRequest("GET", url, "", headers, timeout_ms);
}

std::shared_ptr<interpreter::Value> HTTPModule::post(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("http.post() requires URL and data arguments");
    }

    std::string url = args[0]->toString();
    std::string data = args[1]->toString();

    // Optional: headers
    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";  // Default to JSON
    if (args.size() >= 3) {
        // Parse headers from dict
        // TODO: Implement when dict access is available
    }

    // Optional: timeout
    int timeout_ms = 30000;
    if (args.size() >= 4) {
        timeout_ms = args[3]->toInt();
    }

    return performRequest("POST", url, data, headers, timeout_ms);
}

std::shared_ptr<interpreter::Value> HTTPModule::put(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.size() < 2) {
        throw std::runtime_error("http.put() requires URL and data arguments");
    }

    std::string url = args[0]->toString();
    std::string data = args[1]->toString();

    // Optional: headers
    std::unordered_map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    if (args.size() >= 3) {
        // TODO: Parse headers
    }

    // Optional: timeout
    int timeout_ms = 30000;
    if (args.size() >= 4) {
        timeout_ms = args[3]->toInt();
    }

    return performRequest("PUT", url, data, headers, timeout_ms);
}

std::shared_ptr<interpreter::Value> HTTPModule::del(
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (args.empty()) {
        throw std::runtime_error("http.delete() requires URL argument");
    }

    std::string url = args[0]->toString();

    // Optional: headers
    std::unordered_map<std::string, std::string> headers;
    if (args.size() >= 2) {
        // TODO: Parse headers
    }

    // Optional: timeout
    int timeout_ms = 30000;
    if (args.size() >= 3) {
        timeout_ms = args[2]->toInt();
    }

    return performRequest("DELETE", url, "", headers, timeout_ms);
}

} // namespace stdlib
} // namespace naab
