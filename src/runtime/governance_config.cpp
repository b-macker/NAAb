// governance_config.cpp — GovernanceEngine configuration loading
// Extracted from governance.cpp lines 1-1727

#include "naab/governance.h"
#include "naab/language_registry.h"
#include "naab/interpreter.h"
#include "naab/analyzer/task_pattern_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include "naab/safe_regex.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <functional>
#ifndef _WIN32
#  include <sys/file.h>
#endif
#include <fmt/core.h>


namespace naab {
namespace governance {

// ============================================================================
// Destructor
// ============================================================================

GovernanceEngine::~GovernanceEngine() {
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
        case EnforcementLevel::NONE:     return "none";
        case EnforcementLevel::HARD:     return "hard";
        case EnforcementLevel::SOFT:     return "soft";
        case EnforcementLevel::ADVISORY: return "advisory";
    }
    return "unknown";
}

std::string GovernanceEngine::levelToTag(EnforcementLevel level) {
    switch (level) {
        case EnforcementLevel::NONE:     return "NONE";
        case EnforcementLevel::HARD:     return "HARD-MANDATORY";
        case EnforcementLevel::SOFT:     return "SOFT-MANDATORY";
        case EnforcementLevel::ADVISORY: return "ADVISORY";
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
        oss << "\n  To override: run with --governance-override\n";
        oss << "  Note: Override will be logged to the audit trail\n";
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
        if (s == "hard")     return {true, EnforcementLevel::HARD};
        if (s == "soft")     return {true, EnforcementLevel::SOFT};
        if (s == "advisory") return {true, EnforcementLevel::ADVISORY};
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

static void loadFromJson(const nlohmann::json& j, GovernanceRules& rules_) {
    // Mode
    if (j.contains("mode")) {
        std::string mode = j["mode"].get<std::string>();
        if (mode == "enforce")    rules_.mode = GovernanceMode::ENFORCE;
        else if (mode == "audit") rules_.mode = GovernanceMode::AUDIT;
        else if (mode == "off")   rules_.mode = GovernanceMode::OFF;
    }

    // Languages
    if (j.contains("languages")) {
        auto& lang = j["languages"];
        if (lang.contains("allowed")) {
            for (auto& l : lang["allowed"]) {
                rules_.allowed_languages.insert(l.get<std::string>());
            }
        }
        if (lang.contains("blocked")) {
            for (auto& l : lang["blocked"]) {
                rules_.blocked_languages.insert(l.get<std::string>());
            }
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
        }
        if (lim.contains("data") && lim["data"].is_object()) {
            auto& d = lim["data"];
            if (d.contains("array_size")) { rules_.limits.data.array_size = d["array_size"].get<int>(); rules_.max_array_size = rules_.limits.data.array_size; }
            if (d.contains("dict_size")) rules_.limits.data.dict_size = d["dict_size"].get<int>();
            if (d.contains("string_length")) rules_.limits.data.string_length = d["string_length"].get<int>();
            if (d.contains("nesting_depth")) rules_.limits.data.nesting_depth = d["nesting_depth"].get<int>();
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
        }
        if (res.contains("shell_injection") && res["shell_injection"].is_object()) {
            auto& si = res["shell_injection"];
            rules_.restrictions.shell_injection.enabled = true;
            if (si.contains("level")) { auto [en, lv] = parseEnforcementLevel(si["level"]); rules_.restrictions.shell_injection.level = lv; }
            if (si.contains("patterns")) for (auto& p : si["patterns"]) rules_.restrictions.shell_injection.patterns.push_back(p.get<std::string>());
        }
        if (res.contains("privilege_escalation") && res["privilege_escalation"].is_object()) {
            auto& pe = res["privilege_escalation"];
            rules_.restrictions.privilege_escalation.enabled = true;
            if (pe.contains("level")) { auto [en, lv] = parseEnforcementLevel(pe["level"]); rules_.restrictions.privilege_escalation.level = lv; }
            if (pe.contains("block_sudo")) rules_.restrictions.privilege_escalation.block_sudo = pe["block_sudo"].get<bool>();
            if (pe.contains("block_su")) rules_.restrictions.privilege_escalation.block_su = pe["block_su"].get<bool>();
        }
        if (res.contains("code_injection") && res["code_injection"].is_object()) {
            auto& ci = res["code_injection"];
            rules_.restrictions.code_injection.enabled = true;
            if (ci.contains("level")) { auto [en, lv] = parseEnforcementLevel(ci["level"]); rules_.restrictions.code_injection.level = lv; }
            if (ci.contains("block_dynamic_code_gen")) rules_.restrictions.code_injection.block_dynamic_code_gen = ci["block_dynamic_code_gen"].get<bool>();
            if (ci.contains("block_sql_injection_patterns")) rules_.restrictions.code_injection.block_sql_injection_patterns = ci["block_sql_injection_patterns"].get<bool>();
        }
        if (res.contains("crypto") && res["crypto"].is_object()) {
            auto& cr = res["crypto"];
            rules_.restrictions.crypto.enabled = true;
            if (cr.contains("level")) { auto [en, lv] = parseEnforcementLevel(cr["level"]); rules_.restrictions.crypto.level = lv; }
            if (cr.contains("weak_hashes")) for (auto& h : cr["weak_hashes"]) rules_.restrictions.crypto.weak_hashes.push_back(h.get<std::string>());
            if (cr.contains("weak_ciphers")) for (auto& c : cr["weak_ciphers"]) rules_.restrictions.crypto.weak_ciphers.push_back(c.get<std::string>());
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
            }
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
        }

        // encoding
        if (cq.contains("encoding") && cq["encoding"].is_object()) {
            auto& enc = cq["encoding"];
            rules_.code_quality.encoding.enabled = true;
            if (enc.contains("level")) { auto [en, lv] = parseEnforcementLevel(enc["level"]); rules_.code_quality.encoding.level = lv; }
            if (enc.contains("block_null_bytes")) rules_.code_quality.encoding.block_null_bytes = enc["block_null_bytes"].get<bool>();
            if (enc.contains("block_unicode_bidi")) rules_.code_quality.encoding.block_unicode_bidi = enc["block_unicode_bidi"].get<bool>();
        }

        // no_hardcoded_results (expanded)
        if (cq.contains("no_hardcoded_results") && cq["no_hardcoded_results"].is_object()) {
            auto& hr = cq["no_hardcoded_results"];
            rules_.code_quality.no_hardcoded_results.enabled = true;
            rules_.no_hardcoded_results = true;
            if (hr.contains("level")) { auto [en, lv] = parseEnforcementLevel(hr["level"]); rules_.code_quality.no_hardcoded_results.level = lv; rules_.no_hardcoded_results_level = lv; }
            if (hr.contains("check_return_true_false")) rules_.code_quality.no_hardcoded_results.check_return_true_false = hr["check_return_true_false"].get<bool>();
            if (hr.contains("check_dict_success_fields")) rules_.code_quality.no_hardcoded_results.check_dict_success_fields = hr["check_dict_success_fields"].get<bool>();
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
            }
        }

        // Duplicate calls config
        if (cq.contains("duplicate_calls") && cq["duplicate_calls"].is_object()) {
            auto& dc = cq["duplicate_calls"];
            if (dc.contains("enabled")) rules_.code_quality.duplicate_calls.enabled = dc["enabled"].get<bool>();
            if (dc.contains("threshold")) rules_.code_quality.duplicate_calls.threshold = dc["threshold"].get<int>();
            if (dc.contains("max_entries")) rules_.code_quality.duplicate_calls.max_entries = dc["max_entries"].get<int>();
        }

        // Polyglot try/catch config
        if (cq.contains("polyglot_try_catch") && cq["polyglot_try_catch"].is_object()) {
            auto& ptc = cq["polyglot_try_catch"];
            if (ptc.contains("enabled")) rules_.code_quality.polyglot_try_catch.enabled = ptc["enabled"].get<bool>();
            if (ptc.contains("max_entries")) rules_.code_quality.polyglot_try_catch.max_entries = ptc["max_entries"].get<int>();
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
                if (fn_obj.contains("return_length_min")) fc.return_length_min = fn_obj["return_length_min"].get<int>();
                if (fn_obj.contains("return_length_max")) fc.return_length_max = fn_obj["return_length_max"].get<int>();
                if (fn_obj.contains("return_not_null")) fc.return_not_null = fn_obj["return_not_null"].get<bool>();
                if (fn_obj.contains("params") && fn_obj["params"].is_array()) {
                    for (auto& p : fn_obj["params"]) fc.params.push_back(p.get<std::string>());
                }
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
        }
        if (tt.contains("sanitizers") && tt["sanitizers"].is_array()) {
            for (const auto& s : tt["sanitizers"]) {
                rules_.taint_tracking.sanitizers.push_back(s.get<std::string>());
            }
        }
    }

    // --- Telemetry output config ---
    if (j.contains("telemetry") && j["telemetry"].is_object()) {
        auto& tel = j["telemetry"];
        if (tel.contains("enabled")) rules_.telemetry_output.enabled = tel["enabled"].get<bool>();
        if (tel.contains("output_file")) rules_.telemetry_output.output_file = tel["output_file"].get<std::string>();
    }

    // --- Agent roles ---
    if (j.contains("agent_roles") && j["agent_roles"].is_object()) {
        for (auto& [name, role_json] : j["agent_roles"].items()) {
            AgentRoleConfig role;
            role.name = name;
            if (role_json.contains("allowed_languages") && role_json["allowed_languages"].is_array()) {
                for (const auto& l : role_json["allowed_languages"])
                    role.allowed_languages.push_back(l.get<std::string>());
            }
            // V-GOV-020: parse per-role blocked languages
            if (role_json.contains("blocked_languages") && role_json["blocked_languages"].is_array()) {
                for (const auto& l : role_json["blocked_languages"])
                    role.blocked_languages.push_back(l.get<std::string>());
            }
            if (role_json.contains("blocked_paths") && role_json["blocked_paths"].is_array()) {
                for (const auto& p : role_json["blocked_paths"])
                    role.blocked_paths.push_back(p.get<std::string>());
            }
            if (role_json.contains("allowed_paths") && role_json["allowed_paths"].is_array()) {
                for (const auto& p : role_json["allowed_paths"])
                    role.allowed_paths.push_back(p.get<std::string>());
            }
            // V-GOV-018: parse per-role shell capability
            if (role_json.contains("shell_allowed") && role_json["shell_allowed"].is_boolean()) {
                role.shell_allowed = role_json["shell_allowed"].get<bool>();
                role.shell_allowed_set = true;
            }
            // Also support capabilities.shell (same format as global capabilities)
            if (!role.shell_allowed_set && role_json.contains("capabilities") && role_json["capabilities"].is_object()) {
                auto& caps = role_json["capabilities"];
                if (caps.contains("shell") && caps["shell"].is_boolean()) {
                    role.shell_allowed = caps["shell"].get<bool>();
                    role.shell_allowed_set = true;
                } else if (caps.contains("shell") && caps["shell"].is_object()) {
                    auto& sh = caps["shell"];
                    if (sh.contains("enabled") && sh["enabled"].is_boolean()) {
                        role.shell_allowed = sh["enabled"].get<bool>();
                        role.shell_allowed_set = true;
                    }
                }
            }
            rules_.agent_roles.push_back(role);
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

// EVA-11/EVA-12: Governance integrity check
// Prevents LLM config manipulation by ensuring anti-evasion checks have
// minimum enforcement levels. An LLM could write govern.json with all
// checks set to "advisory" (warn-only) to bypass quality gates.
void GovernanceEngine::enforceMinimumLevels() {
    // Helper: elevate advisory to soft for anti-evasion checks
    auto elevate = [](auto& cfg, const char* name) {
        if (cfg.enabled && cfg.level == EnforcementLevel::ADVISORY) {
            fprintf(stderr, "[governance] WARNING: %s was set to advisory — "
                    "elevating to soft (minimum for anti-evasion checks)\n", name);
            cfg.level = EnforcementLevel::SOFT;
        }
    };

    elevate(rules_.code_quality.no_oversimplification, "no_oversimplification");
    elevate(rules_.code_quality.no_incomplete_logic, "no_incomplete_logic");
    elevate(rules_.code_quality.no_simulation_markers, "no_simulation_markers");
    elevate(rules_.code_quality.no_temporary_code, "no_temporary_code");
    elevate(rules_.code_quality.no_apologetic_language, "no_apologetic_language");

    // Warn about contradictory config: code quality checks enabled but mode is audit/off
    if (rules_.mode != GovernanceMode::ENFORCE) {
        if (rules_.code_quality.no_oversimplification.enabled ||
            rules_.code_quality.no_incomplete_logic.enabled) {
            fprintf(stderr, "[governance] WARNING: Code quality checks are enabled but mode is %s. "
                    "Code quality checks will NOT block — use mode: enforce for protection.\n",
                    rules_.mode == GovernanceMode::AUDIT ? "audit" : "off");
        }
    }
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
        loadFromJson(j, rules_);
        loaded_path_ = path;
        active_ = (rules_.mode != GovernanceMode::OFF);

        // EVA-11/EVA-12: Enforce minimum levels for anti-evasion checks
        enforceMinimumLevels();

        // FIX-DX-15: Schema validation warnings
        // Warn about sanitizer patterns prone to false positives
        for (const auto& san : rules_.taint_tracking.sanitizers) {
            if (!san.empty() && san.back() == '(') {
                fmt::print(stderr, "[WARN] Sanitizer '{}' ends with '(' — with prefix matching, "
                           "names like 'sprint(' or 'print(' will also match if they start with '{}'. "
                           "Consider using a prefix like 'sanitize_' instead.\n", san, san);
            }
        }
        // Warn about empty scope patterns
        for (const auto& scope : rules_.scopes) {
            if (scope.glob_pattern.empty()) {
                fmt::print(stderr, "[WARN] A scope has an empty pattern — will match nothing.\n");
            }
        }

        return true;
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(fmt::format(
            "Governance config error: Failed to parse {}\n\n"
            "  JSON error: {}\n\n"
            "  Help:\n"
            "  - Check for missing commas, brackets, or quotes\n"
            "  - Validate your JSON at jsonlint.com\n",
            path, e.what()));
    } catch (const nlohmann::json::type_error& e) {
        throw std::runtime_error(fmt::format(
            "Governance config error: Invalid value type in {}\n\n"
            "  JSON error: {}\n\n"
            "  Help:\n"
            "  - Check that boolean fields are true/false (not strings)\n"
            "  - Check that arrays are [...] not single values\n"
            "  - Check that numbers are not quoted\n",
            path, e.what()));
    }
}

} // namespace governance
} // namespace naab
