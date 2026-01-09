#include "naab/rest_api.h"
#include "../../external/cpp-httplib/httplib.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

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
        server.Post("/api/v1/execute", [](const httplib::Request& req, httplib::Response& res) {
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

                // TODO: Execute code using interpreter
                // For now, return success
                json response = {
                    {"status", "success"},
                    {"message", "Code execution not yet implemented"},
                    {"code", code}
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
                    {"message", e.what()},
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
};

// RestApiServer implementation
RestApiServer::RestApiServer(int port, const std::string& host)
    : impl_(std::make_unique<Impl>()),
      port_(port),
      host_(host),
      running_(false) {
    spdlog::info("REST API server created on {}:{}", host_, port_);
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
