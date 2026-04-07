#pragma once

#include <string>
#include <memory>
#include "naab/interpreter.h"
#include "naab/block_loader.h"

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
     * Set interpreter for code execution
     */
    void setInterpreter(std::shared_ptr<interpreter::Interpreter> interpreter);

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
     * Set maximum accepted request body size in bytes (V-API-001).
     * Requests exceeding this limit are rejected with HTTP 413 before
     * reaching any handler.  Default: 1 MiB (1048576 bytes).
     */
    void setMaxBodySize(size_t bytes);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    int port_;
    std::string host_;
    bool running_;
    std::string api_key_;
    size_t max_body_bytes_ = 1048576; // 1 MiB default
};

} // namespace api
} // namespace naab
