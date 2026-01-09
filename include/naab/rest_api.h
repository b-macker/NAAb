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

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    int port_;
    std::string host_;
    bool running_;
};

} // namespace api
} // namespace naab
