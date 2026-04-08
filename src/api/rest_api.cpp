#include "naab/rest_api.h"
#include "../../external/cpp-httplib/httplib.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/error_sanitizer.h"
#include "naab/crypto_utils.h"
#include "naab/stdlib.h"
#include <sstream>
#include <filesystem>

using json = nlohmann::json;

namespace naab {
namespace api {

// Implementation class (PIMPL pattern)
class RestApiServer::Impl {
public:
    httplib::Server server;
    std::shared_ptr<interpreter::Interpreter> interpreter;
    std::shared_ptr<runtime::BlockLoader> block_loader;

    Impl() {
        setupRoutes();
    }

    void setupRoutes() {
        // Health check endpoint
        server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            json response = {
                {"status", "healthy"},
                {"version", "1.0.0"},
                {"service", "naab-api"}
            };
            res.set_content(response.dump(2), "application/json");
        });

        // Execute NAAb code endpoint
        server.Post("/api/v1/execute", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                std::string code = body.value("code", "");

                if (code.empty()) {
                    res.status = 400;
                    json error_response = {
                        {"error", "Missing 'code' field"},
                        {"status", "error"}
                    };
                    res.set_content(error_response.dump(2), "application/json");
                    return;
                }

                if (!interpreter) {
                    res.status = 503;
                    res.set_content(json{{"error","Interpreter not available"},{"status","error"}}.dump(2),
                                    "application/json");
                    return;
                }

                // V-DOS-002: use thread-local capture stream for per-request output
                // isolation. Each httplib worker thread has its own ostringstream;
                // no global mutex or rdbuf() redirect needed, so requests run in
                // parallel without serialization.
                std::ostringstream captured;
                std::string error_msg;
                int exit_code = 0;
                naab::stdlib::setIoCaptureStream(&captured);
                try {
                    naab::lexer::Lexer lexer(code);
                    auto tokens = lexer.tokenize();
                    naab::parser::Parser parser(tokens);
                    auto program = parser.parseProgram();
                    // Finding B fix: create a fresh interpreter per request to prevent state
                    // leakage, governance override persistence, and race conditions
                    auto req_interpreter = std::make_shared<interpreter::Interpreter>();
                    auto cwd = std::filesystem::current_path().string();
                    req_interpreter->setSourceCode(code, cwd + "/api-request.naab");
                    req_interpreter->execute(*program);
                } catch (const std::exception& e) {
                    // V-ERR-002: sanitize error messages before returning to caller
                    error_msg = naab::error::ErrorSanitizer::sanitize(e.what());
                    exit_code = 1;
                } catch (...) {
                    error_msg = "Unknown error during execution";
                    exit_code = 1;
                }
                naab::stdlib::setIoCaptureStream(nullptr);  // Always restore

                json response = {
                    {"status",    error_msg.empty() ? "success" : "error"},
                    {"output",    captured.str()},
                    {"error",     error_msg},
                    {"exit_code", exit_code}
                };
                res.set_content(response.dump(2), "application/json");

            } catch (const json::exception& e) {
                res.status = 400;
                json error_response = {
                    {"error", "Invalid JSON"},
                    {"message", e.what()},
                    {"status", "error"}
                };
                res.set_content(error_response.dump(2), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                json error_response = {
                    {"error", "Internal server error"},
                    // V-ERR-002: sanitize before returning to caller
                    {"message", naab::error::ErrorSanitizer::sanitize(e.what())},
                    {"status", "error"}
                };
                res.set_content(error_response.dump(2), "application/json");
            }
        });

        // List blocks endpoint
        server.Get("/api/v1/blocks", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                if (!block_loader) {
                    res.status = 503;
                    json error_response = {
                        {"error", "Block loader not available"},
                        {"status", "error"}
                    };
                    res.set_content(error_response.dump(2), "application/json");
                    return;
                }

                // Get query parameters (currently searchBlocks only takes query param)
                std::string query_str = req.has_param("q") ? req.get_param_value("q") : "";

                // Query blocks
                auto blocks = block_loader->searchBlocks(query_str);

                json blocks_array = json::array();
                for (const auto& block : blocks) {
                    blocks_array.push_back({
                        {"id", block.block_id},
                        {"name", block.name},
                        {"language", block.language},
                        {"description", block.description}
                    });
                }

                json response = {
                    {"status", "success"},
                    {"count", blocks_array.size()},
                    {"blocks", blocks_array}
                };
                res.set_content(response.dump(2), "application/json");

            } catch (const std::exception& e) {
                res.status = 500;
                json error_response = {
                    {"error", "Internal server error"},
                    {"message", e.what()},
                    {"status", "error"}
                };
                res.set_content(error_response.dump(2), "application/json");
            }
        });

        // Search blocks endpoint
        server.Get("/api/v1/blocks/search", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                if (!block_loader) {
                    res.status = 503;
                    json error_response = {
                        {"error", "Block loader not available"},
                        {"status", "error"}
                    };
                    res.set_content(error_response.dump(2), "application/json");
                    return;
                }

                std::string query_str = req.has_param("q") ? req.get_param_value("q") : "";

                if (query_str.empty()) {
                    res.status = 400;
                    json error_response = {
                        {"error", "Missing 'q' parameter"},
                        {"status", "error"}
                    };
                    res.set_content(error_response.dump(2), "application/json");
                    return;
                }

                auto blocks = block_loader->searchBlocks(query_str);

                json blocks_array = json::array();
                for (const auto& block : blocks) {
                    blocks_array.push_back({
                        {"id", block.block_id},
                        {"name", block.name},
                        {"language", block.language},
                        {"description", block.description}
                    });
                }

                json response = {
                    {"status", "success"},
                    {"query", query_str},
                    {"count", blocks_array.size()},
                    {"blocks", blocks_array}
                };
                res.set_content(response.dump(2), "application/json");

            } catch (const std::exception& e) {
                res.status = 500;
                json error_response = {
                    {"error", "Internal server error"},
                    {"message", e.what()},
                    {"status", "error"}
                };
                res.set_content(error_response.dump(2), "application/json");
            }
        });

        // Get usage statistics endpoint
        server.Get("/api/v1/stats", [this](const httplib::Request&, httplib::Response& res) {
            try {
                if (!block_loader) {
                    res.status = 503;
                    json error_response = {
                        {"error", "Block loader not available"},
                        {"status", "error"}
                    };
                    res.set_content(error_response.dump(2), "application/json");
                    return;
                }

                auto top_blocks = block_loader->getTopBlocksByUsage(10);
                auto top_combos = block_loader->getTopCombinations(10);
                auto lang_stats = block_loader->getLanguageStats();
                long long total_tokens = block_loader->getTotalTokensSaved();

                json top_blocks_array = json::array();
                for (const auto& block : top_blocks) {
                    top_blocks_array.push_back({
                        {"name", block.name},
                        {"count", block.times_used},
                        {"language", block.language}
                    });
                }

                json top_combos_array = json::array();
                for (const auto& [block1, block2] : top_combos) {
                    top_combos_array.push_back({
                        {"block1", block1},
                        {"block2", block2}
                    });
                }

                json lang_stats_obj = json::object();
                for (const auto& [lang, count] : lang_stats) {
                    lang_stats_obj[lang] = count;
                }

                json response = {
                    {"status", "success"},
                    {"total_tokens_saved", total_tokens},
                    {"top_blocks", top_blocks_array},
                    {"top_combinations", top_combos_array},
                    {"language_stats", lang_stats_obj}
                };
                res.set_content(response.dump(2), "application/json");

            } catch (const std::exception& e) {
                res.status = 500;
                json error_response = {
                    {"error", "Internal server error"},
                    {"message", e.what()},
                    {"status", "error"}
                };
                res.set_content(error_response.dump(2), "application/json");
            }
        });

        // 404 handler
        server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
            json error_response = {
                {"error", "Endpoint not found"},
                {"status", "error"}
            };
            res.set_content(error_response.dump(2), "application/json");
        });
    }

    // V-API-001: apply body size cap and optional API key auth.
    // Called after construction so the outer RestApiServer members are set.
    void applySecurityConfig(const std::string& api_key, size_t max_body_bytes) {
        // Body size cap — reject oversized requests before handlers run
        server.set_payload_max_length(max_body_bytes);

        // API key guard — skip if no key configured (auth disabled)
        if (api_key.empty()) return;

        server.set_pre_routing_handler(
            [api_key](const httplib::Request& req, httplib::Response& res) {
                // /health is always public — no key required
                if (req.path == "/health") {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
                bool ok = false;
                if (req.has_header("Authorization")) {
                    // V-API-002: constant-time comparison to prevent timing-based key guessing
                    ok = naab::security::CryptoUtils::constantTimeCompare(
                        req.get_header_value("Authorization"), "Bearer " + api_key);
                }
                if (!ok && req.has_header("X-API-Key")) {
                    ok = naab::security::CryptoUtils::constantTimeCompare(
                        req.get_header_value("X-API-Key"), api_key);
                }
                if (!ok) {
                    res.status = 401;
                    res.set_content(
                        nlohmann::json{{"error","Unauthorized"},{"status","error"}}.dump(2),
                        "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            });
    }
};

// RestApiServer implementation
RestApiServer::RestApiServer(int port, const std::string& host)
    : impl_(std::make_unique<Impl>()),
      port_(port),
      host_(host),
      running_(false) {
    spdlog::info("REST API server created on {}:{}", host_, port_);
}

void RestApiServer::setApiKey(const std::string& key) {
    api_key_ = key;
    impl_->applySecurityConfig(api_key_, max_body_bytes_);
    spdlog::info("REST API: API key authentication enabled");
}

void RestApiServer::setMaxBodySize(size_t bytes) {
    max_body_bytes_ = bytes;
    impl_->applySecurityConfig(api_key_, max_body_bytes_);
    spdlog::info("REST API: max body size set to {} bytes", bytes);
}

RestApiServer::~RestApiServer() {
    stop();
}

bool RestApiServer::start() {
    if (running_) {
        spdlog::warn("Server already running");
        return false;
    }

    spdlog::info("Starting REST API server on {}:{}", host_, port_);
    spdlog::info("API endpoints:");
    spdlog::info("  GET  /health                - Health check");
    spdlog::info("  POST /api/v1/execute        - Execute NAAb code");
    spdlog::info("  GET  /api/v1/blocks         - List blocks");
    spdlog::info("  GET  /api/v1/blocks/search  - Search blocks");
    spdlog::info("  GET  /api/v1/stats          - Usage statistics");

    running_ = true;
    bool success = impl_->server.listen(host_.c_str(), port_);
    running_ = false;
    return success;
}

void RestApiServer::stop() {
    if (running_) {
        spdlog::info("Stopping REST API server");
        impl_->server.stop();
        running_ = false;
    }
}

bool RestApiServer::isRunning() const {
    return running_;
}

void RestApiServer::setInterpreter(std::shared_ptr<interpreter::Interpreter> interpreter) {
    impl_->interpreter = interpreter;
    spdlog::info("Interpreter set for REST API");
}

void RestApiServer::setBlockLoader(std::shared_ptr<runtime::BlockLoader> loader) {
    impl_->block_loader = loader;
    spdlog::info("Block loader set for REST API");
}

} // namespace api
} // namespace naab
