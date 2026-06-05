// governance_config.cpp — GovernanceEngine configuration loading
// Extracted from governance.cpp lines 1-1727

#include "naab/governance.h"
#include "naab/telemetry_forwarder.h"
#include "naab/limits.h"
#include "naab/language_registry.h"
#include "naab/interpreter.h"
#include "naab/analyzer/task_pattern_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include "naab/safe_regex.h"
#include "naab/sandbox.h"
#include "naab/subprocess_helpers.h"  // V-SC-006-ext: env scrub policy
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <functional>
#include <set>
#ifndef _WIN32
#  include <sys/file.h>
#  include <unistd.h>  // getpid() for telemetry run_id
#endif
#include <fmt/core.h>


namespace naab {
namespace governance {

// ============================================================================
// Destructor
// ============================================================================

GovernanceEngine::GovernanceEngine()
    : rules_ptr_(std::make_shared<GovernanceRules>()) {}

GovernanceEngine::~GovernanceEngine() {
    // Flush telemetry forwarder before anything else
    if (telemetry_forwarder_) {
        telemetry_forwarder_->shutdown();
        telemetry_forwarder_.reset();
    }
    if (baselines_dirty_) {
        saveBaselines();
    }
    if (baselines_data_) {
        delete static_cast<nlohmann::json*>(baselines_data_);
        baselines_data_ = nullptr;
    }
}

// ============================================================================
// Helper functions
// ============================================================================

std::string GovernanceEngine::levelToString(EnforcementLevel level) {
    switch (level) {
        case EnforcementLevel::NONE:              return "none";
        case EnforcementLevel::HARD:              return "hard";
        case EnforcementLevel::APPROVAL_REQUIRED: return "approval_required";
        case EnforcementLevel::SOFT:              return "soft";
        case EnforcementLevel::ADVISORY:          return "advisory";
    }
    return "unknown";
}

std::string GovernanceEngine::levelToTag(EnforcementLevel level) {
    switch (level) {
        case EnforcementLevel::NONE:              return "NONE";
        case EnforcementLevel::HARD:              return "HARD-MANDATORY";
        case EnforcementLevel::APPROVAL_REQUIRED: return "APPROVAL-REQUIRED";
        case EnforcementLevel::SOFT:              return "SOFT-MANDATORY";
        case EnforcementLevel::ADVISORY:          return "ADVISORY";
    }
    return "UNKNOWN";
}

std::string GovernanceEngine::formatError(
    EnforcementLevel level,
    const std::string& what,
    const std::string& location,
    const std::string& rule,
    const std::string& help,
    const std::string& bad_example,
    const std::string& good_example) {

    std::ostringstream oss;
    if (level == EnforcementLevel::ADVISORY) {
        oss << "Governance warning: " << what << " [" << levelToTag(level) << "]\n\n";
    } else {
        oss << "Governance error: " << what << " [" << levelToTag(level) << "]\n\n";
    }

    if (!location.empty()) {
        oss << "  At: " << location << "\n";
    }
    oss << "  Rule (govern.json): " << rule << "\n\n";

    if (!help.empty()) {
        oss << "  Help:\n";
        // Split help by newlines and indent each line
        std::istringstream help_stream(help);
        std::string line;
        while (std::getline(help_stream, line)) {
            oss << "  - " << line << "\n";
        }
        oss << "\n";
    }

    if (!bad_example.empty() || !good_example.empty()) {
        oss << "  Example:\n";
        if (!bad_example.empty()) {
            oss << "    ✗ Blocked:\n";
            std::istringstream bad_stream(bad_example);
            std::string line;
            while (std::getline(bad_stream, line)) {
                oss << "      " << line << "\n";
            }
        }
        if (!good_example.empty()) {
            oss << "    ✓ Allowed:\n";
            std::istringstream good_stream(good_example);
            std::string line;
            while (std::getline(good_stream, line)) {
                oss << "      " << line << "\n";
            }
        }
    }

    if (level == EnforcementLevel::SOFT) {
        oss << "\n  This rule is enforced by the project's governance configuration.\n";
        oss << "  Contact the project owner if this check needs review.\n";
    } else if (level == EnforcementLevel::ADVISORY) {
        oss << "\n  Note: This is an advisory warning — execution will continue\n";
    }

    return oss.str();
}

// ============================================================================
// JSON Loading
// ============================================================================

static std::pair<bool, EnforcementLevel> parseEnforcementLevel(
    const nlohmann::json& value) {
    if (value.is_boolean()) {
        return {value.get<bool>(), EnforcementLevel::HARD};
    }
    if (value.is_string()) {
        std::string s = value.get<std::string>();
        if (s == "hard")              return {true, EnforcementLevel::HARD};
        if (s == "approval_required") return {true, EnforcementLevel::APPROVAL_REQUIRED};
        if (s == "soft")              return {true, EnforcementLevel::SOFT};
        if (s == "advisory")          return {true, EnforcementLevel::ADVISORY};
    }
    if (value.is_object()) {
        bool enabled = value.value("enabled", true);
        EnforcementLevel level = EnforcementLevel::HARD;
        if (value.contains("level") && value["level"].is_string()) {
            std::string s = value["level"].get<std::string>();
            if (s == "approval_required") level = EnforcementLevel::APPROVAL_REQUIRED;
            else if (s == "soft") level = EnforcementLevel::SOFT;
            else if (s == "advisory") level = EnforcementLevel::ADVISORY;
        }
        return {enabled, level};
    }
    return {false, EnforcementLevel::HARD};
}

// V-GOV-019 (R24): governance configs are parsed by dozens of unbounded
// `for (auto& el : arr) vec.push_back(...)` loops. nlohmann::json caps parse
// depth but not array width, so a single `taint_tracking.sources` array of
// millions of strings would allocate an enormous std::vector before any
// check runs. Walk the parsed tree once and reject any array wider than this
// cap — cheap, and a single guard covers every downstream loop without
// mechanically rewriting each call site.
static constexpr size_t MAX_GOV_ARRAY_ELEMS = 10000;

static bool checkJsonArrayWidth(const nlohmann::json& j, size_t max = MAX_GOV_ARRAY_ELEMS) {
    if (j.is_array() && j.size() > max) return false;
    if (j.is_structured()) {
        for (auto& child : j) {
            if (!checkJsonArrayWidth(child, max)) return false;
        }
    }
    return true;
}

// Helper: parse optional "rationale" string from a JSON object into a target string
static void parseRationale(const nlohmann::json& obj, std::string& target) {
    if (obj.contains("rationale") && obj["rationale"].is_string()) {
        target = obj["rationale"].get<std::string>();
    }
}

static void loadFromJson(const nlohmann::json& j, GovernanceRules& rules_) {
    // Mode
    if (j.contains("mode") && j["mode"].is_string()) {
        std::string mode = j["mode"].get<std::string>();
        if (mode == "enforce")    rules_.mode = GovernanceMode::ENFORCE;
        else if (mode == "audit") rules_.mode = GovernanceMode::AUDIT;
        else if (mode == "off")   rules_.mode = GovernanceMode::OFF;
    }

    // Top-level sandbox_level (convenience — also available under governance. or security.)
    if (j.contains("sandbox_level") && j["sandbox_level"].is_string()) {
        rules_.sandbox_level_config = j["sandbox_level"].get<std::string>();
    }

    // Governance behavior settings (govern.json alternative to CLI flags)
    if (j.contains("governance") && j["governance"].is_object()) {
        auto& gov = j["governance"];
        if (gov.contains("verbose")) rules_.verbose = gov["verbose"].get<bool>();
        if (gov.contains("dashboard")) rules_.dashboard = gov["dashboard"].get<bool>();
        if (gov.contains("baseline_save")) rules_.baseline_save = gov["baseline_save"].get<bool>();
        if (gov.contains("override")) rules_.allow_override = gov["override"].get<bool>();
        if (gov.contains("lint_only")) rules_.lint_only_config = gov["lint_only"].get<bool>();
        if (gov.contains("record_baselines")) rules_.record_baselines = gov["record_baselines"].get<bool>();
        if (gov.contains("check_baselines")) rules_.check_baselines = gov["check_baselines"].get<bool>();
        if (gov.contains("quiet")) rules_.quiet_config = gov["quiet"].get<bool>();
        if (gov.contains("no_color")) rules_.no_color_config = gov["no_color"].get<bool>();
        if (gov.contains("report_json")) rules_.report_json = gov["report_json"].get<std::string>();
        if (gov.contains("report_sarif")) rules_.report_sarif = gov["report_sarif"].get<std::string>();
        if (gov.contains("report_junit")) rules_.report_junit = gov["report_junit"].get<std::string>();
        if (gov.contains("telemetry")) rules_.telemetry_path = gov["telemetry"].get<std::string>();
        if (gov.contains("agent_id")) rules_.agent_id_config = gov["agent_id"].get<std::string>();
        if (gov.contains("default_env")) rules_.default_env = gov["default_env"].get<std::string>();
        if (gov.contains("strict_types")) rules_.strict_types_config = gov["strict_types"].get<bool>();
        if (gov.contains("gc_threshold")) rules_.runtime.gc_threshold = gov["gc_threshold"].get<size_t>();
        if (gov.contains("gc_stats")) rules_.runtime.gc_stats = gov["gc_stats"].get<bool>();
        if (gov.contains("sandbox_level")) rules_.sandbox_level_config = gov["sandbox_level"].get<std::string>();
        if (gov.contains("explanations")) rules_.explanations_enabled = gov["explanations"].get<bool>();
        if (gov.contains("require_override_reason")) rules_.require_override_reason = gov["require_override_reason"].get<bool>();
    }

    // Runtime limits section
    if (j.contains("runtime") && j["runtime"].is_object()) {
        auto& rt = j["runtime"];
        if (rt.contains("timeout")) rules_.runtime.timeout = rt["timeout"].get<int>();
        if (rt.contains("memory_limit")) rules_.runtime.memory_limit = rt["memory_limit"].get<size_t>();
        if (rt.contains("gc_threshold")) rules_.runtime.gc_threshold = rt["gc_threshold"].get<size_t>();
        if (rt.contains("gc_stats")) rules_.runtime.gc_stats = rt["gc_stats"].get<bool>();
    }

    // Security section
    if (j.contains("security") && j["security"].is_object()) {
        auto& sec = j["security"];
        if (sec.contains("sandbox_level")) rules_.sandbox_level_config = sec["sandbox_level"].get<std::string>();
        if (sec.contains("allow_network")) rules_.allow_network_config = sec["allow_network"].get<bool>();
        if (sec.contains("strict_types")) rules_.strict_types_config = sec["strict_types"].get<bool>();
    }

    // API section
    if (j.contains("api") && j["api"].is_object()) {
        auto& api = j["api"];
        if (api.contains("key")) rules_.api.key = api["key"].get<std::string>();
        if (api.contains("timeout")) rules_.api.timeout = api["timeout"].get<int>();
        if (api.contains("rate_limit")) rules_.api.rate_limit = api["rate_limit"].get<int>();
        if (api.contains("max_body")) rules_.api.max_body = api["max_body"].get<size_t>();
        if (api.contains("tls_cert")) rules_.api.tls_cert_path = api["tls_cert"].get<std::string>();
        if (api.contains("tls_key")) rules_.api.tls_key_path = api["tls_key"].get<std::string>();
        if (api.contains("keys") && api["keys"].is_array()) {
            for (auto& k : api["keys"]) {
                if (!k.is_object() || !k.contains("key")) continue;
                GovernanceRules::ApiKeyEntry entry;
                entry.key = k["key"].get<std::string>();
                if (k.contains("name")) entry.name = k["name"].get<std::string>();
                if (k.contains("scopes") && k["scopes"].is_array()) {
                    for (auto& s : k["scopes"]) {
                        if (s.is_string()) entry.scopes.push_back(s.get<std::string>());
                    }
                }
                rules_.api.keys.push_back(std::move(entry));
            }
        }
    }

    // Languages
    if (j.contains("languages")) {
        auto& lang = j["languages"];
        if (lang.contains("allowed") && lang["allowed"].is_array()) {
            for (auto& l : lang["allowed"]) {
                if (l.is_string()) rules_.allowed_languages.insert(l.get<std::string>());
            }
        }
        if (lang.contains("blocked") && lang["blocked"].is_array()) {
            for (auto& l : lang["blocked"]) {
                if (l.is_string()) rules_.blocked_languages.insert(l.get<std::string>());
            }
        }
        // naab-29 EO-08: Resolve contradictory config — blocked takes precedence
        for (const auto& blocked : rules_.blocked_languages) {
            rules_.allowed_languages.erase(blocked);
        }
    }

    // Capabilities (supports both legacy flat and v3.0 object formats)
    if (j.contains("capabilities")) {
        auto& cap = j["capabilities"];
        if (cap.contains("network")) {
            if (cap["network"].is_boolean())
                rules_.network_allowed = cap["network"].get<bool>();
            else if (cap["network"].is_object() && cap["network"].contains("enabled"))
                rules_.network_allowed = cap["network"]["enabled"].get<bool>();
        }
        if (cap.contains("filesystem")) {
            if (cap["filesystem"].is_string())
                rules_.filesystem_mode = cap["filesystem"].get<std::string>();
            else if (cap["filesystem"].is_object() && cap["filesystem"].contains("mode"))
                rules_.filesystem_mode = cap["filesystem"]["mode"].get<std::string>();
        }
        if (cap.contains("shell")) {
            if (cap["shell"].is_boolean())
                rules_.shell_allowed = cap["shell"].get<bool>();
            else if (cap["shell"].is_object() && cap["shell"].contains("enabled"))
                rules_.shell_allowed = cap["shell"]["enabled"].get<bool>();
        }
    }

    // Limits (supports both legacy flat and v3.0 nested formats)
    if (j.contains("limits")) {
        auto& lim = j["limits"];
        if (lim.contains("timeout")) {
            if (lim["timeout"].is_number())
                rules_.timeout_seconds = lim["timeout"].get<int>();
            else if (lim["timeout"].is_object()) {
                if (lim["timeout"].contains("global"))
                    rules_.timeout_seconds = lim["timeout"]["global"].get<int>();
            }
        }
        if (lim.contains("memory")) {
            if (lim["memory"].is_number())
                rules_.memory_limit_mb = lim["memory"].get<int>();
            else if (lim["memory"].is_object()) {
                if (lim["memory"].contains("total_mb"))
                    rules_.memory_limit_mb = lim["memory"]["total_mb"].get<int>();
            }
        }
        if (lim.contains("call_depth"))
            rules_.max_call_depth = lim["call_depth"].get<int>();
        if (lim.contains("array_size"))
            rules_.max_array_size = lim["array_size"].get<int>();
        // V3.0 nested paths
        if (lim.contains("execution") && lim["execution"].is_object()) {
            auto& exec = lim["execution"];
            if (exec.contains("call_depth"))
                rules_.max_call_depth = exec["call_depth"].get<int>();
        }
        if (lim.contains("data") && lim["data"].is_object()) {
            auto& data = lim["data"];
            if (data.contains("array_size"))
                rules_.max_array_size = data["array_size"].get<int>();
        }
    }

    // Requirements (supports both legacy string/bool and v3.0 object formats)
    if (j.contains("requirements")) {
        auto& req = j["requirements"];
        if (req.contains("error_handling")) {
            if (req["error_handling"].is_object()) {
                auto& eh = req["error_handling"];
                if (eh.contains("level")) {
                    auto [en, lv] = parseEnforcementLevel(eh["level"]);
                    rules_.require_error_handling = en;
                    rules_.error_handling_level = lv;
                }
            } else {
                auto [enabled, level] = parseEnforcementLevel(req["error_handling"]);
                rules_.require_error_handling = enabled;
                rules_.error_handling_level = level;
            }
        }
        if (req.contains("main_block")) {
            if (req["main_block"].is_object()) {
                auto& mb = req["main_block"];
                if (mb.contains("level")) {
                    auto [en, lv] = parseEnforcementLevel(mb["level"]);
                    rules_.require_main_block = en;
                    rules_.main_block_level = lv;
                }
            } else {
                auto [enabled, level] = parseEnforcementLevel(req["main_block"]);
                rules_.require_main_block = enabled;
                rules_.main_block_level = level;
            }
        }
    }

    // Restrictions (supports both legacy string/bool and v3.0 object formats)
    if (j.contains("restrictions")) {
        auto& res = j["restrictions"];
        if (res.contains("polyglot_output")) {
            if (res["polyglot_output"].is_string())
                rules_.polyglot_output = res["polyglot_output"].get<std::string>();
            else if (res["polyglot_output"].is_object() && res["polyglot_output"].contains("format"))
                rules_.polyglot_output = res["polyglot_output"]["format"].get<std::string>();
        }
        if (res.contains("dangerous_calls")) {
            if (res["dangerous_calls"].is_object()) {
                auto& dc = res["dangerous_calls"];
                if (dc.contains("level")) {
                    auto [en, lv] = parseEnforcementLevel(dc["level"]);
                    rules_.restrict_dangerous_calls = en;
                    rules_.dangerous_calls_level = lv;
                }
            } else {
                auto [enabled, level] = parseEnforcementLevel(res["dangerous_calls"]);
                rules_.restrict_dangerous_calls = enabled;
                rules_.dangerous_calls_level = level;
            }
        }
        // Allow no_secrets/no_placeholders/no_hardcoded_results under restrictions (alias for code_quality)
        if (res.contains("no_secrets")) {
            auto [en, lv] = parseEnforcementLevel(res["no_secrets"]);
            rules_.no_secrets = en; rules_.no_secrets_level = lv;
            rules_.code_quality.no_secrets.enabled = en;
            rules_.code_quality.no_secrets.level = lv;
        }
        if (res.contains("no_placeholders")) {
            auto [en, lv] = parseEnforcementLevel(res["no_placeholders"]);
            rules_.no_placeholders = en; rules_.no_placeholders_level = lv;
            rules_.code_quality.no_placeholders.enabled = en;
            rules_.code_quality.no_placeholders.level = lv;
        }
        if (res.contains("no_hardcoded_results")) {
            auto [en, lv] = parseEnforcementLevel(res["no_hardcoded_results"]);
            rules_.no_hardcoded_results = en; rules_.no_hardcoded_results_level = lv;
            rules_.code_quality.no_hardcoded_results.enabled = en;
            rules_.code_quality.no_hardcoded_results.level = lv;
        }
    }

    // Code Quality (supports both legacy string/bool and v3.0 object formats)
    if (j.contains("code_quality")) {
        auto& cq = j["code_quality"];

        // Helper: parse a code quality field that may be bool/string or object with "level"
        auto parseCodeQualityField = [](const nlohmann::json& val, bool& out_enabled, EnforcementLevel& out_level) {
            if (val.is_object()) {
                if (val.contains("level")) {
                    auto [en, lv] = parseEnforcementLevel(val["level"]);
                    out_enabled = en;
                    out_level = lv;
                } else {
                    out_enabled = true;
                    out_level = EnforcementLevel::HARD;
                }
            } else {
                auto [en, lv] = parseEnforcementLevel(val);
                out_enabled = en;
                out_level = lv;
            }
        };

        if (cq.contains("no_secrets"))
            parseCodeQualityField(cq["no_secrets"], rules_.no_secrets, rules_.no_secrets_level);
        if (cq.contains("no_placeholders"))
            parseCodeQualityField(cq["no_placeholders"], rules_.no_placeholders, rules_.no_placeholders_level);
        if (cq.contains("no_hardcoded_results"))
            parseCodeQualityField(cq["no_hardcoded_results"], rules_.no_hardcoded_results, rules_.no_hardcoded_results_level);
    }

    // Audit (legacy simple format)
    if (j.contains("audit")) {
        auto& aud = j["audit"];
        if (aud.is_object()) {
            if (aud.contains("level"))
                rules_.audit_level = aud["level"].get<std::string>();
            if (aud.contains("tamper_evidence")) {
                if (aud["tamper_evidence"].is_boolean())
                    rules_.tamper_evidence = aud["tamper_evidence"].get<bool>();
            }
        }
    }

    // --- V3.0 Expanded Sections ---
    if (j.contains("version"))
        rules_.version = j["version"].get<std::string>();
    if (j.contains("extends"))
        rules_.extends_path = j["extends"].get<std::string>();
    if (j.contains("description"))
        rules_.description = j["description"].get<std::string>();

    // V3 Languages: per_language configs
    if (j.contains("languages") && j["languages"].is_object()) {
        auto& lang = j["languages"];
        if (lang.contains("require_explicit"))
            rules_.languages.require_explicit = lang["require_explicit"].get<bool>();

        // Sync to new struct too
        rules_.languages.allowed = rules_.allowed_languages;
        rules_.languages.blocked = rules_.blocked_languages;

        if (lang.contains("per_language") && lang["per_language"].is_object()) {
            for (auto& [lang_name, cfg] : lang["per_language"].items()) {
                LanguageConfig lc;
                if (cfg.contains("timeout")) lc.timeout = cfg["timeout"].get<int>();
                if (cfg.contains("max_lines")) lc.max_lines = cfg["max_lines"].get<int>();
                if (cfg.contains("max_output_size")) lc.max_output_size = cfg["max_output_size"].get<int>();
                if (cfg.contains("version_hint")) lc.version_hint = cfg["version_hint"].get<std::string>();

                if (cfg.contains("dangerous_calls")) {
                    auto [en, lv] = parseEnforcementLevel(cfg["dangerous_calls"]);
                    lc.dangerous_calls_enabled = en;
                    lc.dangerous_calls = lv;
                }
                if (cfg.contains("banned_functions")) {
                    for (auto& f : cfg["banned_functions"])
                        lc.banned_functions.push_back(f.get<std::string>());
                }
                if (cfg.contains("banned_globals")) {
                    for (auto& g : cfg["banned_globals"])
                        lc.banned_globals.push_back(g.get<std::string>());
                }
                if (cfg.contains("banned_keywords")) {
                    for (auto& k : cfg["banned_keywords"])
                        lc.banned_keywords.push_back(k.get<std::string>());
                }
                if (cfg.contains("banned_imports")) {
                    for (auto& i : cfg["banned_imports"])
                        lc.banned_imports.push_back(i.get<std::string>());
                }
                if (cfg.contains("banned_namespaces")) {
                    for (auto& n : cfg["banned_namespaces"])
                        lc.banned_namespaces.push_back(n.get<std::string>());
                }
                if (cfg.contains("banned_commands")) {
                    for (auto& c : cfg["banned_commands"])
                        lc.banned_commands.push_back(c.get<std::string>());
                }
                if (cfg.contains("imports") && cfg["imports"].is_object()) {
                    auto& imp = cfg["imports"];
                    if (imp.contains("mode")) lc.imports.mode = imp["mode"].get<std::string>();
                    if (imp.contains("blocked"))
                        for (auto& b : imp["blocked"]) lc.imports.blocked.push_back(b.get<std::string>());
                    if (imp.contains("allowed"))
                        for (auto& a : imp["allowed"]) lc.imports.allowed.push_back(a.get<std::string>());
                }
                // Shell-specific
                if (cfg.contains("require_set_e")) {
                    auto [en, lv] = parseEnforcementLevel(cfg["require_set_e"]);
                    lc.require_set_e = en; lc.require_set_e_level = lv;
                }
                if (cfg.contains("no_curl_pipe_sh")) {
                    auto [en, lv] = parseEnforcementLevel(cfg["no_curl_pipe_sh"]);
                    lc.no_curl_pipe_sh = en; lc.no_curl_pipe_sh_level = lv;
                }
                if (cfg.contains("no_wget_pipe_bash")) {
                    auto [en, lv] = parseEnforcementLevel(cfg["no_wget_pipe_bash"]);
                    lc.no_wget_pipe_bash = en; lc.no_wget_pipe_bash_level = lv;
                }
                // JS-specific
                if (cfg.contains("strict_mode")) {
                    auto [en, lv] = parseEnforcementLevel(cfg["strict_mode"]);
                    lc.strict_mode = en; lc.strict_mode_level = lv;
                }
                if (cfg.contains("no_var")) {
                    auto [en, lv] = parseEnforcementLevel(cfg["no_var"]);
                    lc.no_var = en; lc.no_var_level = lv;
                }
                if (cfg.contains("no_console_log")) {
                    auto [en, lv] = parseEnforcementLevel(cfg["no_console_log"]);
                    lc.no_console_log = en; lc.no_console_log_level = lv;
                }
                // Go-specific
                if (cfg.contains("require_package_main"))
                    lc.require_package_main = cfg["require_package_main"].get<bool>();

                rules_.languages.per_language[lang_name] = std::move(lc);
            }
        }
    }

    // V3 Capabilities (expanded objects)
    if (j.contains("capabilities") && j["capabilities"].is_object()) {
        auto& cap = j["capabilities"];

        if (cap.contains("network") && cap["network"].is_object()) {
            auto& net = cap["network"];
            auto& nc = rules_.capabilities.network;
            if (net.contains("enabled")) { nc.enabled = net["enabled"].get<bool>(); rules_.network_allowed = nc.enabled; }
            if (net.contains("https_only")) nc.https_only = net["https_only"].get<bool>();
            if (net.contains("allowed_hosts"))
                for (auto& h : net["allowed_hosts"]) nc.allowed_hosts.push_back(h.get<std::string>());
            if (net.contains("blocked_hosts"))
                for (auto& h : net["blocked_hosts"]) nc.blocked_hosts.push_back(h.get<std::string>());
            if (net.contains("allowed_ports"))
                for (auto& p : net["allowed_ports"]) nc.allowed_ports.push_back(p.get<int>());
            if (net.contains("allow_websockets")) nc.allow_websockets = net["allow_websockets"].get<bool>();
            if (net.contains("allow_raw_sockets")) nc.allow_raw_sockets = net["allow_raw_sockets"].get<bool>();
            parseRationale(net, nc.rationale);
        }
        if (cap.contains("filesystem") && cap["filesystem"].is_object()) {
            auto& fs = cap["filesystem"];
            auto& fc = rules_.capabilities.filesystem;
            if (fs.contains("mode")) { fc.mode = fs["mode"].get<std::string>(); rules_.filesystem_mode = fc.mode; }
            if (fs.contains("allowed_paths"))
                for (auto& p : fs["allowed_paths"]) fc.allowed_paths.push_back(p.get<std::string>());
            if (fs.contains("blocked_paths"))
                for (auto& p : fs["blocked_paths"]) fc.blocked_paths.push_back(p.get<std::string>());
            if (fs.contains("allowed_extensions"))
                for (auto& e : fs["allowed_extensions"]) fc.allowed_extensions.push_back(e.get<std::string>());
            if (fs.contains("max_file_size")) fc.max_file_size = fs["max_file_size"].get<int>();
            if (fs.contains("max_files")) fc.max_files = fs["max_files"].get<int>();
            if (fs.contains("allow_symlinks")) fc.allow_symlinks = fs["allow_symlinks"].get<bool>();
            if (fs.contains("allow_hidden_files")) fc.allow_hidden_files = fs["allow_hidden_files"].get<bool>();
            if (fs.contains("allow_absolute_paths")) fc.allow_absolute_paths = fs["allow_absolute_paths"].get<bool>();
            parseRationale(fs, fc.rationale);
        }
        if (cap.contains("shell") && cap["shell"].is_object()) {
            auto& sh = cap["shell"];
            auto& sc = rules_.capabilities.shell;
            if (sh.contains("enabled")) { sc.enabled = sh["enabled"].get<bool>(); rules_.shell_allowed = sc.enabled; }
            if (sh.contains("allowed_commands"))
                for (auto& c : sh["allowed_commands"]) sc.allowed_commands.push_back(c.get<std::string>());
            if (sh.contains("blocked_commands"))
                for (auto& c : sh["blocked_commands"]) sc.blocked_commands.push_back(c.get<std::string>());
            if (sh.contains("allow_pipes")) sc.allow_pipes = sh["allow_pipes"].get<bool>();
            if (sh.contains("allow_redirects")) sc.allow_redirects = sh["allow_redirects"].get<bool>();
            if (sh.contains("max_execution_time")) sc.max_execution_time = sh["max_execution_time"].get<int>();
            parseRationale(sh, sc.rationale);
        }
        if (cap.contains("env_vars") && cap["env_vars"].is_object()) {
            auto& ev = cap["env_vars"];
            auto& ec = rules_.capabilities.env_vars;
            if (ev.contains("read")) ec.read = ev["read"].get<bool>();
            if (ev.contains("write")) ec.write = ev["write"].get<bool>();
            if (ev.contains("allowed_read"))
                for (auto& v : ev["allowed_read"]) ec.allowed_read.push_back(v.get<std::string>());
            if (ev.contains("blocked_read"))
                for (auto& v : ev["blocked_read"]) ec.blocked_read.push_back(v.get<std::string>());
            // V-SC-006-ext: Polyglot subprocess environment scrubbing
            if (ev.contains("subprocess_scrub_mode"))
                ec.subprocess_scrub_mode = ev["subprocess_scrub_mode"].get<std::string>();
            if (ev.contains("blocked_subprocess_prefixes"))
                for (auto& v : ev["blocked_subprocess_prefixes"]) ec.blocked_subprocess_prefixes.push_back(v.get<std::string>());
            if (ev.contains("blocked_subprocess_vars"))
                for (auto& v : ev["blocked_subprocess_vars"]) ec.blocked_subprocess_vars.push_back(v.get<std::string>());
            if (ev.contains("allowed_subprocess_vars"))
                for (auto& v : ev["allowed_subprocess_vars"]) ec.allowed_subprocess_vars.push_back(v.get<std::string>());
            parseRationale(ev, ec.rationale);
        }
        if (cap.contains("process") && cap["process"].is_object()) {
            auto& pr = cap["process"];
            parseRationale(pr, rules_.capabilities.process.rationale);
        }
    }

    // V3 Limits (expanded nested objects)
    if (j.contains("limits") && j["limits"].is_object()) {
        auto& lim = j["limits"];
        if (lim.contains("timeout") && lim["timeout"].is_object()) {
            auto& t = lim["timeout"];
            if (t.contains("global")) { rules_.limits.timeout.global = t["global"].get<int>(); rules_.timeout_seconds = rules_.limits.timeout.global; }
            if (t.contains("per_block")) rules_.limits.timeout.per_block = t["per_block"].get<int>();
            if (t.contains("total_polyglot")) rules_.limits.timeout.total_polyglot = t["total_polyglot"].get<int>();
        }
        if (lim.contains("memory") && lim["memory"].is_object()) {
            auto& m = lim["memory"];
            if (m.contains("per_block_mb")) rules_.limits.memory.per_block_mb = m["per_block_mb"].get<int>();
            if (m.contains("total_mb")) { rules_.limits.memory.total_mb = m["total_mb"].get<int>(); rules_.memory_limit_mb = rules_.limits.memory.total_mb; }
        }
        if (lim.contains("execution") && lim["execution"].is_object()) {
            auto& e = lim["execution"];
            if (e.contains("call_depth")) { rules_.limits.execution.call_depth = e["call_depth"].get<int>(); rules_.max_call_depth = rules_.limits.execution.call_depth; }
            if (e.contains("loop_iterations")) rules_.limits.execution.loop_iterations = e["loop_iterations"].get<int>();
            if (e.contains("polyglot_blocks")) rules_.limits.execution.polyglot_blocks = e["polyglot_blocks"].get<int>();
            if (e.contains("parallel_blocks")) rules_.limits.execution.parallel_blocks = e["parallel_blocks"].get<int>();
            if (e.contains("total_executions")) rules_.limits.execution.total_executions = e["total_executions"].get<int>();
            // limits.execution.timeout_seconds is the canonical key for overall script timeout.
            // Wire it to runtime.timeout so main.cpp enforcement picks it up.
            if (e.contains("timeout_seconds")) { rules_.runtime.timeout = e["timeout_seconds"].get<int>(); }
        }
        if (lim.contains("data") && lim["data"].is_object()) {
            auto& d = lim["data"];
            if (d.contains("array_size")) { rules_.limits.data.array_size = d["array_size"].get<int>(); rules_.max_array_size = rules_.limits.data.array_size; }
            if (d.contains("dict_size")) rules_.limits.data.dict_size = d["dict_size"].get<int>();
            if (d.contains("string_length")) rules_.limits.data.string_length = d["string_length"].get<int>();
            if (d.contains("nesting_depth")) rules_.limits.data.nesting_depth = d["nesting_depth"].get<int>();
            if (d.contains("max_json_depth")) {
                int jd = d["max_json_depth"].get<int>();
                rules_.limits.data.nesting_depth = jd;  // alias: max_json_depth -> nesting_depth
                naab::limits::setMaxJsonDepth(jd);
            }
            if (d.contains("output_size")) rules_.limits.data.output_size = d["output_size"].get<int>();
        }
        if (lim.contains("code") && lim["code"].is_object()) {
            auto& c = lim["code"];
            if (c.contains("max_lines_per_block")) rules_.limits.code.max_lines_per_block = c["max_lines_per_block"].get<int>();
            if (c.contains("max_total_polyglot_lines")) rules_.limits.code.max_total_polyglot_lines = c["max_total_polyglot_lines"].get<int>();
            if (c.contains("max_nesting_depth")) rules_.limits.code.max_nesting_depth = c["max_nesting_depth"].get<int>();
        }
        if (lim.contains("rate") && lim["rate"].is_object()) {
            auto& r = lim["rate"];
            if (r.contains("max_polyglot_per_second")) rules_.limits.rate.max_polyglot_per_second = r["max_polyglot_per_second"].get<int>();
            if (r.contains("max_stdlib_calls_per_second")) rules_.limits.rate.max_stdlib_calls_per_second = r["max_stdlib_calls_per_second"].get<int>();
            if (r.contains("max_file_ops_per_second")) rules_.limits.rate.max_file_ops_per_second = r["max_file_ops_per_second"].get<int>();
            if (r.contains("cooldown_on_limit_ms")) rules_.limits.rate.cooldown_on_limit_ms = r["cooldown_on_limit_ms"].get<int>();
        }
    }

    // Clamp all limits fields: negative → 0 (0 = unlimited/disabled, matches > 0 guards)
    auto clamp0 = [](int& v) { if (v < 0) v = 0; };
    clamp0(rules_.limits.timeout.global);
    clamp0(rules_.limits.timeout.per_block);
    clamp0(rules_.limits.timeout.total_polyglot);
    clamp0(rules_.limits.memory.per_block_mb);
    clamp0(rules_.limits.memory.total_mb);
    clamp0(rules_.limits.execution.call_depth);
    clamp0(rules_.limits.execution.loop_iterations);
    clamp0(rules_.limits.execution.polyglot_blocks);
    clamp0(rules_.limits.execution.parallel_blocks);
    clamp0(rules_.limits.execution.total_executions);
    clamp0(rules_.limits.data.array_size);
    clamp0(rules_.limits.data.dict_size);
    clamp0(rules_.limits.data.string_length);
    clamp0(rules_.limits.data.nesting_depth);
    clamp0(rules_.limits.data.output_size);
    clamp0(rules_.limits.code.max_lines_per_block);
    clamp0(rules_.limits.code.max_total_polyglot_lines);
    clamp0(rules_.limits.code.max_nesting_depth);
    clamp0(rules_.limits.rate.max_polyglot_per_second);
    clamp0(rules_.limits.rate.max_stdlib_calls_per_second);
    clamp0(rules_.limits.rate.max_file_ops_per_second);
    clamp0(rules_.limits.rate.cooldown_on_limit_ms);
    clamp0(rules_.timeout_seconds);
    clamp0(rules_.max_call_depth);
    clamp0(rules_.max_array_size);
    clamp0(rules_.memory_limit_mb);
    clamp0(rules_.runtime.timeout);
    clamp0(rules_.api.timeout);

    // V3 Requirements (expanded objects)
    if (j.contains("requirements") && j["requirements"].is_object()) {
        auto& req = j["requirements"];
        if (req.contains("main_block") && req["main_block"].is_object()) {
            auto& mb = req["main_block"];
            if (mb.contains("level")) {
                auto [en, lv] = parseEnforcementLevel(mb["level"]);
                rules_.requirements.main_block.enabled = true;
                rules_.requirements.main_block.level = lv;
                rules_.require_main_block = true;
                rules_.main_block_level = lv;
            }
            if (mb.contains("message")) rules_.requirements.main_block.message = mb["message"].get<std::string>();
            parseRationale(mb, rules_.requirements.main_block.rationale);
        }
        if (req.contains("error_handling") && req["error_handling"].is_object()) {
            auto& eh = req["error_handling"];
            if (eh.contains("level")) {
                auto [en, lv] = parseEnforcementLevel(eh["level"]);
                rules_.requirements.error_handling.enabled = true;
                rules_.requirements.error_handling.level = lv;
                rules_.require_error_handling = true;
                rules_.error_handling_level = lv;
            }
            if (eh.contains("require_try_catch")) rules_.requirements.error_handling.require_try_catch = eh["require_try_catch"].get<bool>();
            if (eh.contains("require_catch_body")) rules_.requirements.error_handling.require_catch_body = eh["require_catch_body"].get<bool>();
            parseRationale(eh, rules_.requirements.error_handling.rationale);
        }
        if (req.contains("naming_conventions") && req["naming_conventions"].is_object()) {
            auto& nc = req["naming_conventions"];
            rules_.requirements.naming_conventions.enabled = true;
            if (nc.contains("level")) { auto [en, lv] = parseEnforcementLevel(nc["level"]); rules_.requirements.naming_conventions.level = lv; }
            if (nc.contains("variables")) rules_.requirements.naming_conventions.variables = nc["variables"].get<std::string>();
            if (nc.contains("functions")) rules_.requirements.naming_conventions.functions = nc["functions"].get<std::string>();
            if (nc.contains("check_naab_code")) rules_.requirements.naming_conventions.check_naab_code = nc["check_naab_code"].get<bool>();
            if (nc.contains("check_polyglot_code")) rules_.requirements.naming_conventions.check_polyglot_code = nc["check_polyglot_code"].get<bool>();
        }
    }

    // V3 Restrictions (expanded objects)
    if (j.contains("restrictions") && j["restrictions"].is_object()) {
        auto& res = j["restrictions"];

        if (res.contains("polyglot_output") && res["polyglot_output"].is_object()) {
            auto& po = res["polyglot_output"];
            if (po.contains("format")) { rules_.restrictions.polyglot_output.format = po["format"].get<std::string>(); rules_.polyglot_output = rules_.restrictions.polyglot_output.format; }
            if (po.contains("max_size")) rules_.restrictions.polyglot_output.max_size = po["max_size"].get<int>();
            if (po.contains("validate_json")) rules_.restrictions.polyglot_output.validate_json = po["validate_json"].get<bool>();
        }
        if (res.contains("dangerous_calls") && res["dangerous_calls"].is_object()) {
            auto& dc = res["dangerous_calls"];
            rules_.restrictions.dangerous_calls.enabled = true;
            rules_.restrict_dangerous_calls = true;
            if (dc.contains("level")) { auto [en, lv] = parseEnforcementLevel(dc["level"]); rules_.restrictions.dangerous_calls.level = lv; rules_.dangerous_calls_level = lv; }
            if (dc.contains("allowlist")) for (auto& a : dc["allowlist"]) rules_.restrictions.dangerous_calls.allowlist.push_back(a.get<std::string>());
            if (dc.contains("blocklist_extra")) for (auto& b : dc["blocklist_extra"]) rules_.restrictions.dangerous_calls.blocklist_extra.push_back(b.get<std::string>());
            parseRationale(dc, rules_.restrictions.dangerous_calls.rationale);
        }
        if (res.contains("shell_injection") && res["shell_injection"].is_object()) {
            auto& si = res["shell_injection"];
            rules_.restrictions.shell_injection.enabled = true;
            if (si.contains("level")) { auto [en, lv] = parseEnforcementLevel(si["level"]); rules_.restrictions.shell_injection.level = lv; }
            if (si.contains("patterns")) for (auto& p : si["patterns"]) rules_.restrictions.shell_injection.patterns.push_back(p.get<std::string>());
            parseRationale(si, rules_.restrictions.shell_injection.rationale);
        }
        if (res.contains("privilege_escalation") && res["privilege_escalation"].is_object()) {
            auto& pe = res["privilege_escalation"];
            rules_.restrictions.privilege_escalation.enabled = true;
            if (pe.contains("level")) { auto [en, lv] = parseEnforcementLevel(pe["level"]); rules_.restrictions.privilege_escalation.level = lv; }
            if (pe.contains("block_sudo")) rules_.restrictions.privilege_escalation.block_sudo = pe["block_sudo"].get<bool>();
            if (pe.contains("block_su")) rules_.restrictions.privilege_escalation.block_su = pe["block_su"].get<bool>();
            parseRationale(pe, rules_.restrictions.privilege_escalation.rationale);
        }
        if (res.contains("code_injection") && res["code_injection"].is_object()) {
            auto& ci = res["code_injection"];
            rules_.restrictions.code_injection.enabled = true;
            if (ci.contains("level")) { auto [en, lv] = parseEnforcementLevel(ci["level"]); rules_.restrictions.code_injection.level = lv; }
            if (ci.contains("block_dynamic_code_gen")) rules_.restrictions.code_injection.block_dynamic_code_gen = ci["block_dynamic_code_gen"].get<bool>();
            if (ci.contains("block_sql_injection_patterns")) rules_.restrictions.code_injection.block_sql_injection_patterns = ci["block_sql_injection_patterns"].get<bool>();
            parseRationale(ci, rules_.restrictions.code_injection.rationale);
        }
        if (res.contains("crypto") && res["crypto"].is_object()) {
            auto& cr = res["crypto"];
            rules_.restrictions.crypto.enabled = true;
            if (cr.contains("level")) { auto [en, lv] = parseEnforcementLevel(cr["level"]); rules_.restrictions.crypto.level = lv; }
            if (cr.contains("weak_hashes")) for (auto& h : cr["weak_hashes"]) rules_.restrictions.crypto.weak_hashes.push_back(h.get<std::string>());
            if (cr.contains("weak_ciphers")) for (auto& c : cr["weak_ciphers"]) rules_.restrictions.crypto.weak_ciphers.push_back(c.get<std::string>());
            parseRationale(cr, rules_.restrictions.crypto.rationale);
        }
        if (res.contains("vcs_secret_extraction") && res["vcs_secret_extraction"].is_object()) {
            auto& vs = res["vcs_secret_extraction"];
            if (vs.contains("enabled")) rules_.restrictions.vcs_secret_extraction.enabled = vs["enabled"].get<bool>();
            if (vs.contains("level")) { auto [en, lv] = parseEnforcementLevel(vs["level"]); rules_.restrictions.vcs_secret_extraction.level = lv; }
            parseRationale(vs, rules_.restrictions.vcs_secret_extraction.rationale);
        }
        if (res.contains("obfuscation") && res["obfuscation"].is_object()) {
            auto& ob = res["obfuscation"];
            if (ob.contains("enabled")) rules_.restrictions.obfuscation.enabled = ob["enabled"].get<bool>();
            if (ob.contains("level")) { auto [en, lv] = parseEnforcementLevel(ob["level"]); rules_.restrictions.obfuscation.level = lv; }
            parseRationale(ob, rules_.restrictions.obfuscation.rationale);
        }
        if (res.contains("imports") && res["imports"].is_object()) {
            auto& im = res["imports"];
            rules_.restrictions.imports.enabled = true;
            if (im.contains("level")) { auto [en, lv] = parseEnforcementLevel(im["level"]); rules_.restrictions.imports.level = lv; }
            if (im.contains("mode")) rules_.restrictions.imports.mode = im["mode"].get<std::string>();
            if (im.contains("blocked") && im["blocked"].is_object())
                for (auto& [lang, arr] : im["blocked"].items())
                    for (auto& v : arr) rules_.restrictions.imports.blocked[lang].push_back(v.get<std::string>());
            if (im.contains("allowed") && im["allowed"].is_object())
                for (auto& [lang, arr] : im["allowed"].items())
                    for (auto& v : arr) rules_.restrictions.imports.allowed[lang].push_back(v.get<std::string>());
            parseRationale(im, rules_.restrictions.imports.rationale);
        }
        if (res.contains("data_exfiltration") && res["data_exfiltration"].is_object()) {
            auto& de = res["data_exfiltration"];
            if (de.contains("enabled")) rules_.restrictions.data_exfiltration.enabled = de["enabled"].get<bool>();
            if (de.contains("level")) { auto [en, lv] = parseEnforcementLevel(de["level"]); rules_.restrictions.data_exfiltration.level = lv; }
            if (de.contains("block_base64_encode_secrets")) rules_.restrictions.data_exfiltration.block_base64_encode_secrets = de["block_base64_encode_secrets"].get<bool>();
            if (de.contains("block_hex_encode_secrets")) rules_.restrictions.data_exfiltration.block_hex_encode_secrets = de["block_hex_encode_secrets"].get<bool>();
            if (de.contains("block_network_exfil")) rules_.restrictions.data_exfiltration.block_network_exfil = de["block_network_exfil"].get<bool>();
            if (de.contains("block_socket_exfil")) rules_.restrictions.data_exfiltration.block_socket_exfil = de["block_socket_exfil"].get<bool>();
            if (de.contains("block_encoding_chains")) rules_.restrictions.data_exfiltration.block_encoding_chains = de["block_encoding_chains"].get<bool>();
            if (de.contains("patterns") && de["patterns"].is_array()) {
                for (const auto& p : de["patterns"]) {
                    if (p.is_string()) rules_.restrictions.data_exfiltration.patterns.push_back(p.get<std::string>());
                }
            }
            parseRationale(de, rules_.restrictions.data_exfiltration.rationale);
        }
        if (res.contains("resource_abuse") && res["resource_abuse"].is_object()) {
            auto& ra = res["resource_abuse"];
            if (ra.contains("enabled")) rules_.restrictions.resource_abuse.enabled = ra["enabled"].get<bool>();
            if (ra.contains("level")) { auto [en, lv] = parseEnforcementLevel(ra["level"]); rules_.restrictions.resource_abuse.level = lv; }
            if (ra.contains("block_fork_bomb")) rules_.restrictions.resource_abuse.block_fork_bomb = ra["block_fork_bomb"].get<bool>();
            if (ra.contains("block_disk_filling")) rules_.restrictions.resource_abuse.block_disk_filling = ra["block_disk_filling"].get<bool>();
            parseRationale(ra, rules_.restrictions.resource_abuse.rationale);
        }
        if (res.contains("information_disclosure") && res["information_disclosure"].is_object()) {
            auto& id = res["information_disclosure"];
            if (id.contains("enabled")) rules_.restrictions.information_disclosure.enabled = id["enabled"].get<bool>();
            if (id.contains("level")) { auto [en, lv] = parseEnforcementLevel(id["level"]); rules_.restrictions.information_disclosure.level = lv; }
            if (id.contains("block_env_dump")) rules_.restrictions.information_disclosure.block_env_dump = id["block_env_dump"].get<bool>();
            if (id.contains("block_process_listing")) rules_.restrictions.information_disclosure.block_process_listing = id["block_process_listing"].get<bool>();
            if (id.contains("block_system_info_leak")) rules_.restrictions.information_disclosure.block_system_info_leak = id["block_system_info_leak"].get<bool>();
            parseRationale(id, rules_.restrictions.information_disclosure.rationale);
        }
    }

    // V3 Code Quality (expanded objects with per-check configs)
    if (j.contains("code_quality") && j["code_quality"].is_object()) {
        auto& cq = j["code_quality"];

        // no_secrets (expanded)
        if (cq.contains("no_secrets") && cq["no_secrets"].is_object()) {
            auto& ns = cq["no_secrets"];
            rules_.code_quality.no_secrets.enabled = true;
            rules_.no_secrets = true;
            if (ns.contains("level")) { auto [en, lv] = parseEnforcementLevel(ns["level"]); rules_.code_quality.no_secrets.level = lv; rules_.no_secrets_level = lv; }
            if (ns.contains("allowlist")) for (auto& a : ns["allowlist"]) rules_.code_quality.no_secrets.allowlist.push_back(a.get<std::string>());
            if (ns.contains("entropy_check") && ns["entropy_check"].is_object()) {
                auto& ec = ns["entropy_check"];
                rules_.code_quality.no_secrets.entropy_check.enabled = true;
                if (ec.contains("threshold")) rules_.code_quality.no_secrets.entropy_check.threshold = ec["threshold"].get<double>();
                if (ec.contains("min_length")) rules_.code_quality.no_secrets.entropy_check.min_length = ec["min_length"].get<int>();
            }
            if (ns.contains("suspicious_variable_names") && ns["suspicious_variable_names"].is_object()) {
                auto& sv = ns["suspicious_variable_names"];
                if (sv.contains("enabled")) rules_.code_quality.no_secrets.suspicious_variable_names.enabled = sv["enabled"].get<bool>();
                if (sv.contains("names")) for (auto& n : sv["names"]) rules_.code_quality.no_secrets.suspicious_variable_names.names.push_back(n.get<std::string>());
            }
            parseRationale(ns, rules_.code_quality.no_secrets.rationale);
        }

        // no_placeholders (expanded)
        if (cq.contains("no_placeholders") && cq["no_placeholders"].is_object()) {
            auto& np = cq["no_placeholders"];
            rules_.code_quality.no_placeholders.enabled = true;
            rules_.no_placeholders = true;
            if (np.contains("level")) { auto [en, lv] = parseEnforcementLevel(np["level"]); rules_.code_quality.no_placeholders.level = lv; rules_.no_placeholders_level = lv; }
            if (np.contains("markers")) { rules_.code_quality.no_placeholders.markers.clear(); for (auto& m : np["markers"]) rules_.code_quality.no_placeholders.markers.push_back(m.get<std::string>()); }
            if (np.contains("custom_markers")) for (auto& m : np["custom_markers"]) rules_.code_quality.no_placeholders.custom_markers.push_back(m.get<std::string>());
            if (np.contains("case_sensitive")) rules_.code_quality.no_placeholders.case_sensitive = np["case_sensitive"].get<bool>();
            parseRationale(np, rules_.code_quality.no_placeholders.rationale);
        }

        // New code quality checks
        auto loadSimpleCheck = [&](const std::string& key, auto& config) {
            if (cq.contains(key)) {
                if (cq[key].is_boolean() || cq[key].is_string()) {
                    auto [en, lv] = parseEnforcementLevel(cq[key]);
                    config.enabled = en;
                    config.level = lv;
                } else if (cq[key].is_object()) {
                    config.enabled = true;
                    auto& obj = cq[key];
                    if (obj.contains("level")) { auto [en, lv] = parseEnforcementLevel(obj["level"]); config.level = lv; }
                    if (obj.contains("patterns"))
                        for (auto& p : obj["patterns"]) config.patterns.push_back(p.get<std::string>());
                    if (obj.contains("custom_patterns"))
                        for (auto& p : obj["custom_patterns"]) config.patterns.push_back(p.get<std::string>());
                    parseRationale(obj, config.rationale);
                }
            }
        };

        loadSimpleCheck("no_temporary_code", rules_.code_quality.no_temporary_code);
        loadSimpleCheck("no_simulation_markers", rules_.code_quality.no_simulation_markers);
        loadSimpleCheck("no_dead_code", rules_.code_quality.no_dead_code);
        loadSimpleCheck("no_debug_artifacts", rules_.code_quality.no_debug_artifacts);
        loadSimpleCheck("no_unsafe_deserialization", rules_.code_quality.no_unsafe_deserialization);
        loadSimpleCheck("no_sql_injection", rules_.code_quality.no_sql_injection);
        loadSimpleCheck("no_path_traversal", rules_.code_quality.no_path_traversal);
        loadSimpleCheck("no_hardcoded_urls", rules_.code_quality.no_hardcoded_urls);
        loadSimpleCheck("no_hardcoded_ips", rules_.code_quality.no_hardcoded_ips);

        // semantic_checks
        if (cq.contains("semantic_checks")) {
            if (cq["semantic_checks"].is_boolean() || cq["semantic_checks"].is_string()) {
                auto [en, lv] = parseEnforcementLevel(cq["semantic_checks"]);
                rules_.code_quality.semantic_checks.enabled = en;
                rules_.code_quality.semantic_checks.level = lv;
            } else if (cq["semantic_checks"].is_object()) {
                auto& sc = cq["semantic_checks"];
                rules_.code_quality.semantic_checks.enabled = true;
                if (sc.contains("level")) { auto [en, lv] = parseEnforcementLevel(sc["level"]); rules_.code_quality.semantic_checks.level = lv; }
                if (sc.contains("check_imports")) rules_.code_quality.semantic_checks.check_imports = sc["check_imports"].get<bool>();
                if (sc.contains("check_api_signatures")) rules_.code_quality.semantic_checks.check_api_signatures = sc["check_api_signatures"].get<bool>();
                if (sc.contains("check_shell_syntax")) rules_.code_quality.semantic_checks.check_shell_syntax = sc["check_shell_syntax"].get<bool>();
                if (sc.contains("check_dangerous_eval")) rules_.code_quality.semantic_checks.check_dangerous_eval = sc["check_dangerous_eval"].get<bool>();
                parseRationale(sc, rules_.code_quality.semantic_checks.rationale);
            }
        }

        // intent_validation
        if (cq.contains("intent_validation") && cq["intent_validation"].is_object()) {
            auto& iv = cq["intent_validation"];
            rules_.code_quality.intent_validation.enabled = true;
            if (iv.contains("enabled")) rules_.code_quality.intent_validation.enabled = iv["enabled"].get<bool>();
            if (iv.contains("required")) rules_.code_quality.intent_validation.required = iv["required"].get<bool>();
            if (iv.contains("level")) { auto [en, lv] = parseEnforcementLevel(iv["level"]); rules_.code_quality.intent_validation.level = lv; }
            if (iv.contains("missing_level")) { auto [en, lv] = parseEnforcementLevel(iv["missing_level"]); rules_.code_quality.intent_validation.missing_level = lv; }
            if (iv.contains("mode")) rules_.code_quality.intent_validation.mode = iv["mode"].get<std::string>();
            if (iv.contains("min_function_lines")) rules_.code_quality.intent_validation.min_function_lines = iv["min_function_lines"].get<int>();
            if (iv.contains("exempt_functions")) {
                for (auto& f : iv["exempt_functions"])
                    rules_.code_quality.intent_validation.exempt_functions.push_back(f.get<std::string>());
            }
            if (iv.contains("project_intent"))
                rules_.code_quality.intent_validation.project_intent = iv["project_intent"].get<std::string>();
            if (iv.contains("function_intents") && iv["function_intents"].is_object()) {
                for (auto& [name, intent] : iv["function_intents"].items())
                    rules_.code_quality.intent_validation.function_intents[name] = intent.get<std::string>();
            }
            parseRationale(iv, rules_.code_quality.intent_validation.rationale);
        }

        // no_pii
        if (cq.contains("no_pii")) {
            if (cq["no_pii"].is_boolean() || cq["no_pii"].is_string()) {
                auto [en, lv] = parseEnforcementLevel(cq["no_pii"]);
                rules_.code_quality.no_pii.enabled = en;
                rules_.code_quality.no_pii.level = lv;
            } else if (cq["no_pii"].is_object()) {
                auto& pii = cq["no_pii"];
                rules_.code_quality.no_pii.enabled = true;
                if (pii.contains("level")) { auto [en, lv] = parseEnforcementLevel(pii["level"]); rules_.code_quality.no_pii.level = lv; }
                if (pii.contains("detect_ssn")) rules_.code_quality.no_pii.detect_ssn = pii["detect_ssn"].get<bool>();
                if (pii.contains("detect_credit_card")) rules_.code_quality.no_pii.detect_credit_card = pii["detect_credit_card"].get<bool>();
                if (pii.contains("detect_email")) rules_.code_quality.no_pii.detect_email = pii["detect_email"].get<bool>();
                if (pii.contains("detect_phone")) rules_.code_quality.no_pii.detect_phone = pii["detect_phone"].get<bool>();
                if (pii.contains("detect_ip_address")) rules_.code_quality.no_pii.detect_ip_address = pii["detect_ip_address"].get<bool>();
                if (pii.contains("mask_in_errors")) rules_.code_quality.no_pii.mask_in_errors = pii["mask_in_errors"].get<bool>();
                if (pii.contains("allowlist_patterns")) for (auto& a : pii["allowlist_patterns"]) rules_.code_quality.no_pii.allowlist_patterns.push_back(a.get<std::string>());
                parseRationale(pii, rules_.code_quality.no_pii.rationale);
            }
        }

        // no_mock_data
        if (cq.contains("no_mock_data") && cq["no_mock_data"].is_object()) {
            auto& md = cq["no_mock_data"];
            rules_.code_quality.no_mock_data.enabled = true;
            if (md.contains("level")) { auto [en, lv] = parseEnforcementLevel(md["level"]); rules_.code_quality.no_mock_data.level = lv; }
            if (md.contains("variable_prefixes")) for (auto& p : md["variable_prefixes"]) rules_.code_quality.no_mock_data.variable_prefixes.push_back(p.get<std::string>());
            if (md.contains("function_prefixes")) for (auto& p : md["function_prefixes"]) rules_.code_quality.no_mock_data.function_prefixes.push_back(p.get<std::string>());
            if (md.contains("literal_patterns")) for (auto& p : md["literal_patterns"]) rules_.code_quality.no_mock_data.literal_patterns.push_back(p.get<std::string>());
            if (md.contains("ignore_in_test_context")) rules_.code_quality.no_mock_data.ignore_in_test_context = md["ignore_in_test_context"].get<bool>();
            parseRationale(md, rules_.code_quality.no_mock_data.rationale);
        } else if (cq.contains("no_mock_data")) {
            auto [en, lv] = parseEnforcementLevel(cq["no_mock_data"]);
            rules_.code_quality.no_mock_data.enabled = en;
            rules_.code_quality.no_mock_data.level = lv;
        }

        // no_apologetic_language
        if (cq.contains("no_apologetic_language")) {
            if (cq["no_apologetic_language"].is_boolean() || cq["no_apologetic_language"].is_string()) {
                auto [en, lv] = parseEnforcementLevel(cq["no_apologetic_language"]);
                rules_.code_quality.no_apologetic_language.enabled = en;
                rules_.code_quality.no_apologetic_language.level = lv;
            } else if (cq["no_apologetic_language"].is_object()) {
                auto& al = cq["no_apologetic_language"];
                rules_.code_quality.no_apologetic_language.enabled = true;
                if (al.contains("level")) { auto [en, lv] = parseEnforcementLevel(al["level"]); rules_.code_quality.no_apologetic_language.level = lv; }
                if (al.contains("scan_comments_only")) rules_.code_quality.no_apologetic_language.scan_comments_only = al["scan_comments_only"].get<bool>();
                if (al.contains("scan_strings")) rules_.code_quality.no_apologetic_language.scan_strings = al["scan_strings"].get<bool>();
                parseRationale(al, rules_.code_quality.no_apologetic_language.rationale);
            }
        }

        // max_complexity
        if (cq.contains("max_complexity") && cq["max_complexity"].is_object()) {
            auto& mc = cq["max_complexity"];
            rules_.code_quality.max_complexity.enabled = true;
            if (mc.contains("level")) { auto [en, lv] = parseEnforcementLevel(mc["level"]); rules_.code_quality.max_complexity.level = lv; }
            if (mc.contains("max_lines_per_block")) rules_.code_quality.max_complexity.max_lines_per_block = mc["max_lines_per_block"].get<int>();
            if (mc.contains("max_nesting_depth")) rules_.code_quality.max_complexity.max_nesting_depth = mc["max_nesting_depth"].get<int>();
            if (mc.contains("max_parameters")) rules_.code_quality.max_complexity.max_parameters = mc["max_parameters"].get<int>();
            parseRationale(mc, rules_.code_quality.max_complexity.rationale);
        }

        // encoding
        if (cq.contains("encoding") && cq["encoding"].is_object()) {
            auto& enc = cq["encoding"];
            rules_.code_quality.encoding.enabled = true;
            if (enc.contains("level")) { auto [en, lv] = parseEnforcementLevel(enc["level"]); rules_.code_quality.encoding.level = lv; }
            if (enc.contains("block_null_bytes")) rules_.code_quality.encoding.block_null_bytes = enc["block_null_bytes"].get<bool>();
            if (enc.contains("block_unicode_bidi")) rules_.code_quality.encoding.block_unicode_bidi = enc["block_unicode_bidi"].get<bool>();
            parseRationale(enc, rules_.code_quality.encoding.rationale);
        }

        // no_hardcoded_results (expanded)
        if (cq.contains("no_hardcoded_results") && cq["no_hardcoded_results"].is_object()) {
            auto& hr = cq["no_hardcoded_results"];
            rules_.code_quality.no_hardcoded_results.enabled = true;
            rules_.no_hardcoded_results = true;
            if (hr.contains("level")) { auto [en, lv] = parseEnforcementLevel(hr["level"]); rules_.code_quality.no_hardcoded_results.level = lv; rules_.no_hardcoded_results_level = lv; }
            if (hr.contains("check_return_true_false")) rules_.code_quality.no_hardcoded_results.check_return_true_false = hr["check_return_true_false"].get<bool>();
            if (hr.contains("check_dict_success_fields")) rules_.code_quality.no_hardcoded_results.check_dict_success_fields = hr["check_dict_success_fields"].get<bool>();
            parseRationale(hr, rules_.code_quality.no_hardcoded_results.rationale);
        }

        // no_oversimplification
        if (cq.contains("no_oversimplification")) {
            auto& val = cq["no_oversimplification"];
            auto& os = rules_.code_quality.no_oversimplification;
            if (val.is_string()) {
                auto [en, lv] = parseEnforcementLevel(val);
                os.enabled = en; os.level = lv;
            } else if (val.is_object()) {
                os.enabled = true;
                if (val.contains("level")) { auto [en, lv] = parseEnforcementLevel(val["level"]); os.level = lv; }
                if (val.contains("enabled")) os.enabled = val["enabled"].get<bool>();
                if (val.contains("check_empty_bodies")) os.check_empty_bodies = val["check_empty_bodies"].get<bool>();
                if (val.contains("check_trivial_returns")) os.check_trivial_returns = val["check_trivial_returns"].get<bool>();
                if (val.contains("check_identity_functions")) os.check_identity_functions = val["check_identity_functions"].get<bool>();
                if (val.contains("check_not_implemented")) os.check_not_implemented = val["check_not_implemented"].get<bool>();
                if (val.contains("check_comment_only_bodies")) os.check_comment_only_bodies = val["check_comment_only_bodies"].get<bool>();
                if (val.contains("check_fabricated_results")) os.check_fabricated_results = val["check_fabricated_results"].get<bool>();
                if (val.contains("case_sensitive")) os.case_sensitive = val["case_sensitive"].get<bool>();
                if (val.contains("min_function_lines")) os.min_function_lines = val["min_function_lines"].get<int>();
                if (val.contains("custom_patterns")) {
                    for (auto& p : val["custom_patterns"]) os.custom_patterns.push_back(p.get<std::string>());
                }
                parseRationale(val, os.rationale);
            }
        }

        // no_incomplete_logic
        if (cq.contains("no_incomplete_logic")) {
            auto& val = cq["no_incomplete_logic"];
            auto& il = rules_.code_quality.no_incomplete_logic;
            if (val.is_string()) {
                auto [en, lv] = parseEnforcementLevel(val);
                il.enabled = en; il.level = lv;
            } else if (val.is_object()) {
                il.enabled = true;
                if (val.contains("level")) { auto [en, lv] = parseEnforcementLevel(val["level"]); il.level = lv; }
                if (val.contains("enabled")) il.enabled = val["enabled"].get<bool>();
                if (val.contains("check_empty_catch")) il.check_empty_catch = val["check_empty_catch"].get<bool>();
                if (val.contains("check_swallowed_exceptions")) il.check_swallowed_exceptions = val["check_swallowed_exceptions"].get<bool>();
                if (val.contains("check_generic_errors")) il.check_generic_errors = val["check_generic_errors"].get<bool>();
                if (val.contains("check_vague_error_messages")) il.check_vague_error_messages = val["check_vague_error_messages"].get<bool>();
                if (val.contains("check_single_iteration_loops")) il.check_single_iteration_loops = val["check_single_iteration_loops"].get<bool>();
                if (val.contains("check_bare_raise")) il.check_bare_raise = val["check_bare_raise"].get<bool>();
                if (val.contains("check_always_true_false")) il.check_always_true_false = val["check_always_true_false"].get<bool>();
                if (val.contains("check_missing_validation")) il.check_missing_validation = val["check_missing_validation"].get<bool>();
                if (val.contains("case_sensitive")) il.case_sensitive = val["case_sensitive"].get<bool>();
                if (val.contains("custom_patterns")) {
                    for (auto& p : val["custom_patterns"]) il.custom_patterns.push_back(p.get<std::string>());
                }
                if (val.contains("suppressions") && val["suppressions"].is_array()) {
                    for (auto& s : val["suppressions"]) il.suppressions.push_back(s.get<std::string>());
                }
                parseRationale(val, il.rationale);
            }
        }

        // no_hallucinated_apis
        if (cq.contains("no_hallucinated_apis")) {
            auto& val = cq["no_hallucinated_apis"];
            auto& ha = rules_.code_quality.no_hallucinated_apis;
            if (val.is_string()) {
                auto [en, lv] = parseEnforcementLevel(val);
                ha.enabled = en; ha.level = lv;
            } else if (val.is_object()) {
                ha.enabled = true;
                if (val.contains("level")) { auto [en, lv] = parseEnforcementLevel(val["level"]); ha.level = lv; }
                if (val.contains("enabled")) ha.enabled = val["enabled"].get<bool>();
                if (val.contains("check_cross_language")) ha.check_cross_language = val["check_cross_language"].get<bool>();
                if (val.contains("check_made_up_functions")) ha.check_made_up_functions = val["check_made_up_functions"].get<bool>();
                if (val.contains("check_wrong_syntax")) ha.check_wrong_syntax = val["check_wrong_syntax"].get<bool>();
                if (val.contains("case_sensitive")) ha.case_sensitive = val["case_sensitive"].get<bool>();
                auto loadPatterns = [](const nlohmann::json& obj, const std::string& key, std::vector<std::string>& out) {
                    if (obj.contains(key)) for (auto& p : obj[key]) out.push_back(p.get<std::string>());
                };
                loadPatterns(val, "python_patterns", ha.python_patterns);
                loadPatterns(val, "javascript_patterns", ha.javascript_patterns);
                loadPatterns(val, "shell_patterns", ha.shell_patterns);
                loadPatterns(val, "go_patterns", ha.go_patterns);
                loadPatterns(val, "ruby_patterns", ha.ruby_patterns);
                loadPatterns(val, "cross_language_patterns", ha.cross_language_patterns);
                loadPatterns(val, "custom_patterns", ha.custom_patterns);
                parseRationale(val, ha.rationale);
            }
        }

        // complexity_floor
        if (cq.contains("complexity_floor")) {
            auto& val = cq["complexity_floor"];
            auto& cf = rules_.code_quality.complexity_floor;
            cf.enabled = true;  // Presence of section enables it
            if (val.is_string()) {
                auto [en, lv] = parseEnforcementLevel(val);
                if (en) cf.level = lv;
            } else if (val.is_object()) {
                if (val.contains("level")) { auto [en, lv] = parseEnforcementLevel(val["level"]); cf.level = lv; }
                if (val.contains("min_score")) cf.min_score = val["min_score"].get<int>();
                if (val.contains("check_polyglot")) cf.check_polyglot = val["check_polyglot"].get<bool>();
                if (val.contains("check_naab")) cf.check_naab = val["check_naab"].get<bool>();
                if (val.contains("skip_if_has_polyglot_block")) cf.skip_if_has_polyglot_block = val["skip_if_has_polyglot_block"].get<bool>();
                if (val.contains("min_lines_for_check")) cf.min_lines_for_check = val["min_lines_for_check"].get<int>();
                if (val.contains("rules") && val["rules"].is_array()) {
                    for (auto& rule_json : val["rules"]) {
                        ComplexityFloorRule rule;
                        if (rule_json.contains("names") && rule_json["names"].is_array()) {
                            for (auto& n : rule_json["names"]) rule.names.push_back(n.get<std::string>());
                        }
                        if (rule_json.contains("min_score")) rule.min_score = rule_json["min_score"].get<int>();
                        if (rule_json.contains("require_branching_or_loops")) rule.require_branching_or_loops = rule_json["require_branching_or_loops"].get<bool>();
                        if (rule_json.contains("message")) rule.message = rule_json["message"].get<std::string>();
                        cf.rules.push_back(std::move(rule));
                    }
                }
                // Bridge: convert target_prefixes to a rules entry if no explicit rules defined
                if (val.contains("target_prefixes") && val["target_prefixes"].is_array() && cf.rules.empty()) {
                    ComplexityFloorRule prefix_rule;
                    for (auto& p : val["target_prefixes"]) {
                        prefix_rule.names.push_back(p.get<std::string>());
                    }
                    prefix_rule.min_score = cf.min_score;
                    prefix_rule.require_branching_or_loops = false;
                    cf.rules.push_back(std::move(prefix_rule));
                }
                parseRationale(val, cf.rationale);
            }
        }

        // Duplicate calls config
        if (cq.contains("duplicate_calls") && cq["duplicate_calls"].is_object()) {
            auto& dc = cq["duplicate_calls"];
            if (dc.contains("enabled")) rules_.code_quality.duplicate_calls.enabled = dc["enabled"].get<bool>();
            if (dc.contains("threshold")) rules_.code_quality.duplicate_calls.threshold = dc["threshold"].get<int>();
            if (dc.contains("max_entries")) rules_.code_quality.duplicate_calls.max_entries = dc["max_entries"].get<int>();
            parseRationale(dc, rules_.code_quality.duplicate_calls.rationale);
        }

        // Polyglot try/catch config
        if (cq.contains("polyglot_try_catch") && cq["polyglot_try_catch"].is_object()) {
            auto& ptc = cq["polyglot_try_catch"];
            if (ptc.contains("enabled")) rules_.code_quality.polyglot_try_catch.enabled = ptc["enabled"].get<bool>();
            if (ptc.contains("max_entries")) rules_.code_quality.polyglot_try_catch.max_entries = ptc["max_entries"].get<int>();
        }

        // Drift detection: structural regression gate
        if (cq.contains("drift_detection") && cq["drift_detection"].is_object()) {
            auto& dd = cq["drift_detection"];
            rules_.code_quality.drift_detection.enabled = dd.value("enabled", false);
            if (dd.contains("level")) {
                auto [en, lv] = parseEnforcementLevel(dd["level"]);
                rules_.code_quality.drift_detection.level = lv;
            }
            if (dd.contains("baseline_path")) rules_.code_quality.drift_detection.baseline_path = dd["baseline_path"].get<std::string>();
            if (dd.contains("max_function_loss")) rules_.code_quality.drift_detection.max_function_loss = dd["max_function_loss"].get<double>();
            if (dd.contains("max_loc_loss")) rules_.code_quality.drift_detection.max_loc_loss = dd["max_loc_loss"].get<double>();
            if (dd.contains("max_export_loss")) rules_.code_quality.drift_detection.max_export_loss = dd["max_export_loss"].get<double>();
            if (dd.contains("max_struct_loss")) rules_.code_quality.drift_detection.max_struct_loss = dd["max_struct_loss"].get<double>();
            if (dd.contains("auto_save")) rules_.code_quality.drift_detection.auto_save = dd["auto_save"].get<bool>();
            // Gate 1: Signature stability
            if (dd.contains("check_signatures")) rules_.code_quality.drift_detection.check_signatures = dd["check_signatures"].get<bool>();
            if (dd.contains("max_param_loss")) rules_.code_quality.drift_detection.max_param_loss = dd["max_param_loss"].get<double>();
            // Gate 2: Import regression
            if (dd.contains("check_imports")) rules_.code_quality.drift_detection.check_imports = dd["check_imports"].get<bool>();
            if (dd.contains("max_import_loss")) rules_.code_quality.drift_detection.max_import_loss = dd["max_import_loss"].get<double>();
            // Gate 3: Complexity regression
            if (dd.contains("check_complexity")) rules_.code_quality.drift_detection.check_complexity = dd["check_complexity"].get<bool>();
            if (dd.contains("max_complexity_loss")) rules_.code_quality.drift_detection.max_complexity_loss = dd["max_complexity_loss"].get<double>();
            if (dd.contains("min_complexity_baseline")) rules_.code_quality.drift_detection.min_complexity_baseline = dd["min_complexity_baseline"].get<int>();
            // Gate 4: Comment inflation
            if (dd.contains("check_comment_ratio")) rules_.code_quality.drift_detection.check_comment_ratio = dd["check_comment_ratio"].get<bool>();
            if (dd.contains("max_comment_ratio")) rules_.code_quality.drift_detection.max_comment_ratio = dd["max_comment_ratio"].get<double>();
            if (dd.contains("max_comment_only_ratio")) rules_.code_quality.drift_detection.max_comment_only_ratio = dd["max_comment_only_ratio"].get<double>();
            // Gate 5: Dead export
            if (dd.contains("check_hollow_exports")) rules_.code_quality.drift_detection.check_hollow_exports = dd["check_hollow_exports"].get<bool>();
            if (dd.contains("min_hollow_export_complexity")) rules_.code_quality.drift_detection.min_hollow_export_complexity = dd["min_hollow_export_complexity"].get<int>();
            // Gate 6: Polyglot regression
            if (dd.contains("check_polyglot")) rules_.code_quality.drift_detection.check_polyglot = dd["check_polyglot"].get<bool>();
            if (dd.contains("max_polyglot_loss")) rules_.code_quality.drift_detection.max_polyglot_loss = dd["max_polyglot_loss"].get<double>();
            // Gate 7: Struct field stability
            if (dd.contains("check_struct_fields")) rules_.code_quality.drift_detection.check_struct_fields = dd["check_struct_fields"].get<bool>();
            if (dd.contains("max_field_loss")) rules_.code_quality.drift_detection.max_field_loss = dd["max_field_loss"].get<double>();
            // Gate 8: Test function regression
            if (dd.contains("check_test_functions")) rules_.code_quality.drift_detection.check_test_functions = dd["check_test_functions"].get<bool>();
            if (dd.contains("max_test_loss")) rules_.code_quality.drift_detection.max_test_loss = dd["max_test_loss"].get<double>();
            // Gate 9: Function name stability
            if (dd.contains("check_function_names")) rules_.code_quality.drift_detection.check_function_names = dd["check_function_names"].get<bool>();
            if (dd.contains("max_function_name_loss")) rules_.code_quality.drift_detection.max_function_name_loss = dd["max_function_name_loss"].get<double>();
            // Gate 10: Baseline tamper protection
            if (dd.contains("require_baseline")) rules_.code_quality.drift_detection.require_baseline = dd["require_baseline"].get<bool>();
            // Gate 11: Function body hash
            if (dd.contains("check_body_hash")) rules_.code_quality.drift_detection.check_body_hash = dd["check_body_hash"].get<bool>();
            // Gate 12: Parameter utilization
            if (dd.contains("check_param_utilization")) rules_.code_quality.drift_detection.check_param_utilization = dd["check_param_utilization"].get<bool>();
            if (dd.contains("min_param_utilization")) rules_.code_quality.drift_detection.min_param_utilization = dd["min_param_utilization"].get<double>();
            // Gate 13: Config presence
            if (dd.contains("check_config_presence")) rules_.code_quality.drift_detection.check_config_presence = dd["check_config_presence"].get<bool>();
            // Gate 14: Script location
            if (dd.contains("check_script_location")) rules_.code_quality.drift_detection.check_script_location = dd["check_script_location"].get<bool>();
            // Gate 16: Signature presence
            if (dd.contains("check_signature_presence")) rules_.code_quality.drift_detection.check_signature_presence = dd["check_signature_presence"].get<bool>();
            // Gate 17: Polyglot content regression
            if (dd.contains("check_polyglot_content")) rules_.code_quality.drift_detection.check_polyglot_content = dd["check_polyglot_content"].get<bool>();
            if (dd.contains("max_polyglot_shrink")) rules_.code_quality.drift_detection.max_polyglot_shrink = dd["max_polyglot_shrink"].get<double>();
            // Gate 0 extension: Function gain detection
            if (dd.contains("max_function_gain")) rules_.code_quality.drift_detection.max_function_gain = dd["max_function_gain"].get<double>();
            // Gate 18: New function detection
            if (dd.contains("check_new_functions")) rules_.code_quality.drift_detection.check_new_functions = dd["check_new_functions"].get<bool>();
            parseRationale(dd, rules_.code_quality.drift_detection.rationale);
        }
    }

    // Integrity config
    if (j.contains("integrity") && j["integrity"].is_object()) {
        auto& ic = j["integrity"];
        // V-SC-008: require_signature removed — NAAB_GOVERN_KEY presence is the sole authority.
        // The field is silently ignored if present in govern.json for backward compatibility.
        if (ic.contains("blocked_flags") && ic["blocked_flags"].is_array()) {
            for (auto& f : ic["blocked_flags"]) {
                rules_.integrity.blocked_flags.push_back(f.get<std::string>());
            }
        }
    }

    // V3 Custom Rules
    if (j.contains("custom_rules") && j["custom_rules"].is_array()) {
        for (auto& cr : j["custom_rules"]) {
            CustomRule rule;
            if (cr.contains("id")) rule.id = cr["id"].get<std::string>();
            if (cr.contains("name")) rule.name = cr["name"].get<std::string>();
            if (cr.contains("description")) rule.description = cr["description"].get<std::string>();
            if (cr.contains("pattern")) rule.pattern = cr["pattern"].get<std::string>();
            if (cr.contains("languages")) for (auto& l : cr["languages"]) rule.languages.push_back(l.get<std::string>());
            if (cr.contains("level")) { auto [en, lv] = parseEnforcementLevel(cr["level"]); rule.level = lv; }
            if (cr.contains("message")) rule.message = cr["message"].get<std::string>();
            if (cr.contains("help")) rule.help = cr["help"].get<std::string>();
            if (cr.contains("good_example")) rule.good_example = cr["good_example"].get<std::string>();
            if (cr.contains("bad_example")) rule.bad_example = cr["bad_example"].get<std::string>();
            if (cr.contains("enabled")) rule.enabled = cr["enabled"].get<bool>();
            if (cr.contains("case_sensitive")) rule.case_sensitive = cr["case_sensitive"].get<bool>();
            if (cr.contains("tags")) for (auto& t : cr["tags"]) rule.tags.push_back(t.get<std::string>());
            parseRationale(cr, rule.rationale);
            // Compile regex
            if (!rule.pattern.empty() && rule.enabled) {
                // V-GOV-010: validate pattern complexity before compiling to prevent ReDoS
                bool pattern_safe = true;
                if (rule.pattern.size() > 1000) {
                    fprintf(stderr, "[governance] Warning: Unsafe regex in custom rule '%s' — skipped: "
                            "pattern exceeds 1000 character limit (%zu chars)\n",
                            rule.id.c_str(), rule.pattern.size());
                    pattern_safe = false;
                } else {
                    naab::regex_safety::SafeRegex safe_re;
                    auto complexity = safe_re.analyzePattern(rule.pattern);
                    if (!complexity.is_safe) {
                        fprintf(stderr, "[governance] Warning: Unsafe regex in custom rule '%s' — skipped: %s\n",
                                rule.id.c_str(), complexity.warning.c_str());
                        pattern_safe = false;
                    }
                }
                if (!pattern_safe) {
                    rules_.custom_rules.push_back(std::move(rule));
                    continue;
                }
                try {
                    auto flags = std::regex::ECMAScript;
                    if (!rule.case_sensitive) flags |= std::regex::icase;
                    rule.compiled_pattern = std::regex(rule.pattern, flags);
                    rule.pattern_valid = true;
                } catch (const std::regex_error&) {
                    fprintf(stderr, "[governance] Warning: Invalid regex in custom rule '%s': %s\n",
                            rule.id.c_str(), rule.pattern.c_str());
                }
            }
            rules_.custom_rules.push_back(std::move(rule));
        }
    }

    // Governance plugins (NAAb-based custom checks)
    if (j.contains("governance_plugins") && j["governance_plugins"].is_array()) {
        for (auto& gp : j["governance_plugins"]) {
            GovernancePlugin plugin;
            if (gp.contains("file") && gp["file"].is_string()) {
                plugin.file_path = gp["file"].get<std::string>();
            } else {
                fprintf(stderr, "[governance] Warning: governance_plugins entry missing 'file' field, skipping\n");
                continue;
            }
            if (gp.contains("rules") && gp["rules"].is_array()) {
                for (auto& pr : gp["rules"]) {
                    GovernancePluginRule rule;
                    if (pr.contains("id")) rule.id = pr["id"].get<std::string>();
                    if (pr.contains("function")) rule.function_name = pr["function"].get<std::string>();
                    if (pr.contains("description")) rule.description = pr["description"].get<std::string>();
                    if (pr.contains("level")) { auto [en, lv] = parseEnforcementLevel(pr["level"]); rule.level = lv; }
                    if (pr.contains("languages")) for (auto& l : pr["languages"]) rule.languages.push_back(l.get<std::string>());
                    if (pr.contains("trigger")) rule.trigger = pr["trigger"].get<std::string>();
                    if (pr.contains("message")) rule.message = pr["message"].get<std::string>();
                    if (pr.contains("help")) rule.help = pr["help"].get<std::string>();
                    if (pr.contains("good_example")) rule.good_example = pr["good_example"].get<std::string>();
                    if (pr.contains("bad_example")) rule.bad_example = pr["bad_example"].get<std::string>();
                    if (pr.contains("enabled")) rule.enabled = pr["enabled"].get<bool>();
                    if (rule.function_name.empty()) {
                        fprintf(stderr, "[governance] Warning: Plugin rule in '%s' missing 'function' field, skipping\n",
                                plugin.file_path.c_str());
                        continue;
                    }
                    if (rule.trigger.empty()) {
                        fprintf(stderr, "[governance] Warning: Plugin rule '%s' missing 'trigger' field, skipping\n",
                                rule.id.empty() ? rule.function_name.c_str() : rule.id.c_str());
                        continue;
                    }
                    parseRationale(pr, rule.rationale);
                    plugin.rules.push_back(std::move(rule));
                }
            }
            rules_.governance_plugins.push_back(std::move(plugin));
        }
    }

    // V3 Output config
    if (j.contains("output") && j["output"].is_object()) {
        auto& out = j["output"];
        if (out.contains("summary") && out["summary"].is_object()) {
            auto& s = out["summary"];
            if (s.contains("enabled")) rules_.output.summary.enabled = s["enabled"].get<bool>();
            if (s.contains("format")) rules_.output.summary.format = s["format"].get<std::string>();
            if (s.contains("show_passing")) rules_.output.summary.show_passing = s["show_passing"].get<bool>();
            if (s.contains("group_by")) rules_.output.summary.group_by = s["group_by"].get<std::string>();
        }
        if (out.contains("errors") && out["errors"].is_object()) {
            auto& e = out["errors"];
            if (e.contains("verbose")) rules_.output.errors.verbose = e["verbose"].get<bool>();
            if (e.contains("show_help")) rules_.output.errors.show_help = e["show_help"].get<bool>();
            if (e.contains("show_examples")) rules_.output.errors.show_examples = e["show_examples"].get<bool>();
            if (e.contains("max_errors_per_rule")) rules_.output.errors.max_errors_per_rule = e["max_errors_per_rule"].get<int>();
            if (e.contains("max_total_errors")) rules_.output.errors.max_total_errors = e["max_total_errors"].get<int>();
            if (e.contains("show_code_context")) rules_.output.errors.show_code_context = e["show_code_context"].get<int>();
        }
        if (out.contains("formatting") && out["formatting"].is_object()) {
            auto& f = out["formatting"];
            if (f.contains("color")) rules_.output.formatting.color = f["color"].get<bool>();
            if (f.contains("unicode_symbols")) rules_.output.formatting.unicode_symbols = f["unicode_symbols"].get<bool>();
            if (f.contains("width")) rules_.output.formatting.width = f["width"].get<int>();
        }
        if (out.contains("file_output") && out["file_output"].is_object()) {
            auto& fo = out["file_output"];
            if (fo.contains("report_json") && !fo["report_json"].is_null()) rules_.output.file_output.report_json = fo["report_json"].get<std::string>();
            if (fo.contains("report_sarif") && !fo["report_sarif"].is_null()) rules_.output.file_output.report_sarif = fo["report_sarif"].get<std::string>();
            if (fo.contains("report_junit") && !fo["report_junit"].is_null()) rules_.output.file_output.report_junit = fo["report_junit"].get<std::string>();
        }
        if (out.contains("max_advisories")) rules_.output.max_advisories = out["max_advisories"].get<int>();
        if (out.contains("advisory_summary")) rules_.output.advisory_summary = out["advisory_summary"].get<bool>();
        if (out.contains("quiet")) rules_.quiet_config = out["quiet"].get<bool>();
        if (out.contains("no_color")) rules_.no_color_config = out["no_color"].get<bool>();
        if (out.contains("voice")) rules_.output.voice = out["voice"].get<std::string>();
        if (out.contains("voice_cache")) rules_.output.voice_cache = out["voice_cache"].get<bool>();
    }

    // V3 Audit (expanded)
    if (j.contains("audit") && j["audit"].is_object()) {
        auto& aud = j["audit"];
        if (aud.contains("level")) rules_.audit.level = aud["level"].get<std::string>();
        if (aud.contains("output_file")) rules_.audit.output_file = aud["output_file"].get<std::string>();
        if (aud.contains("tamper_evidence") && aud["tamper_evidence"].is_object()) {
            auto& te = aud["tamper_evidence"];
            if (te.contains("enabled")) { rules_.audit.tamper_evidence.enabled = te["enabled"].get<bool>(); rules_.tamper_evidence = rules_.audit.tamper_evidence.enabled; }
            if (te.contains("algorithm")) rules_.audit.tamper_evidence.algorithm = te["algorithm"].get<std::string>();
            if (te.contains("chain_genesis")) rules_.audit.tamper_evidence.chain_genesis = te["chain_genesis"].get<std::string>();
            if (te.contains("hmac_key")) rules_.audit.tamper_evidence.hmac_key = te["hmac_key"].get<std::string>();
            if (te.contains("hmac_key_env")) {
                std::string env_name = te["hmac_key_env"].get<std::string>();
                const char* env_val = std::getenv(env_name.c_str());
                if (env_val && env_val[0] != '\0') {
                    rules_.audit.tamper_evidence.hmac_key = env_val;
                }
            }
        }
        if (aud.contains("log_events") && aud["log_events"].is_object()) {
            auto& le = aud["log_events"];
            if (le.contains("checks_passed")) rules_.audit.log_events.checks_passed = le["checks_passed"].get<bool>();
            if (le.contains("checks_failed")) rules_.audit.log_events.checks_failed = le["checks_failed"].get<bool>();
            if (le.contains("overrides")) rules_.audit.log_events.overrides = le["overrides"].get<bool>();
            if (le.contains("polyglot_executed")) rules_.audit.log_events.polyglot_executed = le["polyglot_executed"].get<bool>();
            if (le.contains("polyglot_timing")) rules_.audit.log_events.polyglot_timing = le["polyglot_timing"].get<bool>();
            if (le.contains("taint_decisions")) rules_.audit.log_events.taint_decisions = le["taint_decisions"].get<bool>();
            if (le.contains("contract_checks")) rules_.audit.log_events.contract_checks = le["contract_checks"].get<bool>();
        }
        if (aud.contains("provenance") && aud["provenance"].is_object()) {
            auto& prov = aud["provenance"];
            if (prov.contains("enabled")) rules_.audit.provenance.enabled = prov["enabled"].get<bool>();
            if (prov.contains("record_proof_objects")) rules_.audit.provenance.record_proof_objects = prov["record_proof_objects"].get<bool>();
            if (prov.contains("record_attestations")) rules_.audit.provenance.record_attestations = prov["record_attestations"].get<bool>();
            if (prov.contains("record_decisions")) rules_.audit.provenance.record_decisions = prov["record_decisions"].get<bool>();
            if (prov.contains("sign_records")) rules_.audit.provenance.sign_records = prov["sign_records"].get<bool>();
            if (prov.contains("signing_key")) rules_.audit.provenance.signing_key = prov["signing_key"].get<std::string>();
            if (prov.contains("signing_key_env")) {
                std::string env_name = prov["signing_key_env"].get<std::string>();
                const char* env_val = std::getenv(env_name.c_str());
                if (env_val) rules_.audit.provenance.signing_key = env_val;
            }
        }
    }

    // V3 Meta
    if (j.contains("meta") && j["meta"].is_object()) {
        auto& meta = j["meta"];
        if (meta.contains("schema_validation") && meta["schema_validation"].is_object()) {
            auto& sv = meta["schema_validation"];
            if (sv.contains("warn_unknown_keys")) rules_.meta.schema_validation.warn_unknown_keys = sv["warn_unknown_keys"].get<bool>();
            if (sv.contains("suggest_corrections")) rules_.meta.schema_validation.suggest_corrections = sv["suggest_corrections"].get<bool>();
        }
        if (meta.contains("inheritance") && meta["inheritance"].is_object()) {
            auto& inh = meta["inheritance"];
            if (inh.contains("max_depth")) rules_.meta.inheritance.max_depth = inh["max_depth"].get<int>();
            if (inh.contains("merge_strategy")) rules_.meta.inheritance.merge_strategy = inh["merge_strategy"].get<std::string>();
        }
        if (meta.contains("environment") && meta["environment"].is_object()) {
            auto& env = meta["environment"];
            if (env.contains("allow_env_var_substitution")) rules_.meta.environment.allow_env_var_substitution = env["allow_env_var_substitution"].get<bool>();
            if (env.contains("env_prefix")) rules_.meta.environment.env_prefix = env["env_prefix"].get<std::string>();
            if (env.contains("allow_cli_override")) rules_.meta.environment.allow_cli_override = env["allow_cli_override"].get<bool>();
        }
        if (meta.contains("feature_flags") && meta["feature_flags"].is_object()) {
            auto& ff = meta["feature_flags"];
            if (ff.contains("experimental_checks")) rules_.meta.feature_flags.experimental_checks = ff["experimental_checks"].get<bool>();
            if (ff.contains("verbose_parsing")) rules_.meta.feature_flags.verbose_parsing = ff["verbose_parsing"].get<bool>();
        }
    }

    // V3 Polyglot rules
    if (j.contains("polyglot") && j["polyglot"].is_object()) {
        auto& pg = j["polyglot"];
        if (pg.contains("variable_binding") && pg["variable_binding"].is_object()) {
            auto& vb = pg["variable_binding"];
            if (vb.contains("require_explicit")) { auto [en, lv] = parseEnforcementLevel(vb["require_explicit"]); rules_.polyglot.variable_binding.require_explicit = en; rules_.polyglot.variable_binding.require_explicit_level = lv; }
            if (vb.contains("max_bound_variables")) rules_.polyglot.variable_binding.max_bound_variables = vb["max_bound_variables"].get<int>();
        }
        if (pg.contains("output") && pg["output"].is_object()) {
            auto& po = pg["output"];
            if (po.contains("require_json_pipe")) rules_.polyglot.output.require_json_pipe = po["require_json_pipe"].get<bool>();
            if (po.contains("max_output_lines")) rules_.polyglot.output.max_output_lines = po["max_output_lines"].get<int>();
            if (po.contains("validate_encoding")) rules_.polyglot.output.validate_encoding = po["validate_encoding"].get<bool>();
        }
        if (pg.contains("parallel") && pg["parallel"].is_object()) {
            auto& par = pg["parallel"];
            if (par.contains("max_parallel_blocks")) rules_.polyglot.parallel.max_parallel_blocks = par["max_parallel_blocks"].get<int>();
            if (par.contains("timeout_per_block")) rules_.polyglot.parallel.timeout_per_block = par["timeout_per_block"].get<int>();
            if (par.contains("fail_strategy")) rules_.polyglot.parallel.fail_strategy = par["fail_strategy"].get<std::string>();
        }
        if (pg.contains("persistent_runtime") && pg["persistent_runtime"].is_object()) {
            auto& pr = pg["persistent_runtime"];
            if (pr.contains("max_sessions")) rules_.polyglot.persistent_runtime.max_sessions = pr["max_sessions"].get<int>();
            if (pr.contains("session_timeout")) rules_.polyglot.persistent_runtime.session_timeout = pr["session_timeout"].get<int>();
            if (pr.contains("max_memory_per_session_mb")) rules_.polyglot.persistent_runtime.max_memory_per_session_mb = pr["max_memory_per_session_mb"].get<int>();
        }
    }

    // V3 Polyglot Optimization (Section 14)
    if (j.contains("polyglot_optimization") && j["polyglot_optimization"].is_object()) {
        auto& po = j["polyglot_optimization"];

        if (po.contains("enabled")) rules_.polyglot_optimization.enabled = po["enabled"].get<bool>();
        if (po.contains("enforcement_level")) rules_.polyglot_optimization.enforcement_level = po["enforcement_level"].get<std::string>();

        // Pattern detection
        if (po.contains("pattern_detection") && po["pattern_detection"].is_object()) {
            auto& pd = po["pattern_detection"];
            if (pd.contains("enabled")) rules_.polyglot_optimization.pattern_detection.enabled = pd["enabled"].get<bool>();

            // Task inference patterns
            if (pd.contains("task_inference") && pd["task_inference"].is_object()) {
                for (auto& [task_name, task_config] : pd["task_inference"].items()) {
                    if (!task_config.is_object()) continue;

                    TaskInferencePattern pattern;
                    if (task_config.contains("patterns") && task_config["patterns"].is_array()) {
                        for (auto& p : task_config["patterns"]) {
                            pattern.patterns.push_back(p.get<std::string>());
                        }
                    }
                    if (task_config.contains("optimal_languages") && task_config["optimal_languages"].is_array()) {
                        for (auto& lang : task_config["optimal_languages"]) {
                            pattern.optimal_languages.push_back(lang.get<std::string>());
                        }
                    }
                    if (task_config.contains("suboptimal_languages") && task_config["suboptimal_languages"].is_array()) {
                        for (auto& lang : task_config["suboptimal_languages"]) {
                            pattern.suboptimal_languages.push_back(lang.get<std::string>());
                        }
                    }
                    if (task_config.contains("message")) pattern.message = task_config["message"].get<std::string>();

                    rules_.polyglot_optimization.pattern_detection.task_inference[task_name] = pattern;
                }
            }
        }

        // Language diversity
        if (po.contains("language_diversity") && po["language_diversity"].is_object()) {
            auto& ld = po["language_diversity"];
            if (ld.contains("enabled")) rules_.polyglot_optimization.language_diversity.enabled = ld["enabled"].get<bool>();
            if (ld.contains("min_languages")) rules_.polyglot_optimization.language_diversity.min_languages = ld["min_languages"].get<int>();
            if (ld.contains("max_single_language_percent")) rules_.polyglot_optimization.language_diversity.max_single_language_percent = ld["max_single_language_percent"].get<int>();
            if (ld.contains("message")) rules_.polyglot_optimization.language_diversity.message = ld["message"].get<std::string>();
        }

        // Helper errors
        if (po.contains("helper_errors") && po["helper_errors"].is_object()) {
            auto& he = po["helper_errors"];
            if (he.contains("enabled")) rules_.polyglot_optimization.helper_errors.enabled = he["enabled"].get<bool>();
            if (he.contains("show_alternative_language")) rules_.polyglot_optimization.helper_errors.show_alternative_language = he["show_alternative_language"].get<bool>();
            if (he.contains("show_example_code")) rules_.polyglot_optimization.helper_errors.show_example_code = he["show_example_code"].get<bool>();
            if (he.contains("fuzzy_match_threshold")) rules_.polyglot_optimization.helper_errors.fuzzy_match_threshold = he["fuzzy_match_threshold"].get<double>();
        }

        // AI guidance
        if (po.contains("ai_guidance") && po["ai_guidance"].is_object()) {
            auto& ag = po["ai_guidance"];
            if (ag.contains("enabled")) rules_.polyglot_optimization.ai_guidance.enabled = ag["enabled"].get<bool>();
            if (ag.contains("include_in_errors")) rules_.polyglot_optimization.ai_guidance.include_in_errors = ag["include_in_errors"].get<bool>();
            if (ag.contains("suggest_refactoring")) rules_.polyglot_optimization.ai_guidance.suggest_refactoring = ag["suggest_refactoring"].get<bool>();
            if (ag.contains("show_benchmarks")) rules_.polyglot_optimization.ai_guidance.show_benchmarks = ag["show_benchmarks"].get<bool>();
        }

        // Empirical profiling
        if (po.contains("profiling") && po["profiling"].is_object()) {
            auto& pf = po["profiling"];
            if (pf.contains("enabled")) rules_.polyglot_optimization.profiling.enabled = pf["enabled"].get<bool>();
            if (pf.contains("profile_path")) rules_.polyglot_optimization.profiling.profile_path = pf["profile_path"].get<std::string>();
            if (pf.contains("max_entries")) rules_.polyglot_optimization.profiling.max_entries = pf["max_entries"].get<int>();
            if (pf.contains("include_code_hash")) rules_.polyglot_optimization.profiling.include_code_hash = pf["include_code_hash"].get<bool>();
        }

        // Calibration
        if (po.contains("calibration") && po["calibration"].is_object()) {
            auto& cb = po["calibration"];
            if (cb.contains("enabled")) rules_.polyglot_optimization.calibration.enabled = cb["enabled"].get<bool>();
            if (cb.contains("auto_calibrate")) rules_.polyglot_optimization.calibration.auto_calibrate = cb["auto_calibrate"].get<bool>();
            if (cb.contains("calibration_path")) rules_.polyglot_optimization.calibration.calibration_path = cb["calibration_path"].get<std::string>();
            if (cb.contains("max_age_days")) rules_.polyglot_optimization.calibration.max_age_days = cb["max_age_days"].get<int>();
            if (cb.contains("iterations")) rules_.polyglot_optimization.calibration.iterations = cb["iterations"].get<int>();
        }

        // Confidence labels
        if (po.contains("confidence") && po["confidence"].is_object()) {
            auto& cf = po["confidence"];
            if (cf.contains("min_display_level")) rules_.polyglot_optimization.confidence.min_display_level = cf["min_display_level"].get<std::string>();
            if (cf.contains("suppress_unknown")) rules_.polyglot_optimization.confidence.suppress_unknown = cf["suppress_unknown"].get<bool>();
            if (cf.contains("show_measurement_details")) rules_.polyglot_optimization.confidence.show_measurement_details = cf["show_measurement_details"].get<bool>();
        }

        // Polyglot consensus verification
        if (po.contains("verification") && po["verification"].is_object()) {
            auto& vf = po["verification"];
            if (vf.contains("enabled"))
                rules_.polyglot_optimization.verification.enabled = vf["enabled"].get<bool>();
            if (vf.contains("enforcement_level"))
                rules_.polyglot_optimization.verification.enforcement_level = vf["enforcement_level"].get<std::string>();
            if (vf.contains("tolerance"))
                rules_.polyglot_optimization.verification.tolerance = vf["tolerance"].get<double>();
            if (vf.contains("min_consensus"))
                rules_.polyglot_optimization.verification.min_consensus = vf["min_consensus"].get<int>();
            if (vf.contains("max_verification_time_ms"))
                rules_.polyglot_optimization.verification.max_verification_time_ms = vf["max_verification_time_ms"].get<int>();
            if (vf.contains("show_drift_details"))
                rules_.polyglot_optimization.verification.show_drift_details = vf["show_drift_details"].get<bool>();
            if (vf.contains("consensus_languages") && vf["consensus_languages"].is_array()) {
                for (auto& lang : vf["consensus_languages"]) {
                    rules_.polyglot_optimization.verification.consensus_languages.push_back(
                        lang.get<std::string>());
                }
            }
            if (vf.contains("verify_task_types") && vf["verify_task_types"].is_array()) {
                for (auto& tt : vf["verify_task_types"]) {
                    rules_.polyglot_optimization.verification.verify_task_types.push_back(
                        tt.get<std::string>());
                }
            }
            // Drift tracking sub-config
            if (vf.contains("drift_tracking") && vf["drift_tracking"].is_object()) {
                auto& dt = vf["drift_tracking"];
                auto& dtc = rules_.polyglot_optimization.verification.drift_tracking;
                if (dt.contains("enabled")) dtc.enabled = dt["enabled"].get<bool>();
                if (dt.contains("path")) dtc.path = dt["path"].get<std::string>();
                if (dt.contains("max_entries")) dtc.max_entries = dt["max_entries"].get<int>();
                if (dt.contains("trend_window")) dtc.trend_window = dt["trend_window"].get<int>();
                if (dt.contains("escalation_threshold")) dtc.escalation_threshold = dt["escalation_threshold"].get<double>();
                if (dt.contains("include_code_hash")) dtc.include_code_hash = dt["include_code_hash"].get<bool>();
            }
        }

        // Task→Language scoring matrix
        if (po.contains("task_language_matrix") && po["task_language_matrix"].is_object()) {
            for (auto& [task_name, lang_scores] : po["task_language_matrix"].items()) {
                if (!lang_scores.is_object()) continue;

                for (auto& [lang_name, score_obj] : lang_scores.items()) {
                    TaskLanguageScore score;
                    if (score_obj.is_object()) {
                        if (score_obj.contains("score")) score.score = score_obj["score"].get<int>();
                        if (score_obj.contains("reason")) score.reason = score_obj["reason"].get<std::string>();
                    } else if (score_obj.is_number()) {
                        // Allow simple numeric scores
                        score.score = score_obj.get<int>();
                    }

                    rules_.polyglot_optimization.task_language_matrix[task_name][lang_name] = score;
                }
            }
        }
    }

    // V3 Hooks
    if (j.contains("hooks") && j["hooks"].is_object()) {
        auto loadHook = [](const nlohmann::json& hj, HookConfig& hc) {
            if (hj.contains("command") && !hj["command"].is_null()) hc.command = hj["command"].get<std::string>();
            if (hj.contains("args")) for (auto& a : hj["args"]) hc.args.push_back(a.get<std::string>());
            if (hj.contains("timeout")) hc.timeout = hj["timeout"].get<int>();
        };
        auto& hk = j["hooks"];
        if (hk.contains("on_violation")) loadHook(hk["on_violation"], rules_.hooks.on_violation);
        if (hk.contains("on_override")) loadHook(hk["on_override"], rules_.hooks.on_override);
        if (hk.contains("on_complete")) loadHook(hk["on_complete"], rules_.hooks.on_complete);
        if (hk.contains("pre_check")) loadHook(hk["pre_check"], rules_.hooks.pre_check);
        if (hk.contains("post_check")) loadHook(hk["post_check"], rules_.hooks.post_check);
    }

    // Project Context Awareness
    if (j.contains("project_context") && j["project_context"].is_object()) {
        auto& pc = j["project_context"];
        if (pc.contains("enabled")) rules_.project_context.enabled = pc["enabled"].get<bool>();
        if (pc.contains("enforcement_level")) rules_.project_context.enforcement_level = pc["enforcement_level"].get<std::string>();
        if (pc.contains("priority_source")) rules_.project_context.priority_source = pc["priority_source"].get<std::string>();
        if (pc.contains("sources") && pc["sources"].is_object()) {
            auto& src = pc["sources"];
            if (src.contains("llm")) rules_.project_context.sources.llm = src["llm"].get<bool>();
            if (src.contains("linters")) rules_.project_context.sources.linters = src["linters"].get<bool>();
            if (src.contains("manifests")) rules_.project_context.sources.manifests = src["manifests"].get<bool>();
        }
        if (pc.contains("watch_files")) {
            for (auto& f : pc["watch_files"]) rules_.project_context.watch_files.push_back(f.get<std::string>());
        }
        if (pc.contains("ignore_files")) {
            for (auto& f : pc["ignore_files"]) rules_.project_context.ignore_files.push_back(f.get<std::string>());
        }
        if (pc.contains("suppress_rules")) {
            for (auto& r : pc["suppress_rules"]) rules_.project_context.suppress_rules.push_back(r.get<std::string>());
        }
        if (pc.contains("extract") && pc["extract"].is_object()) {
            auto& ex = pc["extract"];
            if (ex.contains("language_preferences")) rules_.project_context.extract_language_prefs = ex["language_preferences"].get<bool>();
            if (ex.contains("banned_patterns")) rules_.project_context.extract_banned_patterns = ex["banned_patterns"].get<bool>();
            if (ex.contains("style_rules")) rules_.project_context.extract_style_rules = ex["style_rules"].get<bool>();
            if (ex.contains("custom_directives")) rules_.project_context.extract_custom_directives = ex["custom_directives"].get<bool>();
        }
        if (pc.contains("feed_optimization")) rules_.project_context.feed_optimization = pc["feed_optimization"].get<bool>();
        if (pc.contains("show_extractions")) rules_.project_context.show_extractions = pc["show_extractions"].get<bool>();
        if (pc.contains("dry_run")) rules_.project_context.dry_run = pc["dry_run"].get<bool>();
        if (pc.contains("max_file_size_kb")) rules_.project_context.max_file_size_kb = pc["max_file_size_kb"].get<int>();
    }

    // Contracts
    if (j.contains("contracts") && j["contracts"].is_object()) {
        auto& ct = j["contracts"];
        if (ct.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(ct["level"]);
            if (en) rules_.contracts.level = lv;
        }
        if (ct.contains("validate_inputs")) rules_.contracts.validate_inputs = ct["validate_inputs"].get<bool>();
        if (ct.contains("functions") && ct["functions"].is_object()) {
            for (auto& [fn_name, fn_obj] : ct["functions"].items()) {
                if (!fn_obj.is_object()) continue;
                FunctionContract fc;
                if (fn_obj.contains("description")) fc.description = fn_obj["description"].get<std::string>();
                if (fn_obj.contains("level")) {
                    auto [en, lv] = parseEnforcementLevel(fn_obj["level"]);
                    if (en) fc.level = lv;
                }
                if (fn_obj.contains("return_type")) fc.return_type = fn_obj["return_type"].get<std::string>();
                if (fn_obj.contains("return_range") && fn_obj["return_range"].is_array() && fn_obj["return_range"].size() == 2) {
                    fc.has_return_range = true;
                    fc.return_range_min = fn_obj["return_range"][0].get<double>();
                    fc.return_range_max = fn_obj["return_range"][1].get<double>();
                }
                if (fn_obj.contains("return_min")) { fc.has_return_min = true; fc.return_min = fn_obj["return_min"].get<double>(); }
                if (fn_obj.contains("return_max")) { fc.has_return_max = true; fc.return_max = fn_obj["return_max"].get<double>(); }
                if (fn_obj.contains("return_one_of") && fn_obj["return_one_of"].is_array()) {
                    for (auto& v : fn_obj["return_one_of"]) {
                        // Handle non-string values (ints, bools) by converting to string
                        if (v.is_string()) {
                            fc.return_one_of.push_back(v.get<std::string>());
                        } else {
                            fc.return_one_of.push_back(v.dump());
                        }
                    }
                }
                if (fn_obj.contains("return_non_empty")) fc.return_non_empty = fn_obj["return_non_empty"].get<bool>();
                if (fn_obj.contains("return_keys") && fn_obj["return_keys"].is_array()) {
                    for (auto& k : fn_obj["return_keys"]) fc.return_keys.push_back(k.get<std::string>());
                }
                if (fn_obj.contains("return_keys_non_null")) fc.return_keys_non_null = fn_obj["return_keys_non_null"].get<bool>();
                if (fn_obj.contains("return_keys_non_empty")) fc.return_keys_non_empty = fn_obj["return_keys_non_empty"].get<bool>();
                if (fn_obj.contains("return_length_min")) fc.return_length_min = fn_obj["return_length_min"].get<int>();
                if (fn_obj.contains("return_length_max")) fc.return_length_max = fn_obj["return_length_max"].get<int>();
                if (fn_obj.contains("return_not_null")) fc.return_not_null = fn_obj["return_not_null"].get<bool>();
                if (fn_obj.contains("return_matches")) fc.return_matches = fn_obj["return_matches"].get<std::string>();
                if (fn_obj.contains("params") && fn_obj["params"].is_array()) {
                    for (auto& p : fn_obj["params"]) fc.params.push_back(p.get<std::string>());
                }
                if (fn_obj.contains("must_call") && fn_obj["must_call"].is_array()) {
                    for (auto& mc : fn_obj["must_call"]) fc.must_call.push_back(mc.get<std::string>());
                }
                if (fn_obj.contains("must_contain") && fn_obj["must_contain"].is_array()) {
                    for (auto& mc : fn_obj["must_contain"]) fc.must_contain.push_back(mc.get<std::string>());
                }
                // naab-29 L-08: arity constraints — supports both flat and nested format
                if (fn_obj.contains("min_arity") && fn_obj["min_arity"].is_number_integer()) {
                    fc.min_arity = fn_obj["min_arity"].get<int>();
                }
                if (fn_obj.contains("max_arity") && fn_obj["max_arity"].is_number_integer()) {
                    fc.max_arity = fn_obj["max_arity"].get<int>();
                }
                if (fn_obj.contains("arity") && fn_obj["arity"].is_object()) {
                    auto& ar = fn_obj["arity"];
                    if (ar.contains("min") && ar["min"].is_number_integer()) fc.min_arity = ar["min"].get<int>();
                    if (ar.contains("max") && ar["max"].is_number_integer()) fc.max_arity = ar["max"].get<int>();
                }
                // v6: must_produce — golden tests (array of {args: [...], expect: value})
                if (fn_obj.contains("must_produce") && fn_obj["must_produce"].is_array()) {
                    for (auto& tc : fn_obj["must_produce"]) {
                        if (!tc.is_object()) continue;
                        FunctionContract::MustProduceCase mpc;
                        if (tc.contains("args") && tc["args"].is_array()) {
                            for (auto& a : tc["args"]) mpc.args_json.push_back(a.dump());
                        }
                        if (tc.contains("expect")) mpc.expect_json = tc["expect"].dump();
                        fc.must_produce.push_back(std::move(mpc));
                    }
                }
                // v6: must_derive_from — static derivation check ({return_key: [param1, ...]})
                if (fn_obj.contains("must_derive_from") && fn_obj["must_derive_from"].is_object()) {
                    for (auto& [rk, params] : fn_obj["must_derive_from"].items()) {
                        FunctionContract::MustDeriveFromSpec spec;
                        spec.return_key = rk;
                        if (params.is_array()) {
                            for (auto& p : params) spec.params.push_back(p.get<std::string>());
                        }
                        fc.must_derive_from.push_back(std::move(spec));
                    }
                }
                // v6: must_vary — anti-hardcoding (array of {key, across, fixtures?})
                if (fn_obj.contains("must_vary") && fn_obj["must_vary"].is_array()) {
                    for (auto& mv : fn_obj["must_vary"]) {
                        if (!mv.is_object()) continue;
                        FunctionContract::MustVarySpec spec;
                        if (mv.contains("key")) spec.key = mv["key"].get<std::string>();
                        if (mv.contains("across")) spec.across = mv["across"].get<std::string>();
                        if (mv.contains("fixtures") && mv["fixtures"].is_array()) {
                            for (auto& f : mv["fixtures"]) spec.fixtures_json.push_back(f.dump());
                        }
                        fc.must_vary.push_back(std::move(spec));
                    }
                }
                // v6: must_differentiate — mutation testing (array of {a, b, key})
                if (fn_obj.contains("must_differentiate") && fn_obj["must_differentiate"].is_array()) {
                    for (auto& md : fn_obj["must_differentiate"]) {
                        if (!md.is_object()) continue;
                        FunctionContract::MustDifferentiateCase dc;
                        if (md.contains("a")) dc.input_a_json = md["a"].dump();
                        if (md.contains("b")) dc.input_b_json = md["b"].dump();
                        if (md.contains("key")) dc.key = md["key"].get<std::string>();
                        fc.must_differentiate.push_back(std::move(dc));
                    }
                }
                // v6: must_handle_case — case-sensitivity awareness (array of {inputs: [...], expect: str})
                if (fn_obj.contains("must_handle_case") && fn_obj["must_handle_case"].is_array()) {
                    for (auto& mh : fn_obj["must_handle_case"]) {
                        if (!mh.is_object()) continue;
                        FunctionContract::MustHandleCaseSpec spec;
                        if (mh.contains("inputs") && mh["inputs"].is_array()) {
                            for (auto& inp : mh["inputs"]) spec.inputs_json.push_back(inp.dump());
                        }
                        if (mh.contains("expect")) spec.expect = mh["expect"].get<std::string>();
                        fc.must_handle_case.push_back(std::move(spec));
                    }
                }
                // v6: must_satisfy — invariant expressions (array of strings)
                if (fn_obj.contains("must_satisfy") && fn_obj["must_satisfy"].is_array()) {
                    for (auto& expr : fn_obj["must_satisfy"]) {
                        if (expr.is_string()) fc.must_satisfy.push_back(expr.get<std::string>());
                    }
                }
                // v6: must_satisfy_args — test inputs for must_satisfy (array of JSON values)
                if (fn_obj.contains("must_satisfy_args") && fn_obj["must_satisfy_args"].is_array()) {
                    for (auto& a : fn_obj["must_satisfy_args"]) {
                        fc.must_satisfy_args_json.push_back(a.dump());
                    }
                }
                parseRationale(fn_obj, fc.rationale);
                rules_.contracts.functions[fn_name] = std::move(fc);
            }
        }
    }

    // Baselines
    if (j.contains("baselines") && j["baselines"].is_object()) {
        auto& bl = j["baselines"];
        if (bl.contains("enabled")) rules_.baselines.enabled = bl["enabled"].get<bool>();
        if (bl.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(bl["level"]);
            if (en) rules_.baselines.level = lv;
        }
        if (bl.contains("path")) rules_.baselines.path = bl["path"].get<std::string>();
        if (bl.contains("tolerance")) rules_.baselines.tolerance = bl["tolerance"].get<double>();
        if (bl.contains("auto_record")) rules_.baselines.auto_record = bl["auto_record"].get<bool>();
        if (bl.contains("hash_keys")) rules_.baselines.hash_keys = bl["hash_keys"].get<bool>();
    }

    // Taint Tracking
    if (j.contains("taint_tracking") && j["taint_tracking"].is_object()) {
        auto& tt = j["taint_tracking"];
        if (tt.contains("enabled")) rules_.taint_tracking.enabled = tt["enabled"].get<bool>();
        if (tt.contains("lineage")) rules_.taint_tracking.lineage = tt["lineage"].get<bool>();
        if (tt.contains("level")) rules_.taint_tracking.level = tt["level"].get<std::string>();
        if (tt.contains("sources") && tt["sources"].is_array()) {
            for (const auto& s : tt["sources"]) {
                rules_.taint_tracking.sources.push_back(s.get<std::string>());
            }
        }
        if (tt.contains("sinks") && tt["sinks"].is_array()) {
            for (const auto& s : tt["sinks"]) {
                rules_.taint_tracking.sinks.push_back(s.get<std::string>());
            }
            // Gap 5/9: Auto-expand sink equivalents — file.write and file.append
            // are the same operation, so configuring one should include the other.
            static const std::vector<std::pair<std::string, std::string>> sink_equivalents = {
                {"file.write", "file.append"},
                {"file.append", "file.write"}
            };
            for (const auto& [configured, equivalent] : sink_equivalents) {
                bool has_configured = false, has_equivalent = false;
                for (const auto& s : rules_.taint_tracking.sinks) {
                    if (s == configured) has_configured = true;
                    if (s == equivalent) has_equivalent = true;
                }
                if (has_configured && !has_equivalent) {
                    rules_.taint_tracking.sinks.push_back(equivalent);
                }
            }
        }
        if (tt.contains("sanitizers") && tt["sanitizers"].is_array()) {
            for (const auto& s : tt["sanitizers"]) {
                rules_.taint_tracking.sanitizers.push_back(s.get<std::string>());
            }
        }
        parseRationale(tt, rules_.taint_tracking.rationale);
        if (tt.contains("gate_cross_block")) rules_.taint_tracking.gate_cross_block = tt["gate_cross_block"].get<bool>();
        if (tt.contains("cross_block_level")) {
            auto [en, lv] = parseEnforcementLevel(tt["cross_block_level"]);
            rules_.taint_tracking.cross_block_level = lv;
        }
    }

    // --- Approval config (APPROVAL_REQUIRED tier) ---
    if (j.contains("approval") && j["approval"].is_object()) {
        auto& ap = j["approval"];
        if (ap.contains("store_path")) rules_.approval.store_path = ap["store_path"].get<std::string>();
        if (ap.contains("approver_keys") && ap["approver_keys"].is_array()) {
            for (const auto& k : ap["approver_keys"]) {
                rules_.approval.approver_keys.push_back(k.get<std::string>());
            }
        }
        if (ap.contains("default_expiry_hours")) rules_.approval.default_expiry_hours = ap["default_expiry_hours"].get<int>();
    }

    // --- Trust Policy (Authority Decay) ---
    if (j.contains("trust") && j["trust"].is_object()) {
        auto& tp = j["trust"];
        if (tp.contains("max_signature_age_days"))
            rules_.trust_policy.max_signature_age_days = tp["max_signature_age_days"].get<int>();
        if (tp.contains("require_fresh_signature"))
            rules_.trust_policy.require_fresh_signature = tp["require_fresh_signature"].get<bool>();
        if (tp.contains("stale_signature_level")) {
            std::string lev = tp["stale_signature_level"].get<std::string>();
            if (lev == "hard") rules_.trust_policy.stale_signature_level = EnforcementLevel::HARD;
            else if (lev == "soft") rules_.trust_policy.stale_signature_level = EnforcementLevel::SOFT;
            else rules_.trust_policy.stale_signature_level = EnforcementLevel::ADVISORY;
        }
        if (tp.contains("check_key_expiry"))
            rules_.trust_policy.check_key_expiry = tp["check_key_expiry"].get<bool>();
        if (tp.contains("check_revocation"))
            rules_.trust_policy.check_revocation = tp["check_revocation"].get<bool>();
    }

    // --- Prerequisites (Environment Attestation) ---
    if (j.contains("prerequisites") && j["prerequisites"].is_object()) {
        auto& pr = j["prerequisites"];
        if (pr.contains("enabled")) rules_.prerequisites.enabled = pr["enabled"].get<bool>();
        if (pr.contains("checks") && pr["checks"].is_array()) {
            for (const auto& chk : pr["checks"]) {
                PrerequisiteCheck pc;
                if (chk.contains("type")) pc.type = chk["type"].get<std::string>();
                if (chk.contains("name")) pc.name = chk["name"].get<std::string>();
                if (chk.contains("required")) pc.required = chk["required"].get<std::string>();
                if (chk.contains("level")) {
                    std::string lev = chk["level"].get<std::string>();
                    if (lev == "hard") pc.level = EnforcementLevel::HARD;
                    else if (lev == "soft") pc.level = EnforcementLevel::SOFT;
                    else pc.level = EnforcementLevel::ADVISORY;
                }
                if (chk.contains("message")) pc.message = chk["message"].get<std::string>();
                rules_.prerequisites.checks.push_back(std::move(pc));
            }
        }
    }

    // --- Contradiction Detection ---
    if (j.contains("contradiction_detection") && j["contradiction_detection"].is_object()) {
        auto& cd = j["contradiction_detection"];
        if (cd.contains("enabled")) rules_.contradiction_detection.enabled = cd["enabled"].get<bool>();
        if (cd.contains("max_level")) {
            std::string lev = cd["max_level"].get<std::string>();
            if (lev == "hard") rules_.contradiction_detection.max_level = EnforcementLevel::HARD;
            else if (lev == "soft") rules_.contradiction_detection.max_level = EnforcementLevel::SOFT;
            else rules_.contradiction_detection.max_level = EnforcementLevel::ADVISORY;
        }
    }

    // --- Dynamic Code Generation (codegen) ---
    if (j.contains("codegen") && j["codegen"].is_object()) {
        auto& cg = j["codegen"];
        if (cg.contains("enabled") && cg["enabled"].is_boolean())
            rules_.codegen.enabled = cg["enabled"].get<bool>();
        if (cg.contains("level") && cg["level"].is_string()) {
            std::string lev = cg["level"].get<std::string>();
            if (lev == "hard") rules_.codegen.level = EnforcementLevel::HARD;
            else if (lev == "soft") rules_.codegen.level = EnforcementLevel::SOFT;
            else rules_.codegen.level = EnforcementLevel::ADVISORY;
        }
        if (cg.contains("rationale") && cg["rationale"].is_string())
            rules_.codegen.rationale = cg["rationale"].get<std::string>();
        if (cg.contains("max_code_size_bytes") && cg["max_code_size_bytes"].is_number_integer())
            rules_.codegen.max_code_size_bytes = std::max(0, cg["max_code_size_bytes"].get<int>());
        if (cg.contains("max_code_lines") && cg["max_code_lines"].is_number_integer())
            rules_.codegen.max_code_lines = std::max(0, cg["max_code_lines"].get<int>());
        if (cg.contains("timeout_seconds") && cg["timeout_seconds"].is_number_integer())
            rules_.codegen.timeout_seconds = std::max(0, cg["timeout_seconds"].get<int>());
        if (cg.contains("max_cumulative_code_bytes") && cg["max_cumulative_code_bytes"].is_number_integer())
            rules_.codegen.max_cumulative_code_bytes = std::max(0, cg["max_cumulative_code_bytes"].get<int>());
        if (cg.contains("max_cumulative_calls") && cg["max_cumulative_calls"].is_number_integer())
            rules_.codegen.max_cumulative_calls = std::max(0, cg["max_cumulative_calls"].get<int>());
        if (cg.contains("max_cumulative_calls_per_agent") && cg["max_cumulative_calls_per_agent"].is_number_integer())
            rules_.codegen.max_cumulative_calls_per_agent = std::max(0, cg["max_cumulative_calls_per_agent"].get<int>());
        if (cg.contains("allow_tainted_code") && cg["allow_tainted_code"].is_boolean())
            rules_.codegen.allow_tainted_code = cg["allow_tainted_code"].get<bool>();
        if (cg.contains("max_nesting_depth") && cg["max_nesting_depth"].is_number_integer())
            rules_.codegen.max_nesting_depth = std::max(0, cg["max_nesting_depth"].get<int>());
        if (cg.contains("allowed_languages") && cg["allowed_languages"].is_array()) {
            for (const auto& lang : cg["allowed_languages"]) {
                if (lang.is_string()) rules_.codegen.allowed_languages.push_back(lang.get<std::string>());
            }
        }
        if (cg.contains("blocked_languages") && cg["blocked_languages"].is_array()) {
            for (const auto& lang : cg["blocked_languages"]) {
                if (lang.is_string()) rules_.codegen.blocked_languages.push_back(lang.get<std::string>());
            }
        }
        if (cg.contains("sanitize_stderr") && cg["sanitize_stderr"].is_boolean())
            rules_.codegen.sanitize_stderr = cg["sanitize_stderr"].get<bool>();
        if (cg.contains("max_stderr_chars") && cg["max_stderr_chars"].is_number_integer())
            rules_.codegen.max_stderr_chars = std::max(0, cg["max_stderr_chars"].get<int>());
    }

    // --- Telemetry output config ---
    if (j.contains("telemetry") && j["telemetry"].is_object()) {
        auto& tel = j["telemetry"];
        if (tel.contains("enabled")) rules_.telemetry_output.enabled = tel["enabled"].get<bool>();
        if (tel.contains("output_file")) rules_.telemetry_output.output_file = tel["output_file"].get<std::string>();
        if (tel.contains("tamper_evidence") && tel["tamper_evidence"].is_object()) {
            auto& te = tel["tamper_evidence"];
            if (te.contains("enabled")) rules_.telemetry_output.tamper_evidence.enabled = te["enabled"].get<bool>();
            if (te.contains("algorithm")) rules_.telemetry_output.tamper_evidence.algorithm = te["algorithm"].get<std::string>();
            if (te.contains("chain_genesis")) rules_.telemetry_output.tamper_evidence.chain_genesis = te["chain_genesis"].get<std::string>();
            if (te.contains("hmac_key")) rules_.telemetry_output.tamper_evidence.hmac_key = te["hmac_key"].get<std::string>();
            if (te.contains("hmac_key_env")) {
                std::string env_name = te["hmac_key_env"].get<std::string>();
                const char* env_val = std::getenv(env_name.c_str());
                if (env_val) rules_.telemetry_output.tamper_evidence.hmac_key = env_val;
            }
        }

        // Forwarding config
        if (tel.contains("webhook_url"))
            rules_.telemetry_output.webhook_url = tel["webhook_url"].get<std::string>();
        if (tel.contains("webhook_auth_header"))
            rules_.telemetry_output.webhook_auth_header = tel["webhook_auth_header"].get<std::string>();
        if (tel.contains("webhook_auth_env")) {
            std::string env_name = tel["webhook_auth_env"].get<std::string>();
            const char* env_val = std::getenv(env_name.c_str());
            if (env_val) {
                // H2: raw value — user provides full scheme ("Bearer xxx", "Basic xxx", etc.)
                std::string val(env_val);
                // H1: strip CR/LF to prevent HTTP header injection via env var
                for (auto it = val.begin(); it != val.end(); ) {
                    if (*it == '\r' || *it == '\n') it = val.erase(it);
                    else ++it;
                }
                rules_.telemetry_output.webhook_auth_header = val;
            }
        }
        if (tel.contains("forward_batch_size")) {
            int v = tel["forward_batch_size"].get<int>();
            rules_.telemetry_output.forward_batch_size = v < 1 ? 1 : v;
        }
        if (tel.contains("forward_timeout_ms")) {
            int v = tel["forward_timeout_ms"].get<int>();
            rules_.telemetry_output.forward_timeout_ms = v < 1 ? 1 : v;  // 0 = infinite in libcurl
        }
        if (tel.contains("forward_retry_count")) {
            int v = tel["forward_retry_count"].get<int>();
            rules_.telemetry_output.forward_retry_count = v < 0 ? 0 : v;
        }
        if (tel.contains("forward_buffer_max")) {
            int v = tel["forward_buffer_max"].get<int>();
            rules_.telemetry_output.forward_buffer_max = v < 0 ? 0 : v;
        }
        if (tel.contains("forward_shutdown_drain_ms")) {
            int v = tel["forward_shutdown_drain_ms"].get<int>();
            rules_.telemetry_output.forward_shutdown_drain_ms = v < 0 ? 0 : v;
        }
    }

    // --- Agents (unified config: permissions + LLM) ---
    // Prefer "agents" key; fall back to legacy "agent_roles" for backward compat
    std::string agents_key = j.contains("agents") ? "agents" : "agent_roles";
    if (j.contains(agents_key) && j[agents_key].is_object()) {
        for (auto& [name, cfg_json] : j[agents_key].items()) {
            AgentConfig agent;
            agent.name = name;

            // --- Permissions ---
            if (cfg_json.contains("allowed_languages") && cfg_json["allowed_languages"].is_array())
                for (const auto& l : cfg_json["allowed_languages"])
                    agent.allowed_languages.push_back(l.get<std::string>());
            if (cfg_json.contains("blocked_languages") && cfg_json["blocked_languages"].is_array())
                for (const auto& l : cfg_json["blocked_languages"])
                    agent.blocked_languages.push_back(l.get<std::string>());
            if (cfg_json.contains("blocked_paths") && cfg_json["blocked_paths"].is_array())
                for (const auto& p : cfg_json["blocked_paths"])
                    agent.blocked_paths.push_back(p.get<std::string>());
            if (cfg_json.contains("allowed_paths") && cfg_json["allowed_paths"].is_array())
                for (const auto& p : cfg_json["allowed_paths"])
                    agent.allowed_paths.push_back(p.get<std::string>());
            // V-GOV-018: per-agent shell capability
            if (cfg_json.contains("shell_allowed") && cfg_json["shell_allowed"].is_boolean()) {
                agent.shell_allowed = cfg_json["shell_allowed"].get<bool>();
                agent.shell_allowed_set = true;
            }
            if (!agent.shell_allowed_set && cfg_json.contains("capabilities") && cfg_json["capabilities"].is_object()) {
                auto& caps = cfg_json["capabilities"];
                if (caps.contains("shell") && caps["shell"].is_boolean()) {
                    agent.shell_allowed = caps["shell"].get<bool>();
                    agent.shell_allowed_set = true;
                } else if (caps.contains("shell") && caps["shell"].is_object()) {
                    auto& sh = caps["shell"];
                    if (sh.contains("enabled") && sh["enabled"].is_boolean()) {
                        agent.shell_allowed = sh["enabled"].get<bool>();
                        agent.shell_allowed_set = true;
                    }
                }
            }

            // Per-agent network override
            if (cfg_json.contains("network_allowed") && cfg_json["network_allowed"].is_boolean()) {
                agent.network_allowed = cfg_json["network_allowed"].get<bool>();
                agent.network_allowed_set = true;
            }
            // Fine-grained action matrix
            if (cfg_json.contains("allowed_actions") && cfg_json["allowed_actions"].is_array()) {
                for (const auto& a : cfg_json["allowed_actions"]) {
                    if (a.is_string()) agent.allowed_actions.push_back(a.get<std::string>());
                }
            }

            // --- LLM config (only present in "agents" key, not legacy "agent_roles") ---
            if (agents_key == "agents") {
                if (cfg_json.contains("provider"))
                    agent.provider = cfg_json["provider"].get<std::string>();
                // model: string or array (fallback chain)
                if (cfg_json.contains("model")) {
                    if (cfg_json["model"].is_array()) {
                        agent.model_chain.clear();
                        for (const auto& m : cfg_json["model"])
                            agent.model_chain.push_back(m.get<std::string>());
                        if (!agent.model_chain.empty())
                            agent.model = agent.model_chain[0];
                    } else {
                        agent.model = cfg_json["model"].get<std::string>();
                        agent.model_chain = {agent.model};
                    }
                }
                // api_key_env: string or array (key rotation)
                if (cfg_json.contains("api_key_env")) {
                    if (cfg_json["api_key_env"].is_array()) {
                        agent.api_key_envs.clear();
                        for (const auto& k : cfg_json["api_key_env"])
                            agent.api_key_envs.push_back(k.get<std::string>());
                        if (!agent.api_key_envs.empty())
                            agent.api_key_env = agent.api_key_envs[0];
                    } else {
                        agent.api_key_env = cfg_json["api_key_env"].get<std::string>();
                        agent.api_key_envs = {agent.api_key_env};
                    }
                }
                if (cfg_json.contains("max_tokens"))
                    agent.max_tokens = cfg_json["max_tokens"].get<int>();
                if (cfg_json.contains("system_prompt"))
                    agent.system_prompt = cfg_json["system_prompt"].get<std::string>();
                if (cfg_json.contains("tools") && cfg_json["tools"].is_array())
                    for (const auto& t : cfg_json["tools"])
                        if (t.is_string()) agent.tools.push_back(t.get<std::string>());
                // Tool execution configuration
                if (cfg_json.contains("tools_enabled") && cfg_json["tools_enabled"].is_boolean())
                    agent.tools_enabled = cfg_json["tools_enabled"].get<bool>();
                if (cfg_json.contains("max_tool_calls_per_turn"))
                    agent.max_tool_calls_per_turn = std::max(1, cfg_json["max_tool_calls_per_turn"].get<int>());
                if (cfg_json.contains("max_tool_loop_turns"))
                    agent.max_tool_loop_turns = std::max(1, cfg_json["max_tool_loop_turns"].get<int>());
                if (cfg_json.contains("tool_result_max_chars"))
                    agent.tool_result_max_chars = std::max(0, cfg_json["tool_result_max_chars"].get<int>());
                if (cfg_json.contains("tool_result_max_total_chars"))
                    agent.tool_result_max_total_chars = std::max(0, cfg_json["tool_result_max_total_chars"].get<int>());
                if (cfg_json.contains("tool_timeout_seconds"))
                    agent.tool_timeout_seconds = std::max(1, cfg_json["tool_timeout_seconds"].get<int>());
                if (cfg_json.contains("max_turns"))
                    agent.max_turns = cfg_json["max_turns"].get<int>();
                if (cfg_json.contains("max_total_tokens"))
                    agent.max_total_tokens = cfg_json["max_total_tokens"].get<int>();
                if (cfg_json.contains("temperature"))
                    agent.temperature = cfg_json["temperature"].get<double>();
                if (cfg_json.contains("stop_reason_action"))
                    agent.stop_reason_action = cfg_json["stop_reason_action"].get<std::string>();
                if (cfg_json.contains("stream"))
                    agent.stream = cfg_json["stream"].get<bool>();
                if (cfg_json.contains("timeout"))
                    agent.timeout_seconds = cfg_json["timeout"].get<int>();
                if (cfg_json.contains("response_format"))
                    agent.response_format = cfg_json["response_format"].get<std::string>();
                if (cfg_json.contains("risk_budget")) {
                    agent.risk_budget = cfg_json["risk_budget"].get<int>();
                    if (agent.risk_budget < 0) agent.risk_budget = 0;
                }
                // Standing Lease — TTL on agent authorization
                if (cfg_json.contains("standing_lease_turns"))
                    agent.standing_lease_turns = std::max(0, cfg_json["standing_lease_turns"].get<int>());
                if (cfg_json.contains("standing_lease_seconds"))
                    agent.standing_lease_seconds = std::max(0, cfg_json["standing_lease_seconds"].get<int>());
                // Retry configuration
                if (cfg_json.contains("retry") && cfg_json["retry"].is_object()) {
                    auto& r = cfg_json["retry"];
                    if (r.contains("max_attempts"))
                        agent.retry.max_attempts = std::max(1, r["max_attempts"].get<int>());
                    if (r.contains("backoff_ms"))
                        agent.retry.backoff_ms = std::max(0, r["backoff_ms"].get<int>());
                    if (r.contains("backoff_multiplier"))
                        agent.retry.backoff_multiplier = std::max(1.0, r["backoff_multiplier"].get<double>());
                    if (r.contains("jitter"))
                        agent.retry.jitter = r["jitter"].get<bool>();
                    if (r.contains("retry_on") && r["retry_on"].is_array()) {
                        agent.retry.retry_on.clear();
                        for (const auto& c : r["retry_on"]) agent.retry.retry_on.push_back(c.get<int>());
                    }
                    if (r.contains("skip_key_on") && r["skip_key_on"].is_array()) {
                        agent.retry.skip_key_on.clear();
                        for (const auto& c : r["skip_key_on"]) agent.retry.skip_key_on.push_back(c.get<int>());
                    }
                    if (r.contains("fallback_model_on") && r["fallback_model_on"].is_array()) {
                        agent.retry.fallback_model_on.clear();
                        for (const auto& c : r["fallback_model_on"]) agent.retry.fallback_model_on.push_back(c.get<int>());
                    }
                    if (r.contains("never_retry") && r["never_retry"].is_array()) {
                        agent.retry.never_retry.clear();
                        for (const auto& c : r["never_retry"]) agent.retry.never_retry.push_back(c.get<int>());
                    }
                    if (r.contains("key_retry_after_seconds"))
                        agent.retry.key_retry_after_seconds = std::max(0, r["key_retry_after_seconds"].get<int>());
                }
                // Rate limit configuration
                if (cfg_json.contains("rate_limit") && cfg_json["rate_limit"].is_object()) {
                    auto& rl = cfg_json["rate_limit"];
                    if (rl.contains("requests_per_minute"))
                        agent.rate_limit.requests_per_minute = std::max(0, rl["requests_per_minute"].get<int>());
                    if (rl.contains("delay_between_calls_ms"))
                        agent.rate_limit.delay_between_calls_ms = std::max(0, rl["delay_between_calls_ms"].get<int>());
                }
            }

            rules_.agents.push_back(agent);
        }
    }

    // --- Named Scorers ---
    if (j.contains("scorers") && j["scorers"].is_object()) {
        for (auto& [name, cfg_json] : j["scorers"].items()) {
            ScorerConfig scorer;
            scorer.name = name;
            scorer.enabled = cfg_json.value("enabled", true);
            scorer.default_weight = cfg_json.value("default_weight", 3);
            if (cfg_json.contains("rule_weights") && cfg_json["rule_weights"].is_object()) {
                for (auto& [key, val] : cfg_json["rule_weights"].items()) {
                    if (val.is_number_integer()) {
                        scorer.rule_weights[key] = val.get<int>();
                    }
                }
            }
            scorer.green_threshold = cfg_json.value("green_threshold", 0);
            scorer.yellow_threshold = cfg_json.value("yellow_threshold", 10);
            scorer.red_threshold = cfg_json.value("red_threshold", 25);
            scorer.threshold_mode = cfg_json.value("threshold_mode", "fixed");
            rules_.scorers.push_back(scorer);
        }
    }

    // --- Quality Gate (Feature 2) ---
    if (j.contains("quality_gate") && j["quality_gate"].is_object()) {
        auto& qg = j["quality_gate"];
        if (qg.contains("enabled")) rules_.quality_gate.enabled = qg["enabled"].get<bool>();
        if (qg.contains("conditions") && qg["conditions"].is_array()) {
            for (const auto& cond : qg["conditions"]) {
                QualityGateCondition c;
                if (cond.contains("metric")) c.metric = cond["metric"].get<std::string>();
                if (cond.contains("operator")) c.op = cond["operator"].get<std::string>();
                if (cond.contains("threshold")) c.threshold = cond["threshold"].get<int>();
                rules_.quality_gate.conditions.push_back(c);
            }
        }
    }

    // --- Cumulative Risk Scoring ---
    if (j.contains("scoring") && j["scoring"].is_object()) {
        auto& sc = j["scoring"];
        rules_.scoring.enabled = sc.value("enabled", false);
        rules_.scoring.default_weight = sc.value("default_weight", 3);
        if (sc.contains("rule_weights") && sc["rule_weights"].is_object()) {
            for (auto& [key, val] : sc["rule_weights"].items()) {
                if (val.is_number_integer()) {
                    int w = val.get<int>();
                    if (w < 0) {
                        fmt::print(stderr, "[WARN] scoring.rule_weights.{} is negative ({}) — "
                                   "will be clamped to 0\n", key, w);
                    }
                    rules_.scoring.rule_weights[key] = w;
                }
            }
        }
        rules_.scoring.green_threshold  = sc.value("green_threshold", 0);
        rules_.scoring.yellow_threshold = sc.value("yellow_threshold", 10);
        rules_.scoring.red_threshold    = sc.value("red_threshold", 25);
        rules_.scoring.threshold_mode   = sc.value("threshold_mode", "fixed");
        parseRationale(sc, rules_.scoring.rationale);
        if (rules_.scoring.yellow_threshold > rules_.scoring.red_threshold) {
            fmt::print(stderr, "[WARN] scoring.yellow_threshold ({}) > red_threshold ({}) — "
                       "yellow warnings will never appear\n",
                       rules_.scoring.yellow_threshold, rules_.scoring.red_threshold);
        }
    }

    // --- Agent Review (LLM-based governance phase) ---
    if (j.contains("agent_review") && j["agent_review"].is_object()) {
        auto& ar = j["agent_review"];
        rules_.agent_review.enabled = ar.value("enabled", false);
        rules_.agent_review.scorer = ar.value("scorer", "");
        rules_.agent_review.validation = ar.value("validation", "");
        rules_.agent_review.voice = ar.value("voice", "");
        rules_.agent_review.cache = ar.value("cache", false);
        rules_.agent_review.hints = ar.value("hints", false);
        rules_.agent_review.fail_policy = ar.value("fail_policy", "open");
        rules_.agent_review.dispatch_mode = ar.value("dispatch_mode", "sequential");
        rules_.agent_review.fail_strategy = ar.value("fail_strategy", "fail_fast");
        if (ar.contains("max_parallel")) {
            int mp = ar["max_parallel"].get<int>();
            rules_.agent_review.max_parallel = (mp < 0) ? 0 : mp;
        }
        if (ar.contains("detection") && ar["detection"].is_array()) {
            for (const auto& d : ar["detection"]) {
                if (d.is_string()) rules_.agent_review.detection.push_back(d.get<std::string>());
            }
        }
        if (ar.contains("enforcement") && ar["enforcement"].is_object()) {
            for (auto& [zone, level] : ar["enforcement"].items()) {
                if (level.is_string()) rules_.agent_review.enforcement[zone] = level.get<std::string>();
            }
        }
    }

    // --- Agent Dispatch (parallel agent execution config) ---
    if (j.contains("agent_dispatch") && j["agent_dispatch"].is_object()) {
        auto& ad = j["agent_dispatch"];
        if (ad.contains("max_concurrent"))
            rules_.agent_dispatch.max_concurrent = ad["max_concurrent"].get<int>();
        if (ad.contains("pool_size"))
            rules_.agent_dispatch.pool_size = ad["pool_size"].get<int>();
        if (ad.contains("pool_queue_max"))
            rules_.agent_dispatch.pool_queue_max = ad["pool_queue_max"].get<int>();
        if (ad.contains("max_retries_per_run"))
            rules_.agent_dispatch.max_retries_per_run = std::max(0, ad["max_retries_per_run"].get<int>());
        if (ad.contains("hard_stop") && ad["hard_stop"].is_object()) {
            auto& hs = ad["hard_stop"];
            if (hs.contains("max_calls_per_run"))
                rules_.agent_dispatch.hard_stop.max_calls_per_run = std::max(0, hs["max_calls_per_run"].get<int>());
            if (hs.contains("max_tokens_per_run"))
                rules_.agent_dispatch.hard_stop.max_tokens_per_run = std::max(0, hs["max_tokens_per_run"].get<int>());
            if (hs.contains("max_agent_time_ms"))
                rules_.agent_dispatch.hard_stop.max_agent_time_ms = std::max(0, hs["max_agent_time_ms"].get<int>());
            if (hs.contains("consecutive_failure_limit"))
                rules_.agent_dispatch.hard_stop.consecutive_failure_limit = std::max(0, hs["consecutive_failure_limit"].get<int>());
            if (hs.contains("action"))
                rules_.agent_dispatch.hard_stop.action = hs["action"].get<std::string>();
        }
    }

    // --- Behavioral Sequence Detection ---
    if (j.contains("behavioral_sequences") && j["behavioral_sequences"].is_object()) {
        auto& bs = j["behavioral_sequences"];
        auto& cfg = rules_.behavioral_sequences;
        if (bs.contains("enabled")) cfg.enabled = bs["enabled"].get<bool>();
        if (bs.contains("window_size")) cfg.window_size = bs["window_size"].get<size_t>();
        parseRationale(bs, cfg.rationale);
        if (bs.contains("patterns") && bs["patterns"].is_array()) {
            for (auto& pat : bs["patterns"]) {
                SequencePattern sp;
                sp.name = pat.value("name", "");
                sp.max_gap = pat.value("max_gap", 10);
                sp.decay_seconds = pat.value("decay_seconds", 300);
                sp.decay_turns = pat.value("decay_turns", 20);
                if (pat.contains("level")) {
                    auto [en, lv] = parseEnforcementLevel(pat["level"]);
                    sp.level = lv;
                } else {
                    sp.level = EnforcementLevel::SOFT;
                }
                if (pat.contains("rationale")) sp.rationale = pat["rationale"].get<std::string>();
                if (pat.contains("cross_agent")) sp.cross_agent = pat["cross_agent"].get<bool>();
                if (pat.contains("sequence") && pat["sequence"].is_array()) {
                    for (auto& step_str : pat["sequence"]) {
                        SequenceStep step;
                        std::string s = step_str.get<std::string>();
                        // Split by '|' first, then ':' within each token
                        // Allows "env.get:*KEY*|env.get:*SECRET*" (per-matcher detail globs)
                        std::istringstream iss(s);
                        std::string token;
                        while (std::getline(iss, token, '|')) {
                            if (token.empty()) continue;
                            auto colon = token.find(':');
                            if (colon != std::string::npos) {
                                step.match_any.push_back(token.substr(0, colon));
                                step.detail_globs.push_back(token.substr(colon + 1));
                            } else {
                                step.match_any.push_back(token);
                                step.detail_globs.push_back("");
                            }
                        }
                        sp.steps.push_back(std::move(step));
                    }
                }
                cfg.patterns.push_back(std::move(sp));
            }
        }
    }

    // --- Context Drift Detection ---
    if (j.contains("context_drift") && j["context_drift"].is_object()) {
        auto& cd = j["context_drift"];
        auto& cfg = rules_.context_drift;
        if (cd.contains("enabled")) cfg.enabled = cd["enabled"].get<bool>();
        if (cd.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(cd["level"]);
            cfg.level = lv;
        }
        if (cd.contains("coherence_threshold")) {
            cfg.coherence_threshold = cd["coherence_threshold"].get<double>();
            cfg.coherence_threshold = std::max(0.0, std::min(1.0, cfg.coherence_threshold));
        }
        if (cd.contains("max_contradictions")) cfg.max_contradictions = cd["max_contradictions"].get<int>();
        if (cd.contains("check_interval_turns")) cfg.check_interval_turns = cd["check_interval_turns"].get<int>();
        if (cd.contains("fingerprint_window")) cfg.fingerprint_window = cd["fingerprint_window"].get<int>();
        if (cd.contains("rate_normalized")) cfg.rate_normalized = cd["rate_normalized"].get<bool>();
        if (cd.contains("coherence_recovery_amount")) cfg.coherence_recovery_amount = cd["coherence_recovery_amount"].get<double>();
        if (cd.contains("coherence_natural_healing")) cfg.coherence_natural_healing = cd["coherence_natural_healing"].get<double>();
        if (cd.contains("temporal_decay_enabled")) cfg.temporal_decay_enabled = cd["temporal_decay_enabled"].get<bool>();
        if (cd.contains("temporal_decay_per_minute")) cfg.temporal_decay_per_minute = std::max(0.0, cd["temporal_decay_per_minute"].get<double>());
        if (cd.contains("temporal_decay_grace_minutes")) cfg.temporal_decay_grace_minutes = std::max(0.0, cd["temporal_decay_grace_minutes"].get<double>());
        if (cd.contains("adaptive_baseline_enabled")) cfg.adaptive_baseline_enabled = cd["adaptive_baseline_enabled"].get<bool>();
        if (cd.contains("adaptive_baseline_window")) cfg.adaptive_baseline_window = std::max(1, cd["adaptive_baseline_window"].get<int>());
        if (cd.contains("adaptive_baseline_sensitivity")) cfg.adaptive_baseline_sensitivity = std::max(0.0, cd["adaptive_baseline_sensitivity"].get<double>());
        parseRationale(cd, cfg.rationale);
        if (cd.contains("signals") && cd["signals"].is_object()) {
            auto& sig = cd["signals"];
            if (sig.contains("repeated_failures")) cfg.signals.repeated_failures = sig["repeated_failures"].get<bool>();
            if (sig.contains("circular_actions")) cfg.signals.circular_actions = sig["circular_actions"].get<bool>();
            if (sig.contains("scope_creep")) cfg.signals.scope_creep = sig["scope_creep"].get<bool>();
            if (sig.contains("intent_contradictions")) cfg.signals.intent_contradictions = sig["intent_contradictions"].get<bool>();
            if (sig.contains("vocabulary_contraction")) cfg.signals.vocabulary_contraction = sig["vocabulary_contraction"].get<bool>();
            if (sig.contains("coherence_velocity")) cfg.signals.coherence_velocity = sig["coherence_velocity"].get<bool>();
            if (sig.contains("capability_underutilization")) cfg.signals.capability_underutilization = sig["capability_underutilization"].get<bool>();
            if (sig.contains("semantic_stability")) cfg.signals.semantic_stability = sig["semantic_stability"].get<bool>();
        }
        if (cd.contains("weights") && cd["weights"].is_object()) {
            auto& w = cd["weights"];
            if (w.contains("circular")) cfg.weights.circular = w["circular"].get<double>();
            if (w.contains("scope_creep")) cfg.weights.scope_creep = w["scope_creep"].get<double>();
            if (w.contains("contradiction")) cfg.weights.contradiction = w["contradiction"].get<double>();
            if (w.contains("repeated_failure")) cfg.weights.repeated_failure = w["repeated_failure"].get<double>();
            if (w.contains("vocabulary_contraction")) cfg.weights.vocabulary_contraction = w["vocabulary_contraction"].get<double>();
            if (w.contains("coherence_velocity")) cfg.weights.coherence_velocity = w["coherence_velocity"].get<double>();
            if (w.contains("capability_underutilization")) cfg.weights.capability_underutilization = w["capability_underutilization"].get<double>();
            if (w.contains("semantic_stability")) cfg.weights.semantic_stability = w["semantic_stability"].get<double>();
        }
        if (cd.contains("reality_checkpoint") && cd["reality_checkpoint"].is_object()) {
            auto& rc = cd["reality_checkpoint"];
            auto& rccfg = cfg.reality_checkpoint;
            if (rc.contains("enabled")) rccfg.enabled = rc["enabled"].get<bool>();
            if (rc.contains("level")) {
                auto [en, lv] = parseEnforcementLevel(rc["level"]);
                rccfg.level = lv;
            }
            if (rc.contains("pressure_threshold")) rccfg.pressure_threshold = rc["pressure_threshold"].get<double>();
            if (rc.contains("sustained_turns_required")) rccfg.sustained_turns_required = rc["sustained_turns_required"].get<int>();
            if (rc.contains("min_turns_between_checkpoints")) rccfg.min_turns_between_checkpoints = rc["min_turns_between_checkpoints"].get<int>();
            if (rc.contains("expected_conversation_depth")) rccfg.expected_conversation_depth = rc["expected_conversation_depth"].get<int>();
            parseRationale(rc, rccfg.rationale);
            if (rc.contains("weights") && rc["weights"].is_object()) {
                auto& rw = rc["weights"];
                if (rw.contains("coherence_proximity")) rccfg.weights.coherence_proximity = rw["coherence_proximity"].get<double>();
                if (rw.contains("risk_score_proximity")) rccfg.weights.risk_score_proximity = rw["risk_score_proximity"].get<double>();
                if (rw.contains("signal_density")) rccfg.weights.signal_density = rw["signal_density"].get<double>();
                if (rw.contains("conversation_depth")) rccfg.weights.conversation_depth = rw["conversation_depth"].get<double>();
                if (rw.contains("bsd_partial_progress")) rccfg.weights.bsd_partial_progress = rw["bsd_partial_progress"].get<double>();
                if (rw.contains("pipeline_inherited")) rccfg.weights.pipeline_inherited = rw["pipeline_inherited"].get<double>();
                if (rw.contains("coherence_acceleration")) rccfg.weights.coherence_acceleration = rw["coherence_acceleration"].get<double>();
                if (rw.contains("codegen_pressure")) rccfg.weights.codegen_pressure = rw["codegen_pressure"].get<double>();
                if (rw.contains("bsd_eviction_pressure")) rccfg.weights.bsd_eviction_pressure = rw["bsd_eviction_pressure"].get<double>();
            }
        }
    }

    // --- Exposure Tracking ---
    if (j.contains("exposure_tracking") && j["exposure_tracking"].is_object()) {
        auto& et = j["exposure_tracking"];
        auto& cfg = rules_.exposure_tracking;
        if (et.contains("enabled")) cfg.enabled = et["enabled"].get<bool>();
        if (et.contains("max_autonomous_actions")) {
            cfg.max_autonomous_actions = et["max_autonomous_actions"].get<int>();
            if (cfg.max_autonomous_actions < 0) cfg.max_autonomous_actions = 0;
        }
        if (et.contains("max_unique_agents")) cfg.max_unique_agents = et["max_unique_agents"].get<int>();
        if (et.contains("coherence_floor")) cfg.coherence_floor = et["coherence_floor"].get<double>();
        if (et.contains("max_pipeline_depth")) {
            cfg.max_pipeline_depth = et["max_pipeline_depth"].get<int>();
            if (cfg.max_pipeline_depth < 0) cfg.max_pipeline_depth = 0;
        }
        if (et.contains("checkpoint_cooldown_turns")) {
            cfg.checkpoint_cooldown_turns = et["checkpoint_cooldown_turns"].get<int>();
            if (cfg.checkpoint_cooldown_turns < 0) cfg.checkpoint_cooldown_turns = 0;
        }
        if (et.contains("min_capability_utilization")) cfg.min_capability_utilization = et["min_capability_utilization"].get<double>();
        if (et.contains("utilization_check_after_turns")) cfg.utilization_check_after_turns = et["utilization_check_after_turns"].get<int>();
        if (et.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(et["level"]);
            cfg.level = lv;
        }
        parseRationale(et, cfg.rationale);
    }

    // --- Temporal Coupling (F10) ---
    if (j.contains("temporal_coupling") && j["temporal_coupling"].is_object()) {
        auto& tc = j["temporal_coupling"];
        auto& cfg = rules_.temporal_coupling;
        if (tc.contains("enabled")) cfg.enabled = tc["enabled"].get<bool>();
        if (tc.contains("max_correlation")) cfg.max_correlation = tc["max_correlation"].get<double>();
        if (tc.contains("min_events")) cfg.min_events = tc["min_events"].get<int>();
        parseRationale(tc, cfg.rationale);
    }

    // --- Circuit Breaker (F6) ---
    if (j.contains("circuit_breaker") && j["circuit_breaker"].is_object()) {
        auto& cbj = j["circuit_breaker"];
        auto& cfg = rules_.circuit_breaker;
        if (cbj.contains("enabled")) cfg.enabled = cbj["enabled"].get<bool>();
        if (cbj.contains("elevated_threshold")) cfg.elevated_threshold = cbj["elevated_threshold"].get<double>();
        if (cbj.contains("high_threshold")) cfg.high_threshold = cbj["high_threshold"].get<double>();
        if (cbj.contains("critical_threshold")) cfg.critical_threshold = cbj["critical_threshold"].get<double>();
        if (cbj.contains("elevated_sustained")) cfg.elevated_sustained = cbj["elevated_sustained"].get<int>();
        if (cbj.contains("high_sustained")) cfg.high_sustained = cbj["high_sustained"].get<int>();
        if (cbj.contains("critical_sustained")) cfg.critical_sustained = cbj["critical_sustained"].get<int>();
        if (cbj.contains("step_up_enabled")) cfg.step_up_enabled = cbj["step_up_enabled"].get<bool>();
        if (cbj.contains("step_up_at_level")) cfg.step_up_at_level = cbj["step_up_at_level"].get<std::string>();
        if (cbj.contains("step_up_challenge")) cfg.step_up_challenge = cbj["step_up_challenge"].get<std::string>();
        if (cbj.contains("step_up_min_words")) cfg.step_up_min_words = std::max(1, cbj["step_up_min_words"].get<int>());
        if (cbj.contains("step_up_cooldown_turns")) cfg.step_up_cooldown_turns = std::max(0, cbj["step_up_cooldown_turns"].get<int>());
        if (cbj.contains("step_up_keyword_threshold")) cfg.step_up_keyword_threshold = std::max(0.0, std::min(1.0, cbj["step_up_keyword_threshold"].get<double>()));
        parseRationale(cbj, cfg.rationale);
    }

    // --- Advisory Escalation ---
    if (j.contains("advisory_escalation") && j["advisory_escalation"].is_object()) {
        auto& ae = j["advisory_escalation"];
        auto& cfg = rules_.advisory_escalation;
        if (ae.contains("enabled")) cfg.enabled = ae["enabled"].get<bool>();
        if (ae.contains("soft_after")) cfg.soft_after = std::max(2, ae["soft_after"].get<int>());
        if (ae.contains("weight_multiplier")) cfg.weight_multiplier = std::max(1.0, ae["weight_multiplier"].get<double>());
        parseRationale(ae, cfg.rationale);
    }

    // --- Governance Health (F4) ---
    if (j.contains("governance_health") && j["governance_health"].is_object()) {
        auto& gh = j["governance_health"];
        auto& cfg = rules_.governance_health;
        if (gh.contains("enabled")) cfg.enabled = gh["enabled"].get<bool>();
        if (gh.contains("check_after_turns")) cfg.check_after_turns = gh["check_after_turns"].get<int>();
        if (gh.contains("governance_entropy_warning")) cfg.governance_entropy_warning = gh["governance_entropy_warning"].get<double>();
        parseRationale(gh, cfg.rationale);
    }

    // --- Pipeline Separation (F7) ---
    if (j.contains("pipeline_separation") && j["pipeline_separation"].is_object()) {
        auto& ps = j["pipeline_separation"];
        auto& cfg = rules_.pipeline_separation;
        if (ps.contains("enabled")) cfg.enabled = ps["enabled"].get<bool>();
        if (ps.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(ps["level"]);
            cfg.level = lv;
        }
        parseRationale(ps, cfg.rationale);
    }

    // --- Governance Baseline (Feature 4) ---
    if (j.contains("governance_baseline") && j["governance_baseline"].is_object()) {
        auto& gb = j["governance_baseline"];
        if (gb.contains("enabled")) rules_.governance_baseline.enabled = gb["enabled"].get<bool>();
        if (gb.contains("path")) rules_.governance_baseline.path = gb["path"].get<std::string>();
        if (gb.contains("fail_on_regression")) rules_.governance_baseline.fail_on_regression = gb["fail_on_regression"].get<bool>();
        if (gb.contains("level")) {
            auto lvl = gb["level"].get<std::string>();
            if (lvl == "hard") rules_.governance_baseline.level = EnforcementLevel::HARD;
            else if (lvl == "soft") rules_.governance_baseline.level = EnforcementLevel::SOFT;
            else rules_.governance_baseline.level = EnforcementLevel::ADVISORY;
        }
    }

    // --- Environment Overrides (Feature 5) ---
    if (j.contains("environments") && j["environments"].is_object()) {
        for (auto& [env_name, env_json] : j["environments"].items()) {
            if (!env_json.is_object()) continue;
            std::unordered_map<std::string, std::string> overrides;
            for (auto& [key, val] : env_json.items()) {
                overrides[key] = val.is_string() ? val.get<std::string>() : val.dump();
            }
            rules_.environments[env_name] = overrides;
        }
    }

    // --- Runtime Version Pinning (Phase 8.4) ---
    if (j.contains("runtime_versions") && j["runtime_versions"].is_array()) {
        for (const auto& pin_json : j["runtime_versions"]) {
            GovernanceRules::RuntimeVersionPin pin;
            if (pin_json.contains("language"))
                pin.language = pin_json["language"].get<std::string>();
            if (pin_json.contains("required"))
                pin.required_version = pin_json["required"].get<std::string>();
            if (pin_json.contains("message"))
                pin.message = pin_json["message"].get<std::string>();
            if (pin_json.contains("level")) {
                auto lvl = pin_json["level"].get<std::string>();
                if (lvl == "hard") pin.level = EnforcementLevel::HARD;
                else if (lvl == "soft") pin.level = EnforcementLevel::SOFT;
                else pin.level = EnforcementLevel::ADVISORY;
            }
            if (!pin.language.empty() && !pin.required_version.empty())
                rules_.runtime_versions.push_back(pin);
        }
    }
}

// ============================================================================
// Governance Under Survivability — Helpers and Ratchet Comparator
// ============================================================================

// Static helper: enforce minimum enforcement levels on arbitrary rules
static void enforceMinimumLevelsOnRules(GovernanceRules& rules) {
    // F3: no_secrets and no_pii always minimum SOFT regardless of mode
    if (rules.code_quality.no_secrets.enabled &&
        rules.code_quality.no_secrets.level < EnforcementLevel::SOFT) {
        rules.code_quality.no_secrets.level = EnforcementLevel::SOFT;
    }
    if (rules.code_quality.no_pii.enabled &&
        rules.code_quality.no_pii.level < EnforcementLevel::SOFT) {
        rules.code_quality.no_pii.level = EnforcementLevel::SOFT;
    }

    // Helper: silently elevate advisory to soft for anti-evasion checks
    auto elevate = [](auto& cfg, const char* /*name*/) {
        if (cfg.enabled && cfg.level == EnforcementLevel::ADVISORY) {
            cfg.level = EnforcementLevel::SOFT;
        }
    };

    elevate(rules.code_quality.no_oversimplification, "no_oversimplification");
    elevate(rules.code_quality.no_incomplete_logic, "no_incomplete_logic");
    elevate(rules.code_quality.no_simulation_markers, "no_simulation_markers");
    elevate(rules.code_quality.no_temporary_code, "no_temporary_code");
    elevate(rules.code_quality.no_apologetic_language, "no_apologetic_language");
}

// Helper: convert EnforcementLevel to string (local version to avoid private access)
static const char* ratchetLevelStr(EnforcementLevel level) {
    switch (level) {
        case EnforcementLevel::ADVISORY: return "advisory";
        case EnforcementLevel::SOFT:     return "soft";
        case EnforcementLevel::HARD:     return "hard";
        default:                         return "unknown";
    }
}

// Helper: convert EnforcementLevel to int for comparison (higher = stricter)
static int levelToInt(EnforcementLevel level) {
    switch (level) {
        case EnforcementLevel::ADVISORY: return 1;
        case EnforcementLevel::SOFT:     return 2;
        case EnforcementLevel::HARD:     return 3;
        default:                         return 0;
    }
}

// Check that new_r is at least as strict as old_r. Returns true if ratchet OK.
// Populates violations (loosening attempts) and notices (tightening changes).
static bool checkRatchetViolation(
    const GovernanceRules& old_r,
    const GovernanceRules& new_r,
    std::vector<std::string>& violations,
    std::vector<std::string>& notices)
{
    // --- A. Numeric limits (lower = stricter, 0 = unlimited) ---
    auto chkLimit = [&](int old_v, int new_v, const char* name) {
        if (old_v > 0 && (new_v == 0 || new_v > old_v)) {
            violations.push_back(fmt::format("{}: {} -> {} (loosened)", name, old_v, new_v));
        } else if (new_v > 0 && (old_v == 0 || new_v < old_v)) {
            notices.push_back(fmt::format("{}: {} -> {} (tightened)", name,
                old_v == 0 ? "unlimited" : std::to_string(old_v), new_v));
        }
    };

    // Timeout limits
    chkLimit(old_r.limits.timeout.global, new_r.limits.timeout.global, "limits.timeout.global");
    chkLimit(old_r.limits.timeout.per_block, new_r.limits.timeout.per_block, "limits.timeout.per_block");
    chkLimit(old_r.limits.timeout.total_polyglot, new_r.limits.timeout.total_polyglot, "limits.timeout.total_polyglot");
    // Memory limits
    chkLimit(old_r.limits.memory.per_block_mb, new_r.limits.memory.per_block_mb, "limits.memory.per_block_mb");
    chkLimit(old_r.limits.memory.total_mb, new_r.limits.memory.total_mb, "limits.memory.total_mb");
    // Execution limits
    chkLimit(old_r.limits.execution.call_depth, new_r.limits.execution.call_depth, "limits.execution.call_depth");
    chkLimit(old_r.limits.execution.loop_iterations, new_r.limits.execution.loop_iterations, "limits.execution.loop_iterations");
    chkLimit(old_r.limits.execution.polyglot_blocks, new_r.limits.execution.polyglot_blocks, "limits.execution.polyglot_blocks");
    chkLimit(old_r.limits.execution.parallel_blocks, new_r.limits.execution.parallel_blocks, "limits.execution.parallel_blocks");
    chkLimit(old_r.limits.execution.total_executions, new_r.limits.execution.total_executions, "limits.execution.total_executions");
    // Data limits
    chkLimit(old_r.limits.data.array_size, new_r.limits.data.array_size, "limits.data.array_size");
    chkLimit(old_r.limits.data.dict_size, new_r.limits.data.dict_size, "limits.data.dict_size");
    chkLimit(old_r.limits.data.string_length, new_r.limits.data.string_length, "limits.data.string_length");
    chkLimit(old_r.limits.data.nesting_depth, new_r.limits.data.nesting_depth, "limits.data.nesting_depth");
    chkLimit(old_r.limits.data.output_size, new_r.limits.data.output_size, "limits.data.output_size");
    chkLimit(old_r.limits.data.input_size, new_r.limits.data.input_size, "limits.data.input_size");
    // Code limits
    chkLimit(old_r.limits.code.max_lines_per_block, new_r.limits.code.max_lines_per_block, "limits.code.max_lines_per_block");
    chkLimit(old_r.limits.code.max_total_polyglot_lines, new_r.limits.code.max_total_polyglot_lines, "limits.code.max_total_polyglot_lines");
    chkLimit(old_r.limits.code.max_nesting_depth, new_r.limits.code.max_nesting_depth, "limits.code.max_nesting_depth");
    // Rate limits
    chkLimit(old_r.limits.rate.max_polyglot_per_second, new_r.limits.rate.max_polyglot_per_second, "limits.rate.max_polyglot_per_second");
    chkLimit(old_r.limits.rate.max_stdlib_calls_per_second, new_r.limits.rate.max_stdlib_calls_per_second, "limits.rate.max_stdlib_calls_per_second");
    chkLimit(old_r.limits.rate.max_file_ops_per_second, new_r.limits.rate.max_file_ops_per_second, "limits.rate.max_file_ops_per_second");

    // --- B. Capabilities (false/disabled = stricter) ---
    auto chkCap = [&](bool old_v, bool new_v, const char* name) {
        if (!old_v && new_v) {
            violations.push_back(fmt::format("{}: disabled -> enabled (loosened)", name));
        } else if (old_v && !new_v) {
            notices.push_back(fmt::format("{}: enabled -> disabled (revoked)", name));
        }
    };

    chkCap(old_r.capabilities.network.enabled, new_r.capabilities.network.enabled, "capabilities.network.enabled");
    chkCap(old_r.capabilities.shell.enabled, new_r.capabilities.shell.enabled, "capabilities.shell.enabled");
    chkCap(old_r.capabilities.filesystem.allow_symlinks, new_r.capabilities.filesystem.allow_symlinks, "capabilities.filesystem.allow_symlinks");
    chkCap(old_r.capabilities.filesystem.allow_hidden_files, new_r.capabilities.filesystem.allow_hidden_files, "capabilities.filesystem.allow_hidden_files");
    chkCap(old_r.capabilities.filesystem.allow_absolute_paths, new_r.capabilities.filesystem.allow_absolute_paths, "capabilities.filesystem.allow_absolute_paths");
    chkCap(old_r.capabilities.shell.allow_pipes, new_r.capabilities.shell.allow_pipes, "capabilities.shell.allow_pipes");
    chkCap(old_r.capabilities.shell.allow_redirects, new_r.capabilities.shell.allow_redirects, "capabilities.shell.allow_redirects");

    // Filesystem mode: "none" < "read" < "write" (lower = stricter)
    auto fsModeRank = [](const std::string& m) -> int {
        if (m == "none") return 0;
        if (m == "read") return 1;
        return 2; // "write"
    };
    int old_fs = fsModeRank(old_r.capabilities.filesystem.mode);
    int new_fs = fsModeRank(new_r.capabilities.filesystem.mode);
    if (new_fs > old_fs) {
        violations.push_back(fmt::format("capabilities.filesystem.mode: {} -> {} (loosened)",
            old_r.capabilities.filesystem.mode, new_r.capabilities.filesystem.mode));
    } else if (new_fs < old_fs) {
        notices.push_back(fmt::format("capabilities.filesystem.mode: {} -> {} (tightened)",
            old_r.capabilities.filesystem.mode, new_r.capabilities.filesystem.mode));
    }

    // --- C. Enforcement levels (HARD > SOFT > ADVISORY) ---
    auto chkLevel = [&](EnforcementLevel old_v, EnforcementLevel new_v, const char* name) {
        int oi = levelToInt(old_v), ni = levelToInt(new_v);
        if (ni < oi) {
            violations.push_back(fmt::format("{}: {} -> {} (lowered)", name,
                ratchetLevelStr(old_v), ratchetLevelStr(new_v)));
        } else if (ni > oi) {
            notices.push_back(fmt::format("{}: {} -> {} (elevated)", name,
                ratchetLevelStr(old_v), ratchetLevelStr(new_v)));
        }
    };

    // Restrictions enforcement levels
    chkLevel(old_r.restrictions.dangerous_calls.level, new_r.restrictions.dangerous_calls.level, "restrictions.dangerous_calls.level");
    chkLevel(old_r.restrictions.shell_injection.level, new_r.restrictions.shell_injection.level, "restrictions.shell_injection.level");
    chkLevel(old_r.restrictions.code_injection.level, new_r.restrictions.code_injection.level, "restrictions.code_injection.level");
    chkLevel(old_r.restrictions.privilege_escalation.level, new_r.restrictions.privilege_escalation.level, "restrictions.privilege_escalation.level");
    chkLevel(old_r.restrictions.data_exfiltration.level, new_r.restrictions.data_exfiltration.level, "restrictions.data_exfiltration.level");
    chkLevel(old_r.restrictions.resource_abuse.level, new_r.restrictions.resource_abuse.level, "restrictions.resource_abuse.level");
    // Code quality enforcement levels
    chkLevel(old_r.code_quality.no_secrets.level, new_r.code_quality.no_secrets.level, "code_quality.no_secrets.level");
    chkLevel(old_r.code_quality.no_placeholders.level, new_r.code_quality.no_placeholders.level, "code_quality.no_placeholders.level");
    chkLevel(old_r.code_quality.no_oversimplification.level, new_r.code_quality.no_oversimplification.level, "code_quality.no_oversimplification.level");
    chkLevel(old_r.code_quality.no_incomplete_logic.level, new_r.code_quality.no_incomplete_logic.level, "code_quality.no_incomplete_logic.level");
    chkLevel(old_r.code_quality.no_simulation_markers.level, new_r.code_quality.no_simulation_markers.level, "code_quality.no_simulation_markers.level");

    // --- D. Boolean restrictions (true = stricter) ---
    auto chkRestrict = [&](bool old_v, bool new_v, const char* name) {
        if (old_v && !new_v) {
            violations.push_back(fmt::format("{}: enabled -> disabled (loosened)", name));
        } else if (!old_v && new_v) {
            notices.push_back(fmt::format("{}: disabled -> enabled (tightened)", name));
        }
    };

    chkRestrict(old_r.restrictions.dangerous_calls.enabled, new_r.restrictions.dangerous_calls.enabled, "restrictions.dangerous_calls.enabled");
    chkRestrict(old_r.restrictions.shell_injection.enabled, new_r.restrictions.shell_injection.enabled, "restrictions.shell_injection.enabled");
    chkRestrict(old_r.restrictions.code_injection.enabled, new_r.restrictions.code_injection.enabled, "restrictions.code_injection.enabled");
    chkRestrict(old_r.restrictions.privilege_escalation.enabled, new_r.restrictions.privilege_escalation.enabled, "restrictions.privilege_escalation.enabled");
    chkRestrict(old_r.restrictions.data_exfiltration.enabled, new_r.restrictions.data_exfiltration.enabled, "restrictions.data_exfiltration.enabled");
    chkRestrict(old_r.restrictions.resource_abuse.enabled, new_r.restrictions.resource_abuse.enabled, "restrictions.resource_abuse.enabled");
    chkRestrict(old_r.code_quality.no_secrets.enabled, new_r.code_quality.no_secrets.enabled, "code_quality.no_secrets.enabled");
    chkRestrict(old_r.code_quality.no_placeholders.enabled, new_r.code_quality.no_placeholders.enabled, "code_quality.no_placeholders.enabled");
    chkRestrict(old_r.code_quality.no_oversimplification.enabled, new_r.code_quality.no_oversimplification.enabled, "code_quality.no_oversimplification.enabled");
    chkRestrict(old_r.code_quality.no_incomplete_logic.enabled, new_r.code_quality.no_incomplete_logic.enabled, "code_quality.no_incomplete_logic.enabled");
    chkRestrict(old_r.code_quality.no_simulation_markers.enabled, new_r.code_quality.no_simulation_markers.enabled, "code_quality.no_simulation_markers.enabled");
    chkRestrict(old_r.taint_tracking.enabled, new_r.taint_tracking.enabled, "taint_tracking.enabled");

    // --- E. Per-agent config changes ---
    auto agentByName = [](const std::vector<AgentConfig>& agents, const std::string& n)
        -> const AgentConfig* {
        for (const auto& a : agents) {
            if (a.name == n) return &a;
        }
        return nullptr;
    };

    for (const auto& new_agent : new_r.agents) {
        const auto* old_agent = agentByName(old_r.agents, new_agent.name);
        if (!old_agent) {
            notices.push_back(fmt::format("agent.{}: new agent config added", new_agent.name));
            continue;
        }

        // Tightened limits → notices
        if (new_agent.max_turns < old_agent->max_turns)
            notices.push_back(fmt::format("agent.{}.max_turns: {} -> {} (reduced)",
                new_agent.name, old_agent->max_turns, new_agent.max_turns));
        if (new_agent.max_total_tokens < old_agent->max_total_tokens && new_agent.max_total_tokens > 0)
            notices.push_back(fmt::format("agent.{}.max_total_tokens: {} -> {} (reduced)",
                new_agent.name, old_agent->max_total_tokens, new_agent.max_total_tokens));
        if (new_agent.max_tokens < old_agent->max_tokens)
            notices.push_back(fmt::format("agent.{}.max_tokens: {} -> {} (reduced)",
                new_agent.name, old_agent->max_tokens, new_agent.max_tokens));
        if (new_agent.risk_budget > 0 && new_agent.risk_budget < old_agent->risk_budget)
            notices.push_back(fmt::format("agent.{}.risk_budget: {} -> {} (reduced)",
                new_agent.name, old_agent->risk_budget, new_agent.risk_budget));
        if (new_agent.timeout_seconds < old_agent->timeout_seconds)
            notices.push_back(fmt::format("agent.{}.timeout_seconds: {} -> {} (reduced)",
                new_agent.name, old_agent->timeout_seconds, new_agent.timeout_seconds));
        if (new_agent.temperature != old_agent->temperature)
            notices.push_back(fmt::format("agent.{}.temperature: {:.1f} -> {:.1f}",
                new_agent.name, old_agent->temperature, new_agent.temperature));

        // Model chain changes
        if (new_agent.model_chain != old_agent->model_chain || new_agent.model != old_agent->model)
            notices.push_back(fmt::format("agent.{}.model: chain updated", new_agent.name));

        // Key pool changes
        if (new_agent.api_key_envs.size() != old_agent->api_key_envs.size())
            notices.push_back(fmt::format("agent.{}.api_key_env: pool size {} -> {}",
                new_agent.name, old_agent->api_key_envs.size(), new_agent.api_key_envs.size()));

        // Loosened limits → violations (ratchet enforcement)
        if (new_agent.max_turns > old_agent->max_turns && old_agent->max_turns > 0)
            violations.push_back(fmt::format("agent.{}.max_turns: {} -> {} (loosened)",
                new_agent.name, old_agent->max_turns, new_agent.max_turns));
        if (new_agent.risk_budget > old_agent->risk_budget && old_agent->risk_budget > 0)
            violations.push_back(fmt::format("agent.{}.risk_budget: {} -> {} (loosened)",
                new_agent.name, old_agent->risk_budget, new_agent.risk_budget));

        // Network allowed loosening
        if (new_agent.network_allowed_set && old_agent->network_allowed_set) {
            if (new_agent.network_allowed && !old_agent->network_allowed)
                violations.push_back(fmt::format("agent.{}.network_allowed: false -> true (loosened)",
                    new_agent.name));
            if (!new_agent.network_allowed && old_agent->network_allowed)
                notices.push_back(fmt::format("agent.{}.network_allowed: true -> false (tightened)",
                    new_agent.name));
        }

        // Allowed actions: adding actions = loosening, removing = tightening
        if (!old_agent->allowed_actions.empty() && new_agent.allowed_actions.empty()) {
            violations.push_back(fmt::format("agent.{}.allowed_actions: removed (loosened — all actions now allowed)",
                new_agent.name));
        } else if (!old_agent->allowed_actions.empty() && !new_agent.allowed_actions.empty()) {
            std::unordered_set<std::string> old_set(old_agent->allowed_actions.begin(), old_agent->allowed_actions.end());
            for (const auto& a : new_agent.allowed_actions) {
                if (!old_set.count(a))
                    violations.push_back(fmt::format("agent.{}.allowed_actions: {} added (loosened)",
                        new_agent.name, a));
            }
            std::unordered_set<std::string> new_set(new_agent.allowed_actions.begin(), new_agent.allowed_actions.end());
            for (const auto& a : old_agent->allowed_actions) {
                if (!new_set.count(a))
                    notices.push_back(fmt::format("agent.{}.allowed_actions: {} removed (tightened)",
                        new_agent.name, a));
            }
        } else if (old_agent->allowed_actions.empty() && !new_agent.allowed_actions.empty()) {
            notices.push_back(fmt::format("agent.{}.allowed_actions: added (tightened — {} actions allowed)",
                new_agent.name, new_agent.allowed_actions.size()));
        }

        // Tool config ratchet enforcement (S6)
        // tools_enabled: false→true = loosening
        if (new_agent.tools_enabled && !old_agent->tools_enabled) {
            violations.push_back(fmt::format("agent.{}.tools_enabled: false -> true (loosened)",
                new_agent.name));
        } else if (!new_agent.tools_enabled && old_agent->tools_enabled) {
            notices.push_back(fmt::format("agent.{}.tools_enabled: true -> false (tightened)",
                new_agent.name));
        }
        // tools[] array: adding entries = loosening
        if (!old_agent->tools.empty() || !new_agent.tools.empty()) {
            std::unordered_set<std::string> old_tools(old_agent->tools.begin(), old_agent->tools.end());
            std::unordered_set<std::string> new_tools(new_agent.tools.begin(), new_agent.tools.end());
            for (const auto& t : new_agent.tools) {
                if (!old_tools.count(t))
                    violations.push_back(fmt::format("agent.{}.tools: '{}' added (loosened)",
                        new_agent.name, t));
            }
            for (const auto& t : old_agent->tools) {
                if (!new_tools.count(t))
                    notices.push_back(fmt::format("agent.{}.tools: '{}' removed (tightened)",
                        new_agent.name, t));
            }
        }
        // Numeric tool limits: increasing = loosening
        if (new_agent.max_tool_calls_per_turn > old_agent->max_tool_calls_per_turn)
            violations.push_back(fmt::format("agent.{}.max_tool_calls_per_turn: {} -> {} (loosened)",
                new_agent.name, old_agent->max_tool_calls_per_turn, new_agent.max_tool_calls_per_turn));
        else if (new_agent.max_tool_calls_per_turn < old_agent->max_tool_calls_per_turn)
            notices.push_back(fmt::format("agent.{}.max_tool_calls_per_turn: {} -> {} (tightened)",
                new_agent.name, old_agent->max_tool_calls_per_turn, new_agent.max_tool_calls_per_turn));
        if (new_agent.max_tool_loop_turns > old_agent->max_tool_loop_turns)
            violations.push_back(fmt::format("agent.{}.max_tool_loop_turns: {} -> {} (loosened)",
                new_agent.name, old_agent->max_tool_loop_turns, new_agent.max_tool_loop_turns));
        else if (new_agent.max_tool_loop_turns < old_agent->max_tool_loop_turns)
            notices.push_back(fmt::format("agent.{}.max_tool_loop_turns: {} -> {} (tightened)",
                new_agent.name, old_agent->max_tool_loop_turns, new_agent.max_tool_loop_turns));
        // Standing Lease: increasing lease = loosening (more turns before re-auth)
        if (new_agent.standing_lease_turns > old_agent->standing_lease_turns && old_agent->standing_lease_turns > 0)
            violations.push_back(fmt::format("agent.{}.standing_lease_turns: {} -> {} (loosened)",
                new_agent.name, old_agent->standing_lease_turns, new_agent.standing_lease_turns));
        else if (new_agent.standing_lease_turns < old_agent->standing_lease_turns)
            notices.push_back(fmt::format("agent.{}.standing_lease_turns: {} -> {} (tightened)",
                new_agent.name, old_agent->standing_lease_turns, new_agent.standing_lease_turns));
        // Removing lease (N→0) is loosening
        if (new_agent.standing_lease_turns == 0 && old_agent->standing_lease_turns > 0)
            violations.push_back(fmt::format("agent.{}.standing_lease_turns: {} -> 0 (loosened — lease removed)",
                new_agent.name, old_agent->standing_lease_turns));
        // Wall-clock lease: same ratchet rules
        if (new_agent.standing_lease_seconds > old_agent->standing_lease_seconds && old_agent->standing_lease_seconds > 0)
            violations.push_back(fmt::format("agent.{}.standing_lease_seconds: {} -> {} (loosened)",
                new_agent.name, old_agent->standing_lease_seconds, new_agent.standing_lease_seconds));
        else if (new_agent.standing_lease_seconds < old_agent->standing_lease_seconds)
            notices.push_back(fmt::format("agent.{}.standing_lease_seconds: {} -> {} (tightened)",
                new_agent.name, old_agent->standing_lease_seconds, new_agent.standing_lease_seconds));
        if (new_agent.standing_lease_seconds == 0 && old_agent->standing_lease_seconds > 0)
            violations.push_back(fmt::format("agent.{}.standing_lease_seconds: {} -> 0 (loosened — lease removed)",
                new_agent.name, old_agent->standing_lease_seconds));
    }

    // Detect removed agents — removing constraints is loosening
    for (const auto& old_agent : old_r.agents) {
        if (!agentByName(new_r.agents, old_agent.name)) {
            violations.push_back(fmt::format("agent.{}: config removed (loosened)",
                old_agent.name));
        }
    }

    // --- Codegen ratchet enforcement ---
    // codegen.enabled: false→true = loosening
    if (new_r.codegen.enabled && !old_r.codegen.enabled)
        violations.push_back("codegen.enabled: false -> true (loosened)");
    else if (!new_r.codegen.enabled && old_r.codegen.enabled)
        notices.push_back("codegen.enabled: true -> false (tightened)");
    // allow_tainted_code: false→true = loosening
    if (new_r.codegen.allow_tainted_code && !old_r.codegen.allow_tainted_code)
        violations.push_back("codegen.allow_tainted_code: false -> true (loosened)");
    else if (!new_r.codegen.allow_tainted_code && old_r.codegen.allow_tainted_code)
        notices.push_back("codegen.allow_tainted_code: true -> false (tightened)");
    // Numeric codegen limits: increasing = loosening
    auto chkCodegenLimit = [&](int old_v, int new_v, const char* field) {
        if (new_v > old_v)
            violations.push_back(fmt::format("codegen.{}: {} -> {} (loosened)", field, old_v, new_v));
        else if (new_v < old_v)
            notices.push_back(fmt::format("codegen.{}: {} -> {} (tightened)", field, old_v, new_v));
    };
    chkCodegenLimit(old_r.codegen.max_code_size_bytes, new_r.codegen.max_code_size_bytes, "max_code_size_bytes");
    chkCodegenLimit(old_r.codegen.max_code_lines, new_r.codegen.max_code_lines, "max_code_lines");
    chkCodegenLimit(old_r.codegen.timeout_seconds, new_r.codegen.timeout_seconds, "timeout_seconds");
    chkCodegenLimit(old_r.codegen.max_cumulative_code_bytes, new_r.codegen.max_cumulative_code_bytes, "max_cumulative_code_bytes");
    chkCodegenLimit(old_r.codegen.max_cumulative_calls, new_r.codegen.max_cumulative_calls, "max_cumulative_calls");
    chkCodegenLimit(old_r.codegen.max_cumulative_calls_per_agent, new_r.codegen.max_cumulative_calls_per_agent, "max_cumulative_calls_per_agent");
    chkCodegenLimit(old_r.codegen.max_nesting_depth, new_r.codegen.max_nesting_depth, "max_nesting_depth");

    // --- Advisory Escalation ratchet ---
    // Disabling escalation = loosening (repeated advisories would no longer harden)
    if (!new_r.advisory_escalation.enabled && old_r.advisory_escalation.enabled)
        violations.push_back("advisory_escalation.enabled: true -> false (loosened)");
    else if (new_r.advisory_escalation.enabled && !old_r.advisory_escalation.enabled)
        notices.push_back("advisory_escalation.enabled: false -> true (tightened)");
    // Increasing soft_after = loosening (more occurrences before escalation)
    if (new_r.advisory_escalation.soft_after > old_r.advisory_escalation.soft_after)
        violations.push_back(fmt::format("advisory_escalation.soft_after: {} -> {} (loosened)",
            old_r.advisory_escalation.soft_after, new_r.advisory_escalation.soft_after));
    else if (new_r.advisory_escalation.soft_after < old_r.advisory_escalation.soft_after)
        notices.push_back(fmt::format("advisory_escalation.soft_after: {} -> {} (tightened)",
            old_r.advisory_escalation.soft_after, new_r.advisory_escalation.soft_after));

    return violations.empty();
}

// ============================================================================
// Governance Under Survivability — Mid-Run Config Reload
// ============================================================================

bool GovernanceEngine::reloadIfChanged() {
    std::lock_guard<std::mutex> reload_lock(reload_mutex_);

    if (loaded_path_.empty()) return false;

    // Check mtime
    int64_t current_mtime = 0;
    try {
        auto ftime = std::filesystem::last_write_time(loaded_path_);
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
            ftime.time_since_epoch());
        current_mtime = static_cast<int64_t>(secs.count());
    } catch (...) {
        return false;
    }

    if (current_mtime == loaded_mtime_ns_) return false;

    // File changed — attempt reload
    try {
        std::ifstream ifs(loaded_path_);
        if (!ifs.is_open()) {
            loaded_mtime_ns_ = current_mtime; // don't re-check until next change
            return false;
        }

        nlohmann::json j = nlohmann::json::parse(ifs);
        ifs.close();

        if (!checkJsonArrayWidth(j)) {
            fmt::print(stderr, "[governance] Reload rejected: JSON array exceeds cap\n");
            loaded_mtime_ns_ = current_mtime;
            return false;
        }

        // Parse into temporary rules
        GovernanceRules new_rules;
        loadFromJson(j, new_rules);

        // Verify signature — reject unsigned changes
        if (!verifyFileSignature(loaded_path_)) {
            fmt::print(stderr, "[governance] Reload rejected: signature verification failed\n");
            logAuditEvent("governance_reload_rejected", "governance_config",
                "Signature verification failed on govern.json reload");
            loaded_mtime_ns_ = current_mtime;
            return false;
        }

        // Apply minimum enforcement levels to new rules
        enforceMinimumLevelsOnRules(new_rules);

        // One-way ratchet: reject any loosening
        std::vector<std::string> violations;
        std::vector<std::string> notices;
        if (!checkRatchetViolation(rules(), new_rules, violations, notices)) {
            std::string detail;
            for (const auto& v : violations) {
                if (!detail.empty()) detail += "; ";
                detail += v;
            }
            fmt::print(stderr, "[governance] Reload rejected: ratchet violation(s): {}\n", detail);
            logAuditEvent("governance_reload_rejected", "governance_config",
                fmt::format("Ratchet violation: {}", detail));
            loaded_mtime_ns_ = current_mtime;
            return false;
        }

        // Read optional update_reason from JSON
        std::string update_reason;
        if (j.contains("update_reason") && j["update_reason"].is_string()) {
            update_reason = j["update_reason"].get<std::string>();
        }
        if (!update_reason.empty()) {
            notices.insert(notices.begin(),
                fmt::format("Governance update: {}", update_reason));
        }

        // C1: Atomic swap via shared_ptr — readers see consistent snapshot
        auto new_rp = std::make_shared<const GovernanceRules>(std::move(new_rules));
        std::atomic_store(&rules_ptr_, new_rp);
        active_.store(new_rp->mode != GovernanceMode::OFF);
        loaded_mtime_ns_ = current_mtime;
        reload_count_++;

        // Evidence Epoch: config reload invalidates prior-epoch evidence
        governance_epoch_++;
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            decayAdvisoryHistory();
        }

        // Fail-closed: sync tightened governance into active sandbox
        auto* sb = security::ScopedSandbox::getCurrent();
        if (sb) {
            if (!new_rp->network_allowed) {
                sb->setNetworkEnabled(false);
                sb->removeCapability(security::Capability::NET_CONNECT);
            }
            if (!new_rp->shell_allowed) {
                sb->setAllowExec(false);
                sb->removeCapability(security::Capability::SYS_EXEC);
            }
        }

        // Re-configure BSD/CDD detectors with new rules
        // Use updateConfig() when already enabled (preserve behavioral evidence)
        // Use configure() for fresh enable (off→on transition)
        bool was_bsd = bsd_enabled_.load(std::memory_order_relaxed);
        bool was_cdd = cdd_enabled_.load(std::memory_order_relaxed);

        if (new_rp->behavioral_sequences.enabled) {
            if (was_bsd) {
                sequence_detector_.updateConfig(new_rp->behavioral_sequences);
            } else {
                sequence_detector_.configure(new_rp->behavioral_sequences);
            }
        } else {
            sequence_detector_.reset();
        }
        if (new_rp->context_drift.enabled) {
            if (was_cdd) {
                drift_analyzer_.updateConfig(new_rp->context_drift);
            } else {
                drift_analyzer_.configure(new_rp->context_drift);
            }
        } else {
            drift_analyzer_.reset();
        }
        bsd_enabled_.store(new_rp->behavioral_sequences.enabled, std::memory_order_release);
        cdd_enabled_.store(new_rp->context_drift.enabled, std::memory_order_release);

        // C3: restart telemetry forwarder if webhook config changed
        {
            std::shared_ptr<TelemetryForwarder> old_fwd;
            {
                std::lock_guard<std::mutex> lock(telemetry_fwd_mutex_);
                bool needs_restart = false;
                if (telemetry_forwarder_ && !new_rp->telemetry_output.webhook_url.empty()) {
                    needs_restart = true;  // config may have changed — restart with new settings
                } else if (!telemetry_forwarder_ && !new_rp->telemetry_output.webhook_url.empty()) {
                    needs_restart = true;  // webhook newly configured mid-run
                }

                if (needs_restart) {
                    old_fwd = std::move(telemetry_forwarder_);
                    TelemetryForwarderConfig fwd_cfg;
                    fwd_cfg.webhook_url = new_rp->telemetry_output.webhook_url;
                    fwd_cfg.auth_header = new_rp->telemetry_output.webhook_auth_header;
                    fwd_cfg.batch_size = new_rp->telemetry_output.forward_batch_size;
                    fwd_cfg.timeout_ms = new_rp->telemetry_output.forward_timeout_ms;
                    fwd_cfg.retry_count = new_rp->telemetry_output.forward_retry_count;
                    fwd_cfg.buffer_max = new_rp->telemetry_output.forward_buffer_max;
                    fwd_cfg.shutdown_drain_ms = new_rp->telemetry_output.forward_shutdown_drain_ms;
                    telemetry_forwarder_ = std::make_shared<TelemetryForwarder>(fwd_cfg);
                } else if (telemetry_forwarder_ && new_rp->telemetry_output.webhook_url.empty()) {
                    // Webhook removed — shut down forwarder
                    old_fwd = std::move(telemetry_forwarder_);
                }
            }
            // Shutdown old forwarder OUTSIDE the lock (may block on final drain)
            if (old_fwd) old_fwd->shutdown();
        }

        // Store notices for retrieval by agent.send()
        size_t notice_count = notices.size();
        {
            std::lock_guard<std::mutex> lock(notices_mutex_);
            pending_notices_ = std::move(notices);
        }

        // Audit trail
        logAuditEvent("governance_reloaded", "governance_config",
            fmt::format("Config reloaded: {} change(s){}",
                notice_count,
                update_reason.empty() ? "" : ", reason: " + update_reason));

        // Dashboard notification
        fmt::print(stderr, "[governance] Config reloaded mid-run: {} change(s) applied{}\n",
            notice_count,
            update_reason.empty() ? "" : " (" + update_reason + ")");

        return true;

    } catch (const std::exception& e) {
        fmt::print(stderr, "[governance] Reload failed: {}\n", e.what());
        loaded_mtime_ns_ = current_mtime;
        return false;
    }
}

std::vector<std::string> GovernanceEngine::getAndClearNotices() {
    std::lock_guard<std::mutex> lock(notices_mutex_);
    std::vector<std::string> result = std::move(pending_notices_);
    pending_notices_.clear();
    return result;
}

// ============================================================================
// EVA-11/EVA-12: Governance integrity check
// ============================================================================

// Prevents LLM config manipulation by ensuring anti-evasion checks have
// minimum enforcement levels. An LLM could write govern.json with all
// checks set to "advisory" (warn-only) to bypass quality gates.
void GovernanceEngine::enforceMinimumLevels(GovernanceRules& r) {
    // F3: Signed configs cannot use AUDIT mode — override to ENFORCE
    if (r.mode == GovernanceMode::AUDIT && !loaded_path_.empty()) {
        std::string sig_path = loaded_path_ + ".sig";
        std::error_code ec;
        if (std::filesystem::exists(sig_path, ec)) {
            fprintf(stderr,
                "[governance] WARNING: Signed govern.json has mode:audit — "
                "overriding to ENFORCE.\n");
            r.mode = GovernanceMode::ENFORCE;
        }
    }

    enforceMinimumLevelsOnRules(r);

    // Warn about contradictory config: code quality checks enabled but mode is audit/off
    if (r.mode != GovernanceMode::ENFORCE) {
        if (r.code_quality.no_oversimplification.enabled ||
            r.code_quality.no_incomplete_logic.enabled) {
            fprintf(stderr, "[governance] WARNING: Code quality checks are enabled but mode is %s. "
                    "Code quality checks will NOT block — use mode: enforce for protection.\n",
                    r.mode == GovernanceMode::AUDIT ? "audit" : "off");
        }
    }
}

// ============================================================================
// Policy Distribution: extends / inheritance
// ============================================================================

std::string GovernanceEngine::resolveExtendsPath(const std::string& extends_val,
                                                  const std::string& config_dir) {
    if (extends_val.empty()) return "";
    // Absolute path
    if (extends_val[0] == '/') return extends_val;
    // Home-relative
    if (extends_val.size() >= 2 && extends_val[0] == '~' && extends_val[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) return std::string(home) + extends_val.substr(1);
        return extends_val; // fallback: literal
    }
    // Remote: not yet supported
    if (extends_val.find("http://") == 0 || extends_val.find("https://") == 0) {
        return ""; // caller will error
    }
    // Relative to config directory
    auto p = std::filesystem::path(config_dir) / extends_val;
    return std::filesystem::weakly_canonical(p).string();
}

void GovernanceEngine::mergeRules(const GovernanceRules& base, GovernanceRules& child,
                                   const InheritanceConfig& cfg) {
    bool parent_wins = (cfg.merge_strategy == "parent_wins");

    // Helper: for scalar fields, base fills gaps in child.
    // With child_wins (default): child keeps its value if explicitly different from default.
    // With parent_wins: base value takes precedence if different from default.
    // Since GovernanceRules lacks "was explicitly set" tracking, we compare against
    // a default-constructed GovernanceRules to detect which fields were set.
    GovernanceRules defaults;

    // --- Mode: child can only set equal-or-stricter (ratchet semantics) ---
    // Strictness: ENFORCE > AUDIT > OFF
    // NOTE (M5): Both branches below are intentionally identical. Mode is a
    // security floor — base always constrains child regardless of merge_strategy.
    if (parent_wins) {
        // Parent mode wins if stricter
        if (static_cast<int>(base.mode) < static_cast<int>(child.mode)) {
            child.mode = base.mode;  // lower enum = stricter
        }
    } else {
        // Child mode wins, but base provides floor
        if (static_cast<int>(base.mode) < static_cast<int>(child.mode)) {
            child.mode = base.mode;  // can't be looser than base
        }
    }

    // --- Capabilities: merge each sub-field ---
    auto& cc = child.capabilities;
    const auto& bc = base.capabilities;
    const auto& dc = defaults.capabilities;

    // Shell
    if (parent_wins) {
        if (!bc.shell.enabled) cc.shell.enabled = false;  // parent disables → stays disabled
    } else {
        if (cc.shell.enabled == dc.shell.enabled && !bc.shell.enabled) cc.shell.enabled = false;
    }
    // Network
    if (parent_wins) {
        if (!bc.network.enabled) cc.network.enabled = false;
    } else {
        if (cc.network.enabled == dc.network.enabled && !bc.network.enabled) cc.network.enabled = false;
    }
    // Filesystem
    if (parent_wins) {
        if (!bc.filesystem.mode.empty()) cc.filesystem.mode = bc.filesystem.mode;
    } else {
        if (cc.filesystem.mode == dc.filesystem.mode && !bc.filesystem.mode.empty()) {
            cc.filesystem.mode = bc.filesystem.mode;
        }
    }
    // Blocked commands: merge arrays
    if (cfg.merge_arrays == "append") {
        for (const auto& cmd : bc.shell.blocked_commands) {
            if (std::find(cc.shell.blocked_commands.begin(), cc.shell.blocked_commands.end(), cmd) ==
                cc.shell.blocked_commands.end()) {
                cc.shell.blocked_commands.push_back(cmd);
            }
        }
    } else if (cc.shell.blocked_commands.empty()) {
        cc.shell.blocked_commands = bc.shell.blocked_commands;
    }

    // --- Limits: child wins or parent wins, with gap-filling ---
    auto& cl = child.limits;
    const auto& bl = base.limits;
    const auto& dl = defaults.limits;

    if (parent_wins) {
        if (bl.timeout.global > 0) cl.timeout.global = bl.timeout.global;
        if (bl.timeout.total_polyglot > 0) cl.timeout.total_polyglot = bl.timeout.total_polyglot;
        if (bl.execution.loop_iterations > 0) cl.execution.loop_iterations = bl.execution.loop_iterations;
        if (bl.execution.call_depth > 0) cl.execution.call_depth = bl.execution.call_depth;
        if (bl.execution.polyglot_blocks > 0) cl.execution.polyglot_blocks = bl.execution.polyglot_blocks;
    } else {
        // Child wins: base fills gaps (where child has default value)
        if (cl.timeout.global == dl.timeout.global && bl.timeout.global > 0)
            cl.timeout.global = bl.timeout.global;
        if (cl.timeout.total_polyglot == dl.timeout.total_polyglot && bl.timeout.total_polyglot > 0)
            cl.timeout.total_polyglot = bl.timeout.total_polyglot;
        if (cl.execution.loop_iterations == dl.execution.loop_iterations && bl.execution.loop_iterations > 0)
            cl.execution.loop_iterations = bl.execution.loop_iterations;
        if (cl.execution.call_depth == dl.execution.call_depth && bl.execution.call_depth > 0)
            cl.execution.call_depth = bl.execution.call_depth;
        if (cl.execution.polyglot_blocks == dl.execution.polyglot_blocks && bl.execution.polyglot_blocks > 0)
            cl.execution.polyglot_blocks = bl.execution.polyglot_blocks;
    }
    // Memory limits
    if (parent_wins) {
        if (bl.memory.per_block_mb > 0) cl.memory.per_block_mb = bl.memory.per_block_mb;
        if (bl.memory.total_mb > 0) cl.memory.total_mb = bl.memory.total_mb;
    } else {
        if (cl.memory.per_block_mb == dl.memory.per_block_mb && bl.memory.per_block_mb > 0)
            cl.memory.per_block_mb = bl.memory.per_block_mb;
        if (cl.memory.total_mb == dl.memory.total_mb && bl.memory.total_mb > 0)
            cl.memory.total_mb = bl.memory.total_mb;
    }
    // Data limits
    if (parent_wins) {
        if (bl.data.array_size > 0) cl.data.array_size = bl.data.array_size;
        if (bl.data.dict_size > 0) cl.data.dict_size = bl.data.dict_size;
        if (bl.data.string_length > 0) cl.data.string_length = bl.data.string_length;
        if (bl.data.nesting_depth > 0) cl.data.nesting_depth = bl.data.nesting_depth;
        if (bl.data.output_size > 0) cl.data.output_size = bl.data.output_size;
        if (bl.data.input_size > 0) cl.data.input_size = bl.data.input_size;
    } else {
        if (cl.data.array_size == dl.data.array_size && bl.data.array_size > 0)
            cl.data.array_size = bl.data.array_size;
        if (cl.data.dict_size == dl.data.dict_size && bl.data.dict_size > 0)
            cl.data.dict_size = bl.data.dict_size;
        if (cl.data.string_length == dl.data.string_length && bl.data.string_length > 0)
            cl.data.string_length = bl.data.string_length;
        if (cl.data.nesting_depth == dl.data.nesting_depth && bl.data.nesting_depth > 0)
            cl.data.nesting_depth = bl.data.nesting_depth;
        if (cl.data.output_size == dl.data.output_size && bl.data.output_size > 0)
            cl.data.output_size = bl.data.output_size;
        if (cl.data.input_size == dl.data.input_size && bl.data.input_size > 0)
            cl.data.input_size = bl.data.input_size;
    }
    // Code limits
    if (parent_wins) {
        if (bl.code.max_lines_per_block > 0) cl.code.max_lines_per_block = bl.code.max_lines_per_block;
        if (bl.code.max_total_polyglot_lines > 0) cl.code.max_total_polyglot_lines = bl.code.max_total_polyglot_lines;
        if (bl.code.max_functions > 0) cl.code.max_functions = bl.code.max_functions;
        if (bl.code.max_variables > 0) cl.code.max_variables = bl.code.max_variables;
        if (bl.code.max_nesting_depth > 0) cl.code.max_nesting_depth = bl.code.max_nesting_depth;
    } else {
        if (cl.code.max_lines_per_block == dl.code.max_lines_per_block && bl.code.max_lines_per_block > 0)
            cl.code.max_lines_per_block = bl.code.max_lines_per_block;
        if (cl.code.max_total_polyglot_lines == dl.code.max_total_polyglot_lines && bl.code.max_total_polyglot_lines > 0)
            cl.code.max_total_polyglot_lines = bl.code.max_total_polyglot_lines;
        if (cl.code.max_functions == dl.code.max_functions && bl.code.max_functions > 0)
            cl.code.max_functions = bl.code.max_functions;
        if (cl.code.max_variables == dl.code.max_variables && bl.code.max_variables > 0)
            cl.code.max_variables = bl.code.max_variables;
        if (cl.code.max_nesting_depth == dl.code.max_nesting_depth && bl.code.max_nesting_depth > 0)
            cl.code.max_nesting_depth = bl.code.max_nesting_depth;
    }
    // Rate limits (cooldown_on_limit_ms defaults to 100, rest to 0)
    if (parent_wins) {
        if (bl.rate.max_polyglot_per_second > 0) cl.rate.max_polyglot_per_second = bl.rate.max_polyglot_per_second;
        if (bl.rate.max_stdlib_calls_per_second > 0) cl.rate.max_stdlib_calls_per_second = bl.rate.max_stdlib_calls_per_second;
        if (bl.rate.max_file_ops_per_second > 0) cl.rate.max_file_ops_per_second = bl.rate.max_file_ops_per_second;
        if (bl.rate.cooldown_on_limit_ms != dl.rate.cooldown_on_limit_ms)
            cl.rate.cooldown_on_limit_ms = bl.rate.cooldown_on_limit_ms;
    } else {
        if (cl.rate.max_polyglot_per_second == dl.rate.max_polyglot_per_second && bl.rate.max_polyglot_per_second > 0)
            cl.rate.max_polyglot_per_second = bl.rate.max_polyglot_per_second;
        if (cl.rate.max_stdlib_calls_per_second == dl.rate.max_stdlib_calls_per_second && bl.rate.max_stdlib_calls_per_second > 0)
            cl.rate.max_stdlib_calls_per_second = bl.rate.max_stdlib_calls_per_second;
        if (cl.rate.max_file_ops_per_second == dl.rate.max_file_ops_per_second && bl.rate.max_file_ops_per_second > 0)
            cl.rate.max_file_ops_per_second = bl.rate.max_file_ops_per_second;
        if (cl.rate.cooldown_on_limit_ms == dl.rate.cooldown_on_limit_ms &&
            bl.rate.cooldown_on_limit_ms != dl.rate.cooldown_on_limit_ms)
            cl.rate.cooldown_on_limit_ms = bl.rate.cooldown_on_limit_ms;
    }

    // --- Requirements: inherit from base if child didn't set ---
    if (!parent_wins) {
        if (!child.require_error_handling && base.require_error_handling)
            child.require_error_handling = base.require_error_handling;
        if (!child.require_main_block && base.require_main_block)
            child.require_main_block = base.require_main_block;
    } else {
        if (base.require_error_handling) child.require_error_handling = true;
        if (base.require_main_block) child.require_main_block = true;
    }

    // --- Restrictions: inherit from base if child didn't set ---
    if (!parent_wins) {
        if (!child.no_placeholders && base.no_placeholders) child.no_placeholders = true;
        if (!child.no_secrets && base.no_secrets) child.no_secrets = true;
        if (!child.restrict_dangerous_calls && base.restrict_dangerous_calls)
            child.restrict_dangerous_calls = true;
        if (!child.no_hardcoded_results && base.no_hardcoded_results)
            child.no_hardcoded_results = true;
    } else {
        if (base.no_placeholders) child.no_placeholders = true;
        if (base.no_secrets) child.no_secrets = true;
        if (base.restrict_dangerous_calls) child.restrict_dangerous_calls = true;
        if (base.no_hardcoded_results) child.no_hardcoded_results = true;
    }

    // --- Custom rules: array merge ---
    if (cfg.merge_arrays == "append") {
        // Append base rules that don't exist in child (by name)
        std::set<std::string> child_names;
        for (const auto& r : child.custom_rules) child_names.insert(r.name);
        for (const auto& r : base.custom_rules) {
            if (child_names.find(r.name) == child_names.end()) {
                child.custom_rules.push_back(r);
            }
        }
    } else if (child.custom_rules.empty()) {
        child.custom_rules = base.custom_rules;
    }

    // --- Taint tracking: inherit if child didn't configure ---
    if (child.taint_tracking.level == defaults.taint_tracking.level &&
        base.taint_tracking.level != defaults.taint_tracking.level) {
        child.taint_tracking.level = base.taint_tracking.level;
    }
    // Taint sources/sinks/sanitizers: array merge
    if (cfg.merge_arrays == "append") {
        for (const auto& s : base.taint_tracking.sources) {
            if (std::find(child.taint_tracking.sources.begin(),
                          child.taint_tracking.sources.end(), s) == child.taint_tracking.sources.end())
                child.taint_tracking.sources.push_back(s);
        }
        for (const auto& s : base.taint_tracking.sinks) {
            if (std::find(child.taint_tracking.sinks.begin(),
                          child.taint_tracking.sinks.end(), s) == child.taint_tracking.sinks.end())
                child.taint_tracking.sinks.push_back(s);
        }
        for (const auto& s : base.taint_tracking.sanitizers) {
            if (std::find(child.taint_tracking.sanitizers.begin(),
                          child.taint_tracking.sanitizers.end(), s) == child.taint_tracking.sanitizers.end())
                child.taint_tracking.sanitizers.push_back(s);
        }
    } else {
        if (child.taint_tracking.sources.empty()) child.taint_tracking.sources = base.taint_tracking.sources;
        if (child.taint_tracking.sinks.empty()) child.taint_tracking.sinks = base.taint_tracking.sinks;
        if (child.taint_tracking.sanitizers.empty()) child.taint_tracking.sanitizers = base.taint_tracking.sanitizers;
    }

    // --- Scoring: inherit if child didn't configure ---
    if (!child.scoring.enabled && base.scoring.enabled) {
        child.scoring = base.scoring;
    } else if (child.scoring.enabled && base.scoring.enabled) {
        // Both set: child wins on thresholds it set, base fills gaps
        if (!parent_wins) {
            if (child.scoring.yellow_threshold == defaults.scoring.yellow_threshold)
                child.scoring.yellow_threshold = base.scoring.yellow_threshold;
            if (child.scoring.red_threshold == defaults.scoring.red_threshold)
                child.scoring.red_threshold = base.scoring.red_threshold;
        }
    }

    // --- Contracts: inherit function contracts from base (map by name) ---
    for (const auto& [name, contract] : base.contracts.functions) {
        if (child.contracts.functions.find(name) == child.contracts.functions.end()) {
            child.contracts.functions[name] = contract;
        }
    }

    // --- Behavioral sequences: inherit if child didn't configure ---
    if (!child.behavioral_sequences.enabled && base.behavioral_sequences.enabled) {
        child.behavioral_sequences = base.behavioral_sequences;
    }

    // --- Context drift: inherit if child didn't configure ---
    if (!child.context_drift.enabled && base.context_drift.enabled) {
        child.context_drift = base.context_drift;
    }

    // --- Agent configs: array merge (by agent name) ---
    if (cfg.merge_arrays == "append") {
        std::set<std::string> child_agents;
        for (const auto& a : child.agents) child_agents.insert(a.name);
        for (const auto& a : base.agents) {
            if (child_agents.find(a.name) == child_agents.end()) {
                child.agents.push_back(a);
            }
        }
    } else if (child.agents.empty()) {
        child.agents = base.agents;
    }

    // --- Blocked/allowed languages ---
    if (child.allowed_languages.empty() && !base.allowed_languages.empty())
        child.allowed_languages = base.allowed_languages;
    if (child.blocked_languages.empty() && !base.blocked_languages.empty())
        child.blocked_languages = base.blocked_languages;

    // --- Legacy scalar fields: gap-fill from base ---
    if (!parent_wins) {
        if (child.timeout_seconds == 0 && base.timeout_seconds > 0)
            child.timeout_seconds = base.timeout_seconds;
        if (child.memory_limit_mb == 0 && base.memory_limit_mb > 0)
            child.memory_limit_mb = base.memory_limit_mb;
        if (child.max_call_depth == 0 && base.max_call_depth > 0)
            child.max_call_depth = base.max_call_depth;
        if (child.max_array_size == 0 && base.max_array_size > 0)
            child.max_array_size = base.max_array_size;
    } else {
        if (base.timeout_seconds > 0) child.timeout_seconds = base.timeout_seconds;
        if (base.memory_limit_mb > 0) child.memory_limit_mb = base.memory_limit_mb;
        if (base.max_call_depth > 0) child.max_call_depth = base.max_call_depth;
        if (base.max_array_size > 0) child.max_array_size = base.max_array_size;
    }

    // --- Governance depth features: inherit if child didn't configure ---
    if (!child.circuit_breaker.enabled && base.circuit_breaker.enabled)
        child.circuit_breaker = base.circuit_breaker;
    if (!child.advisory_escalation.enabled && base.advisory_escalation.enabled)
        child.advisory_escalation = base.advisory_escalation;
    if (!child.exposure_tracking.enabled && base.exposure_tracking.enabled)
        child.exposure_tracking = base.exposure_tracking;
    if (!child.pipeline_separation.enabled && base.pipeline_separation.enabled)
        child.pipeline_separation = base.pipeline_separation;
    if (!child.governance_health.enabled && base.governance_health.enabled)
        child.governance_health = base.governance_health;
    if (!child.temporal_coupling.enabled && base.temporal_coupling.enabled)
        child.temporal_coupling = base.temporal_coupling;
    if (!child.contradiction_detection.enabled && base.contradiction_detection.enabled)
        child.contradiction_detection = base.contradiction_detection;
    if (!child.codegen.enabled && base.codegen.enabled)
        child.codegen = base.codegen;
    if (!child.telemetry_output.enabled && base.telemetry_output.enabled)
        child.telemetry_output = base.telemetry_output;
    if (!child.quality_gate.enabled && base.quality_gate.enabled)
        child.quality_gate = base.quality_gate;
    if (!child.governance_baseline.enabled && base.governance_baseline.enabled)
        child.governance_baseline = base.governance_baseline;
    if (!child.baselines.enabled && base.baselines.enabled)
        child.baselines = base.baselines;
    if (!child.prerequisites.enabled && base.prerequisites.enabled)
        child.prerequisites = base.prerequisites;
    if (!child.agent_review.enabled && base.agent_review.enabled)
        child.agent_review = base.agent_review;

    // --- Restriction sub-configs: parent_wins can force-enable ---
    if (parent_wins) {
        if (base.restrictions.dangerous_calls.enabled)
            child.restrictions.dangerous_calls = base.restrictions.dangerous_calls;
        if (base.restrictions.shell_injection.enabled)
            child.restrictions.shell_injection = base.restrictions.shell_injection;
        if (base.restrictions.imports.enabled)
            child.restrictions.imports = base.restrictions.imports;
        if (base.restrictions.data_exfiltration.enabled)
            child.restrictions.data_exfiltration = base.restrictions.data_exfiltration;
        if (base.restrictions.resource_abuse.enabled)
            child.restrictions.resource_abuse = base.restrictions.resource_abuse;
        if (base.restrictions.privilege_escalation.enabled)
            child.restrictions.privilege_escalation = base.restrictions.privilege_escalation;
        if (base.restrictions.information_disclosure.enabled)
            child.restrictions.information_disclosure = base.restrictions.information_disclosure;
        if (base.restrictions.code_injection.enabled)
            child.restrictions.code_injection = base.restrictions.code_injection;
        if (base.restrictions.crypto.enabled)
            child.restrictions.crypto = base.restrictions.crypto;
        if (base.restrictions.vcs_secret_extraction.enabled)
            child.restrictions.vcs_secret_extraction = base.restrictions.vcs_secret_extraction;
        if (base.restrictions.obfuscation.enabled)
            child.restrictions.obfuscation = base.restrictions.obfuscation;
    } else {
        if (!child.restrictions.dangerous_calls.enabled && base.restrictions.dangerous_calls.enabled)
            child.restrictions.dangerous_calls = base.restrictions.dangerous_calls;
        if (!child.restrictions.shell_injection.enabled && base.restrictions.shell_injection.enabled)
            child.restrictions.shell_injection = base.restrictions.shell_injection;
        if (!child.restrictions.imports.enabled && base.restrictions.imports.enabled)
            child.restrictions.imports = base.restrictions.imports;
        if (!child.restrictions.data_exfiltration.enabled && base.restrictions.data_exfiltration.enabled)
            child.restrictions.data_exfiltration = base.restrictions.data_exfiltration;
        if (!child.restrictions.resource_abuse.enabled && base.restrictions.resource_abuse.enabled)
            child.restrictions.resource_abuse = base.restrictions.resource_abuse;
        if (!child.restrictions.privilege_escalation.enabled && base.restrictions.privilege_escalation.enabled)
            child.restrictions.privilege_escalation = base.restrictions.privilege_escalation;
        if (!child.restrictions.information_disclosure.enabled && base.restrictions.information_disclosure.enabled)
            child.restrictions.information_disclosure = base.restrictions.information_disclosure;
        if (!child.restrictions.code_injection.enabled && base.restrictions.code_injection.enabled)
            child.restrictions.code_injection = base.restrictions.code_injection;
        if (!child.restrictions.crypto.enabled && base.restrictions.crypto.enabled)
            child.restrictions.crypto = base.restrictions.crypto;
        if (!child.restrictions.vcs_secret_extraction.enabled && base.restrictions.vcs_secret_extraction.enabled)
            child.restrictions.vcs_secret_extraction = base.restrictions.vcs_secret_extraction;
        if (!child.restrictions.obfuscation.enabled && base.restrictions.obfuscation.enabled)
            child.restrictions.obfuscation = base.restrictions.obfuscation;
    }
    // PolyglotOutputRestriction (no enabled) — field-by-field gap-fill
    {
        auto& cpo = child.restrictions.polyglot_output;
        const auto& bpo = base.restrictions.polyglot_output;
        const auto& dpo = defaults.restrictions.polyglot_output;
        if (cpo.format == dpo.format && bpo.format != dpo.format) cpo.format = bpo.format;
        if (cpo.max_size == dpo.max_size && bpo.max_size > 0) cpo.max_size = bpo.max_size;
        if (!cpo.require_structured && bpo.require_structured) cpo.require_structured = true;
        if (!cpo.validate_json && bpo.validate_json) cpo.validate_json = true;
    }

    // --- Code quality: inherit each check if child didn't configure ---
    if (!child.code_quality.no_secrets.enabled && base.code_quality.no_secrets.enabled)
        child.code_quality.no_secrets = base.code_quality.no_secrets;
    if (!child.code_quality.no_placeholders.enabled && base.code_quality.no_placeholders.enabled)
        child.code_quality.no_placeholders = base.code_quality.no_placeholders;
    if (!child.code_quality.no_hardcoded_results.enabled && base.code_quality.no_hardcoded_results.enabled)
        child.code_quality.no_hardcoded_results = base.code_quality.no_hardcoded_results;
    if (!child.code_quality.no_pii.enabled && base.code_quality.no_pii.enabled)
        child.code_quality.no_pii = base.code_quality.no_pii;
    if (!child.code_quality.no_temporary_code.enabled && base.code_quality.no_temporary_code.enabled)
        child.code_quality.no_temporary_code = base.code_quality.no_temporary_code;
    if (!child.code_quality.no_simulation_markers.enabled && base.code_quality.no_simulation_markers.enabled)
        child.code_quality.no_simulation_markers = base.code_quality.no_simulation_markers;
    if (!child.code_quality.no_mock_data.enabled && base.code_quality.no_mock_data.enabled)
        child.code_quality.no_mock_data = base.code_quality.no_mock_data;
    if (!child.code_quality.no_apologetic_language.enabled && base.code_quality.no_apologetic_language.enabled)
        child.code_quality.no_apologetic_language = base.code_quality.no_apologetic_language;
    if (!child.code_quality.no_dead_code.enabled && base.code_quality.no_dead_code.enabled)
        child.code_quality.no_dead_code = base.code_quality.no_dead_code;
    if (!child.code_quality.no_debug_artifacts.enabled && base.code_quality.no_debug_artifacts.enabled)
        child.code_quality.no_debug_artifacts = base.code_quality.no_debug_artifacts;
    if (!child.code_quality.no_unsafe_deserialization.enabled && base.code_quality.no_unsafe_deserialization.enabled)
        child.code_quality.no_unsafe_deserialization = base.code_quality.no_unsafe_deserialization;
    if (!child.code_quality.no_sql_injection.enabled && base.code_quality.no_sql_injection.enabled)
        child.code_quality.no_sql_injection = base.code_quality.no_sql_injection;
    if (!child.code_quality.no_path_traversal.enabled && base.code_quality.no_path_traversal.enabled)
        child.code_quality.no_path_traversal = base.code_quality.no_path_traversal;
    if (!child.code_quality.no_hardcoded_urls.enabled && base.code_quality.no_hardcoded_urls.enabled)
        child.code_quality.no_hardcoded_urls = base.code_quality.no_hardcoded_urls;
    if (!child.code_quality.no_hardcoded_ips.enabled && base.code_quality.no_hardcoded_ips.enabled)
        child.code_quality.no_hardcoded_ips = base.code_quality.no_hardcoded_ips;
    if (!child.code_quality.max_complexity.enabled && base.code_quality.max_complexity.enabled)
        child.code_quality.max_complexity = base.code_quality.max_complexity;
    if (!child.code_quality.encoding.enabled && base.code_quality.encoding.enabled)
        child.code_quality.encoding = base.code_quality.encoding;
    if (!child.code_quality.no_oversimplification.enabled && base.code_quality.no_oversimplification.enabled)
        child.code_quality.no_oversimplification = base.code_quality.no_oversimplification;
    if (!child.code_quality.no_incomplete_logic.enabled && base.code_quality.no_incomplete_logic.enabled)
        child.code_quality.no_incomplete_logic = base.code_quality.no_incomplete_logic;
    if (!child.code_quality.no_hallucinated_apis.enabled && base.code_quality.no_hallucinated_apis.enabled)
        child.code_quality.no_hallucinated_apis = base.code_quality.no_hallucinated_apis;
    if (!child.code_quality.complexity_floor.enabled && base.code_quality.complexity_floor.enabled)
        child.code_quality.complexity_floor = base.code_quality.complexity_floor;
    if (!child.code_quality.duplicate_calls.enabled && base.code_quality.duplicate_calls.enabled)
        child.code_quality.duplicate_calls = base.code_quality.duplicate_calls;
    if (!child.code_quality.polyglot_try_catch.enabled && base.code_quality.polyglot_try_catch.enabled)
        child.code_quality.polyglot_try_catch = base.code_quality.polyglot_try_catch;
    if (!child.code_quality.semantic_checks.enabled && base.code_quality.semantic_checks.enabled)
        child.code_quality.semantic_checks = base.code_quality.semantic_checks;
    if (!child.code_quality.intent_validation.enabled && base.code_quality.intent_validation.enabled)
        child.code_quality.intent_validation = base.code_quality.intent_validation;
    if (!child.code_quality.drift_detection.enabled && base.code_quality.drift_detection.enabled)
        child.code_quality.drift_detection = base.code_quality.drift_detection;

    // --- Requirements sub-configs: parent_wins can force-enable ---
    if (parent_wins) {
        if (base.requirements.main_block.enabled)
            child.requirements.main_block = base.requirements.main_block;
        if (base.requirements.error_handling.enabled)
            child.requirements.error_handling = base.requirements.error_handling;
        if (base.requirements.strict_types.enabled)
            child.requirements.strict_types = base.requirements.strict_types;
        if (base.requirements.no_global_state.enabled)
            child.requirements.no_global_state = base.requirements.no_global_state;
        if (base.requirements.naming_conventions.enabled)
            child.requirements.naming_conventions = base.requirements.naming_conventions;
        if (base.requirements.documentation.enabled)
            child.requirements.documentation = base.requirements.documentation;
        if (base.requirements.version_pinning.enabled)
            child.requirements.version_pinning = base.requirements.version_pinning;
    } else {
        if (!child.requirements.main_block.enabled && base.requirements.main_block.enabled)
            child.requirements.main_block = base.requirements.main_block;
        if (!child.requirements.error_handling.enabled && base.requirements.error_handling.enabled)
            child.requirements.error_handling = base.requirements.error_handling;
        if (!child.requirements.strict_types.enabled && base.requirements.strict_types.enabled)
            child.requirements.strict_types = base.requirements.strict_types;
        if (!child.requirements.no_global_state.enabled && base.requirements.no_global_state.enabled)
            child.requirements.no_global_state = base.requirements.no_global_state;
        if (!child.requirements.naming_conventions.enabled && base.requirements.naming_conventions.enabled)
            child.requirements.naming_conventions = base.requirements.naming_conventions;
        if (!child.requirements.documentation.enabled && base.requirements.documentation.enabled)
            child.requirements.documentation = base.requirements.documentation;
        if (!child.requirements.version_pinning.enabled && base.requirements.version_pinning.enabled)
            child.requirements.version_pinning = base.requirements.version_pinning;
    }

    // --- Trust policy ---
    {
        const auto& bt = base.trust_policy;
        auto& ct = child.trust_policy;
        const auto& dt = defaults.trust_policy;
        if (parent_wins) {
            if (bt.max_signature_age_days > 0)
                ct.max_signature_age_days = bt.max_signature_age_days;
            if (bt.require_fresh_signature) ct.require_fresh_signature = true;
            if (bt.stale_signature_level != dt.stale_signature_level)
                ct.stale_signature_level = bt.stale_signature_level;
        } else {
            if (ct.max_signature_age_days == dt.max_signature_age_days && bt.max_signature_age_days > 0)
                ct.max_signature_age_days = bt.max_signature_age_days;
            if (!ct.require_fresh_signature && bt.require_fresh_signature)
                ct.require_fresh_signature = true;
            if (ct.stale_signature_level == dt.stale_signature_level &&
                bt.stale_signature_level != dt.stale_signature_level)
                ct.stale_signature_level = bt.stale_signature_level;
        }
        if (!ct.check_key_expiry && bt.check_key_expiry) ct.check_key_expiry = true;
        if (!ct.check_revocation && bt.check_revocation) ct.check_revocation = true;
    }

    // --- Approval ---
    if (child.approval.approver_keys.empty() && !base.approval.approver_keys.empty())
        child.approval.approver_keys = base.approval.approver_keys;
    if (child.approval.default_expiry_hours == defaults.approval.default_expiry_hours &&
        base.approval.default_expiry_hours != defaults.approval.default_expiry_hours)
        child.approval.default_expiry_hours = base.approval.default_expiry_hours;

    // --- Agent dispatch ---
    {
        const auto& bad = base.agent_dispatch;
        auto& cad = child.agent_dispatch;
        const auto& dad = defaults.agent_dispatch;
        if (parent_wins) {
            if (bad.max_concurrent != dad.max_concurrent) cad.max_concurrent = bad.max_concurrent;
            if (bad.pool_size != dad.pool_size) cad.pool_size = bad.pool_size;
            if (bad.pool_queue_max != dad.pool_queue_max) cad.pool_queue_max = bad.pool_queue_max;
            if (bad.max_retries_per_run > 0) cad.max_retries_per_run = bad.max_retries_per_run;
            if (bad.hard_stop.max_calls_per_run > 0) cad.hard_stop.max_calls_per_run = bad.hard_stop.max_calls_per_run;
            if (bad.hard_stop.max_tokens_per_run > 0) cad.hard_stop.max_tokens_per_run = bad.hard_stop.max_tokens_per_run;
            if (bad.hard_stop.max_agent_time_ms > 0) cad.hard_stop.max_agent_time_ms = bad.hard_stop.max_agent_time_ms;
            if (bad.hard_stop.consecutive_failure_limit > 0)
                cad.hard_stop.consecutive_failure_limit = bad.hard_stop.consecutive_failure_limit;
            if (bad.hard_stop.action != dad.hard_stop.action) cad.hard_stop.action = bad.hard_stop.action;
        } else {
            if (cad.max_concurrent == dad.max_concurrent && bad.max_concurrent != dad.max_concurrent)
                cad.max_concurrent = bad.max_concurrent;
            if (cad.pool_size == dad.pool_size && bad.pool_size != dad.pool_size)
                cad.pool_size = bad.pool_size;
            if (cad.pool_queue_max == dad.pool_queue_max && bad.pool_queue_max != dad.pool_queue_max)
                cad.pool_queue_max = bad.pool_queue_max;
            if (cad.max_retries_per_run == 0 && bad.max_retries_per_run > 0)
                cad.max_retries_per_run = bad.max_retries_per_run;
            if (cad.hard_stop.max_calls_per_run == 0 && bad.hard_stop.max_calls_per_run > 0)
                cad.hard_stop.max_calls_per_run = bad.hard_stop.max_calls_per_run;
            if (cad.hard_stop.max_tokens_per_run == 0 && bad.hard_stop.max_tokens_per_run > 0)
                cad.hard_stop.max_tokens_per_run = bad.hard_stop.max_tokens_per_run;
            if (cad.hard_stop.max_agent_time_ms == 0 && bad.hard_stop.max_agent_time_ms > 0)
                cad.hard_stop.max_agent_time_ms = bad.hard_stop.max_agent_time_ms;
            if (cad.hard_stop.consecutive_failure_limit == 0 && bad.hard_stop.consecutive_failure_limit > 0)
                cad.hard_stop.consecutive_failure_limit = bad.hard_stop.consecutive_failure_limit;
            if (cad.hard_stop.action == dad.hard_stop.action && bad.hard_stop.action != dad.hard_stop.action)
                cad.hard_stop.action = bad.hard_stop.action;
        }
    }

    // --- Audit ---
    if (parent_wins) {
        if (base.audit.level != defaults.audit.level) child.audit.level = base.audit.level;
    } else {
        if (child.audit.level == defaults.audit.level && base.audit.level != defaults.audit.level)
            child.audit.level = base.audit.level;
    }
    if (child.audit.output_file == defaults.audit.output_file &&
        base.audit.output_file != defaults.audit.output_file)
        child.audit.output_file = base.audit.output_file;
    if (!child.audit.tamper_evidence.enabled && base.audit.tamper_evidence.enabled)
        child.audit.tamper_evidence = base.audit.tamper_evidence;
    if (!child.audit.provenance.enabled && base.audit.provenance.enabled)
        child.audit.provenance = base.audit.provenance;

    // --- Hooks ---
    if (child.hooks.on_violation.command.empty() && !base.hooks.on_violation.command.empty())
        child.hooks.on_violation = base.hooks.on_violation;
    if (child.hooks.on_override.command.empty() && !base.hooks.on_override.command.empty())
        child.hooks.on_override = base.hooks.on_override;
    if (child.hooks.on_complete.command.empty() && !base.hooks.on_complete.command.empty())
        child.hooks.on_complete = base.hooks.on_complete;
    if (child.hooks.pre_check.command.empty() && !base.hooks.pre_check.command.empty())
        child.hooks.pre_check = base.hooks.pre_check;
    if (child.hooks.post_check.command.empty() && !base.hooks.post_check.command.empty())
        child.hooks.post_check = base.hooks.post_check;

    // --- Languages ---
    if (parent_wins) {
        if (base.languages.require_explicit) child.languages.require_explicit = true;
    } else {
        if (!child.languages.require_explicit && base.languages.require_explicit)
            child.languages.require_explicit = true;
    }
    if (child.languages.allowed.empty() && !base.languages.allowed.empty())
        child.languages.allowed = base.languages.allowed;
    if (child.languages.blocked.empty() && !base.languages.blocked.empty())
        child.languages.blocked = base.languages.blocked;
    for (const auto& [lang, cfg] : base.languages.per_language) {
        if (child.languages.per_language.find(lang) == child.languages.per_language.end())
            child.languages.per_language[lang] = cfg;
    }

    // --- Output ---
    if (child.output.max_advisories == defaults.output.max_advisories &&
        base.output.max_advisories != defaults.output.max_advisories)
        child.output.max_advisories = base.output.max_advisories;
    if (child.output.file_output.report_json.empty() && !base.output.file_output.report_json.empty())
        child.output.file_output.report_json = base.output.file_output.report_json;
    if (child.output.file_output.report_sarif.empty() && !base.output.file_output.report_sarif.empty())
        child.output.file_output.report_sarif = base.output.file_output.report_sarif;
    if (child.output.file_output.report_junit.empty() && !base.output.file_output.report_junit.empty())
        child.output.file_output.report_junit = base.output.file_output.report_junit;
    if (child.output.file_output.report_csv.empty() && !base.output.file_output.report_csv.empty())
        child.output.file_output.report_csv = base.output.file_output.report_csv;
    if (child.output.file_output.report_html.empty() && !base.output.file_output.report_html.empty())
        child.output.file_output.report_html = base.output.file_output.report_html;

    // --- Polyglot ---
    if (!child.polyglot.context_isolation.enabled && base.polyglot.context_isolation.enabled)
        child.polyglot.context_isolation = base.polyglot.context_isolation;
    if (!child.polyglot.variable_binding.require_explicit && base.polyglot.variable_binding.require_explicit)
        child.polyglot.variable_binding.require_explicit = true;
    if (child.polyglot.variable_binding.max_bound_variables == 0 &&
        base.polyglot.variable_binding.max_bound_variables > 0)
        child.polyglot.variable_binding.max_bound_variables = base.polyglot.variable_binding.max_bound_variables;
    if (!child.polyglot.output.require_json_pipe && base.polyglot.output.require_json_pipe)
        child.polyglot.output.require_json_pipe = true;
    if (!child.polyglot.output.require_naab_return && base.polyglot.output.require_naab_return)
        child.polyglot.output.require_naab_return = true;
    if (child.polyglot.output.max_output_lines == 0 && base.polyglot.output.max_output_lines > 0)
        child.polyglot.output.max_output_lines = base.polyglot.output.max_output_lines;
    if (child.polyglot.parallel.max_parallel_blocks == 0 && base.polyglot.parallel.max_parallel_blocks > 0)
        child.polyglot.parallel.max_parallel_blocks = base.polyglot.parallel.max_parallel_blocks;
    if (child.polyglot.parallel.timeout_per_block == 0 && base.polyglot.parallel.timeout_per_block > 0)
        child.polyglot.parallel.timeout_per_block = base.polyglot.parallel.timeout_per_block;
    if (child.polyglot.parallel.fail_strategy == defaults.polyglot.parallel.fail_strategy &&
        base.polyglot.parallel.fail_strategy != defaults.polyglot.parallel.fail_strategy)
        child.polyglot.parallel.fail_strategy = base.polyglot.parallel.fail_strategy;
    if (child.polyglot.persistent_runtime.max_sessions == 0 && base.polyglot.persistent_runtime.max_sessions > 0)
        child.polyglot.persistent_runtime.max_sessions = base.polyglot.persistent_runtime.max_sessions;
    if (child.polyglot.persistent_runtime.session_timeout == 0 && base.polyglot.persistent_runtime.session_timeout > 0)
        child.polyglot.persistent_runtime.session_timeout = base.polyglot.persistent_runtime.session_timeout;
    if (child.polyglot.persistent_runtime.max_memory_per_session_mb == 0 &&
        base.polyglot.persistent_runtime.max_memory_per_session_mb > 0)
        child.polyglot.persistent_runtime.max_memory_per_session_mb =
            base.polyglot.persistent_runtime.max_memory_per_session_mb;
}

bool GovernanceEngine::loadWithExtends(const std::string& path, int depth,
                                        int max_depth,
                                        std::set<std::string>& visited,
                                        GovernanceRules& child_rules) {
    // Circular detection
    auto canonical = std::filesystem::weakly_canonical(path).string();
    if (visited.count(canonical)) {
        fmt::print(stderr, "[governance] Circular extends detected: {}\n", canonical);
        return false;
    }
    visited.insert(canonical);

    // C4: max_depth passed from originating child — consistent throughout chain
    if (max_depth <= 0) max_depth = 5;
    if (depth > max_depth) {
        fmt::print(stderr, "[governance] extends chain exceeds max_depth ({})\n", max_depth);
        return false;
    }

    // Load this config file
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        fmt::print(stderr, "[governance] Cannot open extends base: {}\n", path);
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ifs);
    } catch (const std::exception& e) {
        fmt::print(stderr, "[governance] Parse error in extends base {}: {}\n", path, e.what());
        return false;
    }

    if (!checkJsonArrayWidth(j)) {
        fmt::print(stderr, "[governance] extends base rejected: JSON array exceeds cap\n");
        return false;
    }

    // Parse base config into temporary rules
    GovernanceRules base_rules;
    loadFromJson(j, base_rules);

    // Verify signature on base config
    if (!verifyFileSignature(path)) {
        fmt::print(stderr, "[governance] extends base signature verification failed: {}\n", path);
        g_governance_hard_block = true;
        return false;
    }

    // Recursively resolve base's extends (if it has one)
    if (!base_rules.extends_path.empty()) {
        auto base_dir = std::filesystem::path(path).parent_path().string();
        auto resolved = resolveExtendsPath(base_rules.extends_path, base_dir);
        if (resolved.empty()) {
            fmt::print(stderr, "[governance] Cannot resolve extends path in base: {}\n",
                       base_rules.extends_path);
            return false;
        }

        // Recursively resolve grandparent — pass base_rules as the child
        if (!loadWithExtends(resolved, depth + 1, max_depth, visited, base_rules)) {
            return false;
        }
    }

    // Merge: base fills gaps in child
    mergeRules(base_rules, child_rules, child_rules.meta.inheritance);

    return true;
}

bool GovernanceEngine::loadFromFile(const std::string& path) {
    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) return false;

        nlohmann::json j = nlohmann::json::parse(ifs);
        // V-GOV-019 (R24): reject configs with any array wider than the cap.
        if (!checkJsonArrayWidth(j)) {
            fmt::print(stderr,
                       "[governance] Config rejected: JSON array exceeds {} elements "
                       "(V-GOV-019 cap)\n", MAX_GOV_ARRAY_ELEMS);
            return false;
        }
        auto new_rules = std::make_shared<GovernanceRules>();
        loadFromJson(j, *new_rules);
        loaded_path_ = path;

        // H4: verify child signature BEFORE resolving extends chain —
        // reject untrusted children before touching anything they reference
        if (!verifyFileSignature(path)) {
            g_governance_hard_block = true;
            return false;
        }

        // --- Policy distribution: resolve extends chain ---
        if (!new_rules->extends_path.empty()) {
            auto config_dir = std::filesystem::path(path).parent_path().string();
            auto resolved = resolveExtendsPath(new_rules->extends_path, config_dir);
            if (resolved.empty()) {
                fmt::print(stderr, "[governance] Cannot resolve extends: {}\n",
                           new_rules->extends_path);
                return false;
            }
            std::set<std::string> visited;
            visited.insert(std::filesystem::weakly_canonical(path).string());
            int max_depth = new_rules->meta.inheritance.max_depth;
            if (max_depth <= 0) max_depth = 5;
            if (!loadWithExtends(resolved, 1, max_depth, visited, *new_rules)) {
                return false;
            }
        }

        active_.store(new_rules->mode != GovernanceMode::OFF);

        // Generate unique run_id for telemetry run separation
        if (run_id_.empty()) {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            run_id_ = fmt::format("{}-{}", ms, ::getpid());
        }

        // Initialize telemetry forwarder if webhook configured
        if (!new_rules->telemetry_output.webhook_url.empty() && !telemetry_forwarder_) {
            TelemetryForwarderConfig fwd_cfg;
            fwd_cfg.webhook_url = new_rules->telemetry_output.webhook_url;
            fwd_cfg.auth_header = new_rules->telemetry_output.webhook_auth_header;
            fwd_cfg.batch_size = new_rules->telemetry_output.forward_batch_size;
            fwd_cfg.timeout_ms = new_rules->telemetry_output.forward_timeout_ms;
            fwd_cfg.retry_count = new_rules->telemetry_output.forward_retry_count;
            fwd_cfg.buffer_max = new_rules->telemetry_output.forward_buffer_max;
            fwd_cfg.shutdown_drain_ms = new_rules->telemetry_output.forward_shutdown_drain_ms;
            telemetry_forwarder_ = std::make_shared<TelemetryForwarder>(fwd_cfg);
        }

        // Store mtime for mid-run reload detection (truncate to seconds for portability)
        try {
            auto ftime = std::filesystem::last_write_time(path);
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                ftime.time_since_epoch());
            loaded_mtime_ns_ = static_cast<int64_t>(secs.count());
        } catch (...) {
            loaded_mtime_ns_ = 0;
        }

        // EVA-11/EVA-12: Enforce minimum levels for anti-evasion checks
        enforceMinimumLevels(*new_rules);

        // V-SC-006-ext: Apply subprocess env scrub policy from capabilities.env_vars
        {
            const auto& ev = new_rules->capabilities.env_vars;
            runtime::EnvScrubPolicy policy;
            if (!ev.subprocess_scrub_mode.empty() ||
                !ev.blocked_subprocess_prefixes.empty() ||
                !ev.blocked_subprocess_vars.empty() ||
                !ev.allowed_subprocess_vars.empty()) {
                policy.active = true;
                if (ev.subprocess_scrub_mode == "allowlist") {
                    policy.mode = runtime::EnvScrubMode::ALLOWLIST;
                    policy.allowed_vars = ev.allowed_subprocess_vars;
                } else {
                    policy.mode = runtime::EnvScrubMode::BLOCKLIST;
                    policy.blocked_vars = ev.blocked_subprocess_vars;
                    policy.blocked_prefixes = ev.blocked_subprocess_prefixes;
                }
                runtime::setEnvScrubPolicy(policy);
            }
        }

        // Configure behavioral sequence + context drift detectors
        if (new_rules->behavioral_sequences.enabled) {
            sequence_detector_.configure(new_rules->behavioral_sequences);
        }
        if (new_rules->context_drift.enabled) {
            drift_analyzer_.configure(new_rules->context_drift);
        }
        bsd_enabled_.store(new_rules->behavioral_sequences.enabled, std::memory_order_release);
        cdd_enabled_.store(new_rules->context_drift.enabled, std::memory_order_release);

        // C1: Publish rules atomically — all mutations done
        std::atomic_store(&rules_ptr_,
            std::const_pointer_cast<const GovernanceRules>(new_rules));

        // Signature already verified before extends resolution (H4 ordering)

        // Schema key validation is done by caller (main.cpp) where --no-governance is known

        // FIX-DX-15: Schema validation warnings (only emit once per process)
        static bool schema_warnings_emitted = false;
        if (!schema_warnings_emitted) {
            schema_warnings_emitted = true;
            // Warn about sanitizer patterns prone to false positives
            // Exempt well-known type-cast builtins (int(, float(, string(, bool()
            static const std::set<std::string> builtin_casts = {
                "int(", "float(", "string(", "bool("
            };
            for (const auto& san : rules().taint_tracking.sanitizers) {
                if (!san.empty() && san.back() == '(' &&
                    builtin_casts.find(san) == builtin_casts.end()) {
                    fmt::print(stderr, "[WARN] Sanitizer '{}' ends with '(' — with prefix matching, "
                               "names like 'sprint(' or 'print(' will also match if they start with '{}'. "
                               "Consider using a prefix like 'sanitize_' instead.\n", san, san);
                }
            }
            // Warn about empty scope patterns
            for (const auto& scope : rules().scopes) {
                if (scope.glob_pattern.empty()) {
                    fmt::print(stderr, "[WARN] A scope has an empty pattern — will match nothing.\n");
                }
            }
        }

        // Contradiction Detection: static analysis of config conflicts
        detectContradictions();

        // Environment Attestation: verify prerequisites if configured
        runAttestation();

        return true;
    } catch (const nlohmann::json::parse_error& e) {
        // naab-29 A-03: Return false instead of throwing — matches loadFromString behavior
        last_error_ = fmt::format(
            "Governance config error: Failed to parse {}\n\n"
            "  JSON error: {}\n\n"
            "  Help:\n"
            "  - Check for missing commas, brackets, or quotes\n"
            "  - Validate your JSON at jsonlint.com\n",
            path, e.what());
        fmt::print(stderr, "[governance] {}\n", last_error_);
        return false;
    } catch (const nlohmann::json::type_error& e) {
        last_error_ = fmt::format(
            "Governance config error: Invalid value type in {}\n\n"
            "  JSON error: {}\n\n"
            "  Help:\n"
            "  - Check that boolean fields are true/false (not strings)\n"
            "  - Check that arrays are [...] not single values\n"
            "  - Check that numbers are not quoted\n",
            path, e.what());
        fmt::print(stderr, "[governance] {}\n", last_error_);
        return false;
    } catch (const nlohmann::json::exception& e) {
        last_error_ = fmt::format(
            "Governance config error: Invalid config in {}\n\n"
            "  JSON error: {}\n",
            path, e.what());
        fmt::print(stderr, "[governance] {}\n", last_error_);
        return false;
    } catch (const std::exception& e) {
        last_error_ = fmt::format(
            "Governance config error: Failed to load {}\n\n"
            "  Error: {}\n",
            path, e.what());
        fmt::print(stderr, "[governance] {}\n", last_error_);
        return false;
    }
}

bool GovernanceEngine::loadFromString(const std::string& json_config) {
    try {
        nlohmann::json j = nlohmann::json::parse(json_config);
        if (!checkJsonArrayWidth(j)) {
            last_error_ = "Config rejected: JSON array exceeds element cap (V-GOV-019)";
            return false;
        }
        auto new_rules = std::make_shared<GovernanceRules>();
        loadFromJson(j, *new_rules);
        loaded_path_ = "<inline>";
        active_.store(new_rules->mode != GovernanceMode::OFF);

        enforceMinimumLevels(*new_rules);

        if (new_rules->behavioral_sequences.enabled)
            sequence_detector_.configure(new_rules->behavioral_sequences);
        if (new_rules->context_drift.enabled)
            drift_analyzer_.configure(new_rules->context_drift);
        bsd_enabled_.store(new_rules->behavioral_sequences.enabled, std::memory_order_release);
        cdd_enabled_.store(new_rules->context_drift.enabled, std::memory_order_release);

        // C1: Publish rules atomically
        std::atomic_store(&rules_ptr_,
            std::const_pointer_cast<const GovernanceRules>(new_rules));

        return true;
    } catch (const nlohmann::json::parse_error& e) {
        last_error_ = std::string("JSON parse error: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        last_error_ = std::string("Config error: ") + e.what();
        return false;
    } catch (...) {
        last_error_ = "Unknown error loading config";
        return false;
    }
}

} // namespace governance
} // namespace naab
