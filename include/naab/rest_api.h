#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include "naab/block_loader.h"
#include "naab/governance.h"

namespace naab {
namespace api {

/**
 * REST API Server for NAAb
 * Provides HTTP endpoints for:
 * - Executing NAAb code
 * - Querying block registry
 * - Usage analytics
 * - Health checks
 */
class RestApiServer {
public:
    /**
     * Constructor
     * @param port Server port (default: 8080)
     * @param host Server host (default: "0.0.0.0")
     */
    RestApiServer(int port = 8080, const std::string& host = "0.0.0.0");

    /**
     * Destructor
     */
    ~RestApiServer();

    /**
     * Start the server (blocking)
     * @return true if server started successfully
     */
    bool start();

    /**
     * Stop the server
     */
    void stop();

    /**
     * Check if server is running
     */
    bool isRunning() const;

    /**
     * Set the naab-lang executable that runs each /api/v1/execute request.
     * A request runs in its own `naab-lang run` worker process on the default
     * engine, under the govern.json the server's directory resolves to, so no
     * user code runs in the server process (POSIX; on Windows /execute is not
     * available yet). Without one, /execute answers 503.
     */
    void setWorkerExecutable(const std::string& path);

    /**
     * The server was started with --no-governance: workers get it too, so a
     * request may run where no govern.json is found (the CLI's waiver; a
     * discovered govern.json still applies in full). Off by default: like a
     * file run, a request with no config is refused (exit 4).
     */
    void setWaiveMissingConfig(bool waive);

    /**
     * Set block loader for registry queries
     */
    void setBlockLoader(std::shared_ptr<runtime::BlockLoader> loader);

    /**
     * Set API key for authentication (V-API-001).
     * When set, all endpoints except /health require the caller to present
     * the key via  Authorization: Bearer <key>  OR  X-API-Key: <key>.
     * Passing an empty string disables authentication (default).
     */
    void setApiKey(const std::string& key);

    /**
     * Set multi-key authentication with scoped permissions.
     * Each key has a name and a list of allowed scopes.
     * Valid scopes: "execute", "check", "blocks", "stats".
     * Keys with empty scopes have full access (backward compat).
     */
    void setApiKeys(const std::vector<governance::GovernanceRules::ApiKeyEntry>& keys);

    /**
     * Set maximum accepted request body size in bytes (V-API-001).
     * Requests exceeding this limit are rejected with HTTP 413 before
     * reaching any handler.  Default: 1 MiB (1048576 bytes).
     */
    void setMaxBodySize(size_t bytes);

    /**
     * Set per-request execution timeout in seconds (V-API-004).
     * /api/v1/execute passes it to the worker as --timeout (a govern.json
     * limit still caps it) and kills the worker 5 s after it, so an infinite
     * loop cannot pin a cpp-httplib thread; /api/v1/check wraps its checks in
     * a ScopedTimeout. 0 disables the timeout (tests/debug only). Default: 10.
     */
    void setApiTimeout(unsigned int seconds);

    /**
     * Set per-client rate limit in requests per minute (V-DOS-005).
     * Token-bucket algorithm keyed by API key (or remote IP if no key).
     * 0 disables rate limiting (default).
     */
    void setApiRateLimit(unsigned int requests_per_minute);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    int port_;
    std::string host_;
    std::atomic<bool> running_;  // H4 fix: atomic for thread safety
    std::string api_key_;
    size_t max_body_bytes_ = 1048576; // 1 MiB default
};

} // namespace api
} // namespace naab
