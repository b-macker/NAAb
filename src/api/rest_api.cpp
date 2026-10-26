#include "naab/rest_api.h"
#include "../../external/cpp-httplib/httplib.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/error_sanitizer.h"
#include "naab/crypto_utils.h"
#include "naab/stdlib.h"
#include "naab/resource_limits.h"
#include "worker.h"
#include "naab/governance.h"
#include <sstream>
#include <filesystem>
#include <atomic>
#ifndef _WIN32
#include <unistd.h>   // _exit()
#else
#include <process.h>  // _exit() on MSVC
#endif
#include <chrono>
#include <mutex>
#include <unordered_map>

using json = nlohmann::json;

namespace naab {
namespace api {

// Implementation class (PIMPL pattern)
class RestApiServer::Impl {
public:
    httplib::Server server;
    std::string worker_exe;  // naab-lang, which runs each /execute request
    std::string cwd;         // the server's directory, where workers run
    bool waive_missing_config = false;  // server started with --no-governance
    std::shared_ptr<runtime::BlockLoader> block_loader;
    // V-API-004 (R24): per-request execution timeout in seconds. 0 disables.
    std::atomic<unsigned int> api_timeout_seconds{10};
    // V-DOS-005 (R25): token-bucket rate limiter. 0 = disabled.
    std::atomic<unsigned int> api_rate_limit_rpm{0};
    std::mutex rate_limit_mutex_;
    struct TokenBucket {
        double tokens = 0;
        std::chrono::steady_clock::time_point last_refill{};
    };
    std::unordered_map<std::string, TokenBucket> rate_buckets_;

    // Returns true if the request is allowed, false if rate-limited.
    static constexpr size_t MAX_RATE_BUCKETS = 65536;

    bool checkRateLimit(const std::string& key) {
        unsigned int limit = api_rate_limit_rpm.load(std::memory_order_relaxed);
        if (limit == 0) return true;
        std::lock_guard<std::mutex> lock(rate_limit_mutex_);
        // Evict oldest bucket if at capacity to prevent unbounded growth
        if (rate_buckets_.size() > MAX_RATE_BUCKETS) {
            auto oldest = rate_buckets_.begin();
            for (auto it = rate_buckets_.begin(); it != rate_buckets_.end(); ++it) {
                if (it->second.last_refill < oldest->second.last_refill)
                    oldest = it;
            }
            rate_buckets_.erase(oldest);
        }
        auto now = std::chrono::steady_clock::now();
        auto& bucket = rate_buckets_[key];
        if (bucket.last_refill.time_since_epoch().count() == 0) {
            // First request: initialize with full capacity
            bucket.tokens = static_cast<double>(limit);
            bucket.last_refill = now;
        }
        double elapsed = std::chrono::duration<double>(now - bucket.last_refill).count();
        bucket.tokens = std::min(static_cast<double>(limit),
                                 bucket.tokens + elapsed * (limit / 60.0));
        bucket.last_refill = now;
        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }
        return false;
    }

    Impl() {
        std::error_code ec;
        cwd = std::filesystem::current_path(ec).string();
#ifndef _WIN32
        if (!cwd.empty()) removeStaleWorkerPrograms(cwd, ".naab-rest-");
#endif
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

#ifdef _WIN32
                res.status = 501;
                res.set_content(json{
                    {"error", "Not available on this platform yet"},
                    {"message", "Each request runs in a worker process, which needs POSIX "
                                "process groups and pipes. Run the server on Linux or macOS."},
                    {"status", "error"}
                }.dump(2), "application/json");
                return;
#else
                if (worker_exe.empty()) {
                    res.status = 503;
                    res.set_content(json{{"error","Executor not available"},{"status","error"}}.dump(2),
                                    "application/json");
                    return;
                }

                // The request runs in its own `naab-lang run` worker, on the
                // default engine, under the govern.json the server's directory
                // resolves to -- the MCP server's design (docs/plan-mcp-service.md).
                // It used to run on a tree-walker inside this process, which is
                // refused under enforce (Bundle 2) and shared the process with
                // every other request: state leaked between requests and one
                // request's crash took the server down. The program file sits
                // in the server's directory so relative imports and discovery
                // behave as for a script there.
                std::string path = writeWorkerProgram(cwd, ".naab-rest-", code);
                if (path.empty()) {
                    res.status = 500;
                    res.set_content(json{{"error","Could not write the program file"},{"status","error"}}.dump(2),
                                    "application/json");
                    return;
                }
                // V-API-004 (R24): api_timeout_seconds bounds the run. The
                // worker gets it as --timeout (a govern.json limit still caps
                // it) and runWorker() kills the worker 5 s after it, so an
                // infinite loop cannot pin a cpp-httplib thread. 0 opts out
                // (tests/debug only): no --timeout, and a one-day ceiling.
                const unsigned int t = api_timeout_seconds.load();
                std::vector<std::string> wargs;
                if (t > 0) { wargs.push_back("--timeout"); wargs.push_back(std::to_string(t)); }
                if (waive_missing_config) wargs.push_back("--no-governance");
                wargs.push_back("--");
                wargs.push_back(path);
                auto fl = std::make_shared<InFlight>();
                WorkerResult w = runWorker(worker_exe, cwd, wargs, t > 0 ? static_cast<int>(t) : 86400, fl,
                                           [](const std::string&) { return false; });
                unlink(path.c_str());

                // The worker's governance banners and warnings go where the
                // in-process run printed them: the server's own stderr.
                if (!w.err.empty()) fprintf(stderr, "%s", w.err.c_str());

                // The CLI prints a fatal error to stdout after the program's
                // own output, as a block starting "Error: " on a new line; the
                // last one is the error that ended the run. Split it off so
                // "output" stays what the program printed.
                std::string output = w.out;
                std::string error_msg;
                int exit_code = 0;
                if (!w.started) {
                    exit_code = 1;
                    error_msg = "The program could not be started";
                } else if (w.timed_out) {
                    exit_code = 1;
                    error_msg = "Execution timed out";
                } else if (w.term_signal != 0) {
                    exit_code = 1;
                    error_msg = "The program stopped on signal " + std::to_string(w.term_signal);
                } else {
                    exit_code = w.exit_code;
                    if (exit_code != 0) {
                        size_t pos = std::string::npos;
                        if (output.rfind("Error: ", 0) == 0) pos = 0;
                        size_t nl = output.rfind("\nError: ");
                        if (nl != std::string::npos) pos = nl + 1;
                        if (pos != std::string::npos) {
                            error_msg = output.substr(pos);
                            output.erase(pos);
                        } else {
                            // Not on stdout: the worker's own stderr, without
                            // the lines every run prints.
                            std::istringstream es(w.err);
                            std::string line;
                            while (std::getline(es, line)) {
                                if (line.empty() || line.rfind("[governance] Loaded:", 0) == 0 ||
                                    line.rfind("-- Governance:", 0) == 0) continue;
                                error_msg += line + "\n";
                            }
                            if (error_msg.empty())
                                error_msg = "The program exited with code " + std::to_string(exit_code);
                        }
                        // V-ERR-002: sanitize error messages before returning to caller
                        error_msg = naab::error::ErrorSanitizer::sanitize(error_msg);
                    }
                }
                if (exit_code == 3) {
                    // A HARD block is a verdict about THIS REQUEST, not about
                    // the server: the worker exits 3, the server keeps serving.
                    fprintf(stderr, "[governance] HARD block in REST /execute\n");
                }
                fflush(stderr);

                json response = {
                    {"status",    exit_code == 0 && error_msg.empty() ? "success" : "error"},
                    {"output",    output},
                    {"error",     error_msg},
                    {"exit_code", exit_code}
                };
                if (w.out_truncated) response["output_truncated"] = true;
                res.set_content(response.dump(2), "application/json");
#endif

            } catch (const json::exception& e) {
                res.status = 400;
                json error_response = {
                    {"error", "Invalid JSON"},
                    {"message", e.what()},
                    {"status", "error"}
                };
                res.set_content(error_response.dump(2), "application/json");
            } catch (const governance::GovernanceHardError& e) {
                // Same reasoning as above: refuse the request, keep the server.
                fprintf(stderr, "[governance] HARD block in REST /execute: %s\n", e.what());
                fflush(stderr);
                res.status = 403;
                res.set_content(json{
                    {"error", "Governance block"},
                    {"message", naab::error::ErrorSanitizer::sanitize(e.what())},
                    {"status", "error"},
                    {"exit_code", 3}
                }.dump(2), "application/json");
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
                    {"message", naab::error::ErrorSanitizer::sanitize(e.what())},
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
                    {"message", naab::error::ErrorSanitizer::sanitize(e.what())},
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
                    {"message", naab::error::ErrorSanitizer::sanitize(e.what())},
                    {"status", "error"}
                };
                res.set_content(error_response.dump(2), "application/json");
            }
        });

        // Governance check endpoint — static analysis without code execution
        server.Post("/api/v1/check", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                std::string code = body.value("code", "");
                std::string language = body.value("language", "");

                if (code.empty() || language.empty()) {
                    res.status = 400;
                    res.set_content(json{
                        {"error", "Missing required fields: 'code' and 'language'"},
                        {"status", "error"}
                    }.dump(2), "application/json");
                    return;
                }

                std::string source_file = body.value("source_file", "api-check");
                int start_line = body.value("line", 1);

                // Fresh engine per request — thread-safe, no state leakage
                naab::governance::GovernanceEngine engine;

                // Always use server-side governance config — never accept client-supplied config
                const bool config_ok = engine.discoverAndLoad(std::filesystem::current_path().string());

                // Refuse rather than answer "blocked": false for a request that was
                // never governed. discoverAndLoad() is best-effort: with no config
                // found the checks below run against nothing, wasBlocked() is false,
                // and a 200 carrying "blocked": false is indistinguishable from a
                // genuine pass unless the caller also reads config_loaded. That is
                // the same fail-open shape naab-gov carried (no JSON => reads as
                // "not blocked"); a governance endpoint must not report a verdict it
                // did not reach.
                //
                // The predicate is "was a config FILE discovered", not isActive().
                // A govern.json carrying "mode": "off" loads fine and leaves
                // active_ false -- that is an operator who has deliberately turned
                // governance off, not an absent policy, and refusing it would break
                // a supported configuration. tests/api/test_api_auth.sh runs exactly
                // that config and caught this when the guard first used isActive().
                if (engine.getGovernDir().empty()) {
                    res.status = 400;
                    res.set_content(json{
                        {"error", "No governance config"},
                        {"status", "error"}
                    }.dump(2), "application/json");
                    return;
                }
                // A config that was found and did not load (a parse error, an
                // unresolvable extends, a failed signature) is the same case: the
                // checks would run against nothing. Discovery records the
                // directory before it loads, so the test above does not see it
                // (A84; measured before this: 200 with "blocked": false).
                if (!config_ok) {
                    res.status = 400;
                    res.set_content(json{
                        {"error", "Governance config could not be loaded"},
                        {"status", "error"}
                    }.dump(2), "application/json");
                    return;
                }

                engine.setCheckContext(source_file, start_line);
                // V-API-004: Apply same timeout to /check as /execute to prevent ReDoS
                unsigned int t = api_timeout_seconds.load();
                if (t > 0) {
                    naab::security::ScopedTimeout _to(t);
                    engine.checkPolyglotBlock(language, code, source_file, start_line);
                } else {
                    engine.checkPolyglotBlock(language, code, source_file, start_line);
                }

                // Build violation array
                json violations = json::array();
                for (const auto& cr : engine.getCheckResults()) {
                    if (!cr.passed) {
                        json v;
                        v["rule"] = cr.rule_name;
                        v["level"] = static_cast<int>(cr.level);
                        v["message"] = cr.message;
                        v["severity"] = cr.severity;
                        v["line"] = cr.line;
                        if (!cr.cwe_ids.empty()) v["cwe_ids"] = cr.cwe_ids;
                        if (!cr.owasp_ids.empty()) v["owasp_ids"] = cr.owasp_ids;
                        violations.push_back(std::move(v));
                    }
                }

                json response;
                response["blocked"] = engine.wasBlocked();
                response["violations"] = violations;
                response["violation_count"] = violations.size();
                response["config_loaded"] = engine.isActive();

                // Include full report if violations found
                if (!violations.empty()) {
                    std::string json_report = engine.generateJsonReport();
                    if (!json_report.empty()) {
                        try {
                            response["report"] = json::parse(json_report);
                        } catch (...) {
                            response["report"] = json::object();
                        }
                    }
                }

                res.set_content(response.dump(2), "application/json");

            } catch (const json::exception& e) {
                res.status = 400;
                res.set_content(json{
                    {"error", "Invalid JSON"},
                    {"message", naab::error::ErrorSanitizer::sanitize(e.what())},
                    {"status", "error"}
                }.dump(2), "application/json");
            } catch (const governance::GovernanceHardError& e) {
                // Same reasoning as /execute: refuse the request, keep the server.
                fprintf(stderr, "[governance] HARD block in REST /check: %s\n", e.what());
                fflush(stderr);
                res.status = 403;
                res.set_content(json{
                    {"error", "Governance block"},
                    {"message", naab::error::ErrorSanitizer::sanitize(e.what())},
                    {"status", "error"},
                    {"exit_code", 3}
                }.dump(2), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{
                    {"error", "Internal server error"},
                    {"message", naab::error::ErrorSanitizer::sanitize(e.what())},
                    {"status", "error"}
                }.dump(2), "application/json");
            }
        });

        // 404 handler — only set content if the route handler didn't already
        server.set_error_handler([](const httplib::Request&, httplib::Response& res)
                                     -> httplib::Server::HandlerResponse {
            if (!res.body.empty()) {
                // Route handler already set an error response — keep it
                return httplib::Server::HandlerResponse::Handled;
            }
            res.set_content(json{
                {"error", "Endpoint not found"},
                {"status", "error"}
            }.dump(2), "application/json");
            return httplib::Server::HandlerResponse::Handled;
        });
    }

    // Multi-key storage (populated by setApiKeys)
    std::vector<governance::GovernanceRules::ApiKeyEntry> api_keys_;
    std::mutex keys_mutex_;

    // Thread-local matched scopes (set by pre-routing handler, read by route handlers)
    // Using a simple thread-local since cpp-httplib doesn't have per-request context.
    static thread_local std::vector<std::string> t_matched_scopes_;
    static thread_local bool t_has_scoped_key_;
    static thread_local std::string t_matched_key_id_;

    // Scope required by endpoint path
    static std::string scopeForPath(const std::string& path) {
        if (path.find("/api/v1/execute") == 0) return "execute";
        if (path.find("/api/v1/check") == 0) return "check";
        if (path.find("/api/v1/blocks") == 0) return "blocks";
        if (path.find("/api/v1/stats") == 0) return "stats";
        return "";
    }

    // V-API-001: apply body size cap and optional API key auth.
    // V-DOS-005 (R25): rate limiting added after auth check.
    // Called after construction so the outer RestApiServer members are set.
    // NOTE (M6): Each call reinstalls the pre-routing handler lambda. The legacy
    // api_key is captured by value (snapshot), while api_keys_/key_scopes_ are
    // read live from `this`. Callers must ensure api_key_ is current before
    // calling. Setter ordering is fragile but correct as-is.
    void applySecurityConfig(const std::string& api_key, size_t max_body_bytes) {
        // Body size cap — reject oversized requests before handlers run
        server.set_payload_max_length(max_body_bytes);

        // Pre-routing handler: auth + rate limiting + scope check
        server.set_pre_routing_handler(
            [this, api_key](const httplib::Request& req, httplib::Response& res) {
                // /health is always public — no auth, no rate limit
                if (req.path == "/health") {
                    return httplib::Server::HandlerResponse::Unhandled;
                }

                // Reset thread-local scope state
                t_matched_scopes_.clear();
                t_has_scoped_key_ = false;
                t_matched_key_id_.clear();

                // Determine if we have any auth configured
                bool has_legacy_key = !api_key.empty();
                bool has_multi_keys = false;
                {
                    std::lock_guard<std::mutex> lock(keys_mutex_);
                    has_multi_keys = !api_keys_.empty();
                }
                bool auth_required = has_legacy_key || has_multi_keys;

                if (auth_required) {
                    std::string presented_key;
                    if (req.has_header("Authorization")) {
                        auto auth_val = req.get_header_value("Authorization");
                        if (auth_val.substr(0, 7) == "Bearer ") {
                            presented_key = auth_val.substr(7);
                        }
                    }
                    if (presented_key.empty() && req.has_header("X-API-Key")) {
                        presented_key = req.get_header_value("X-API-Key");
                    }

                    if (presented_key.empty()) {
                        res.status = 401;
                        res.set_content(
                            nlohmann::json{{"error","Unauthorized"},{"status","error"}}.dump(2),
                            "application/json");
                        return httplib::Server::HandlerResponse::Handled;
                    }

                    bool authenticated = false;

                    // Check multi-key list first (takes priority)
                    if (has_multi_keys) {
                        std::lock_guard<std::mutex> lock(keys_mutex_);
                        for (const auto& entry : api_keys_) {
                            // V-API-002: constant-time comparison
                            if (naab::security::CryptoUtils::constantTimeCompare(
                                    presented_key, entry.key)) {
                                authenticated = true;
                                t_matched_scopes_ = entry.scopes;
                                t_has_scoped_key_ = !entry.scopes.empty();
                                t_matched_key_id_ = entry.name.empty()
                                    ? ("key_" + std::to_string(&entry - &api_keys_[0]))
                                    : entry.name;
                                break;
                            }
                        }
                    }

                    // Fall back to legacy single key
                    if (!authenticated && has_legacy_key) {
                        if (naab::security::CryptoUtils::constantTimeCompare(
                                presented_key, api_key)) {
                            authenticated = true;
                            t_matched_key_id_ = "legacy";
                            // Legacy key has full access (no scopes)
                        }
                    }

                    if (!authenticated) {
                        res.status = 401;
                        res.set_content(
                            nlohmann::json{{"error","Unauthorized"},{"status","error"}}.dump(2),
                            "application/json");
                        return httplib::Server::HandlerResponse::Handled;
                    }

                    // Scope check: if the matched key has scopes, verify this endpoint is allowed
                    if (t_has_scoped_key_) {
                        std::string required = scopeForPath(req.path);
                        if (required.empty()) {
                            // H3: default-deny — scoped keys cannot access unrecognized endpoints
                            res.status = 403;
                            res.set_content(
                                nlohmann::json{{"error","Forbidden: unknown endpoint for scoped key"},
                                               {"status","error"}}.dump(2),
                                "application/json");
                            return httplib::Server::HandlerResponse::Handled;
                        }
                        bool scope_ok = false;
                        for (const auto& s : t_matched_scopes_) {
                            if (s == required) { scope_ok = true; break; }
                        }
                        if (!scope_ok) {
                            res.status = 403;
                            res.set_content(
                                nlohmann::json{{"error","Forbidden: insufficient scope"},
                                               {"status","error"},
                                               {"required_scope", required}}.dump(2),
                                "application/json");
                            return httplib::Server::HandlerResponse::Handled;
                        }
                    }
                }

                // V-DOS-008: rate limiting keyed by per-key identity
                std::string rate_key = auth_required
                    ? (t_matched_key_id_.empty() ? "authenticated" : t_matched_key_id_)
                    : req.remote_addr;
                if (!checkRateLimit(rate_key)) {
                    res.status = 429;
                    res.set_header("Retry-After", "1");
                    res.set_content(
                        nlohmann::json{{"error","Rate limit exceeded"},{"status","error"}}.dump(2),
                        "application/json");
                    return httplib::Server::HandlerResponse::Handled;
                }

                return httplib::Server::HandlerResponse::Unhandled;
            });
    }
};

// Thread-local scope state
thread_local std::vector<std::string> RestApiServer::Impl::t_matched_scopes_;
thread_local bool RestApiServer::Impl::t_has_scoped_key_ = false;
thread_local std::string RestApiServer::Impl::t_matched_key_id_;

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
    if (!key.empty()) spdlog::info("REST API: API key authentication enabled");
}

void RestApiServer::setApiKeys(const std::vector<governance::GovernanceRules::ApiKeyEntry>& keys) {
    {
        std::lock_guard<std::mutex> lock(impl_->keys_mutex_);
        impl_->api_keys_ = keys;
    }
    impl_->applySecurityConfig(api_key_, max_body_bytes_);
    spdlog::info("REST API: {} API key(s) with scoped permissions configured", keys.size());
}

void RestApiServer::setMaxBodySize(size_t bytes) {
    max_body_bytes_ = bytes;
    impl_->applySecurityConfig(api_key_, max_body_bytes_);
    spdlog::info("REST API: max body size set to {} bytes", bytes);
}

void RestApiServer::setApiTimeout(unsigned int seconds) {
    impl_->api_timeout_seconds.store(seconds);
    spdlog::info("REST API: per-request execution timeout set to {} seconds", seconds);
}

void RestApiServer::setApiRateLimit(unsigned int requests_per_minute) {
    impl_->api_rate_limit_rpm.store(requests_per_minute, std::memory_order_relaxed);
    // Re-apply security config so the pre-routing handler is installed
    // (rate limiting works even without an API key)
    impl_->applySecurityConfig(api_key_, max_body_bytes_);
    spdlog::info("REST API: rate limit set to {} requests/minute per client", requests_per_minute);
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
    spdlog::info("  POST /api/v1/check          - Governance check");

    running_ = true;
    bool success = impl_->server.listen(host_.c_str(), port_);
    running_ = false;
    return success;
}

void RestApiServer::stop() {
    if (running_.exchange(false)) {
        spdlog::info("Stopping REST API server");
        impl_->server.stop();
    }
}

bool RestApiServer::isRunning() const {
    return running_;
}

void RestApiServer::setWorkerExecutable(const std::string& path) {
    impl_->worker_exe = path;
}

void RestApiServer::setWaiveMissingConfig(bool waive) {
    impl_->waive_missing_config = waive;
}

void RestApiServer::setBlockLoader(std::shared_ptr<runtime::BlockLoader> loader) {
    impl_->block_loader = loader;
    spdlog::info("Block loader set for REST API");
}

} // namespace api
} // namespace naab
