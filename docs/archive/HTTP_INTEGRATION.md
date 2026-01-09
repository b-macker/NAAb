# HTTP Module Integration

## Overview

The NAAb language now includes a fully functional HTTP client module built using **libcurl** (v8.17.0). This module provides HTTP request capabilities including GET, POST, PUT, and DELETE methods with SSL/TLS support, timeout handling, and automatic redirects.

## Implementation Details

### Library
- **libcurl 8.17.0** - Industry-standard HTTP client library
- Package: `libcurl` (already installed in Termux)
- SSL/TLS: Built-in support for HTTPS

### Source Files
- `src/stdlib/http_impl.cpp` - Real implementation using libcurl (~245 lines)
- `include/naab/stdlib.h` - HTTPModule class declaration (updated with PUT/DELETE)
- Integrated into `naab_stdlib` library via CMakeLists.txt
- Linked with `-lcurl` flag

### Architecture
The HTTP module implements the standard `Module` interface:
```cpp
class HTTPModule : public Module {
public:
    std::string getName() const override { return "http"; }
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(...) override;

private:
    std::shared_ptr<interpreter::Value> get(...);
    std::shared_ptr<interpreter::Value> post(...);
    std::shared_ptr<interpreter::Value> put(...);
    std::shared_ptr<interpreter::Value> del(...);
};
```

## Available Functions

### `get(url, [headers], [timeout])`
Performs an HTTP GET request.

**Parameters:**
- `url` (string) - Target URL (supports http:// and https://)
- `headers` (dict, optional) - Custom HTTP headers (TODO: dict parsing)
- `timeout` (int, optional) - Timeout in milliseconds (default: 30000)

**Returns:**
- Dictionary with keys:
  - `status` (int) - HTTP status code (200, 404, etc.)
  - `body` (string) - Response body
  - `ok` (bool) - true if status is 2xx, false otherwise

**Example:**
```cpp
auto url = std::make_shared<interpreter::Value>(std::string("https://api.github.com/users/octocat"));
auto response = http_module.call("get", {url});

// Response:
// {
//   "status": 200,
//   "body": "{\"login\": \"octocat\", ...}",
//   "ok": true
// }
```

**Error Handling:**
- Throws `std::runtime_error` on connection failures
- Throws on DNS resolution failures
- Throws on timeout
- Includes curl error code and message

### `post(url, data, [headers], [timeout])`
Performs an HTTP POST request with data.

**Parameters:**
- `url` (string) - Target URL
- `data` (string) - Request body (typically JSON)
- `headers` (dict, optional) - Custom headers (defaults to `Content-Type: application/json`)
- `timeout` (int, optional) - Timeout in milliseconds (default: 30000)

**Returns:**
- Same structure as GET (status, body, ok)

**Example:**
```cpp
auto url = std::make_shared<interpreter::Value>(std::string("https://httpbin.org/post"));
auto data = std::make_shared<interpreter::Value>(std::string(R"({"key": "value"})"));
auto response = http_module.call("post", {url, data});

// Automatically sets: Content-Type: application/json
```

### `put(url, data, [headers], [timeout])`
Performs an HTTP PUT request.

**Parameters:**
- Same as POST

**Returns:**
- Same structure as GET

**Example:**
```cpp
auto url = std::make_shared<interpreter::Value>(std::string("https://api.example.com/resource/123"));
auto data = std::make_shared<interpreter::Value>(std::string(R"({"updated": true})"));
auto response = http_module.call("put", {url, data});
```

### `delete(url, [headers], [timeout])`
Performs an HTTP DELETE request.

**Parameters:**
- `url` (string) - Target URL
- `headers` (dict, optional) - Custom headers
- `timeout` (int, optional) - Timeout in milliseconds

**Returns:**
- Same structure as GET

**Example:**
```cpp
auto url = std::make_shared<interpreter::Value>(std::string("https://api.example.com/resource/123"));
auto response = http_module.call("delete", {url});
```

## Features

### ✅ SSL/TLS Support
- HTTPS enabled by default
- Peer verification enabled (`CURLOPT_SSL_VERIFYPEER`)
- Host verification enabled (`CURLOPT_SSL_VERIFYHOST`)
- Uses system CA certificates

### ✅ Automatic Redirects
- Follows HTTP redirects automatically
- Maximum 5 redirects (configurable)
- `CURLOPT_FOLLOWLOCATION` enabled

### ✅ Timeout Handling
- Configurable timeout per request
- Default: 30 seconds (30000ms)
- Prevents hanging requests
- Uses `CURLOPT_TIMEOUT_MS`

### ✅ Error Handling
- Connection errors caught and reported
- DNS resolution failures
- Timeout errors with descriptive messages
- Includes curl error code for debugging

### ✅ Response Structure
Every HTTP response includes:
- `status`: HTTP status code (int)
- `body`: Response body (string)
- `ok`: Convenience boolean (true for 2xx status codes)

## Test Results

All HTTP tests passing against httpbin.org:

```
=== HTTP Module Test ===

Test 1: HTTP GET request
  Status: 200
  OK: true
  Body length: 220 bytes
  ✓ GET request successful

Test 2: HTTP POST request
  Status: 200
  Body length: 429 bytes
  ✓ POST request successful

Test 3: HTTP PUT request
  Status: 200
  ✓ PUT request successful

Test 4: HTTP DELETE request
  Status: 200
  ✓ DELETE request successful

Test 5: Error handling (invalid URL)
  ✓ Correctly caught error: HTTP request failed: Could not resolve hostname (6)

=== HTTP tests complete! ===
```

### Test Coverage
- ✅ HTTP GET requests
- ✅ HTTP POST with body
- ✅ HTTP PUT with body
- ✅ HTTP DELETE
- ✅ Error handling (invalid URLs, connection failures)
- ✅ SSL/TLS (all tests use HTTPS)
- ✅ Response parsing (status, body, ok)

## Build Integration

### CMakeLists.txt Changes
```cmake
# Standard library
add_library(naab_stdlib
    src/stdlib/core.cpp
    src/stdlib/io.cpp
    src/stdlib/collections.cpp
    src/stdlib/json_impl.cpp
    src/stdlib/http_impl.cpp      # ← Added
    src/stdlib/stdlib.cpp
)
target_link_libraries(naab_stdlib
    fmt::fmt
    spdlog::spdlog
    absl::strings
    curl                          # ← Added
)
```

### Module Registration
The HTTP module is automatically registered in `StdLib::registerModules()`:
```cpp
void StdLib::registerModules() {
    modules_["io"] = std::make_shared<IOModule>();
    modules_["json"] = std::make_shared<JSONModule>();
    modules_["http"] = std::make_shared<HTTPModule>();  // ← Auto-registered
    modules_["collections"] = std::make_shared<CollectionsModule>();
}
```

## Implementation Details

### libcurl Integration
The implementation uses libcurl's easy interface:

```cpp
// Initialize curl
CURL* curl = curl_easy_init();

// Set options
curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000);
curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

// Perform request
CURLcode res = curl_easy_perform(curl);

// Cleanup
curl_easy_cleanup(curl);
```

### Write Callback
Response data is collected using a callback function:
```cpp
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), total_size);
    return total_size;
}
```

## Known Limitations

1. **NAAb Language Access**: Currently, the HTTP module is available in C++ but not yet accessible from NAAb programs. The interpreter needs to be extended to expose stdlib modules.

2. **Header Parsing**: Custom headers dictionary parsing is not yet implemented (marked as TODO). Currently defaults to `Content-Type: application/json` for POST/PUT.

3. **Response Headers**: Currently only body is captured, not response headers. Future enhancement could add `headers` field to response dict.

4. **Streaming**: No support for streaming large responses. Entire response is loaded into memory.

5. **Authentication**: No built-in support for auth schemes. Must be provided via custom headers when implemented.

## Future Enhancements

1. **Expose to NAAb Language**
   ```naab
   use http
   let resp = http.get("https://api.github.com/users/octocat")
   print("Status:", resp["status"])
   ```

2. **Response Headers**
   ```cpp
   response["headers"] = std::make_shared<interpreter::Value>(headers_dict);
   ```

3. **Authentication Support**
   ```cpp
   // Basic auth
   curl_easy_setopt(curl, CURLOPT_USERPWD, "user:pass");

   // Bearer token
   headers["Authorization"] = "Bearer " + token;
   ```

4. **Request Progress**
   ```cpp
   curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
   ```

5. **File Uploads**
   ```cpp
   curl_mime* mime = curl_mime_init(curl);
   // Add file parts
   ```

6. **Cookie Support**
   ```cpp
   curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "cookies.txt");
   ```

## Dependencies

- **libcurl**: v8.17.0+ (system library)
- **fmt**: For error message formatting
- **C++17**: Required for implementation

## Performance

- **Connection pooling**: Not implemented (each request creates new connection)
- **Keep-alive**: libcurl default behavior
- **DNS caching**: libcurl default behavior
- **Timeout**: 30 seconds default (configurable)

## Security Considerations

1. **SSL/TLS Verification**: Enabled by default ✅
2. **Certificate Validation**: Uses system CA certificates ✅
3. **Follow Redirects**: Limited to 5 redirects ✅
4. **Timeout Protection**: Prevents indefinite hangs ✅
5. **Input Validation**: URL validated by libcurl ✅

## Error Codes

Common curl error codes:
- `CURLE_COULDNT_RESOLVE_HOST (6)` - DNS resolution failed
- `CURLE_OPERATION_TIMEDOUT (28)` - Request timeout
- `CURLE_SSL_CONNECT_ERROR (35)` - SSL/TLS handshake failed
- `CURLE_GOT_NOTHING (52)` - Server sent no data

## Files

- Implementation: `src/stdlib/http_impl.cpp` (245 lines)
- Header: `include/naab/stdlib.h` (HTTPModule class)
- Tests: `test_http_module.cpp` (132 lines)
- Example: `examples/test_http.naab` (TODO)

## Phase 5 Status

✅ **JSON Library Integration** - COMPLETE (Phase 5a)
✅ **HTTP Library Integration** - COMPLETE (Phase 5b)

**Next**: REPL Performance Optimization (Phase 5c)
