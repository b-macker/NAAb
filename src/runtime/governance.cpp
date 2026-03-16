// NAAb Governance Engine Implementation
// Runtime enforcement of project governance rules via govern.json
//
// Three-tier enforcement model (inspired by HashiCorp Sentinel):
//   HARD      - Block execution. No override possible.
//   SOFT      - Block execution. Override with --governance-override flag.
//   ADVISORY  - Warn only. Execution continues.

#include "naab/governance.h"
#include "naab/language_registry.h"
#include "naab/interpreter.h"
#include "naab/analyzer/task_pattern_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <functional>
#include <fmt/core.h>

namespace naab {
namespace governance {

// ============================================================================
// Pattern Databases
// ============================================================================

static const std::vector<SecretPattern> SECRET_PATTERNS = {
    {"sk-[a-zA-Z0-9]{32,}",             "OpenAI API Key", "critical"},
    {"sk-ant-[a-zA-Z0-9\\-]{20,}",      "Anthropic API Key", "critical"},
    {"ghp_[a-zA-Z0-9]{36,}",            "GitHub Personal Access Token", "critical"},
    {"gho_[a-zA-Z0-9]{36,}",            "GitHub OAuth Token", "critical"},
    {"AKIA[0-9A-Z]{16}",                "AWS Access Key ID", "critical"},
    {"-----BEGIN[\\s\\S]*PRIVATE KEY-----", "Private Key", "critical"},
    {"xox[bpsa]-[0-9]{10,13}-[0-9]{10,13}-[a-zA-Z0-9]{24}", "Slack Token", "critical"},
    {"(?:sk|pk)_(?:test|live)_[a-zA-Z0-9]{24,}", "Stripe Key", "critical"},
    {"SG\\.[a-zA-Z0-9_-]{22}\\.[a-zA-Z0-9_-]{43}", "SendGrid Key", "critical"},
    {"AIza[0-9A-Za-z\\-_]{35}",         "Google API Key", "high"},
    {"eyJ[a-zA-Z0-9_-]*\\.[a-zA-Z0-9_-]*\\.[a-zA-Z0-9_-]*", "JWT Token", "high"},
    {"(?:mongodb|postgres|mysql|redis)://[^\\s]+", "Connection String", "high"},
    {"Bearer\\s+[A-Za-z0-9\\-._~+/]+=*", "Bearer Token", "high"},
    {"password\\s*=\\s*['\"][^'\"]{8,}['\"]", "Hardcoded Password", "high"},
    {"api[_-]?key\\s*=\\s*['\"][^'\"]{20,}['\"]", "API Key Assignment", "high"},
    {"token\\s*=\\s*['\"][^'\"]{20,}['\"]", "Hardcoded Token", "high"},
    {"secret\\s*=\\s*['\"][^'\"]{8,}['\"]", "Hardcoded Secret", "high"},
    {"aws_secret_access_key\\s*=\\s*['\"][^'\"]{40}['\"]", "AWS Secret Key", "critical"},
};

static const std::vector<DangerousPattern> DANGEROUS_PATTERNS_DB = {
    // Python
    {"python", "os\\.system\\s*\\(",      "os.system() call",
     "Use subprocess.run() with shell=False, or NAAb stdlib"},
    {"python", "subprocess\\.call\\s*\\(.*shell\\s*=\\s*True",
     "subprocess.call() with shell=True",
     "Use subprocess.run() with shell=False"},
    {"python", "\\beval\\s*\\(",           "eval() call",
     "Use json.loads() for data parsing, ast.literal_eval() for literals"},
    {"python", "\\bexec\\s*\\(",           "exec() call",
     "Restructure code to avoid dynamic execution"},
    {"python", "__import__\\s*\\(",        "__import__() call",
     "Use standard import statements"},
    {"python", "pickle\\.loads?\\s*\\(",   "pickle.load() call",
     "Use json.loads() — pickle can execute arbitrary code"},
    {"python", "yaml\\.load\\s*\\([^)]*(?!Loader)", "yaml.load() without SafeLoader",
     "Use yaml.safe_load() instead"},
    // EVA-EXTRA-2: Additional Python dangerous patterns
    {"python", "\\bcompile\\s*\\(.*\\bexec\\b",  "compile()+exec (dynamic code execution)",
     "Use direct function calls instead of dynamic code generation"},
    {"python", "\\bgetattr\\s*\\(",  "getattr() call (dynamic attribute access)",
     "Use direct attribute access instead of dynamic lookup"},
    {"python", "\\bimportlib\\.",    "importlib usage (dynamic imports)",
     "Use standard import statements"},

    // JavaScript
    {"javascript", "\\beval\\s*\\(",       "eval() call",
     "Parse data with JSON.parse() instead"},
    {"javascript", "\\bFunction\\s*\\(",   "Function() constructor",
     "Define functions statically"},
    {"javascript", "require\\s*\\(\\s*['\"]child_process['\"]\\s*\\)",
     "child_process import",
     "Use NAAb stdlib for subprocess execution"},

    // Shell
    {"shell", "rm\\s+-rf\\s+/",           "rm -rf / (recursive root delete)",
     "Specify exact paths, never recursive from root"},
    {"shell", "\\bdd\\s+if=",             "dd command (disk destroyer)",
     "Use NAAb file module for safe file operations"},
    {"shell", "\\bmkfs\\.",               "mkfs (format filesystem)",
     "Extremely dangerous — do not format filesystems in polyglot blocks"},
    {"shell", ">\\s*/dev/(?!null\\b)",     "Writing to device files (excluding /dev/null)",
     "Avoid writing to device files (> /dev/null is safe and common)"},
    {"shell", "chmod\\s+777",             "chmod 777 (world-writable)",
     "Use specific permissions (644 for files, 755 for executables)"},
    {"shell", "curl[^|]*\\|\\s*(?:ba)?sh\\b", "curl | sh (remote code execution)",
     "Download and inspect scripts before executing"},
    {"shell", "wget[^|]*\\|\\s*(?:ba)?sh\\b", "wget | sh (remote code execution)",
     "Download and inspect scripts before executing"},

    // Any language
    {"any", "\\bsudo\\s",                 "sudo (privilege escalation)",
     "Avoid privilege escalation in polyglot blocks"},
};

// Network library import patterns (for capabilities.network enforcement in polyglot blocks)
static const std::vector<DangerousPattern> NETWORK_IMPORT_PATTERNS = {
    // Python
    {"python", "\\bimport\\s+urllib",            "urllib import (network access)", ""},
    {"python", "\\bfrom\\s+urllib",              "urllib import (network access)", ""},
    {"python", "\\bimport\\s+requests",          "requests import (network access)", ""},
    {"python", "\\bimport\\s+http\\.client",     "http.client import (network access)", ""},
    {"python", "\\bimport\\s+aiohttp",           "aiohttp import (network access)", ""},
    {"python", "\\bimport\\s+socket",            "socket import (network access)", ""},
    {"python", "\\bimport\\s+httpx",             "httpx import (network access)", ""},
    // JavaScript
    {"javascript", "require\\s*\\(\\s*['\"]https?['\"]", "http/https require (network access)", ""},
    {"javascript", "require\\s*\\(\\s*['\"]node-fetch['\"]", "node-fetch require (network access)", ""},
    {"javascript", "\\bfetch\\s*\\(",            "fetch() call (network access)", ""},
    {"javascript", "XMLHttpRequest",             "XMLHttpRequest (network access)", ""},
    // Ruby
    {"ruby", "require\\s+['\"]net/http['\"]",    "net/http require (network access)", ""},
    {"ruby", "require\\s+['\"]open-uri['\"]",    "open-uri require (network access)", ""},
    // Go
    {"go", "\"net/http\"",                        "net/http import (network access)", ""},
};

static const std::vector<std::string> PLACEHOLDER_PATTERNS_DB = {
    "TODO", "FIXME", "STUB", "PLACEHOLDER", "XXX", "TBD",
    "HACK", "IMPLEMENT_ME", "RUNTIME_COMPUTED",
    "NOT_IMPLEMENTED", "UNFINISHED", "INCOMPLETE", "TEMPORARY",
    "PROTOTYPE", "DRAFT", "WIP", "WORK_IN_PROGRESS",
    "NEEDS_IMPLEMENTATION", "IMPLEMENT_LATER", "NEEDS_WORK",
    "NOT_YET_IMPLEMENTED", "UNIMPLEMENTED", "SKELETON",
    "BOILERPLATE", "SAMPLE_DATA", "DUMMY_DATA", "FAKE_DATA",
    "MOCK_RESULT", "SIMULATED", "HARDCODED_RESPONSE"
};

struct HardcodedResultPattern {
    std::string pattern;
    std::string description;
};

static const std::vector<HardcodedResultPattern> HARDCODED_RESULT_PATTERNS_DB = {
    // Return-with-comment patterns (both # and // comment styles)
    {"return\\s+True\\s*(?:#|//)",    "Hardcoded return True with comment"},
    {"return\\s+False\\s*(?:#|//)",   "Hardcoded return False with comment"},
    {"return\\s+0\\s*(?:#|//)",       "Hardcoded return 0 with comment"},
    {"return\\s+None\\s*(?:#|//)",    "Hardcoded return None with comment"},
    // EVA-EXTRA-4: Comment markers — both # and // styles
    {"(?:#|//)\\s*for now",           "Temporary implementation marker"},
    {"(?:#|//)\\s*simplified",        "Simplified implementation marker"},
    {"(?:#|//)\\s*placeholder",       "Placeholder implementation marker"},
    {"(?:#|//)\\s*stub",              "Stub implementation marker"},
    {"(?:#|//)\\s*not implemented",   "Not implemented marker"},
    {"(?:#|//)\\s*basic implementation", "Basic implementation marker"},
    {"(?:#|//)\\s*minimal",           "Minimal implementation marker"},
};

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
        loadFromJson(j, rules_);
        loaded_path_ = path;
        active_ = (rules_.mode != GovernanceMode::OFF);

        // EVA-11/EVA-12: Enforce minimum levels for anti-evasion checks
        enforceMinimumLevels();

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

bool GovernanceEngine::discoverAndLoad(const std::string& start_dir) {
    namespace fs = std::filesystem;

    fs::path dir(start_dir);
    while (true) {
        fs::path candidate = dir / "govern.json";
        if (fs::exists(candidate)) {
            bool loaded = loadFromFile(candidate.string());
            if (!loaded) return false;

            // Project Context Awareness — load supplemental rules from project files
            if (rules_.project_context.enabled) {
                ProjectContextLoader loader;
                auto extractions = loader.loadContext(start_dir, rules_.project_context);

                if (!extractions.empty()) {
                    if (!rules_.project_context.dry_run) {
                        // Parse enforcement level
                        EnforcementLevel ctx_level = EnforcementLevel::ADVISORY;
                        if (rules_.project_context.enforcement_level == "soft")
                            ctx_level = EnforcementLevel::SOFT;
                        else if (rules_.project_context.enforcement_level == "hard")
                            ctx_level = EnforcementLevel::HARD;

                        loader.applyToRules(rules_, extractions, ctx_level);

                        if (rules_.project_context.feed_optimization) {
                            loader.applyOptimizationHints(
                                rules_.polyglot_optimization, extractions);
                        }
                    } else {
                        // Dry run: mark all as dry_run status
                        for (auto& ext : extractions) {
                            if (ext.status.empty()) ext.status = "dry_run";
                        }
                    }

                    if (rules_.project_context.show_extractions) {
                        std::string report = loader.formatReport(extractions);
                        if (!report.empty()) {
                            fprintf(stderr, "%s", report.c_str());
                        }
                    }
                }
            }

            return true;
        }

        fs::path parent = dir.parent_path();
        if (parent == dir) break;  // Reached root
        dir = parent;
    }
    return false;
}

// ============================================================================
// Core Enforcement Logic
// ============================================================================

void GovernanceEngine::recordPass(const std::string& rule_name,
                                   EnforcementLevel level) {
    check_results_.push_back({rule_name, level, true, "", "", "", 0});
}

std::string GovernanceEngine::enforce(
    const std::string& rule_name,
    EnforcementLevel level,
    const std::string& violation_message) {

    // Record the failing check
    check_results_.push_back({rule_name, level, false, violation_message, "", "", 0});

    // Audit mode: never block, just log
    if (rules_.mode == GovernanceMode::AUDIT) {
        fprintf(stderr, "[governance] AUDIT %s: %s\n",
                rule_name.c_str(),
                violation_message.substr(0, violation_message.find('\n')).c_str());
        return "";  // Don't block
    }

    switch (level) {
        case EnforcementLevel::NONE:
            return "";  // Not enforced

        case EnforcementLevel::HARD:
            return violation_message;

        case EnforcementLevel::SOFT:
            if (override_enabled_) {
                fprintf(stderr, "[governance] OVERRIDE %s\n", rule_name.c_str());
                return "";  // Don't block
            }
            return violation_message;

        case EnforcementLevel::ADVISORY:
            fprintf(stderr, "[governance] WARNING %s\n", rule_name.c_str());
            return "";  // Don't block
    }
    return "";
}

// ============================================================================
// Enforcement Checks
// ============================================================================

std::string GovernanceEngine::checkLanguageAllowed(
    const std::string& language, int line) {

    // Check blocked list first
    if (rules_.blocked_languages.count(language)) {
        std::string location = line > 0
            ? fmt::format("line {}: <<{}", line, language)
            : fmt::format("<<{}", language);

        std::string blocked_list;
        for (auto& l : rules_.blocked_languages) {
            if (!blocked_list.empty()) blocked_list += ", ";
            blocked_list += l;
        }

        return enforce("languages.blocked", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Language \"{}\" is blocked", language),
                location,
                fmt::format("languages.blocked contains \"{}\"", language),
                fmt::format("The \"{}\" language is explicitly blocked in governance", language),
                fmt::format("let result = <<{}\n...\n>>", language),
                !rules_.allowed_languages.empty()
                    ? fmt::format("let result = <<{}\n...\n>>",
                        *rules_.allowed_languages.begin())
                    : ""));
    }

    // Check allowed list (only if non-empty — empty means all allowed)
    if (!rules_.allowed_languages.empty() &&
        !rules_.allowed_languages.count(language)) {

        std::string location = line > 0
            ? fmt::format("line {}: <<{}", line, language)
            : fmt::format("<<{}", language);

        std::string allowed_list;
        for (auto& l : rules_.allowed_languages) {
            if (!allowed_list.empty()) allowed_list += ", ";
            allowed_list += l;
        }

        return enforce("languages.allowed", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Language \"{}\" is not allowed", language),
                location,
                fmt::format("languages.allowed = [{}]", allowed_list),
                fmt::format("Only {} polyglot blocks are permitted\n"
                    "To allow {}, add it to the \"allowed\" array in govern.json",
                    allowed_list, language),
                fmt::format("let result = <<{}\n...\n>>", language),
                fmt::format("let result = <<{}\n...\n>>",
                    *rules_.allowed_languages.begin())));
    }

    recordPass("languages", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkNetworkAllowed() {
    if (!rules_.network_allowed) {
        return enforce("capabilities.network", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                "Network access is not allowed",
                "",
                "capabilities.network = false",
                "Network operations are disabled by governance\n"
                "This prevents outbound connections from polyglot blocks",
                "http.get(\"https://api.example.com\")",
                "let data = json.parse(file.read(\"cached_data.json\"))"));
    }
    recordPass("capabilities.network", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkNetworkImports(
    const std::string& language, const std::string& code, int line) {
    if (rules_.network_allowed) {
        recordPass("capabilities.network", EnforcementLevel::HARD);
        return "";
    }
    // Scan polyglot code for network library usage patterns
    for (const auto& pat : NETWORK_IMPORT_PATTERNS) {
        if (pat.language != language && pat.language != "any") continue;
        try {
            std::regex re(pat.pattern);
            if (std::regex_search(code, re)) {
                return enforce("capabilities.network", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        fmt::format("Network access blocked: {} in {} block",
                                    pat.description, language),
                        fmt::format("line {}", line),
                        "capabilities.network = false",
                        "Network operations are disabled by governance.\n"
                        "This prevents outbound connections from polyglot blocks.",
                        "", "Use cached/local data or NAAb stdlib instead"));
            }
        } catch (...) {}
    }
    recordPass("capabilities.network", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkFilesystemAllowed(const std::string& mode) {
    if (rules_.filesystem_mode == "none") {
        return enforce("capabilities.filesystem", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                "Filesystem access is not allowed",
                "",
                "capabilities.filesystem = \"none\"",
                "All filesystem operations are disabled by governance",
                "file.write(\"output.txt\", data)",
                "print(data)  // Use stdout instead"));
    }
    if (rules_.filesystem_mode == "read" && mode == "write") {
        return enforce("capabilities.filesystem", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                "Filesystem write access is not allowed",
                "",
                "capabilities.filesystem = \"read\"",
                "Only read operations are allowed\n"
                "Writing files is disabled by governance",
                "file.write(\"output.txt\", data)",
                "let data = file.read(\"input.txt\")"));
    }
    recordPass("capabilities.filesystem", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkShellAllowed() {
    if (!rules_.shell_allowed) {
        return enforce("capabilities.shell", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                "Shell execution is not allowed",
                "",
                "capabilities.shell = false",
                "Shell/bash polyglot blocks are disabled by governance\n"
                "Use NAAb stdlib or other allowed languages instead",
                "let result = <<shell\nls -la\n>>",
                "let files = file.list(\".\")"));
    }
    recordPass("capabilities.shell", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkCallDepth(size_t current_depth) {
    if (rules_.max_call_depth > 0 &&
        static_cast<int>(current_depth) > rules_.max_call_depth) {
        return enforce("limits.call_depth", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Call depth {} exceeds limit of {}",
                    current_depth, rules_.max_call_depth),
                "",
                fmt::format("limits.call_depth = {}", rules_.max_call_depth),
                "Maximum function call depth exceeded\n"
                "This usually indicates infinite recursion",
                "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkArraySize(size_t size) {
    if (rules_.max_array_size > 0 &&
        static_cast<int>(size) > rules_.max_array_size) {
        return enforce("limits.array_size", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Array size {} exceeds limit of {}",
                    size, rules_.max_array_size),
                "",
                fmt::format("limits.array_size = {}", rules_.max_array_size),
                "Maximum array size exceeded\n"
                "Consider processing data in smaller batches",
                "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkPolyglotOutput(const std::string& output) {
    if (rules_.polyglot_output == "json") {
        // Try to parse as JSON
        try {
            (void)nlohmann::json::parse(output);
        } catch (...) {
            return enforce("restrictions.polyglot_output", EnforcementLevel::HARD,
                formatError(EnforcementLevel::HARD,
                    "Polyglot block must return valid JSON",
                    "",
                    "restrictions.polyglot_output = \"json\"",
                    "All polyglot blocks must return valid JSON output\n"
                    "Use json.dumps() or JSON.stringify() to format output",
                    "print(\"hello world\")",
                    "import json\nprint(json.dumps({\"message\": \"hello world\"}))"));
        }
    }
    return "";
}

std::string GovernanceEngine::checkDangerousCall(
    const std::string& language, const std::string& code, int line) {

    if (!rules_.restrict_dangerous_calls) return "";

    for (const auto& pattern : DANGEROUS_PATTERNS_DB) {
        // Check if pattern applies to this language
        if (pattern.language != "any" && pattern.language != language) continue;

        try {
            std::regex re(pattern.pattern, std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string location = line > 0
                    ? fmt::format("line {}: {} block", line, language)
                    : fmt::format("{} block", language);

                // FIX 29: Include matched text for easier debugging
                std::string matched_text = match[0].str();
                if (matched_text.size() > 60) matched_text = matched_text.substr(0, 60) + "...";

                return enforce("restrictions.dangerous_calls",
                    rules_.dangerous_calls_level,
                    formatError(rules_.dangerous_calls_level,
                        fmt::format("Dangerous pattern in {} block: {}",
                            language, pattern.description),
                        location,
                        fmt::format("restrictions.dangerous_calls = \"{}\"",
                            levelToString(rules_.dangerous_calls_level)),
                        fmt::format("{}\n  Matched: \"{}\"\n{}",
                            pattern.description, matched_text,
                            pattern.safe_alternative),
                        "", ""));
            }
        } catch (const std::regex_error&) {
            // Skip invalid patterns silently
        }
    }

    recordPass("restrictions.dangerous_calls", rules_.dangerous_calls_level);
    return "";
}

std::string GovernanceEngine::checkSecrets(
    const std::string& code, int line) {

    if (!rules_.no_secrets) return "";

    for (const auto& pattern : SECRET_PATTERNS) {
        try {
            std::regex re(pattern.pattern, std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string matched = match[0].str();
                // Mask the secret (show first 4, mask middle, show last 4)
                std::string masked;
                if (matched.size() > 10) {
                    masked = matched.substr(0, 4) +
                             std::string(matched.size() - 8, '*') +
                             matched.substr(matched.size() - 4);
                } else {
                    masked = std::string(matched.size(), '*');
                }

                std::string location = line > 0
                    ? fmt::format("line {}: {}", line, masked)
                    : masked;

                return enforce("code_quality.no_secrets",
                    rules_.no_secrets_level,
                    formatError(rules_.no_secrets_level,
                        fmt::format("Secret detected: {}", pattern.description),
                        location,
                        fmt::format("code_quality.no_secrets = \"{}\"",
                            levelToString(rules_.no_secrets_level)),
                        "Never hardcode secrets in source code\n"
                        "Use environment variables instead",
                        fmt::format("{} = \"{}\"",
                            pattern.description, masked),
                        "import os\n"
                        "key = os.environ[\"YOUR_KEY_NAME\"]\n\n"
                        "  In NAAb:\n"
                        "    let key = env.get_var(\"YOUR_KEY_NAME\")"));
            }
        } catch (const std::regex_error&) {
            // Skip invalid patterns
        }
    }

    recordPass("code_quality.no_secrets", rules_.no_secrets_level);
    return "";
}

std::string GovernanceEngine::checkPlaceholders(
    const std::string& code, int line) {

    if (!rules_.no_placeholders) return "";

    for (const auto& placeholder : PLACEHOLDER_PATTERNS_DB) {
        // Case-insensitive word boundary search
        try {
            std::regex re("\\b" + placeholder + "\\b", std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                // Find the line containing the match
                std::string matched_line;
                std::istringstream stream(code);
                std::string l;
                auto offset = static_cast<int>(match.position());
                int pos = 0;
                while (std::getline(stream, l)) {
                    if (pos + static_cast<int>(l.size()) >= offset) {
                        matched_line = l;
                        break;
                    }
                    pos += l.size() + 1;
                }

                // Trim the matched line
                size_t start = matched_line.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    matched_line = matched_line.substr(start);
                }
                if (matched_line.size() > 80) {
                    matched_line = matched_line.substr(0, 80) + "...";
                }

                std::string location = line > 0
                    ? fmt::format("line {}: {}", line, matched_line)
                    : matched_line;

                return enforce("code_quality.no_placeholders",
                    rules_.no_placeholders_level,
                    formatError(rules_.no_placeholders_level,
                        fmt::format("Placeholder \"{}\" found in code", placeholder),
                        location,
                        fmt::format("code_quality.no_placeholders = \"{}\"",
                            levelToString(rules_.no_placeholders_level)),
                        "Code must be complete — no placeholder markers allowed\n"
                        "Implement the actual functionality instead of deferring",
                        "", ""));
            }
        } catch (const std::regex_error&) {
            // Skip invalid patterns
        }
    }

    recordPass("code_quality.no_placeholders", rules_.no_placeholders_level);
    return "";
}

std::string GovernanceEngine::checkHardcodedResults(
    const std::string& code, int line) {

    if (!rules_.no_hardcoded_results) return "";

    for (const auto& pattern : HARDCODED_RESULT_PATTERNS_DB) {
        try {
            std::regex re(pattern.pattern, std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string matched = match[0].str();
                if (matched.size() > 60) {
                    matched = matched.substr(0, 60) + "...";
                }

                std::string location = line > 0
                    ? fmt::format("line {}: {}", line, matched)
                    : matched;

                return enforce("code_quality.no_hardcoded_results",
                    rules_.no_hardcoded_results_level,
                    formatError(rules_.no_hardcoded_results_level,
                        fmt::format("Hardcoded result: {}", pattern.description),
                        location,
                        fmt::format("code_quality.no_hardcoded_results = \"{}\"",
                            levelToString(rules_.no_hardcoded_results_level)),
                        "Code must contain real logic, not hardcoded return values\n"
                        "Implement actual validation/processing instead",
                        "def validate(data):\n    return True  # for now",
                        "def validate(data):\n"
                        "    if not isinstance(data, dict):\n"
                        "        return False\n"
                        "    return \"name\" in data and \"value\" in data"));
            }
        } catch (const std::regex_error&) {
            // Skip invalid patterns
        }
    }

    recordPass("code_quality.no_hardcoded_results",
        rules_.no_hardcoded_results_level);
    return "";
}

// ============================================================================
// Execution Summary
// ============================================================================

std::string GovernanceEngine::formatSummary() const {
    if (check_results_.empty()) return "";

    int passed = 0, warned = 0, blocked = 0;
    for (const auto& r : check_results_) {
        if (r.passed) {
            passed++;
        } else if (r.level == EnforcementLevel::ADVISORY) {
            warned++;
        } else {
            blocked++;
        }
    }

    std::ostringstream oss;
    std::string mode_str = "enforce";
    if (rules_.mode == GovernanceMode::AUDIT) mode_str = "audit";
    else if (rules_.mode == GovernanceMode::OFF) mode_str = "off";

    oss << "[governance] Summary (mode: " << mode_str << "): "
        << passed << " passed, "
        << warned << " warning" << (warned != 1 ? "s" : "") << ", "
        << blocked << " blocked\n";

    // Deduplicate results by rule_name (show only unique rules)
    std::unordered_map<std::string, const CheckResult*> unique_results;
    for (const auto& r : check_results_) {
        auto it = unique_results.find(r.rule_name);
        if (it == unique_results.end()) {
            unique_results[r.rule_name] = &r;
        } else if (!r.passed) {
            // Prefer showing failures over passes
            unique_results[r.rule_name] = &r;
        }
    }

    for (const auto& [name, r] : unique_results) {
        if (r->passed) {
            oss << fmt::format("  ✓ {:<35} [{}]  PASS\n",
                name, levelToString(r->level));
        } else if (r->level == EnforcementLevel::ADVISORY) {
            // Extract first line of message
            std::string first_line = r->message.substr(
                0, r->message.find('\n'));
            oss << fmt::format("  ⚠ {:<35} [{}]  WARN\n",
                name, levelToString(r->level));
        } else {
            oss << fmt::format("  ✗ {:<35} [{}]  BLOCKED\n",
                name, levelToString(r->level));
        }
    }

    return oss.str();
}

std::string GovernanceEngine::formatSummaryOneLine() const {
    if (check_results_.empty()) return "";

    int passed = 0, warned = 0, blocked = 0;
    for (const auto& r : check_results_) {
        if (r.passed) passed++;
        else if (r.level == EnforcementLevel::ADVISORY) warned++;
        else blocked++;
    }

    // Silent when all passed — clean output
    if (warned == 0 && blocked == 0) return "";

    std::ostringstream oss;
    std::string mode_str = "enforce";
    if (rules_.mode == GovernanceMode::AUDIT) mode_str = "audit";
    else if (rules_.mode == GovernanceMode::OFF) mode_str = "off";

    oss << "[governance] Summary (mode: " << mode_str << "): "
        << passed << " passed, "
        << warned << " warning" << (warned != 1 ? "s" : "") << ", "
        << blocked << " blocked\n";

    // Show details only for non-passing rules
    std::unordered_map<std::string, const CheckResult*> unique_results;
    for (const auto& r : check_results_) {
        if (r.passed) continue;
        auto it = unique_results.find(r.rule_name);
        if (it == unique_results.end()) unique_results[r.rule_name] = &r;
        else if (!r.passed) unique_results[r.rule_name] = &r;
    }
    for (const auto& [name, r] : unique_results) {
        if (r->level == EnforcementLevel::ADVISORY) {
            oss << fmt::format("  ⚠ {:<35} [{}]  WARN\n", name, levelToString(r->level));
        } else {
            oss << fmt::format("  ✗ {:<35} [{}]  BLOCKED\n", name, levelToString(r->level));
        }
    }
    return oss.str();
}

// ============================================================================
// V3.0 New Check Implementations
// ============================================================================

// FIX 16: Strip string literal contents to prevent false positive pattern matches
// This prevents governance checks from triggering on code/paths inside strings
static std::string stripStringLiterals(const std::string& code) {
    std::string result;
    result.reserve(code.size());
    bool in_single = false, in_double = false, in_backtick = false;
    bool escaped = false;
    for (size_t i = 0; i < code.size(); ++i) {
        char c = code[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\' && (in_single || in_double)) { escaped = true; continue; }

        // EVA-6: Triple-double-quote: """...""" (Python docstrings)
        if (c == '"' && !in_single && !in_backtick && !in_double
            && i+2 < code.size() && code[i+1] == '"' && code[i+2] == '"') {
            i += 3;  // skip opening """
            while (i+2 < code.size()) {
                if (code[i] == '"' && code[i+1] == '"' && code[i+2] == '"') {
                    i += 2;  // will be incremented by loop
                    break;
                }
                i++;
            }
            continue;
        }
        // EVA-6: Triple-single-quote: '''...'''
        if (c == '\'' && !in_double && !in_backtick && !in_single
            && i+2 < code.size() && code[i+1] == '\'' && code[i+2] == '\'') {
            i += 3;
            while (i+2 < code.size()) {
                if (code[i] == '\'' && code[i+1] == '\'' && code[i+2] == '\'') {
                    i += 2;
                    break;
                }
                i++;
            }
            continue;
        }

        if (c == '"' && !in_single && !in_backtick) { in_double = !in_double; continue; }
        if (c == '\'' && !in_double && !in_backtick) { in_single = !in_single; continue; }
        if (c == '`' && !in_double && !in_single) { in_backtick = !in_backtick; continue; }
        if (!in_single && !in_double && !in_backtick) {
            result += c;
        }
    }
    return result;
}

// FIX 16: Strip comments (Python #, JS //, C /* */) for checks that shouldn't trigger on comments
static std::string stripComments(const std::string& code) {
    std::string result;
    result.reserve(code.size());
    bool in_line_comment = false;
    bool in_block_comment = false;
    for (size_t i = 0; i < code.size(); ++i) {
        if (in_line_comment) {
            if (code[i] == '\n') { in_line_comment = false; result += '\n'; }
            continue;
        }
        if (in_block_comment) {
            if (code[i] == '*' && i+1 < code.size() && code[i+1] == '/') {
                in_block_comment = false; i++;
            }
            continue;
        }
        if (code[i] == '/' && i+1 < code.size()) {
            if (code[i+1] == '/') { in_line_comment = true; continue; }
            if (code[i+1] == '*') { in_block_comment = true; i++; continue; }
        }
        if (code[i] == '#') { in_line_comment = true; continue; }
        result += code[i];
    }
    return result;
}

// FIX 18: Normalize language aliases for consistent governance matching
static std::string normalizeLanguage(const std::string& language) {
    if (language == "bash" || language == "sh") return "shell";
    if (language == "golang") return "go";
    if (language == "cs") return "csharp";
    if (language == "ts") return "typescript";
    if (language == "c++") return "cpp";
    if (language == "rb") return "ruby";
    if (language == "js") return "javascript";
    if (language == "node") return "javascript";
    return language;
}

// Helper: regex search against a list of patterns
static std::string searchPatterns(const std::string& code,
    const std::vector<std::string>& patterns, bool case_insensitive = true) {
    for (const auto& pat : patterns) {
        try {
            auto flags = std::regex::ECMAScript;
            if (case_insensitive) flags |= std::regex::icase;
            std::regex re(pat, flags);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                return match[0].str();
            }
        } catch (const std::regex_error&) {}
    }
    return "";
}

// --- PII Detection ---
static const std::vector<std::pair<std::string, std::string>> DEFAULT_PII_PATTERNS = {
    {"\\b\\d{3}-\\d{2}-\\d{4}\\b", "SSN"},
    {"\\b\\d{4}[-\\s]?\\d{4}[-\\s]?\\d{4}[-\\s]?\\d{4}\\b", "Credit Card"},
    {"\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}\\b", "Email"},
    {"\\b(?:\\+?1[-.]?)?\\d{3}[-.]?\\d{3}[-.]?\\d{4}\\b", "Phone"},
    {"\\b(?:(?:25[0-5]|2[0-4]\\d|[01]?\\d\\d?)\\.){3}(?:25[0-5]|2[0-4]\\d|[01]?\\d\\d?)\\b", "IP Address"},
};

std::string GovernanceEngine::checkPii(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_pii;
    if (!cfg.enabled) return "";

    std::vector<std::pair<std::string, std::string>> patterns;
    if (cfg.detect_ssn) patterns.push_back(DEFAULT_PII_PATTERNS[0]);
    if (cfg.detect_credit_card) patterns.push_back(DEFAULT_PII_PATTERNS[1]);
    if (cfg.detect_email) patterns.push_back(DEFAULT_PII_PATTERNS[2]);
    if (cfg.detect_phone) patterns.push_back(DEFAULT_PII_PATTERNS[3]);
    if (cfg.detect_ip_address) patterns.push_back(DEFAULT_PII_PATTERNS[4]);

    for (const auto& [pat, desc] : patterns) {
        try {
            std::regex re(pat);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string found = match[0].str();
                // Check allowlist
                bool allowed = false;
                for (const auto& a : cfg.allowlist_patterns) {
                    if (found.find(a) != std::string::npos) { allowed = true; break; }
                }
                if (allowed) continue;

                std::string display = cfg.mask_in_errors
                    ? (found.substr(0, 3) + std::string(found.size() - 3, '*'))
                    : found;
                return enforce("code_quality.no_pii", cfg.level,
                    formatError(cfg.level,
                        fmt::format("PII detected: {} ({})", desc, display),
                        line > 0 ? fmt::format("line {}", line) : "",
                        "code_quality.no_pii",
                        "Remove personally identifiable information from code\nUse environment variables or config files instead",
                        "", ""));
            }
        } catch (const std::regex_error&) {}
    }
    recordPass("code_quality.no_pii", cfg.level);
    return "";
}

// --- Temporary Code ---
static const std::vector<std::string> DEFAULT_TEMP_PATTERNS = {
    // EVA-5: Comment-prefix-agnostic patterns — match # (Python/Shell/Ruby)
    // and // (JS/Go/Rust/NAAb/C++/C#/Java) comment styles
    "(?:#|//)\\s*[Ff]or now", "(?:#|//)\\s*[Tt]emporary", "(?:#|//)\\s*[Qq]uick fix",
    "(?:#|//)\\s*[Ww]ill implement later", "(?:#|//)\\s*[Ss]implified",
    "(?:#|//)\\s*[Bb]asic implementation", "(?:#|//)\\s*[Mm]inimal implementation",
    "(?:#|//)\\s*[Ww]ill (?:replace|refactor|rewrite)",
    "(?:#|//)\\s*[Nn]eeds? (?:refactoring|improvement|work)",
    "(?:#|//)\\s*[Ss]kipping for now", "(?:#|//)\\s*[Dd]efer(?:red)?",
    "(?:#|//)\\s*[Pp]rototype", "(?:#|//)\\s*[Ww]orkaround",
    "(?:#|//)\\s*[Bb]and.?aid",
    // Multi-line comment markers (C-style /* */)
    "\\*\\s*[Tt]emporary", "\\*\\s*[Ss]implified",
    "\\*\\s*[Ff]or now", "\\*\\s*[Pp]laceholder",
};

std::string GovernanceEngine::checkTemporaryCode(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_temporary_code;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_TEMP_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats, !cfg.case_sensitive);
    if (!found.empty()) {
        return enforce("code_quality.no_temporary_code", cfg.level,
            formatError(cfg.level, fmt::format("Temporary code marker: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_temporary_code",
                "The most common fix is to simply DELETE the comment.\n"
                "If the code underneath works, the comment was the only problem.\n"
                "Replace temporary code markers with production implementation.",
                "# for now, just return empty\nresult = []\nreturn result",
                "result = [analyze(item) for item in data]\nreturn result"));
    }
    recordPass("code_quality.no_temporary_code", cfg.level);
    return "";
}

// --- Simulation Markers ---
static const std::vector<std::string> DEFAULT_SIMULATION_PATTERNS = {
    "[Ss]imulate[ds]?", "[Mm]ock(?:ed|ing)?\\s+(?:execution|data|response|result)",
    "[Ww]ould\\s+(?:\\w+\\s+)?in\\s+production", "[Rr]eplace\\s+this\\s+with",
    "[Ff]ake\\s+(?:data|response|result|output|implementation)",
    "[Dd]ummy\\s+(?:data|response|result|output|implementation)",
    "[Ss]tub(?:bed)?\\s+(?:out|implementation|response)",
};

std::string GovernanceEngine::checkSimulationMarkers(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_simulation_markers;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_SIMULATION_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats, !cfg.case_sensitive);
    if (!found.empty()) {
        return enforce("code_quality.no_simulation_markers", cfg.level,
            formatError(cfg.level, fmt::format("Simulation marker: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_simulation_markers",
                "Replace simulated/mocked code with real computation from actual data.",
                "# simulated result\nresult = random.uniform(0.8, 0.95)",
                "result = len(matches) / len(total) if total else 0.0"));
    }
    recordPass("code_quality.no_simulation_markers", cfg.level);
    return "";
}

// --- Mock Data ---
std::string GovernanceEngine::checkMockData(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_mock_data;
    if (!cfg.enabled) return "";

    auto& prefixes = cfg.variable_prefixes.empty()
        ? (const std::vector<std::string>&)(std::vector<std::string>{"mock_", "fake_", "dummy_", "stub_", "sample_", "example_"})
        : cfg.variable_prefixes;

    for (const auto& prefix : prefixes) {
        try {
            std::regex re("\\b" + prefix + "\\w+", std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                return enforce("code_quality.no_mock_data", cfg.level,
                    formatError(cfg.level, fmt::format("Mock data variable: \"{}\"", match[0].str()),
                        line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_mock_data",
                        "Use real data sources instead of mock/fake data.\n"
                        "Rename variables to describe their actual purpose.",
                        "mock_users = [{\"name\": \"John\"}]",
                        "users = load_users(data_path)"));
            }
        } catch (const std::regex_error&) {}
    }

    // Check literal patterns
    static const std::vector<std::string> default_literals = {
        "['\"]foo['\"]", "['\"]bar['\"]", "['\"]baz['\"]",
        "['\"]lorem ipsum['\"]", "['\"]John Doe['\"]", "['\"]Jane Doe['\"]",
        "['\"]123 Main St['\"]", "['\"]test@test\\.com['\"]",
    };
    auto& lits = cfg.literal_patterns.empty() ? default_literals : cfg.literal_patterns;
    std::string found = searchPatterns(code, lits);
    if (!found.empty()) {
        return enforce("code_quality.no_mock_data", cfg.level,
            formatError(cfg.level, fmt::format("Mock literal: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_mock_data",
                "Replace placeholder literals with computed or configured values.",
                "email = \"test@test.com\"\nname = \"John Doe\"",
                "email = config.get(\"admin_email\")\nname = user.get(\"display_name\")"));
    }

    recordPass("code_quality.no_mock_data", cfg.level);
    return "";
}

// --- Apologetic Language ---
std::string GovernanceEngine::checkApologeticLanguage(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_apologetic_language;
    if (!cfg.enabled) return "";

    static const std::vector<std::string> default_apology_patterns = {
        "[Ii]'?m\\s+(?:very\\s+)?sorry", "[Ii]\\s+apologize", "[Mm]y\\s+apologies",
        "[Oo]ops!?", "[Yy]ikes!?", "[Uu]h\\s+oh!?",
        "[Ii]'?ll\\s+fix\\s+(?:it|this)\\s+(?:immediately|right\\s+away)",
        "[Ii]\\s+didn'?t\\s+(?:check|verify|test)",
        "[Ii]\\s+should\\s+have\\s+(?:checked|verified|tested)",
    };

    std::string found = searchPatterns(code, default_apology_patterns);
    if (!found.empty()) {
        return enforce("code_quality.no_apologetic_language", cfg.level,
            formatError(cfg.level, fmt::format("Apologetic language: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_apologetic_language",
                "LLM-generated code should not contain apologies or self-deprecation.\n"
                "This indicates the code may not have been properly verified.\n"
                "Delete the apologetic comment — if the code works, ship it.",
                "# I'm sorry, this is a basic implementation\ndef process(data): pass",
                "def process(data):\n    return [transform(item) for item in data]"));
    }
    recordPass("code_quality.no_apologetic_language", cfg.level);
    return "";
}

// --- Dead Code ---
std::string GovernanceEngine::checkDeadCode(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_dead_code;
    if (!cfg.enabled) return "";

    static const std::vector<std::string> default_dead_patterns = {
        "if\\s+(?:True|1)\\s*:", "if\\s+(?:False|0)\\s*:",
        "except:\\s*(?:pass|\\.\\.\\.)\\s*$",
    };

    auto& pats = cfg.patterns.empty() ? default_dead_patterns : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("code_quality.no_dead_code", cfg.level,
            formatError(cfg.level, fmt::format("Dead code pattern: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_dead_code",
                "Remove dead/unreachable code. Code behind always-true/false conditions\n"
                "or empty except blocks serves no purpose.",
                "if True:\n    do_thing()  # always runs — remove the if\nexcept: pass  # swallows errors",
                "do_thing()  # just call it directly\nexcept ValueError as e:\n    log.error(e); raise"));
    }
    recordPass("code_quality.no_dead_code", cfg.level);
    return "";
}

// --- Debug Artifacts ---
static const std::vector<std::string> DEFAULT_DEBUG_PATTERNS = {
    // Only flag print/log when clearly debug-related (contains "debug" keyword)
    "print\\(.*debug", "console\\.debug\\(",
    // Actual debug tools — these ARE debug artifacts
    "import\\s+pdb", "import\\s+ipdb", "breakpoint\\(\\)",
    "debugger;?", "binding\\.pry",
    // NOTE: fmt.Println, console.log, System.out.println are standard I/O,
    // NOT debug artifacts. They are the normal output mechanism in polyglot blocks.
};

std::string GovernanceEngine::checkDebugArtifacts(const std::string& language,
                                                   const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_debug_artifacts;
    if (!cfg.enabled) return "";

    auto& pats = cfg.patterns.empty() ? DEFAULT_DEBUG_PATTERNS : cfg.patterns;
    for (const auto& pat : pats) {
        try {
            std::regex re(pat, std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string found = match[0].str();
                // Check allowlist
                bool allowed = false;
                for (const auto& a : cfg.allowlist) {
                    if (found.find(a) != std::string::npos) { allowed = true; break; }
                }
                if (allowed) continue;

                return enforce("code_quality.no_debug_artifacts", cfg.level,
                    formatError(cfg.level, fmt::format("Debug artifact in {} block: \"{}\"", language, found),
                        line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_debug_artifacts",
                        "Remove debug statements before deployment.\n"
                        "Note: print(), console.log(), fmt.Println() are standard I/O, NOT debug.\n"
                        "Only debugger tools (pdb, breakpoint(), debugger;) are flagged.",
                        "import pdb; pdb.set_trace()\nbreakpoint()",
                        "# Remove debug tools entirely — use print() for normal output"));
            }
        } catch (const std::regex_error&) {}
    }
    recordPass("code_quality.no_debug_artifacts", cfg.level);
    return "";
}

// --- Unsafe Deserialization ---
static const std::vector<std::string> DEFAULT_DESER_PATTERNS = {
    "pickle\\.loads?\\(", "yaml\\.load\\(", "yaml\\.unsafe_load",
    "marshal\\.load", "shelve\\.open", "jsonpickle\\.decode",
    "unserialize\\(", "ObjectInputStream", "BinaryFormatter\\.Deserialize",
};

std::string GovernanceEngine::checkUnsafeDeserialization(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_unsafe_deserialization;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_DESER_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("code_quality.no_unsafe_deserialization", cfg.level,
            formatError(cfg.level, fmt::format("Unsafe deserialization: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_unsafe_deserialization",
                "Use safe deserialization methods (json.loads, yaml.safe_load).\n"
                "Unsafe deserializers can execute arbitrary code from untrusted input.",
                "data = pickle.loads(user_input)\nconfig = yaml.load(file)",
                "data = json.loads(user_input)\nconfig = yaml.safe_load(file)"));
    }
    recordPass("code_quality.no_unsafe_deserialization", cfg.level);
    return "";
}

// --- SQL Injection ---
static const std::vector<std::string> DEFAULT_SQL_PATTERNS = {
    "(?:SELECT|INSERT|UPDATE|DELETE|DROP|ALTER)\\s+.*['\"]\\s*\\+",
    "(?:SELECT|INSERT|UPDATE|DELETE)\\s+.*%s",
    "f['\"].*(?:SELECT|INSERT|UPDATE|DELETE).*\\{",
    "\\.format\\(.*(?:SELECT|INSERT|UPDATE|DELETE)",
};

std::string GovernanceEngine::checkSqlInjection(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_sql_injection;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_SQL_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("code_quality.no_sql_injection", cfg.level,
            formatError(cfg.level, "SQL injection pattern detected",
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_sql_injection",
                "Use parameterized queries instead of string concatenation",
                "cursor.execute(\"SELECT * FROM users WHERE id=\" + user_id)",
                "cursor.execute(\"SELECT * FROM users WHERE id=?\", (user_id,))"));
    }
    recordPass("code_quality.no_sql_injection", cfg.level);
    return "";
}

// --- Path Traversal ---
static const std::vector<std::string> DEFAULT_PATH_PATTERNS = {
    // FIX 23: Require path context to avoid ellipsis false positives
    // "Initializing... /tmp" no longer triggers (g before .., 3 dots)
    "(?:^|[/\\\\\"'\\s])\\.\\.(?:/|\\\\)",  // ../ or ..\ preceded by path separator, quote, or space
    "%2e%2e(?:%2f|/)",                       // URL-encoded traversal
    "\\.\\.%2f",                             // Partial URL-encoding
};

std::string GovernanceEngine::checkPathTraversal(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_path_traversal;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_PATH_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        // FIX 29: Include matched text in error for easier debugging
        return enforce("code_quality.no_path_traversal", cfg.level,
            formatError(cfg.level,
                fmt::format("Path traversal pattern detected: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_path_traversal",
                "Use absolute paths or os.path.realpath() to prevent traversal.\n"
                "Never construct paths by concatenating user input with '..'.",
                "path = base_dir + \"/../\" + user_input",
                "path = os.path.realpath(os.path.join(base_dir, user_input))\n"
                "assert path.startswith(base_dir)  # validate resolved path"));
    }
    recordPass("code_quality.no_path_traversal", cfg.level);
    return "";
}

// --- Hardcoded URLs ---
std::string GovernanceEngine::checkHardcodedUrls(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_hardcoded_urls;
    if (!cfg.enabled) return "";
    try {
        std::regex re("https?://(?!example\\.com|localhost|127\\.0\\.0\\.1|0\\.0\\.0\\.0)[a-zA-Z0-9.-]+");
        std::smatch match;
        if (std::regex_search(code, match, re)) {
            std::string url = match[0].str();
            for (const auto& a : cfg.allowlist) {
                if (url.find(a) != std::string::npos) { recordPass("code_quality.no_hardcoded_urls", cfg.level); return ""; }
            }
            return enforce("code_quality.no_hardcoded_urls", cfg.level,
                formatError(cfg.level, fmt::format("Hardcoded URL: \"{}\"", url),
                    line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_hardcoded_urls",
                    "Use configuration or environment variables for URLs.\n"
                    "localhost, 127.0.0.1, and example.com are allowed.",
                    "api_url = \"https://api.production.com/v1\"",
                    "api_url = os.environ.get(\"API_URL\", \"http://localhost:8080\")"));
        }
    } catch (const std::regex_error&) {}
    recordPass("code_quality.no_hardcoded_urls", cfg.level);
    return "";
}

// --- Hardcoded IPs ---
std::string GovernanceEngine::checkHardcodedIps(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_hardcoded_ips;
    if (!cfg.enabled) return "";
    try {
        std::regex re("\\b(?:(?:25[0-5]|2[0-4]\\d|[01]?\\d\\d?)\\.){3}(?:25[0-5]|2[0-4]\\d|[01]?\\d\\d?)\\b");
        std::smatch match;
        if (std::regex_search(code, match, re)) {
            std::string ip = match[0].str();
            static const std::vector<std::string> default_allow = {"127.0.0.1", "0.0.0.0", "255.255.255.0", "255.255.255.255"};
            auto& allowlist = cfg.allowlist.empty() ? default_allow : cfg.allowlist;
            for (const auto& a : allowlist) { if (ip == a) { recordPass("code_quality.no_hardcoded_ips", cfg.level); return ""; } }
            return enforce("code_quality.no_hardcoded_ips", cfg.level,
                formatError(cfg.level, fmt::format("Hardcoded IP: \"{}\"", ip),
                    line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_hardcoded_ips",
                    "Use configuration or DNS for IP addresses.\n"
                    "127.0.0.1, 0.0.0.0, and 255.x.x.x are allowed.",
                    "server = \"192.168.1.100\"",
                    "server = os.environ.get(\"SERVER_HOST\", \"localhost\")"));
        }
    } catch (const std::regex_error&) {}
    recordPass("code_quality.no_hardcoded_ips", cfg.level);
    return "";
}

// --- Encoding ---
std::string GovernanceEngine::checkEncoding(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.encoding;
    if (!cfg.enabled) return "";

    if (cfg.block_null_bytes && code.find('\0') != std::string::npos) {
        return enforce("code_quality.encoding", cfg.level,
            formatError(cfg.level, "Null byte detected in code",
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.encoding.block_null_bytes",
                "Null bytes can be used for injection attacks.\n"
                "Ensure code does not contain embedded \\0 characters.",
                "data = \"hello\\x00world\"  # hidden null byte",
                "data = \"helloworld\"  # clean string"));
    }
    if (cfg.block_unicode_bidi) {
        // Check for Unicode bidirectional override characters
        for (size_t i = 0; i < code.size() - 2; i++) {
            unsigned char c1 = code[i], c2 = code[i+1], c3 = code[i+2];
            // U+202A-U+202E, U+2066-U+2069 (UTF-8 encoded)
            if (c1 == 0xE2 && c2 == 0x80 && (c3 >= 0xAA && c3 <= 0xAE)) {
                return enforce("code_quality.encoding", cfg.level,
                    formatError(cfg.level, "Unicode bidirectional override character detected",
                        line > 0 ? fmt::format("line {}", line) : "", "code_quality.encoding.block_unicode_bidi",
                        "Bidi override characters can be used for trojan source attacks.\n"
                        "These invisible Unicode characters make code appear different from what executes.",
                        "access_level = \"user\u202Enimda\"  # hidden bidi override",
                        "access_level = \"admin\"  # plain ASCII string"));
            }
        }
    }
    recordPass("code_quality.encoding", cfg.level);
    return "";
}

// --- Complexity ---
std::string GovernanceEngine::checkComplexity(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.max_complexity;
    if (!cfg.enabled) return "";

    if (cfg.max_lines_per_block > 0) {
        int lines = 1;
        for (char c : code) if (c == '\n') lines++;
        if (lines > cfg.max_lines_per_block) {
            return enforce("code_quality.max_complexity", cfg.level,
                formatError(cfg.level, fmt::format("Block has {} lines (max: {})", lines, cfg.max_lines_per_block),
                    line > 0 ? fmt::format("line {}", line) : "", "code_quality.max_complexity.max_lines_per_block",
                    "Break large blocks into smaller functions or multiple blocks.\n"
                    "Extract helpers and call them from the main block.",
                    "<<python\n# 200 lines of code in one block\n>>",
                    "<<python\ndef helper_a(): ...\ndef helper_b(): ...\nresult = helper_a() + helper_b()\n>>"));
        }
    }
    recordPass("code_quality.max_complexity", cfg.level);
    return "";
}

// ============================================================================
// LLM Anti-Drift Checks
// ============================================================================

// --- Oversimplification ---
static const std::vector<std::string> DEFAULT_OVERSIMPLIFICATION_PATTERNS = {
    // Empty/pass-only function bodies
    "def\\s+\\w+\\([^)]*\\):\\s*pass\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*\\.\\.\\.",
    "function\\s+\\w+\\([^)]*\\)\\s*\\{\\s*\\}",
    "\\w+\\s*=\\s*\\([^)]*\\)\\s*=>\\s*\\{\\s*\\}",
    "=>\\s*(?:null|undefined|None)\\s*[;\\n]",
    "lambda\\s+[^:]+:\\s*None",
    "func\\s+\\w+\\([^)]*\\)\\s*\\{\\s*\\}",
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*\\}",
    // Trivial return stubs
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s+True\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s+False\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s+None\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s+0\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s*\"\"\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s*\\[\\]\\s*$",
    "def\\s+\\w+\\([^)]*\\):\\s*return\\s*\\{\\}\\s*$",
    "function\\s+\\w+[^{]*\\{\\s*return\\s+(?:true|false|null|undefined|0|\"\"|''|\\[\\]|\\{\\})\\s*;?\\s*\\}",
    // Identity/passthrough functions
    "def\\s+validate\\w*\\([^)]+\\):\\s*return\\s+True",
    "def\\s+check\\w*\\([^)]+\\):\\s*return\\s+True",
    "def\\s+is_\\w+\\([^)]+\\):\\s*return\\s+True",
    // Not implemented markers
    "raise\\s+NotImplementedError",
    "throw\\s+new\\s+Error\\([\"']not\\s+implemented",
    "throw\\s+new\\s+Error\\([\"']TODO",
    "panic!\\([\"']not\\s+implemented",
    "panic!\\([\"']todo",
    "unimplemented!\\(\\)",
    "todo!\\(\\)",
    // Comment-only / placeholder bodies
    "#\\s*implementation\\s+here",
    "//\\s*implementation\\s+here",
    "/\\*\\s*\\.\\.\\.\\s*\\*/",
    "#\\s*your\\s+code\\s+here",
    "//\\s*TODO:?\\s*implement",
    "#\\s*add\\s+(?:your|actual|real)\\s+(?:code|logic|implementation)",
    // Hardcoded/fabricated results
    "return\\s+\\{[\"']status[\"']:\\s*[\"'](?:ok|success|done)[\"']",
    "print\\([\"'](?:Processing|Done|Complete|Success|Working)[\"']\\)\\s*$",

    // ============================================================
    // LLM stub evasion patterns — comments that admit code is incomplete
    // EVA-5: Merged #//-prefixed patterns into (?:#|//) for all comment styles
    // ============================================================

    "(?:#|//)\\s*(?:basic|simplified|simple|minimal|trivial|naive|rough|crude)\\s+(?:implementation|version|approach|logic|solution|code|algorithm)",
    "(?:#|//)\\s*(?:for|in)\\s+(?:demonstration|demo|testing|test|example|simplicity|now|this exercise)\\s*(?:purposes?|only)?",
    "(?:#|//)\\s*(?:mock|dummy|fake|placeholder|simulated|hardcoded|synthetic|fabricated|generated)\\s+(?:data|implementation|result|value|response|output|return)",
    "(?:#|//)\\s*(?:will be|to be|should be|needs to be|can be|could be)\\s+(?:replaced|implemented|completed|finished|done|updated|improved|expanded|fleshed out)",
    "(?:#|//)\\s*(?:left as|serves as|acts as|used as)\\s+(?:an?\\s+)?(?:exercise|example|placeholder|starting point|skeleton|template|scaffold|stub|fallback)",
    "(?:#|//)\\s*(?:in a|in the|for a)\\s+(?:real|production|actual|proper|full|complete)\\s+(?:system|application|implementation|version|world|scenario|environment)",
    "(?:#|//)\\s*(?:not\\s+)?(?:fully|completely|properly|actually|really|truly)\\s+(?:implemented|finished|done|functional|working|complete|developed)",
    "(?:#|//)\\s*(?:this is|this was|here we|we just|i just|just)\\s+(?:a simplified|just a|only a|a basic|a rough|a naive|a quick|a temporary|a stopgap|using a simple)",
    "(?:#|//)\\s*(?:for now|temporary|interim|stopgap|quick and dirty|quick fix|short.?term|band.?aid)",
    "(?:#|//)\\s*(?:no.op|noop|no operation|does nothing|empty|stub|pass.?through|identity|dummy|skip|bypass|shortcut)",
    "(?:#|//)\\s*(?:would normally|would actually|should actually|in reality|ideally|in practice)\\s+(?:do|use|call|implement|process|compute|calculate|analyze|check|validate)",
    "(?:#|//)\\s*(?:skipping|omitting|ignoring|bypassing|not doing|not implementing|eliding)\\s+(?:actual|real|proper|full|the)\\s+(?:logic|implementation|computation|analysis|processing|validation|checking)",

    // ============================================================
    // EVA-9: NAAb-specific function stub patterns (fn keyword, {} braces)
    // ============================================================
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+true\\s*\\}",       // fn x() { return true }
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+false\\s*\\}",
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+null\\s*\\}",
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+0\\s*\\}",
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+\\[\\]\\s*\\}",     // fn x() { return [] }
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+\\{\\}\\s*\\}",     // fn x() { return {} }
    "fn\\s+\\w+\\([^)]*\\)\\s*\\{\\s*return\\s+\"\"\\s*\\}",       // fn x() { return "" }

    // ============================================================
    // EVA-EXTRA-1: Creative LLM synonyms for stubs
    // ============================================================
    "(?:#|//)\\s*(?:approximate|toy|pedagogical|illustrative|proof.of.concept)\\s+(?:implementation|version|code|example)",
    "(?:#|//)\\s*(?:MVP|bare.minimum|stripped.down|condensed|abridged|truncated)\\s+(?:version|implementation|code)",
    "(?:#|//)\\s*(?:for brevity|for simplicity|for clarity|for readability)",
    "(?:#|//)\\s*(?:skeleton|boilerplate|starter|template|scaffold)\\s+(?:code|implementation|version)",
    "(?:#|//)\\s*(?:stand.?in|filler|surrogate|substitute)\\s+(?:implementation|code|data|value)",
    "(?:#|//)\\s*(?:dummy|fake|fabricated|synthetic|contrived|artificial)\\s+(?:result|data|output|value|response|computation|calculation)",
    "(?:#|//)\\s*(?:omitted|elided|redacted|removed|cut)\\s+(?:for|due to)\\s+(?:brevity|space|time|simplicity)",
    "(?:#|//)\\s*(?:actual|real|proper|production|full)\\s+(?:implementation|logic|code|version)\\s+(?:would|should|goes|belongs)\\s+here",
};

std::string GovernanceEngine::checkOversimplification(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_oversimplification;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_OVERSIMPLIFICATION_PATTERNS : cfg.patterns;

    // Build active patterns based on config flags
    std::vector<std::string> active_pats;
    for (const auto& p : pats) active_pats.push_back(p);
    for (const auto& p : cfg.custom_patterns) active_pats.push_back(p);

    std::string found = searchPatterns(code, active_pats, !cfg.case_sensitive);
    if (!found.empty()) {
        return enforce("code_quality.no_oversimplification", cfg.level,
            formatError(cfg.level, fmt::format("Oversimplified code detected: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_oversimplification",
                "This code is a stub, placeholder, or trivially incomplete.\n"
                "The most common fix: DELETE the qualifying comment.\n"
                "If the code underneath works, the comment was the only problem.\n"
                "Comments should explain WHY, not admit the code is incomplete.\n\n"
                "  Do NOT add hedging comments like:\n"
                "    \"for now\", \"simplified\", \"basic implementation\",\n"
                "    \"in a real system\", \"would normally\", \"for demonstration\"",
                "// basic implementation for now\n"
                "fn process(data) {\n"
                "    return data  // would normally transform\n"
                "}",
                "fn process(data) {\n"
                "    let result = []\n"
                "    for item in data {\n"
                "        let transformed = apply_rules(item)\n"
                "        result.push(transformed)\n"
                "    }\n"
                "    return result\n"
                "}"));
    }
    recordPass("code_quality.no_oversimplification", cfg.level);
    return "";
}

// --- Incomplete Logic ---
static const std::vector<std::string> DEFAULT_INCOMPLETE_LOGIC_PATTERNS = {
    // Empty/swallowed error handling
    "except:\\s*pass",
    "except\\s+\\w+(?:\\s+as\\s+\\w+)?:\\s*pass",
    "except\\s+\\w+(?:\\s+as\\s+\\w+)?:\\s*\\.\\.\\.",
    "catch\\s*\\([^)]*\\)\\s*\\{\\s*\\}",
    "catch\\s*\\([^)]*\\)\\s*\\{\\s*//",
    "except[^:]*:\\s*print\\([\"'](?:error|Error|ERROR)[\"']\\)",
    "catch\\s*\\(\\w+\\)\\s*\\{\\s*console\\.log\\(\\w+\\)\\s*;?\\s*\\}",
    "rescue\\s*(?:=>)?\\s*(?:nil|end)",
    "except[^:]*:\\s*return\\s+None",
    "except[^:]*:\\s*return\\s+(?:False|\\[\\]|\\{\\}|\"\"|0)",
    // Bare/generic error raising
    "raise\\s+Exception\\s*$",
    "raise\\s+Exception\\([\"'](?:error|Error|e|err|exception)[\"']\\)",
    "raise\\s+Exception\\([\"'](?:something went wrong|an error occurred|failed|unknown error)[\"']\\)",
    "raise\\s+Exception\\([\"'](?:todo|fixme|implement|not done)[\"']\\)",
    "throw\\s+new\\s+Error\\([\"'](?:error|Error|e|err)[\"']\\)",
    "throw\\s+new\\s+Error\\([\"'](?:something went wrong|failed|unknown)[\"']\\)",
    "raise\\s+ValueError\\([\"'](?:invalid|bad|wrong)\\s*(?:value|input|data)?[\"']\\)",
    // Degenerate loops
    "for\\s+\\w+\\s+in\\s+\\w+:\\s*return\\s+\\w+",
    "for\\s+\\w+\\s+in\\s+\\w+:\\s*break",
    "for\\s*\\([^)]*\\)\\s*\\{\\s*return",
    "while\\s+True:\\s*break",
    "for\\s+\\w+\\s+in\\s+range\\(1\\)",
    // Dummy/incomplete conditionals
    "if\\s+True\\s*:",
    "if\\s+False\\s*:",
    "if\\s*\\(\\s*true\\s*\\)",
    "if\\s*\\(\\s*false\\s*\\)",
    "if\\s+1\\s*:",
    "if\\s+0\\s*:",
    "if\\s+.*:\\s*pass\\s*$",
    "else:\\s*pass\\s*$",
    // Placeholder error messages
    "[\"'](?:Something went wrong|An error occurred|Failed|Unknown error|Unexpected error)[\"']",

    // ============================================================
    // LLM no-op / passthrough disguises
    // ============================================================

    // Return empty collections (suspicious when in analysis/compute functions)
    "return\\s+\\[\\]\\s*(?:#|//|$)",
    "return\\s+\\{\\}\\s*(?:#|//|$)",

    // Passthrough/identity functions
    "return\\s+(?:input|data|text|value|args?|params?|x|obj|item|content|payload|message|request|response)\\s*(?:#|//)\\s*(?:unchanged|as.?is|pass.?through|no.?change|unmodified|untouched|directly)",

    // Random/simulated data generators (pretending to compute real results)
    "random\\.(?:random|uniform|randint|choice|gauss|sample|shuffle)\\(\\)\\s*(?:#|//)",
    "Math\\.random\\(\\)\\s*(?:#|//)",

    // Hardcoded score arrays (suspicious in analysis functions)
    "scores?\\s*=\\s*\\[\\s*(?:0\\.\\d+,?\\s*){3,}\\]",

    // Hardcoded numeric results (suspicious in analysis functions)
    "(?:score|accuracy|precision|recall|f1|confidence|probability|similarity|distance|weight)\\s*=\\s*(?:0\\.\\d+|[1-5]\\.\\d)\\s*(?:#|//|$)",

    // Degenerate implementations
    "for\\s+\\w+\\s+in\\s+\\w+\\s*\\{\\s*\\}",
    "while\\s+\\w+\\s*\\{\\s*break\\s*\\}",

    // EVA-9: NAAb-specific incomplete logic patterns
    // NAAb catch-and-swallow (empty catch blocks)
    "catch\\s*\\([^)]*\\)\\s*\\{\\s*\\}",
    // NAAb identity/passthrough validation functions
    "fn\\s+(?:validate|check|verify|is_)\\w*\\([^)]*\\)\\s*\\{\\s*return\\s+true\\s*\\}",

    // EVA-EXTRA-3: Numeric result fabrication patterns
    // Suspiciously precise hardcoded scores (LLMs love 0.85, 0.92, etc.)
    "(?:score|accuracy|precision|recall|f1|confidence|similarity)\\s*=\\s*0\\.[89]\\d\\s*(?:#|//|$)",
    // Array of hardcoded floats (fabricated results)
    "\\[\\s*(?:0\\.\\d+,\\s*){4,}\\]",
};

std::string GovernanceEngine::checkIncompleteLogic(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_incomplete_logic;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_INCOMPLETE_LOGIC_PATTERNS : cfg.patterns;

    std::vector<std::string> active_pats;
    for (const auto& p : pats) active_pats.push_back(p);
    for (const auto& p : cfg.custom_patterns) active_pats.push_back(p);

    std::string found = searchPatterns(code, active_pats, !cfg.case_sensitive);
    if (!found.empty()) {
        return enforce("code_quality.no_incomplete_logic", cfg.level,
            formatError(cfg.level, fmt::format("Incomplete logic detected: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_incomplete_logic",
                "This code has logic gaps — shortcuts or lazy implementation.\n"
                "Every function should DO something meaningful:\n"
                "  - Error handlers must log or re-raise, never silently swallow\n"
                "  - Return values must be computed, not hardcoded\n"
                "  - Loops must process data, not immediately break\n\n"
                "  NAAb-specific fixes:\n"
                "  - catch (e) { } → catch (e) { print(\"Error: \" + string(e)); throw e }\n"
                "  - fn validate(x) { return true } → add real checks\n"
                "  - score = 0.85 → score = len(matches) / len(total)",
                "catch (e) { }  // empty catch swallows error\n"
                "fn validate(x) { return true }  // always passes\n"
                "let score = 0.85  // hardcoded",
                "catch (e) { print(\"Error: \" + string(e)); throw e }\n"
                "fn validate(x) {\n"
                "    if x == null { return false }\n"
                "    if len(x) == 0 { return false }\n"
                "    return true\n"
                "}\n"
                "let score = len(matches) / len(total)"));
    }
    recordPass("code_quality.no_incomplete_logic", cfg.level);
    return "";
}

// --- Function Contract Check ---

std::string GovernanceEngine::checkFunctionContract(
    const std::string& func_name,
    const std::string& result_str,
    const std::string& result_type,
    int line) {

    auto it = rules_.contracts.functions.find(func_name);
    if (it == rules_.contracts.functions.end()) return "";

    const auto& contract = it->second;
    EnforcementLevel level = contract.level != EnforcementLevel::NONE
        ? contract.level : rules_.contracts.level;

    auto make_err = [&](const std::string& detail) -> std::string {
        // Build contract-specific examples
        std::string bad_ex, good_ex;
        if (detail.find("return_type") != std::string::npos && detail.find("dict") != std::string::npos) {
            bad_ex = "return null  // or return 42";
            good_ex = "return {\"id\": computed_id, \"type\": entity_type}";
        } else if (detail.find("missing required key") != std::string::npos) {
            bad_ex = "return {\"id\": 1}  // missing required keys";
            std::string keys_str;
            for (const auto& k : contract.return_keys) {
                if (!keys_str.empty()) keys_str += ", ";
                keys_str += "\"" + k + "\": value";
            }
            good_ex = "return {" + keys_str + "}";
        } else if (detail.find("return_one_of") != std::string::npos) {
            bad_ex = "return \"invalid_value\"";
            std::string vals;
            for (const auto& v : contract.return_one_of) {
                if (!vals.empty()) vals += ", ";
                vals += "\"" + v + "\"";
            }
            good_ex = "return one of: " + vals;
        } else if (detail.find("non-null") != std::string::npos) {
            bad_ex = "return null";
            good_ex = "return computed_value  // ensure non-null";
        } else if (detail.find("below minimum") != std::string::npos) {
            bad_ex = "return -5  // below minimum";
            good_ex = "return int(math.max(" + std::to_string(contract.return_min) + ", result))";
        }

        // Build help text with contract spec
        std::string help = contract.description.empty()
            ? "Function return value did not match contract"
            : contract.description;
        if (!contract.return_type.empty())
            help += "\n  Expected return type: " + contract.return_type;
        if (!contract.return_keys.empty()) {
            help += "\n  Required keys: ";
            for (size_t i = 0; i < contract.return_keys.size(); i++) {
                if (i > 0) help += ", ";
                help += contract.return_keys[i];
            }
        }
        if (!contract.return_one_of.empty()) {
            help += "\n  Valid values: ";
            for (size_t i = 0; i < contract.return_one_of.size(); i++) {
                if (i > 0) help += ", ";
                help += "\"" + contract.return_one_of[i] + "\"";
            }
        }

        return enforce("contracts." + func_name, level,
            formatError(level,
                fmt::format("Contract violation for '{}': {}", func_name, detail),
                line > 0 ? fmt::format("line {}", line) : "",
                "contracts",
                help,
                bad_ex, good_ex));
    };

    // return_not_null
    if (contract.return_not_null && result_type == "null") {
        return make_err("expected non-null return, got null");
    }

    // return_type
    if (!contract.return_type.empty() && result_type != contract.return_type) {
        return make_err(fmt::format("expected return_type '{}', got '{}'",
            contract.return_type, result_type));
    }

    // Numeric checks — single parse, wrapped in try-catch for safety
    bool is_numeric = (result_type == "int" || result_type == "float");
    bool needs_numeric = is_numeric && (contract.has_return_range ||
                                         contract.has_return_min ||
                                         contract.has_return_max);

    if (needs_numeric) {
        double val;
        try {
            val = std::stod(result_str);
        } catch (...) {
            return make_err(fmt::format("return value '{}' is not parseable as a number", result_str));
        }

        if (contract.has_return_range &&
            (val < contract.return_range_min || val > contract.return_range_max)) {
            return make_err(fmt::format("return value {} outside range [{}, {}]",
                result_str, contract.return_range_min, contract.return_range_max));
        }

        if (contract.has_return_min && val < contract.return_min) {
            return make_err(fmt::format("return value {} below minimum {}",
                result_str, contract.return_min));
        }

        if (contract.has_return_max && val > contract.return_max) {
            return make_err(fmt::format("return value {} above maximum {}",
                result_str, contract.return_max));
        }
    }

    // return_one_of
    if (!contract.return_one_of.empty()) {
        bool found = false;
        for (const auto& opt : contract.return_one_of) {
            if (result_str == opt) { found = true; break; }
        }
        if (!found) {
            std::string opts;
            for (const auto& opt : contract.return_one_of) {
                if (!opts.empty()) opts += ", ";
                opts += "\"" + opt + "\"";
            }
            return make_err(fmt::format("return value \"{}\" not in [{}]", result_str, opts));
        }
    }

    // return_non_empty
    if (contract.return_non_empty) {
        bool empty = result_str.empty() || result_str == "[]" || result_str == "{}"
                     || result_str == "\"\"" || result_type == "null";
        if (empty) {
            return make_err("expected non-empty return value");
        }
    }

    // return_keys (for dicts)
    if (!contract.return_keys.empty() && result_type == "dict") {
        for (const auto& key : contract.return_keys) {
            // Match key as a proper dict key, not as a substring of another key or value
            // NAAb dict toString format: {"key1": val1, "key2": val2}
            std::string quoted_key = "\"" + key + "\":";
            if (result_str.find(quoted_key) == std::string::npos) {
                return make_err(fmt::format("return dict missing required key '{}'", key));
            }
        }
    }

    // return_length_min / return_length_max
    if (contract.return_length_min >= 0 || contract.return_length_max >= 0) {
        int length = -1;
        if (result_type == "string") {
            length = static_cast<int>(result_str.size());
        } else if (result_type == "array") {
            if (result_str == "[]") {
                length = 0;
            } else {
                // Depth-aware counting: only count commas at top-level (depth 1)
                // Handles nested arrays, dicts, and strings with commas correctly
                length = 1;
                int depth = 0;
                bool in_string = false;
                char prev = 0;
                for (char c : result_str) {
                    if (c == '"' && prev != '\\') in_string = !in_string;
                    if (!in_string) {
                        if (c == '[' || c == '{') depth++;
                        if (c == ']' || c == '}') depth--;
                        if (c == ',' && depth == 1) length++;
                    }
                    prev = c;
                }
            }
        }
        if (length >= 0) {
            if (contract.return_length_min >= 0 && length < contract.return_length_min) {
                return make_err(fmt::format("return length {} below minimum {}",
                    length, contract.return_length_min));
            }
            if (contract.return_length_max >= 0 && length > contract.return_length_max) {
                return make_err(fmt::format("return length {} above maximum {}",
                    length, contract.return_length_max));
            }
        }
    }

    return "";
}

// --- Complexity Floor Check ---

std::string GovernanceEngine::checkComplexityFloor(
    const std::string& code,
    const std::string& function_name,
    int line) {

    auto& cfg = rules_.code_quality.complexity_floor;

    // B2: Skip floor for short functions (can't meaningfully reach high scores)
    if (cfg.min_lines_for_check > 0) {
        int line_count = static_cast<int>(std::count(code.begin(), code.end(), '\n')) + 1;
        if (line_count < cfg.min_lines_for_check) return "";
    }

    // Analyze code structure (defensive — never crash the host program)
    analyzer::SyntacticAnalyzer sa;
    analyzer::SyntacticProfile profile;
    try {
        profile = sa.analyze(code);
    } catch (...) {
        return "";  // Can't analyze — skip floor check
    }

    // Determine which rule applies (if any)
    int required_score = cfg.min_score;
    bool require_branching = false;
    std::string custom_message;

    // Check name-specific rules (first match wins)
    std::string fn_lower = function_name;
    std::transform(fn_lower.begin(), fn_lower.end(), fn_lower.begin(), ::tolower);

    for (const auto& rule : cfg.rules) {
        bool matched = false;
        for (const auto& name : rule.names) {
            std::string name_lower = name;
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
            auto pos = fn_lower.find(name_lower);
            if (pos != std::string::npos) {
                // Word-boundary matching using '_' as delimiter (snake_case)
                bool boundary_before = (pos == 0 || fn_lower[pos - 1] == '_');
                size_t end = pos + name_lower.size();
                bool boundary_after = (end >= fn_lower.size() || fn_lower[end] == '_');
                // Prefix patterns ending in '_' (is_, has_, to_) only need boundary before
                bool is_prefix = !name_lower.empty() && name_lower.back() == '_';
                if (boundary_before && (is_prefix || boundary_after)) {
                    matched = true;
                    break;
                }
            }
        }
        if (matched) {
            required_score = rule.min_score;
            require_branching = rule.require_branching_or_loops;
            custom_message = rule.message;
            break;
        }
    }

    // Check complexity score against floor
    if (profile.complexity_score < required_score) {
        std::string msg = custom_message.empty()
            ? fmt::format("Function '{}' has complexity score {} (minimum: {})",
                function_name, profile.complexity_score, required_score)
            : custom_message;

        return enforce("code_quality.complexity_floor", cfg.level,
            formatError(cfg.level, msg,
                line > 0 ? fmt::format("line {}", line) : "",
                "code_quality.complexity_floor",
                fmt::format("Score {}/100: loops={}, nesting={}, calls={}, recursion={}\n\n"
                    "  What adds complexity:\n"
                    "    +5  each real loop (for/while over data)     +5  try/catch\n"
                    "    +15 nested loops                              +5  array operations (map_fn, filter_fn)\n"
                    "    +3  each function definition                  +10 recursion\n"
                    "    +1  each external function call               +5  pipeline (|>)\n\n"
                    "  Tip: Add real logic — input validation, edge cases, error handling.\n"
                    "  Do NOT pad with for i in 0..1 {{}} loops.",
                    profile.complexity_score, profile.loop_count,
                    (profile.has_try_catch ? 1 : 0) + profile.max_function_depth,
                    profile.external_call_count, profile.has_recursion ? "yes" : "no"),
                "fn apply_damage(ent, damage) {\n"
                "    ent[\"hp\"] = ent.get(\"hp\") - damage\n"
                "    for i in 0..1 { if ent.get(\"hp\") < 0 { ent[\"hp\"] = 0 } }\n"
                "    return ent\n"
                "}",
                "fn apply_damage(ent, damage) {\n"
                "    let max_hp = ent.get(\"max_hp\") ?? 100\n"
                "    let actual = int(math.max(0, damage))\n"
                "    let new_hp = ent.get(\"hp\") - actual\n"
                "    if new_hp <= 0 { ent[\"hp\"] = 0; ent[\"alive\"] = false }\n"
                "    else if new_hp > max_hp { ent[\"hp\"] = max_hp }\n"
                "    else { ent[\"hp\"] = new_hp }\n"
                "    return ent\n"
                "}"));
    }

    // Check branching/loops requirement
    if (require_branching) {
        // max_function_depth > 0 means nested loops exist (valid non-trivial indicator)
        bool has_branching = profile.loop_count > 0 || profile.has_try_catch ||
                            profile.max_function_depth > 0 || profile.has_recursion;
        if (!has_branching) {
            std::string msg = custom_message.empty()
                ? fmt::format("Function '{}' requires branching or loops but has none", function_name)
                : custom_message;

            return enforce("code_quality.complexity_floor", cfg.level,
                formatError(cfg.level, msg,
                    line > 0 ? fmt::format("line {}", line) : "",
                    "code_quality.complexity_floor",
                    "Functions named analyze/compute/process must contain loops,\n"
                    "conditionals, or recursive logic — not just assignments and returns.",
                    "fn compute_score(data) {\n"
                    "    let score = data.get(\"base\") * 1.5\n"
                    "    return score\n"
                    "}",
                    "fn compute_score(data) {\n"
                    "    let score = 0\n"
                    "    for item in data.get(\"items\") {\n"
                    "        let weight = item.get(\"weight\") ?? 1\n"
                    "        if item.get(\"type\") == \"bonus\" {\n"
                    "            score = score + weight * 2\n"
                    "        } else {\n"
                    "            score = score + weight\n"
                    "        }\n"
                    "    }\n"
                    "    return score\n"
                    "}"));
        }
    }

    return "";
}

// --- NAAb Function Body Quality Check ---

std::string GovernanceEngine::checkNaabFunctionBody(
    const std::string& function_name,
    const std::string& source_code,
    int line) {

    auto& os_cfg = rules_.code_quality.no_oversimplification;
    auto& il_cfg = rules_.code_quality.no_incomplete_logic;
    auto& ph_cfg = rules_.code_quality.no_placeholders;

    // Skip if all checks disabled (but still allow complexity_floor if enabled)
    auto& cf_cfg_early = rules_.code_quality.complexity_floor;
    if (!os_cfg.enabled && !il_cfg.enabled && !ph_cfg.enabled && !cf_cfg_early.enabled) return "";

    // EVA-10: Pre-strip strings to prevent false positives from string literals
    // e.g. a legitimate string "TODO" shouldn't trigger checkPlaceholders
    std::string stripped = stripStringLiterals(source_code);

    // Run applicable checks on the stripped source code
    std::string err;

    if (ph_cfg.enabled) {
        err = checkPlaceholders(stripped, line);
        if (!err.empty()) return err;
    }

    if (os_cfg.enabled) {
        err = checkOversimplification(stripped, line);
        if (!err.empty()) return err;
    }

    if (il_cfg.enabled) {
        err = checkIncompleteLogic(stripped, line);
        if (!err.empty()) return err;
    }

    // Complexity floor check for NAAb functions
    auto& cf_cfg = rules_.code_quality.complexity_floor;
    if (cf_cfg.enabled && cf_cfg.check_naab) {
        // Skip if function contains polyglot blocks and skip_if_has_polyglot_block is set
        bool has_polyglot = source_code.find("<<") != std::string::npos;
        if (!has_polyglot || !cf_cfg.skip_if_has_polyglot_block) {
            err = checkComplexityFloor(source_code, function_name, line);
            if (!err.empty()) return err;
        }
    }

    return "";
}

// --- Hallucinated APIs ---

// Python patterns: JS/Java methods used in Python, made-up Python functions
static const std::vector<std::pair<std::string, std::string>> PYTHON_HALLUCINATION_PATTERNS = {
    // JS methods in Python
    {"\\.length\\b", ".length is JavaScript — in Python, use len()"},
    {"\\.push\\(", ".push() is JavaScript — in Python, use .append()"},
    {"\\.forEach\\(", ".forEach() is JavaScript — in Python, use a for loop"},
    {"\\.indexOf\\(", ".indexOf() is JavaScript — in Python, use .index() or 'in'"},
    {"\\.includes\\(", ".includes() is JavaScript — in Python, use 'in' operator"},
    {"\\.toString\\(", ".toString() is JavaScript — in Python, use str()"},
    {"\\.toUpperCase\\(", ".toUpperCase() is JavaScript — in Python, use .upper()"},
    {"\\.toLowerCase\\(", ".toLowerCase() is JavaScript — in Python, use .lower()"},
    {"\\.trim\\(", ".trim() is JavaScript — in Python, use .strip()"},
    {"\\.charAt\\(", ".charAt() is JavaScript — in Python, use indexing []"},
    {"\\.substring\\(", ".substring() is JavaScript — in Python, use slicing [:]"},
    {"\\.splice\\(", ".splice() is JavaScript — in Python, use slicing or del"},
    {"\\.concat\\(", ".concat() is JavaScript — in Python, use + or .extend()"},
    {"console\\.log\\(", "console.log() is JavaScript — in Python, use print()"},
    {"\\btypeof\\s+", "typeof is JavaScript — in Python, use type()"},
    {"\\binstanceof\\b", "instanceof is JavaScript — in Python, use isinstance()"},
    {"===", "=== is JavaScript — in Python, use =="},
    {"!==", "!== is JavaScript — in Python, use !="},
    {"\\bnull\\b", "null is JavaScript — in Python, use None"},
    {"\\bundefined\\b", "undefined is JavaScript — Python has no equivalent (use None)"},
    {"\\bconst\\s+\\w+\\s*=", "const is JavaScript — in Python, just assign variables"},
    {"\\blet\\s+\\w+\\s*=", "let is JavaScript — in Python, just assign variables"},
    {"\\bvar\\s+\\w+\\s*=", "var is JavaScript — in Python, just assign variables"},
    // Made-up Python functions
    {"json\\.stringify\\(", "json.stringify() is JavaScript — in Python, use json.dumps()"},
    {"json\\.parse\\(", "json.parse() is JavaScript — in Python, use json.loads()"},
    {"Math\\.round\\(", "Math.round() is JavaScript — in Python, use round()"},
    {"Math\\.floor\\(", "Math.floor() is JavaScript — in Python, use math.floor() or int()"},
    {"Math\\.ceil\\(", "Math.ceil() is JavaScript — in Python, use math.ceil()"},
    {"Math\\.abs\\(", "Math.abs() is JavaScript — in Python, use abs()"},
    {"Math\\.random\\(", "Math.random() is JavaScript — in Python, use random.random()"},
    {"Math\\.max\\(", "Math.max() is JavaScript — in Python, use max()"},
    {"Math\\.min\\(", "Math.min() is JavaScript — in Python, use min()"},
    {"list\\.flatten\\(", "list.flatten() doesn't exist — use itertools.chain.from_iterable()"},
    {"dict\\.to_json\\(", "dict.to_json() doesn't exist — use json.dumps()"},
    {"\\.toInt\\(", ".toInt() doesn't exist in Python — use int()"},
    {"\\.toFloat\\(", ".toFloat() doesn't exist in Python — use float()"},
    {"\\.size\\(\\)", ".size() doesn't exist for sequences in Python — use len()"},
    {"\\bArray\\(", "Array() is JavaScript — in Python, use list()"},
    {"Object\\.keys\\(", "Object.keys() is JavaScript — in Python, use .keys()"},
    {"Object\\.values\\(", "Object.values() is JavaScript — in Python, use .values()"},
    {"\\bString\\(", "String() is JavaScript — in Python, use str()"},
    {"\\bNumber\\(", "Number() is JavaScript — in Python, use int() or float()"},
    {"\\bBoolean\\(", "Boolean() is JavaScript — in Python, use bool()"},
    // Python self in wrong context
    {"\\bthis\\.\\w+", "this.x is JavaScript — in Python, use self.x"},
    {"\\basync\\s+function\\b", "async function is JavaScript — in Python, use async def"},
};

// JavaScript patterns: Python syntax/builtins used in JS
static const std::vector<std::pair<std::string, std::string>> JS_HALLUCINATION_PATTERNS = {
    // Python builtins in JS
    {"\\bprint\\(", "print() is Python — in JavaScript, use console.log()"},
    {"\\blen\\(", "len() is Python — in JavaScript, use .length"},
    {"\\brange\\(", "range() is Python — in JavaScript, use for loop or Array.from()"},
    {"\\bdef\\s+\\w+", "def is Python — in JavaScript, use function or arrow functions"},
    {"\\belif\\b", "elif is Python — in JavaScript, use else if"},
    {"\\bTrue\\b", "True is Python — in JavaScript, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in JavaScript, use false (lowercase)"},
    {"\\bNone\\b", "None is Python — in JavaScript, use null"},
    {"\\band\\b(?=\\s)", "and is Python — in JavaScript, use &&"},
    {"\\bor\\b(?=\\s)", "or is Python — in JavaScript, use ||"},
    {"\\bnot\\b(?=\\s)", "not is Python — in JavaScript, use !"},
    {"\\b__\\w+__\\b", "Dunder methods (__x__) are Python — no equivalent in JavaScript"},
    // Python methods in JS
    {"\\.append\\(", ".append() is Python — in JavaScript, use .push()"},
    {"\\.extend\\(", ".extend() is Python — in JavaScript, use .concat() or spread"},
    {"\\.strip\\(", ".strip() is Python — in JavaScript, use .trim()"},
    {"\\.upper\\(", ".upper() is Python — in JavaScript, use .toUpperCase()"},
    {"\\.lower\\(", ".lower() is Python — in JavaScript, use .toLowerCase()"},
    {"\\.items\\(\\)", ".items() is Python — in JavaScript, use Object.entries()"},
    // Made-up JS functions
    {"\\barray\\.contains\\(", "array.contains() doesn't exist — use .includes()"},
    {"\\bstring\\.contains\\(", "string.contains() doesn't exist — use .includes()"},
    {"Array\\.flatten\\(", "Array.flatten() doesn't exist — use .flat()"},
    {"JSON\\.load\\(", "JSON.load() is Python-style — in JavaScript, use JSON.parse()"},
    {"JSON\\.dump\\(", "JSON.dump() is Python-style — in JavaScript, use JSON.stringify()"},
    {"console\\.write\\(", "console.write() doesn't exist — use console.log()"},
    {"Math\\.sum\\(", "Math.sum() doesn't exist — use array.reduce((a,b) => a+b, 0)"},
    {"fs\\.readfile\\(", "fs.readfile() wrong case — use fs.readFile() or fs.readFileSync()"},
    // Python self/class in JS
    {"\\bself\\.\\w+", "self.x is Python — in JavaScript, use this.x"},
    {"\\basync\\s+def\\b", "async def is Python — in JavaScript, use async function"},
};

// Go patterns: Python/JS idioms in Go
static const std::vector<std::pair<std::string, std::string>> GO_HALLUCINATION_PATTERNS = {
    {"\\bprint\\(", "print() is Python — in Go, use fmt.Print() or fmt.Println()"},
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Go, use fmt.Println()"},
    {"\\bNone\\b", "None is Python — in Go, use nil"},
    {"\\bTrue\\b", "True is Python — in Go, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in Go, use false (lowercase)"},
    {"\\bnull\\b", "null is JavaScript — in Go, use nil"},
    {"\\blen\\(", "len() is a builtin in Go too, but ensure you import fmt for printing"},
    {"\\.append\\(", ".append() is Python — in Go, use append(slice, elem)"},
    {"\\.push\\(", ".push() is JavaScript — in Go, use append(slice, elem)"},
    {"\\bdef\\s+\\w+", "def is Python — in Go, use func"},
    {"\\bfunction\\s+\\w+", "function is JavaScript — in Go, use func"},
};

// Ruby patterns: Python/JS idioms in Ruby
static const std::vector<std::pair<std::string, std::string>> RUBY_HALLUCINATION_PATTERNS = {
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Ruby, use puts or print"},
    {"\\bnull\\b", "null is JavaScript — in Ruby, use nil"},
    {"\\bNone\\b", "None is Python — in Ruby, use nil"},
    {"\\bTrue\\b", "True is Python — in Ruby, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in Ruby, use false (lowercase)"},
    {"\\bdef\\s+\\w+\\s*\\(", "Python-style def — in Ruby, use def without parens in definition"},
    {"\\blen\\(", "len() is Python — in Ruby, use .length or .size"},
    {"\\bprint\\(", "print() with parens — in Ruby, use puts (adds newline) or print without parens"},
};

// Shell patterns: Python/JS idioms in Shell
static const std::vector<std::pair<std::string, std::string>> SHELL_HALLUCINATION_PATTERNS = {
    {"\\bprint\\(", "print() is Python — in Shell, use echo"},
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Shell, use echo"},
    {"(?:^|[=\\s(,])null(?:\\s|$|[);,])", "null is JavaScript — in Shell, variables are unset or empty strings"},
    {"\\bNone\\b", "None is Python — in Shell, use empty string or unset"},
    {"\\bTrue\\b", "True is Python — in Shell, use true (lowercase command)"},
    {"\\bFalse\\b", "False is Python — in Shell, use false (lowercase command)"},
    {"\\bdef\\s+\\w+", "def is Python — in Shell, use function_name() { ... }"},
    {"\\bimport\\s+", "import is Python — in Shell, use source or . to include files"},
};

// Nim patterns: Python/JS idioms in Nim
static const std::vector<std::pair<std::string, std::string>> NIM_HALLUCINATION_PATTERNS = {
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Nim, use echo"},
    {"\\bnull\\b", "null is JavaScript — in Nim, use nil"},
    {"\\bNone\\b", "None is Python — in Nim, use nil"},
    {"\\bTrue\\b", "True is Python — in Nim, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in Nim, use false (lowercase)"},
    {"\\bdef\\s+\\w+", "def is Python — in Nim, use proc or func"},
    {"\\blen\\(", "len() is also valid in Nim — make sure to use it without import"},
};

// Cross-language confusion patterns
static const std::vector<std::pair<std::string, std::string>> CROSS_LANG_PATTERNS = {
    {"#\\s+\\w", "# comments are Python/Ruby — in JavaScript, use //"},
    {"//\\s+\\w", "// comments are JS/C++ — in Python, use #"},
};

std::string GovernanceEngine::checkHallucinatedApis(const std::string& language,
                                                     const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_hallucinated_apis;
    if (!cfg.enabled) return "";

    // Select patterns based on language
    const std::vector<std::pair<std::string, std::string>>* lang_patterns = nullptr;
    if (language == "python") lang_patterns = &PYTHON_HALLUCINATION_PATTERNS;
    else if (language == "javascript" || language == "js" || language == "node")
        lang_patterns = &JS_HALLUCINATION_PATTERNS;
    else if (language == "go" || language == "golang")
        lang_patterns = &GO_HALLUCINATION_PATTERNS;
    else if (language == "ruby" || language == "rb")
        lang_patterns = &RUBY_HALLUCINATION_PATTERNS;
    else if (language == "shell" || language == "bash" || language == "sh")
        lang_patterns = &SHELL_HALLUCINATION_PATTERNS;
    else if (language == "nim")
        lang_patterns = &NIM_HALLUCINATION_PATTERNS;

    // Strip string literal contents before checking patterns.
    // This prevents false positives when code generates source code for
    // another language inside strings (e.g., Go code that builds Rust source).
    std::string code_no_strings;
    {
        bool in_single = false, in_double = false, in_backtick = false;
        bool escaped = false;
        for (size_t i = 0; i < code.size(); ++i) {
            char c = code[i];
            if (escaped) { escaped = false; continue; }
            if (c == '\\' && (in_single || in_double)) { escaped = true; continue; }
            if (c == '"' && !in_single && !in_backtick) { in_double = !in_double; continue; }
            if (c == '\'' && !in_double && !in_backtick) { in_single = !in_single; continue; }
            if (c == '`' && !in_double && !in_single) { in_backtick = !in_backtick; continue; }
            if (!in_single && !in_double && !in_backtick) {
                code_no_strings += c;
            }
        }
    }

    // Check language-specific patterns (on code with strings stripped)
    if (lang_patterns) {
        for (const auto& [pattern, suggestion] : *lang_patterns) {
            try {
                auto flags = cfg.case_sensitive ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase);
                std::regex re(pattern, flags);
                std::smatch match;
                if (std::regex_search(code_no_strings, match, re)) {
                    // C1: Prepend language context + C2: Add NAAb equivalent hint
                    std::string full_suggestion =
                        fmt::format("Inside <<{}>> polyglot block:\n  {}\n\n"
                            "Note: In NAAb native code (outside << >> blocks), use NAAb stdlib.\n"
                            "  Arrays: array.push(arr, item), array.pop(arr)\n"
                            "  Strings: string.upper(s), string.lower(s), string.split(s, \",\")\n"
                            "  Length: len(x)", language, suggestion);
                    return enforce("code_quality.no_hallucinated_apis", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Cross-language syntax in {} block: \"{}\"", language, match[0].str()),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.no_hallucinated_apis",
                            full_suggestion, "", ""));
                }
            } catch (const std::regex_error&) {}
        }
    }

    // Check cross-language confusion patterns
    if (cfg.check_cross_language) {
        // Only check relevant cross-language patterns
        if (language == "python") {
            // Check for JS comment style in Python
            try {
                std::regex re("^\\s*//\\s+", std::regex::multiline);
                std::smatch match;
                if (std::regex_search(code, match, re)) {
                    return enforce("code_quality.no_hallucinated_apis", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Cross-language confusion in {} block: \"{}\"", language, match[0].str()),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.no_hallucinated_apis",
                            "// comments are JavaScript — in Python, use #", "", ""));
                }
            } catch (const std::regex_error&) {}
        } else if (language == "javascript" || language == "js") {
            // Check for Python comment style in JS
            try {
                std::regex re("^\\s*#\\s+", std::regex::multiline);
                std::smatch match;
                if (std::regex_search(code, match, re)) {
                    return enforce("code_quality.no_hallucinated_apis", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Cross-language confusion in {} block: \"{}\"", language, match[0].str()),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.no_hallucinated_apis",
                            "# comments are Python — in JavaScript, use //", "", ""));
                }
            } catch (const std::regex_error&) {}
        }
    }

    // Check custom patterns
    if (!cfg.custom_patterns.empty()) {
        std::string found = searchPatterns(code, cfg.custom_patterns);
        if (!found.empty()) {
            return enforce("code_quality.no_hallucinated_apis", cfg.level,
                formatError(cfg.level,
                    fmt::format("Hallucinated API pattern in {} block: \"{}\"", language, found),
                    line > 0 ? fmt::format("line {}", line) : "",
                    "code_quality.no_hallucinated_apis",
                    "This pattern matches a known hallucinated or incorrect API usage", "", ""));
        }
    }

    recordPass("code_quality.no_hallucinated_apis", cfg.level);
    return "";
}

// --- Security: Shell Injection ---
std::string GovernanceEngine::checkShellInjection(const std::string& code, int line) {
    auto& cfg = rules_.restrictions.shell_injection;
    if (!cfg.enabled) return "";
    static const std::vector<std::string> default_patterns = {
        "curl.*\\|\\s*sh", "wget.*\\|\\s*bash", "eval\\s+\\$",
        "\\$\\(curl", "\\$\\(wget", "bash\\s+-c.*\\$",
        "chmod\\s+777", "chmod\\s+\\+x.*\\$",
    };
    auto& pats = cfg.patterns.empty() ? default_patterns : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.shell_injection", cfg.level,
            formatError(cfg.level, fmt::format("Shell injection pattern: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.shell_injection",
                "Avoid piping untrusted input to shell execution", "", ""));
    }
    recordPass("restrictions.shell_injection", cfg.level);
    return "";
}

// --- Security: Code Injection ---
std::string GovernanceEngine::checkCodeInjection(const std::string& language,
                                                  const std::string& code, int line) {
    auto& cfg = rules_.restrictions.code_injection;
    if (!cfg.enabled) return "";
    std::vector<std::string> pats;
    if (cfg.block_dynamic_code_gen) { pats.push_back("\\beval\\s*\\("); pats.push_back("\\bexec\\s*\\("); pats.push_back("\\bFunction\\s*\\("); }
    if (cfg.block_sql_injection_patterns) {
        pats.push_back("(?:SELECT|INSERT|UPDATE|DELETE)\\s+.*['\"]\\s*\\+");
        pats.push_back("f['\"].*(?:SELECT|INSERT|UPDATE|DELETE).*\\{");
    }
    if (cfg.block_command_injection) { pats.push_back("os\\.system\\s*\\("); pats.push_back("subprocess\\.call.*shell\\s*=\\s*True"); }
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.code_injection", cfg.level,
            formatError(cfg.level, fmt::format("Code injection pattern in {} block: \"{}\"", language, found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.code_injection",
                "Avoid dynamic code execution and use safe alternatives", "", ""));
    }
    recordPass("restrictions.code_injection", cfg.level);
    return "";
}

// --- Security: Privilege Escalation ---
std::string GovernanceEngine::checkPrivilegeEscalation(const std::string& code, int line) {
    auto& cfg = rules_.restrictions.privilege_escalation;
    if (!cfg.enabled) return "";
    std::vector<std::string> pats;
    if (cfg.block_sudo) pats.push_back("\\bsudo\\s");
    if (cfg.block_su) pats.push_back("\\bsu\\s+-");
    if (cfg.block_chmod_suid) pats.push_back("chmod\\s+[ugo]*s");
    if (cfg.block_setuid) pats.push_back("\\bsetuid\\b");
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.privilege_escalation", cfg.level,
            formatError(cfg.level, fmt::format("Privilege escalation: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.privilege_escalation",
                "Avoid privilege escalation in polyglot blocks", "", ""));
    }
    recordPass("restrictions.privilege_escalation", cfg.level);
    return "";
}

// --- Security: Data Exfiltration ---
std::string GovernanceEngine::checkDataExfiltration(const std::string& code, int line) {
    auto& cfg = rules_.restrictions.data_exfiltration;
    if (!cfg.enabled) return "";
    std::vector<std::string> pats;
    if (cfg.block_base64_encode_secrets) pats.push_back("base64\\.(?:b64encode|encode).*(?:password|secret|key|token)");
    if (cfg.block_hex_encode_secrets) pats.push_back("\\.hex\\(\\).*(?:password|secret|key|token)");
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.data_exfiltration", cfg.level,
            formatError(cfg.level, "Potential data exfiltration pattern detected",
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.data_exfiltration",
                "Do not encode secrets for transmission", "", ""));
    }
    recordPass("restrictions.data_exfiltration", cfg.level);
    return "";
}

// --- Security: Resource Abuse ---
std::string GovernanceEngine::checkResourceAbuse(const std::string& code, int line) {
    auto& cfg = rules_.restrictions.resource_abuse;
    if (!cfg.enabled) return "";
    std::vector<std::string> pats;
    if (cfg.block_fork_bomb) { pats.push_back(":\\(\\)\\{\\s*:\\|:&\\s*\\};:"); pats.push_back("fork\\(\\).*fork\\(\\)"); }
    if (cfg.block_disk_filling) pats.push_back("dd\\s+if=/dev/zero");
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.resource_abuse", cfg.level,
            formatError(cfg.level, fmt::format("Resource abuse pattern: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.resource_abuse",
                "This pattern could cause resource exhaustion", "", ""));
    }
    recordPass("restrictions.resource_abuse", cfg.level);
    return "";
}

// --- Security: Info Disclosure ---
std::string GovernanceEngine::checkInfoDisclosure(const std::string& /*language*/,
                                                   const std::string& code, int line) {
    auto& cfg = rules_.restrictions.information_disclosure;
    if (!cfg.enabled) return "";
    std::vector<std::string> pats;
    if (cfg.block_env_dump) { pats.push_back("os\\.environ(?!\\[)"); pats.push_back("process\\.env(?!\\.)"); pats.push_back("\\benv\\b(?!\\.)"); }
    if (cfg.block_process_listing) { pats.push_back("ps\\s+aux"); pats.push_back("ps\\s+-ef"); }
    if (cfg.block_system_info_leak) { pats.push_back("uname\\s+-a"); pats.push_back("cat\\s+/etc/passwd"); }
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.information_disclosure", cfg.level,
            formatError(cfg.level, fmt::format("Information disclosure pattern: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.information_disclosure",
                "Avoid leaking system/environment information", "", ""));
    }
    recordPass("restrictions.information_disclosure", cfg.level);
    return "";
}

// --- Security: Crypto Weakness ---
std::string GovernanceEngine::checkCryptoWeakness(const std::string& code, int line) {
    auto& cfg = rules_.restrictions.crypto;
    if (!cfg.enabled) return "";
    std::vector<std::string> pats;
    if (cfg.block_weak_hashing) {
        auto& hashes = cfg.weak_hashes.empty() ? (const std::vector<std::string>&)(std::vector<std::string>{"md5", "sha1"}) : cfg.weak_hashes;
        for (const auto& h : hashes) { pats.push_back("\\b" + h + "\\b"); pats.push_back("hashlib\\." + h); }
    }
    if (cfg.block_weak_encryption) {
        auto& ciphers = cfg.weak_ciphers.empty() ? (const std::vector<std::string>&)(std::vector<std::string>{"des", "rc4", "blowfish"}) : cfg.weak_ciphers;
        for (const auto& c : ciphers) pats.push_back("\\b" + c + "\\b");
    }
    if (cfg.block_hardcoded_keys) pats.push_back("(?:encryption|signing|crypto)_key\\s*=\\s*['\"][^'\"]+['\"]");
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("restrictions.crypto", cfg.level,
            formatError(cfg.level, fmt::format("Cryptographic weakness: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "restrictions.crypto",
                "Use strong cryptographic algorithms (SHA-256+, AES-256)", "", ""));
    }
    recordPass("restrictions.crypto", cfg.level);
    return "";
}

// --- Per-Language: Imports ---
std::string GovernanceEngine::checkImports(const std::string& language,
                                            const std::string& code, int line) {
    auto& cfg = rules_.restrictions.imports;
    if (!cfg.enabled) return "";

    // Build import patterns for this language
    std::vector<std::string> blocked;
    if (cfg.blocked.count(language)) blocked = cfg.blocked.at(language);
    if (cfg.blocked.count("any")) {
        auto& any = cfg.blocked.at("any");
        blocked.insert(blocked.end(), any.begin(), any.end());
    }

    // Also check per-language config
    auto it = rules_.languages.per_language.find(language);
    if (it != rules_.languages.per_language.end()) {
        auto& lc = it->second;
        for (const auto& b : lc.imports.blocked) blocked.push_back(b);
        for (const auto& b : lc.banned_imports) blocked.push_back(b);
    }

    for (const auto& imp : blocked) {
        std::string pat;
        if (language == "python") pat = "(?:import\\s+" + imp + "|from\\s+" + imp + ")";
        else if (language == "javascript") pat = "(?:require\\s*\\(\\s*['\"]" + imp + "['\"]|import.*from\\s*['\"]" + imp + "['\"])";
        else if (language == "go") pat = "\"" + imp + "\"";
        else if (language == "ruby") pat = "require\\s*['\"]" + imp + "['\"]";
        else pat = imp;

        try {
            std::regex re(pat, std::regex::icase);
            if (std::regex_search(code, re)) {
                return enforce("restrictions.imports", cfg.level,
                    formatError(cfg.level, fmt::format("Blocked import in {} block: \"{}\"", language, imp),
                        line > 0 ? fmt::format("line {}", line) : "", "restrictions.imports",
                        fmt::format("The import \"{}\" is blocked by governance", imp), "", ""));
            }
        } catch (const std::regex_error&) {}
    }
    recordPass("restrictions.imports", cfg.level);
    return "";
}

// --- Per-Language: Banned Functions ---
std::string GovernanceEngine::checkBannedFunctions(const std::string& language,
                                                    const std::string& code, int line) {
    auto it = rules_.languages.per_language.find(language);
    if (it == rules_.languages.per_language.end()) return "";
    auto& lc = it->second;
    if (lc.banned_functions.empty()) return "";

    for (const auto& func : lc.banned_functions) {
        try {
            std::regex re(func, std::regex::icase);
            if (std::regex_search(code, re)) {
                return enforce("languages.per_language.banned_functions", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        fmt::format("Banned function in {} block: \"{}\"", language, func),
                        line > 0 ? fmt::format("line {}", line) : "",
                        fmt::format("languages.per_language.{}.banned_functions", language),
                        "This function is banned by governance policy", "", ""));
            }
        } catch (const std::regex_error&) {}
    }
    return "";
}

// --- Per-Language: Style Rules ---
std::string GovernanceEngine::checkLanguageStyle(const std::string& language,
                                                  const std::string& code, int line) {
    auto it = rules_.languages.per_language.find(language);
    if (it == rules_.languages.per_language.end()) return "";
    auto& lc = it->second;

    // Shell: require set -e
    if (language == "shell" || language == "bash") {
        if (lc.require_set_e && code.find("set -e") == std::string::npos) {
            return enforce("languages.per_language.shell.require_set_e", lc.require_set_e_level,
                formatError(lc.require_set_e_level, "Shell block missing 'set -e'",
                    line > 0 ? fmt::format("line {}", line) : "",
                    "languages.per_language.shell.require_set_e",
                    "Add 'set -e' to exit on errors",
                    "echo \"hello\"",
                    "set -e\necho \"hello\""));
        }
    }

    // JS: no var
    if (language == "javascript" && lc.no_var) {
        try {
            std::regex re("\\bvar\\s+\\w");
            if (std::regex_search(code, re)) {
                return enforce("languages.per_language.javascript.no_var", lc.no_var_level,
                    formatError(lc.no_var_level, "Use 'let' or 'const' instead of 'var'",
                        line > 0 ? fmt::format("line {}", line) : "",
                        "languages.per_language.javascript.no_var",
                        "'var' has function scope — use 'let' or 'const' for block scope",
                        "var x = 1;", "let x = 1;  // or const x = 1;"));
            }
        } catch (const std::regex_error&) {}
    }

    return "";
}

// --- Per-Language: Code Size ---
std::string GovernanceEngine::checkCodeSize(const std::string& language,
                                             const std::string& code, int line) {
    auto it = rules_.languages.per_language.find(language);
    if (it == rules_.languages.per_language.end()) return "";
    auto& lc = it->second;

    if (lc.max_lines > 0) {
        int lines = 1;
        for (char c : code) if (c == '\n') lines++;
        if (lines > lc.max_lines) {
            return enforce("languages.per_language.max_lines", EnforcementLevel::HARD,
                formatError(EnforcementLevel::HARD,
                    fmt::format("{} block has {} lines (max: {})", language, lines, lc.max_lines),
                    line > 0 ? fmt::format("line {}", line) : "",
                    fmt::format("languages.per_language.{}.max_lines = {}", language, lc.max_lines),
                    "Break large blocks into smaller functions", "", ""));
        }
    }
    return "";
}

// --- Custom Rules ---
std::string GovernanceEngine::checkCustomRules(const std::string& language,
                                                const std::string& code, int line) {
    for (const auto& rule : rules_.custom_rules) {
        if (!rule.enabled || !rule.pattern_valid) continue;
        if (!rule.languages.empty()) {
            bool matches = false;
            for (const auto& l : rule.languages) { if (l == language) { matches = true; break; } }
            if (!matches) continue;
        }
        try {
            if (std::regex_search(code, rule.compiled_pattern)) {
                std::string msg = rule.message.empty()
                    ? fmt::format("Custom rule '{}' violated", rule.name)
                    : rule.message;
                return enforce("custom_rules." + rule.id, rule.level,
                    formatError(rule.level, msg,
                        line > 0 ? fmt::format("line {}", line) : "",
                        "custom_rules[\"" + rule.id + "\"]",
                        rule.help, rule.bad_example, rule.good_example));
            }
        } catch (const std::regex_error&) {}
    }
    return "";
}

// --- Resource Limits ---
std::string GovernanceEngine::checkLoopIterations(size_t count) {
    int max = rules_.limits.execution.loop_iterations;
    if (max > 0 && static_cast<int>(count) > max) {
        return enforce("limits.execution.loop_iterations", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Loop iteration count {} exceeds limit of {}", count, max),
                "", fmt::format("limits.execution.loop_iterations = {}", max),
                "Maximum loop iterations exceeded", "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkPolyglotBlockCount(size_t count) {
    int max = rules_.limits.execution.polyglot_blocks;
    if (max > 0 && static_cast<int>(count) > max) {
        return enforce("limits.execution.polyglot_blocks", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Polyglot block count {} exceeds limit of {}", count, max),
                "", fmt::format("limits.execution.polyglot_blocks = {}", max),
                "Maximum polyglot block count exceeded", "", ""));
    }
    return "";
}

std::string GovernanceEngine::incrementAndCheckPolyglotBlockCount() {
    ++polyglot_block_count_;
    return checkPolyglotBlockCount(static_cast<size_t>(polyglot_block_count_));
}

std::string GovernanceEngine::checkStringLength(size_t length) {
    int max = rules_.limits.data.string_length;
    if (max > 0 && static_cast<int>(length) > max) {
        return enforce("limits.data.string_length", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("String length {} exceeds limit of {}", length, max),
                "", fmt::format("limits.data.string_length = {}", max),
                "Maximum string length exceeded", "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkNestingDepth(size_t depth) {
    int max = rules_.limits.data.nesting_depth;
    if (max > 0 && static_cast<int>(depth) > max) {
        return enforce("limits.data.nesting_depth", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Nesting depth {} exceeds limit of {}", depth, max),
                "", fmt::format("limits.data.nesting_depth = {}", max),
                "Maximum data nesting depth exceeded", "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkOutputSize(size_t size) {
    int max = rules_.limits.data.output_size;
    if (max > 0 && static_cast<int>(size) > max) {
        return enforce("limits.data.output_size", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Output size {} exceeds limit of {}", size, max),
                "", fmt::format("limits.data.output_size = {}", max),
                "Maximum output size exceeded", "", ""));
    }
    return "";
}

std::string GovernanceEngine::checkDictSize(size_t size) {
    int max = rules_.limits.data.dict_size;
    if (max > 0 && static_cast<int>(size) > max) {
        return enforce("limits.data.dict_size", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Dictionary size {} exceeds limit of {}", size, max),
                "", fmt::format("limits.data.dict_size = {}", max),
                "Maximum dictionary size exceeded", "", ""));
    }
    return "";
}

// --- Rate Limiting ---
bool GovernanceEngine::checkPolyglotRate() {
    polyglot_rate_.max_per_second = rules_.limits.rate.max_polyglot_per_second;
    return polyglot_rate_.check();
}

bool GovernanceEngine::checkStdlibRate() {
    stdlib_rate_.max_per_second = rules_.limits.rate.max_stdlib_calls_per_second;
    return stdlib_rate_.check();
}

bool GovernanceEngine::checkFileOpsRate() {
    file_ops_rate_.max_per_second = rules_.limits.rate.max_file_ops_per_second;
    return file_ops_rate_.check();
}

// --- Per-Language Getters ---
int GovernanceEngine::getTimeoutForLanguage(const std::string& lang) const {
    auto it = rules_.languages.per_language.find(lang);
    if (it != rules_.languages.per_language.end() && it->second.timeout > 0)
        return it->second.timeout;
    if (rules_.limits.timeout.per_block > 0) return rules_.limits.timeout.per_block;
    return rules_.timeout_seconds;
}

int GovernanceEngine::getMaxLinesForLanguage(const std::string& lang) const {
    // FIX 18: Normalize language alias before lookup
    std::string normalized = normalizeLanguage(lang);
    auto it = rules_.languages.per_language.find(normalized);
    if (it != rules_.languages.per_language.end() && it->second.max_lines > 0)
        return it->second.max_lines;
    return rules_.limits.code.max_lines_per_block;
}

const LanguageConfig* GovernanceEngine::getLanguageConfig(const std::string& lang) const {
    // FIX 18: Normalize language alias before lookup
    std::string normalized = normalizeLanguage(lang);
    auto it = rules_.languages.per_language.find(normalized);
    if (it != rules_.languages.per_language.end()) return &it->second;
    return nullptr;
}

// --- Comprehensive Polyglot Block Check ---
std::string GovernanceEngine::checkVariableBinding(size_t binding_count, int line) {
    if (!rules_.polyglot.variable_binding.require_explicit) return "";
    if (binding_count > 0) return "";  // Has bindings, OK

    std::string msg = "[governance] Polyglot block at line " + std::to_string(line) +
        " has no variable bindings.\n"
        "  Rule: polyglot.variable_binding.require_explicit = true\n"
        "  Fix: Add variable bindings: <<python[var1, var2] ... >>\n";

    return enforce("polyglot.variable_binding.require_explicit",
                   rules_.polyglot.variable_binding.require_explicit_level, msg);
}

std::string GovernanceEngine::checkPolyglotBlock(
    const std::string& language, const std::string& code,
    const std::string& source_file, int line,
    size_t binding_count) {

    // Check variable binding requirement first
    std::string bind_err = checkVariableBinding(binding_count, line);
    if (!bind_err.empty()) return bind_err;

    // Delegate to existing comprehensive check
    return checkPolyglotBlock(language, code, source_file, line);
}

std::string GovernanceEngine::checkPolyglotBlock(
    const std::string& language, const std::string& code,
    const std::string& /*source_file*/, int line) {

    // FIX 18: Normalize language aliases (bash→shell, js→javascript, etc.)
    std::string lang = normalizeLanguage(language);

    // FIX 16: Pre-process code — strip string literals for pattern matching
    // This prevents false positives from code/paths inside strings
    std::string stripped = stripStringLiterals(code);
    std::string stripped_all = stripComments(stripped);  // Also strip comments

    std::string err;

    // Language allowed? (uses normalized name)
    err = checkLanguageAllowed(lang, line);
    if (!err.empty()) return err;

    // Shell capability check
    if (lang == "shell") {
        err = checkShellAllowed();
        if (!err.empty()) return err;
    }

    // Code quality checks — secrets/PII use RAW code (secrets CAN be in strings)
    err = checkSecrets(code, line);
    if (!err.empty()) return err;
    err = checkPii(code, line);
    if (!err.empty()) return err;

    // Pattern-based checks use STRIPPED code to avoid string-content false positives
    // EVA-1 through EVA-4: Placeholders, temp code, simulation markers, and apologetic
    // language patterns match COMMENT text, so use `stripped` (comments preserved)
    // instead of `stripped_all` (comments removed) which made them 100% dead
    err = checkPlaceholders(stripped, line);
    if (!err.empty()) return err;
    err = checkHardcodedResults(stripped, line);
    if (!err.empty()) return err;
    err = checkDangerousCall(lang, stripped, line);
    if (!err.empty()) return err;

    // New v3.0 checks — use stripped (strings removed, comments preserved)
    err = checkTemporaryCode(stripped, line);
    if (!err.empty()) return err;
    err = checkSimulationMarkers(stripped, line);
    if (!err.empty()) return err;
    err = checkMockData(code, line);  // Mock data: literals ARE in strings, keep raw
    if (!err.empty()) return err;
    err = checkApologeticLanguage(stripped, line);
    if (!err.empty()) return err;
    err = checkDeadCode(stripped, line);
    if (!err.empty()) return err;
    err = checkDebugArtifacts(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkUnsafeDeserialization(stripped, line);
    if (!err.empty()) return err;
    err = checkSqlInjection(code, line);  // SQL queries ARE in strings, keep raw
    if (!err.empty()) return err;
    err = checkPathTraversal(stripped, line);  // FIX 16: No more false positives from string paths
    if (!err.empty()) return err;
    err = checkHardcodedUrls(code, line);  // URLs ARE in strings
    if (!err.empty()) return err;
    err = checkHardcodedIps(code, line);   // IPs ARE in strings
    if (!err.empty()) return err;
    err = checkEncoding(stripped, line);
    if (!err.empty()) return err;
    err = checkComplexity(stripped, line);
    if (!err.empty()) return err;

    // LLM anti-drift checks
    err = checkOversimplification(stripped, line);
    if (!err.empty()) return err;
    err = checkIncompleteLogic(stripped, line);
    if (!err.empty()) return err;
    err = checkHallucinatedApis(lang, code, line);  // Has its own stripping
    if (!err.empty()) return err;

    // NOTE: Complexity floor intentionally NOT applied to polyglot blocks.
    // Polyglot blocks have their own quality checks (max_lines, banned_functions,
    // hallucinated APIs, security). The complexity floor is designed for NAAb function
    // bodies to prevent trivial compute_*/calculate_* stubs.

    // Security checks — use stripped code
    err = checkShellInjection(stripped, line);
    if (!err.empty()) return err;
    err = checkCodeInjection(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkPrivilegeEscalation(stripped, line);
    if (!err.empty()) return err;
    err = checkDataExfiltration(stripped, line);
    if (!err.empty()) return err;
    err = checkResourceAbuse(stripped, line);
    if (!err.empty()) return err;
    err = checkInfoDisclosure(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkCryptoWeakness(stripped, line);
    if (!err.empty()) return err;

    // Capability checks for polyglot blocks
    err = checkNetworkImports(lang, stripped, line);
    if (!err.empty()) return err;

    // Per-language checks
    err = checkImports(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkBannedFunctions(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkLanguageStyle(lang, stripped, line);
    if (!err.empty()) return err;
    err = checkCodeSize(lang, code, line);  // Code size counts raw lines
    if (!err.empty()) return err;

    // Custom rules — use both raw and stripped
    err = checkCustomRules(lang, code, line);
    if (!err.empty()) return err;

    // Polyglot optimization (language choice suggestions)
    err = checkPolyglotOptimization(lang, code, line);
    if (!err.empty()) return err;

    return "";
}

// --- Schema Validation ---
size_t GovernanceEngine::levenshteinDistance(const std::string& s1, const std::string& s2) {
    size_t len1 = s1.size(), len2 = s2.size();
    std::vector<std::vector<size_t>> d(len1 + 1, std::vector<size_t>(len2 + 1));
    for (size_t i = 0; i <= len1; i++) d[i][0] = i;
    for (size_t j = 0; j <= len2; j++) d[0][j] = j;
    for (size_t i = 1; i <= len1; i++)
        for (size_t j = 1; j <= len2; j++) {
            size_t cost = (s1[i-1] == s2[j-1]) ? 0 : 1;
            d[i][j] = std::min({d[i-1][j]+1, d[i][j-1]+1, d[i-1][j-1]+cost});
        }
    return d[len1][len2];
}

std::string GovernanceEngine::suggestKey(const std::string& key,
                                          const std::vector<std::string>& valid_keys) {
    size_t best_dist = 999;
    std::string best_match;
    for (const auto& vk : valid_keys) {
        size_t dist = levenshteinDistance(key, vk);
        if (dist < best_dist && dist <= 3) { best_dist = dist; best_match = vk; }
    }
    return best_match;
}

std::vector<std::string> GovernanceEngine::validateSchema(const std::string& json_path) {
    std::vector<std::string> warnings;
    static const std::vector<std::string> VALID_TOP_KEYS = {
        "version", "mode", "extends", "description",
        "languages", "capabilities", "limits", "requirements",
        "restrictions", "code_quality", "custom_rules", "scopes",
        "output", "audit", "meta", "hooks", "polyglot", "polyglot_optimization",
        "contracts", "baselines", "project_context", "scanner"
    };

    try {
        std::ifstream ifs(json_path);
        if (!ifs.is_open()) return warnings;
        auto j = nlohmann::json::parse(ifs);

        for (auto& [key, val] : j.items()) {
            bool found = false;
            for (const auto& vk : VALID_TOP_KEYS) { if (key == vk) { found = true; break; } }
            if (!found) {
                std::string suggestion = suggestKey(key, VALID_TOP_KEYS);
                if (!suggestion.empty()) {
                    warnings.push_back(fmt::format(
                        "[governance] Warning: Unknown key \"{}\" — did you mean \"{}\"?", key, suggestion));
                } else {
                    warnings.push_back(fmt::format(
                        "[governance] Warning: Unknown key \"{}\"", key));
                }
            }
        }
    } catch (...) {}
    return warnings;
}

// --- Entropy Analysis ---
double GovernanceEngine::calculateEntropy(const std::string& str) {
    if (str.empty()) return 0.0;
    std::unordered_map<char, int> freq;
    for (char c : str) freq[c]++;
    double entropy = 0.0;
    double len = static_cast<double>(str.size());
    for (const auto& [ch, count] : freq) {
        double p = count / len;
        if (p > 0) entropy -= p * std::log2(p);
    }
    return entropy;
}

bool GovernanceEngine::looksLikeBase64(const std::string& str) {
    if (str.size() < 20) return false;
    try {
        std::regex re("^[A-Za-z0-9+/]+=*$");
        return std::regex_match(str, re);
    } catch (...) { return false; }
}

bool GovernanceEngine::looksLikeHex(const std::string& str) {
    if (str.size() < 20 || str.size() % 2 != 0) return false;
    try {
        std::regex re("^[0-9a-fA-F]+$");
        return std::regex_match(str, re);
    } catch (...) { return false; }
}

// --- Audit Trail ---
void GovernanceEngine::logAuditEvent(const std::string& event_type,
                                      const std::string& rule_name,
                                      const std::string& message,
                                      const std::string& file, int line) {
    if (rules_.audit.level == "none") return;
    std::lock_guard<std::mutex> lock(audit_mutex_);

    std::string output_file = rules_.audit.output_file;
    if (output_file.empty()) output_file = ".governance-audit.jsonl";

    // Build entry
    nlohmann::json entry;
    entry["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();
    entry["event"] = event_type;
    entry["rule"] = rule_name;
    entry["message"] = message;
    if (!file.empty()) entry["file"] = file;
    if (line > 0) entry["line"] = line;

    // Tamper-evident hash chain
    if (rules_.audit.tamper_evidence.enabled) {
        entry["prev_hash"] = last_audit_hash_.empty()
            ? rules_.audit.tamper_evidence.chain_genesis
            : last_audit_hash_;
        last_audit_hash_ = computeAuditHash(entry.dump());
        entry["hash"] = last_audit_hash_;
    }

    try {
        std::ofstream ofs(output_file, std::ios::app);
        if (ofs.is_open()) {
            ofs << entry.dump() << "\n";
        }
    } catch (...) {}
}

std::string GovernanceEngine::computeAuditHash(const std::string& data) const {
    // Simple hash (not cryptographic — for tamper evidence only)
    std::hash<std::string> hasher;
    size_t hash = hasher(data);
    std::ostringstream oss;
    oss << std::hex << hash;
    return oss.str();
}

// --- Hooks ---
void GovernanceEngine::fireHook(const HookConfig& hook,
                                 const std::unordered_map<std::string, std::string>& vars) {
    if (hook.command.empty()) return;

    std::string cmd = hook.command;
    for (const auto& arg : hook.args) {
        std::string expanded = arg;
        for (const auto& [key, val] : vars) {
            std::string placeholder = "${" + key + "}";
            size_t pos = expanded.find(placeholder);
            while (pos != std::string::npos) {
                expanded.replace(pos, placeholder.size(), val);
                pos = expanded.find(placeholder, pos + val.size());
            }
        }
        cmd += " " + expanded;
    }

    // Execute with timeout (fire-and-forget)
    (void)system(cmd.c_str());
}

// --- Report Generation Stubs ---
std::string GovernanceEngine::generateJsonReport() const {
    nlohmann::json report;
    report["version"] = "3.0";
    report["mode"] = rules_.mode == GovernanceMode::ENFORCE ? "enforce" : (rules_.mode == GovernanceMode::AUDIT ? "audit" : "off");
    report["results"] = nlohmann::json::array();
    for (const auto& r : check_results_) {
        nlohmann::json entry;
        entry["rule"] = r.rule_name;
        entry["level"] = levelToString(r.level);
        entry["passed"] = r.passed;
        if (!r.message.empty()) entry["message"] = r.message;
        report["results"].push_back(entry);
    }
    return report.dump(2);
}

std::string GovernanceEngine::generateSarifReport() const {
    // SARIF 2.1.0 format
    nlohmann::json sarif;
    sarif["version"] = "2.1.0";
    sarif["$schema"] = "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json";
    auto& runs = sarif["runs"] = nlohmann::json::array();
    nlohmann::json run;
    run["tool"]["driver"]["name"] = "NAAb Governance Engine";
    run["tool"]["driver"]["version"] = "3.0";
    run["results"] = nlohmann::json::array();
    for (const auto& r : check_results_) {
        if (r.passed) continue;
        nlohmann::json result;
        result["ruleId"] = r.rule_name;
        result["level"] = r.level == EnforcementLevel::ADVISORY ? "warning" : "error";
        result["message"]["text"] = r.message.empty() ? r.rule_name : r.message.substr(0, r.message.find('\n'));
        run["results"].push_back(result);
    }
    runs.push_back(run);
    return sarif.dump(2);
}

std::string GovernanceEngine::generateJunitReport() const {
    std::ostringstream oss;
    int total = static_cast<int>(check_results_.size()), failures = 0;
    for (const auto& r : check_results_) if (!r.passed && r.level != EnforcementLevel::ADVISORY) failures++;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << fmt::format("<testsuite name=\"NAAb Governance\" tests=\"{}\" failures=\"{}\">\n", total, failures);
    for (const auto& r : check_results_) {
        oss << fmt::format("  <testcase name=\"{}\"", r.rule_name);
        if (r.passed) { oss << "/>\n"; }
        else {
            oss << ">\n";
            oss << fmt::format("    <failure type=\"{}\">{}</failure>\n",
                levelToString(r.level), r.rule_name);
            oss << "  </testcase>\n";
        }
    }
    oss << "</testsuite>\n";
    return oss.str();
}

std::string GovernanceEngine::generateCsvReport() const {
    std::ostringstream oss;
    oss << "rule,level,passed,message\n";
    for (const auto& r : check_results_) {
        oss << r.rule_name << "," << levelToString(r.level) << ","
            << (r.passed ? "true" : "false") << ","
            << "\"" << r.rule_name << "\"\n";
    }
    return oss.str();
}

std::string GovernanceEngine::generateHtmlReport() const {
    std::ostringstream oss;
    oss << "<html><head><title>NAAb Governance Report</title></head><body>\n";
    oss << "<h1>NAAb Governance Report</h1>\n<table border='1'>\n";
    oss << "<tr><th>Rule</th><th>Level</th><th>Status</th></tr>\n";
    for (const auto& r : check_results_) {
        std::string color = r.passed ? "green" : (r.level == EnforcementLevel::ADVISORY ? "orange" : "red");
        oss << fmt::format("<tr><td>{}</td><td>{}</td><td style='color:{}'>{}</td></tr>\n",
            r.rule_name, levelToString(r.level), color, r.passed ? "PASS" : "FAIL");
    }
    oss << "</table></body></html>\n";
    return oss.str();
}

void GovernanceEngine::writeReports() const {
    auto writeFile = [](const std::string& path, const std::string& content) {
        if (path.empty()) return;
        std::ofstream ofs(path);
        if (ofs.is_open()) ofs << content;
    };
    writeFile(rules_.output.file_output.report_json, generateJsonReport());
    writeFile(rules_.output.file_output.report_sarif, generateSarifReport());
    writeFile(rules_.output.file_output.report_junit, generateJunitReport());
    writeFile(rules_.output.file_output.report_csv, generateCsvReport());
    writeFile(rules_.output.file_output.report_html, generateHtmlReport());
}

// --- Environment Variable Substitution ---
std::string GovernanceEngine::substituteEnvVars(const std::string& value) const {
    if (!rules_.meta.environment.allow_env_var_substitution) return value;
    std::string result = value;
    try {
        std::regex re("\\$\\{([^}:]+)(?::-([^}]*))?\\}");
        std::smatch match;
        std::string::const_iterator search_start = result.cbegin();
        std::string output;
        while (std::regex_search(search_start, result.cend(), match, re)) {
            output.append(search_start, search_start + match.position());
            std::string var_name = match[1].str();
            std::string default_val = match.size() > 2 ? match[2].str() : "";
            const char* env_val = std::getenv(var_name.c_str());
            output.append(env_val ? env_val : default_val);
            search_start += match.position() + match.length();
        }
        output.append(search_start, result.cend());
        return output;
    } catch (...) { return value; }
}

// --- Config Inheritance ---
void GovernanceEngine::loadInheritedConfig(const std::string& base_dir, int depth) {
    if (rules_.extends_path.empty()) return;
    if (depth >= rules_.meta.inheritance.max_depth) {
        fprintf(stderr, "[governance] Warning: Max inheritance depth (%d) reached\n",
                rules_.meta.inheritance.max_depth);
        return;
    }
    namespace fs = std::filesystem;
    fs::path parent_path = fs::path(base_dir) / rules_.extends_path;
    if (!fs::exists(parent_path)) {
        fprintf(stderr, "[governance] Warning: Extended config not found: %s\n",
                parent_path.string().c_str());
        return;
    }
    // Load parent first, then child overrides
    GovernanceEngine parent_engine;
    if (parent_engine.loadFromFile(parent_path.string())) {
        parent_engine.loadInheritedConfig(parent_path.parent_path().string(), depth + 1);
        // In child_wins strategy, child values already set — nothing to merge back
        // The parent is loaded only for any values NOT set in child
    }
}

// ============================================================================
// Polyglot Optimization Checks
// ============================================================================

std::string GovernanceEngine::checkPolyglotOptimization(
    const std::string& language,
    const std::string& code,
    int line
) {
    if (!active_) return "";
    if (!rules_.polyglot_optimization.enabled) return "";

    // Create detector with task→language matrix from config
    std::map<std::string, std::map<std::string, int>> matrix;
    for (const auto& [task, lang_scores] : rules_.polyglot_optimization.task_language_matrix) {
        for (const auto& [lang, score_data] : lang_scores) {
            matrix[task][lang] = score_data.score;
        }
    }

    // Phase 2: Fuse calibration data — measured scores override hardcoded defaults
    // Priority: calibration > govern.json matrix > hardcoded defaults
    if (rules_.polyglot_optimization.calibration.enabled) {
        const_cast<GovernanceEngine*>(this)->loadCalibration();
        for (const auto& [task, lang_entries] : calibration_data_) {
            for (const auto& [lang, entry] : lang_entries) {
                if (entry.score > 0) {
                    matrix[task][lang] = entry.score;
                }
            }
        }
    }

    analyzer::ComprehensiveTaskDetector detector(matrix);

    // Analyze code
    auto result = detector.analyze(code, language);

    // Check enforcement level
    std::string level = rules_.polyglot_optimization.enforcement_level;

    // Helper errors config
    bool show_suggestions = rules_.polyglot_optimization.helper_errors.enabled;

    // Determine if we should suggest different language
    bool should_suggest = false;
    std::string message;

    // Never suggest switching to the same language or when there's no real improvement
    if (result.optimal_language == language ||
        result.improvement_percent <= 0 ||
        result.optimal_language_score <= result.current_language_score) {
        should_suggest = false;
    }
    // Only suggest when improvement is substantial (>50%) to avoid
    // driving LLMs into infinite language-switching loops over marginal gains.
    // A 25% improvement (40→50) is not worth the code rewrite.
    else if (result.improvement_percent > 50) {
        should_suggest = true;
    } else if (result.current_language_score < 40 && result.optimal_language_score > 80) {
        // Only flag truly bad choices (score < 40 vs optimal > 80)
        should_suggest = true;
    }

    if (should_suggest && show_suggestions) {
        suggestBetterLanguage(
            language, code,
            analyzer::taskIntentToString(result.primary_task),
            {result.optimal_language},
            result.improvement_percent,
            result.reasons
        );

        // Build enforcement message based on level
        if (level == "hard") {
            message = fmt::format(
                "HARD violation: Suboptimal language choice\n"
                "  Current: {} (score: {}/100)\n"
                "  Optimal: {} (score: {}/100)\n"
                "  Improvement: +{}%\n\n"
                "  This code MUST use a more appropriate language.",
                language, result.current_language_score,
                result.optimal_language, result.optimal_language_score,
                result.improvement_percent
            );
        } else if (level == "soft") {
            // SOFT optimization violations are advisory-only (warn, don't block).
            // Blocking on language choice causes LLMs to enter infinite
            // language-switching loops, which is worse than a suboptimal choice.
            message = fmt::format(
                "Note: Consider using {} instead of {} (score: {}/100 vs {}/100, +{}% improvement)",
                result.optimal_language, language,
                result.optimal_language_score, result.current_language_score,
                result.improvement_percent
            );
            fmt::print(stderr, "[governance] {}\n", message);
        } else if (level == "advisory") {
            message = fmt::format(
                "Advisory: Consider using {} instead of {} for +{}% improvement",
                result.optimal_language, language, result.improvement_percent
            );
        }

        // Record check result
        CheckResult check;
        check.rule_name = "polyglot_optimization";
        check.level = level == "hard" ? EnforcementLevel::HARD :
                     level == "soft" ? EnforcementLevel::SOFT :
                                      EnforcementLevel::ADVISORY;
        check.passed = false;
        check.message = message;
        check.category = "polyglot";
        check.severity = result.improvement_percent > 50 ? "high" :
                        result.improvement_percent > 30 ? "medium" : "low";
        check.line = line;
        check_results_.push_back(check);

        // Return message based on enforcement level
        // Only HARD blocks execution. SOFT optimization is advisory (printed above).
        if (level == "hard") {
            return message;
        }
    }

    return "";
}

void GovernanceEngine::suggestBetterLanguage(
    const std::string& current_lang,
    const std::string& /* code */,
    const std::string& task_type,
    const std::vector<std::string>& optimal_langs,
    int improvement_percent,
    const std::vector<std::string>& reasons
) {
    if (!rules_.polyglot_optimization.helper_errors.enabled) return;

    bool show_example = rules_.polyglot_optimization.helper_errors.show_example_code;

    // Phase 3: Determine confidence level
    std::string confidence = "ESTIMATED";
    std::string confidence_detail;

    // Check calibration data for this task type
    if (!calibration_data_.empty()) {
        // Map semantic task types to calibration categories
        // Calibration uses directory names (numerical, string, etc.)
        // Analyzer uses intent strings (numerical_computation, string_manipulation, etc.)
        std::string cal_task;
        if (task_type.find("numerical") != std::string::npos ||
            task_type.find("linear") != std::string::npos ||
            task_type.find("statistical") != std::string::npos)
            cal_task = "numerical";
        else if (task_type.find("string") != std::string::npos)
            cal_task = "string";
        else if (task_type.find("file") != std::string::npos)
            cal_task = "file_io";
        else if (task_type.find("json") != std::string::npos ||
                 task_type.find("data_serialization") != std::string::npos ||
                 task_type.find("data_parsing") != std::string::npos)
            cal_task = "json";
        else if (task_type.find("concurrent") != std::string::npos ||
                 task_type.find("async") != std::string::npos ||
                 task_type.find("parallel") != std::string::npos)
            cal_task = "concurrency";
        else if (task_type.find("cli") != std::string::npos ||
                 task_type.find("batch") != std::string::npos)
            cal_task = "cli";
        else if (task_type.find("web") != std::string::npos ||
                 task_type.find("network") != std::string::npos)
            cal_task = "web_apis";
        else if (task_type.find("system") != std::string::npos ||
                 task_type.find("memory") != std::string::npos ||
                 task_type.find("process") != std::string::npos)
            cal_task = "systems";

        if (!cal_task.empty() && calibration_data_.count(cal_task)) {
            auto& cal_cat = calibration_data_.at(cal_task);
            bool have_current = cal_cat.count(current_lang) > 0;
            bool have_optimal = !optimal_langs.empty() && cal_cat.count(optimal_langs[0]) > 0;

            if (have_current && have_optimal) {
                confidence = "CALIBRATED";
                auto& cur = cal_cat.at(current_lang);
                auto& opt = cal_cat.at(optimal_langs[0]);
                if (cur.us > 0 && opt.us > 0) {
                    double speedup = (double)cur.us / (double)opt.us;
                    confidence_detail = fmt::format("{} {:.1f}x faster (calibrated on this machine)",
                        optimal_langs[0], speedup);
                }
            }
        }
    }

    // Check confidence display level
    const auto& conf_cfg = rules_.polyglot_optimization.confidence;
    if (conf_cfg.min_display_level == "measured" && confidence != "MEASURED") return;
    if (conf_cfg.min_display_level == "calibrated" &&
        confidence != "MEASURED" && confidence != "CALIBRATED") return;
    if (conf_cfg.suppress_unknown && confidence == "UNKNOWN") return;

    // Format helper error similar to stdlib helper errors
    fmt::print("\n  Hint: Language optimization opportunity detected.\n\n");
    fmt::print("  Current language: {} (for {} task)\n", current_lang, task_type);

    if (!optimal_langs.empty()) {
        if (optimal_langs.size() == 1) {
            fmt::print("  Optimal language: {}\n", optimal_langs[0]);
        } else {
            std::string langs_str;
            for (size_t i = 0; i < optimal_langs.size(); ++i) {
                if (i > 0) langs_str += ", ";
                langs_str += optimal_langs[i];
            }
            fmt::print("  Optimal languages: {}\n", langs_str);
        }
    }

    // Phase 3: Show confidence level
    fmt::print("  Confidence: {}\n", confidence);
    if (conf_cfg.show_measurement_details && !confidence_detail.empty()) {
        fmt::print("  Detail: {}\n", confidence_detail);
    }

    if (improvement_percent > 0) {
        fmt::print("  Potential improvement: +{}%\n\n", improvement_percent);
    }

    // Show reasons
    if (!reasons.empty()) {
        fmt::print("  Reasons:\n");
        int count = 0;
        for (const auto& reason : reasons) {
            if (count++ >= 3) break;  // Show top 3 reasons
            fmt::print("    • {}\n", reason);
        }
        fmt::print("\n");
    }

    // Show example if enabled (only if optimal differs from current)
    if (show_example && !optimal_langs.empty() && optimal_langs[0] != current_lang) {
        fmt::print("  Example refactoring:\n");
        fmt::print("    ✗ Current: <<{}  [code] >>\n", current_lang);
        fmt::print("    ✓ Better:  <<{}  [code] >>\n\n", optimal_langs[0]);
    }

    fmt::print("  For more: docs/polyglot/optimization_guide.md\n\n");
}

// ============================================================================
// Empirical Profiling
// ============================================================================

bool GovernanceEngine::isProfilingEnabled() const {
    return active_ && rules_.polyglot_optimization.enabled &&
           rules_.polyglot_optimization.profiling.enabled;
}

void GovernanceEngine::writeProfileEntry(const std::string& language,
                                         const std::string& task_category,
                                         const std::string& code_hash,
                                         int64_t duration_us) {
    if (!isProfilingEnabled()) return;

    auto& cfg = rules_.polyglot_optimization.profiling;

    // Expand ~ in path
    std::string path = cfg.profile_path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    // Ensure parent directory exists
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    // Read existing entries (ring buffer)
    nlohmann::json entries = nlohmann::json::array();
    {
        std::ifstream in(path);
        if (in.is_open()) {
            try {
                nlohmann::json existing;
                in >> existing;
                if (existing.is_array()) entries = existing;
            } catch (...) {
                // Corrupted file — start fresh
            }
        }
    }

    // Build new entry
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    nlohmann::json entry;
    entry["lang"] = language;
    entry["task"] = task_category;
    if (cfg.include_code_hash && !code_hash.empty()) {
        entry["hash"] = code_hash;
    }
    entry["us"] = duration_us;
    entry["ts"] = ts;

    entries.push_back(entry);

    // Ring buffer: trim to max_entries
    if (cfg.max_entries > 0 && (int)entries.size() > cfg.max_entries) {
        int excess = (int)entries.size() - cfg.max_entries;
        entries.erase(entries.begin(), entries.begin() + excess);
    }

    // Write back
    std::ofstream out(path);
    if (out.is_open()) {
        out << entries.dump(2);
    }
}

bool GovernanceEngine::loadCalibration() {
    if (calibration_loaded_) return !calibration_data_.empty();
    calibration_loaded_ = true;

    auto& cfg = rules_.polyglot_optimization.calibration;
    if (!cfg.enabled) return false;

    // Expand ~ in path
    std::string path = cfg.calibration_path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    std::ifstream in(path);
    if (!in.is_open()) return false;

    try {
        nlohmann::json j;
        in >> j;

        if (!j.contains("results") || !j["results"].is_object()) return false;

        // Check age
        if (cfg.max_age_days > 0 && j.contains("timestamp") && j["timestamp"].is_string()) {
            // Simple age check: compare epoch-based if available
            // For now, just load the data regardless of age
        }

        for (auto& [task, lang_scores] : j["results"].items()) {
            if (!lang_scores.is_object()) continue;
            for (auto& [lang, data] : lang_scores.items()) {
                CalibrationEntry entry;
                if (data.is_object()) {
                    if (data.contains("us")) entry.us = data["us"].get<int64_t>();
                    if (data.contains("score")) entry.score = data["score"].get<int>();
                }
                calibration_data_[task][lang] = entry;
            }
        }

        return !calibration_data_.empty();
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Polyglot Consensus Verification
// ============================================================================

bool GovernanceEngine::isVerificationEnabled() const {
    return active_ &&
           rules_.polyglot_optimization.enabled &&
           rules_.polyglot_optimization.verification.enabled &&
           !rules_.polyglot_optimization.verification.consensus_languages.empty();
}

bool GovernanceEngine::isNumericString(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

std::string GovernanceEngine::escapeStringForVerification(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '\'') result += "\\'";
        else if (c == '\"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') continue;
        else result += c;
    }
    return result;
}

bool GovernanceEngine::compareResults(const std::string& a, const std::string& b, double tolerance) {
    // Exact string match first (fast path)
    if (a == b) return true;

    // Whitespace-normalized string match
    auto normalize = [](const std::string& s) -> std::string {
        std::string result;
        for (char c : s) {
            if (c == '\r') continue;
            result += c;
        }
        // Trim trailing whitespace/newlines
        while (!result.empty() && (result.back() == '\n' || result.back() == ' ' || result.back() == '\t'))
            result.pop_back();
        // Trim leading whitespace
        size_t start = result.find_first_not_of(" \t\n");
        if (start != std::string::npos) result = result.substr(start);
        return result;
    };

    std::string na = normalize(a);
    std::string nb = normalize(b);
    if (na == nb) return true;

    // Numeric comparison with tolerance
    if (isNumericString(na) && isNumericString(nb)) {
        double da = std::stod(na);
        double db = std::stod(nb);
        return std::abs(da - db) <= tolerance;
    }

    return false;
}

std::string GovernanceEngine::classifyTaskForVerification(
    const std::string& code, const std::string& language) {

    std::map<std::string, std::map<std::string, int>> matrix;
    for (const auto& [task, lang_scores] : rules_.polyglot_optimization.task_language_matrix) {
        for (const auto& [lang, score_data] : lang_scores) {
            matrix[task][lang] = score_data.score;
        }
    }

    analyzer::ComprehensiveTaskDetector detector(matrix);
    auto result = detector.analyze(code, language);
    return analyzer::taskIntentToString(result.primary_task);
}

std::string GovernanceEngine::extractMathExpression(
    const std::string& code, const std::string& lang) {

    std::string trimmed = code;
    auto start = trimmed.find_first_not_of(" \t\n\r");
    auto end = trimmed.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    trimmed = trimmed.substr(start, end - start + 1);

    // Single line? Likely a pure expression
    if (trimmed.find('\n') == std::string::npos) {
        // Strip common wrappers
        if (lang == "python" && trimmed.substr(0, 6) == "print(" && trimmed.back() == ')')
            return trimmed.substr(6, trimmed.size() - 7);
        if ((lang == "javascript" || lang == "js") && trimmed.substr(0, 12) == "console.log(" && trimmed.back() == ')')
            return trimmed.substr(12, trimmed.size() - 13);
        if (lang == "ruby" && trimmed.size() > 5 && trimmed.substr(0, 5) == "puts ")
            return trimmed.substr(5);

        // Check if it looks like a math expression
        bool looks_numeric = true;
        for (char c : trimmed) {
            if (!std::isdigit(c) && c != '.' && c != '+' && c != '-' &&
                c != '*' && c != '/' && c != '(' && c != ')' && c != ' ' &&
                c != '%' && c != 'e' && c != 'E') {
                looks_numeric = false;
                break;
            }
        }
        if (looks_numeric) return trimmed;
    }

    // Multi-line: look for last line as the result expression
    std::istringstream stream(trimmed);
    std::string line, last_line;
    while (std::getline(stream, line)) {
        auto ls = line.find_first_not_of(" \t");
        if (ls != std::string::npos) last_line = line.substr(ls);
    }

    // Check if last line is a simple expression (no assignment, no import)
    if (!last_line.empty() && last_line.find('=') == std::string::npos &&
        last_line.find("import") == std::string::npos) {
        // Strip print wrappers from last line too
        if (last_line.substr(0, 6) == "print(" && last_line.back() == ')')
            return last_line.substr(6, last_line.size() - 7);
        if (last_line.substr(0, 12) == "console.log(" && last_line.back() == ')')
            return last_line.substr(12, last_line.size() - 13);
        return last_line;
    }

    return "";  // Can't extract — fallback to echo strategy
}

std::string GovernanceEngine::generateEchoCode(
    const std::string& target_lang, const std::string& value) {
    std::string esc = escapeStringForVerification(value);
    if (target_lang == "python") return "print('" + esc + "')";
    if (target_lang == "javascript" || target_lang == "js")
        return "console.log('" + esc + "')";
    if (target_lang == "go")
        return "package main\nimport \"fmt\"\nfunc main(){fmt.Print(\"" + esc + "\")}";
    if (target_lang == "ruby") return "print '" + esc + "'";
    if (target_lang == "nim") return "import std/strutils\nstdout.write(\"" + esc + "\")";
    if (target_lang == "julia") return "print(\"" + esc + "\")";
    if (target_lang == "rust")
        return "fn main(){print!(\"" + esc + "\");}";
    if (target_lang == "shell" || target_lang == "sh" || target_lang == "bash")
        return "printf '%s' '" + esc + "'";
    // Default fallback
    return "print('" + esc + "')";
}

std::string GovernanceEngine::generateVerificationCode(
    const std::string& task_type,
    const std::string& original_code,
    const std::string& original_result,
    const std::string& source_lang,
    const std::string& target_lang)
{
    // ================================================================
    // NUMERICAL VERIFICATION
    // ================================================================
    if (task_type.find("numerical") != std::string::npos ||
        task_type.find("statistical") != std::string::npos ||
        task_type.find("linear") != std::string::npos) {

        std::string expr = extractMathExpression(original_code, source_lang);

        if (target_lang == "python") {
            if (!expr.empty())
                return "result = " + expr + "\nprint(result)";
            return "print(" + original_result + ")";
        }
        else if (target_lang == "javascript" || target_lang == "js") {
            if (!expr.empty())
                return "console.log(" + expr + ")";
            return "console.log(" + original_result + ")";
        }
        else if (target_lang == "go") {
            std::string e = expr.empty() ? original_result : expr;
            return "package main\nimport \"fmt\"\nfunc main() {\n\tfmt.Print(" + e + ")\n}";
        }
        else if (target_lang == "ruby") {
            return "print " + (expr.empty() ? original_result : expr);
        }
        else if (target_lang == "nim") {
            std::string e = expr.empty() ? original_result : expr;
            return "import std/strutils\nstdout.write($(" + e + "))";
        }
        else if (target_lang == "julia") {
            return "print(" + (expr.empty() ? original_result : expr) + ")";
        }
        else if (target_lang == "rust") {
            std::string e = expr.empty() ? original_result : expr;
            return "fn main() { print!(\"{}\", " + e + "); }";
        }
        else if (target_lang == "shell" || target_lang == "sh" || target_lang == "bash") {
            std::string e = expr.empty() ? original_result : expr;
            return "echo $(( " + e + " ))";
        }
    }

    // ================================================================
    // STRING VERIFICATION
    // ================================================================
    if (task_type.find("string") != std::string::npos) {
        return generateEchoCode(target_lang, original_result);
    }

    // ================================================================
    // JSON / DATA VERIFICATION
    // ================================================================
    if (task_type.find("json") != std::string::npos ||
        task_type.find("data_parsing") != std::string::npos ||
        task_type.find("data_serialization") != std::string::npos) {

        std::string esc = escapeStringForVerification(original_result);
        if (target_lang == "python") {
            return "import json\ndata = json.loads('" + esc + "')\nprint(json.dumps(data, sort_keys=True))";
        }
        else if (target_lang == "javascript" || target_lang == "js") {
            return "const d = JSON.parse('" + esc + "');\n"
                   "const keys = Object.keys(d).sort();\n"
                   "const sorted = {}; keys.forEach(k => sorted[k] = d[k]);\n"
                   "console.log(JSON.stringify(sorted))";
        }
        else if (target_lang == "go") {
            return "package main\nimport(\"encoding/json\"\n\"fmt\")\n"
                   "func main() {\n\tvar d map[string]interface{}\n"
                   "\tjson.Unmarshal([]byte(`" + original_result + "`), &d)\n"
                   "\tb, _ := json.Marshal(d)\n\tfmt.Print(string(b))\n}";
        }
        // Other languages: echo the result
        return generateEchoCode(target_lang, original_result);
    }

    // ================================================================
    // FILE / CLI / WEB / CONCURRENCY / SYSTEMS — echo only
    // (Can't safely re-run side effects)
    // ================================================================
    return generateEchoCode(target_lang, original_result);
}

// --- Output Baselines ---

void GovernanceEngine::loadBaselines() {
    if (baselines_loaded_) return;
    baselines_loaded_ = true;

    // Resolve path relative to govern.json directory
    std::string path = rules_.baselines.path;
    if (!path.empty() && path[0] != '/') {
        auto gov_dir = std::filesystem::path(loaded_path_).parent_path();
        path = (gov_dir / path).string();
    }
    baselines_path_ = path;

    // Allocate JSON object on heap (opaque via void*)
    auto* data = new nlohmann::json();
    baselines_data_ = data;

    std::ifstream in(path);
    if (in.is_open()) {
        try {
            in >> *data;
        } catch (...) {
            // Corrupt file — start fresh
            *data = nlohmann::json::object();
        }
    }

    if (!data->contains("version")) (*data)["version"] = "1.0";
    if (!data->contains("entries")) (*data)["entries"] = nlohmann::json::object();
}

void GovernanceEngine::saveBaselines() {
    if (!baselines_data_) return;
    auto* data = static_cast<nlohmann::json*>(baselines_data_);

    auto parent = std::filesystem::path(baselines_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    std::ofstream out(baselines_path_);
    if (out.is_open()) {
        out << data->dump(2) << "\n";
    }
}

void GovernanceEngine::recordBaseline(const std::string& key,
                                       const std::string& output,
                                       const std::string& type) {
    loadBaselines();
    if (!baselines_data_) return;
    auto* data = static_cast<nlohmann::json*>(baselines_data_);

    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    nlohmann::json entry;
    entry["output"] = output;
    entry["type"] = type;
    entry["recorded_at"] = ts;
    entry["runs"] = 1;
    entry["last_seen"] = ts;

    (*data)["entries"][key] = entry;
    baselines_dirty_ = true;
}

std::string GovernanceEngine::checkBaseline(const std::string& key,
                                             const std::string& output,
                                             const std::string& type,
                                             int line) {
    if (!rules_.baselines.enabled) return "";

    loadBaselines();
    if (!baselines_data_) return "";
    auto* data = static_cast<nlohmann::json*>(baselines_data_);

    auto& entries = (*data)["entries"];
    if (!entries.contains(key)) {
        // No baseline exists
        if (rules_.baselines.auto_record) {
            recordBaseline(key, output, type);
        }
        return "";
    }

    auto& entry = entries[key];
    std::string expected = entry["output"].get<std::string>();
    std::string expected_type = entry.contains("type") ? entry["type"].get<std::string>() : "";

    // Compare using tolerance for numeric types
    bool matches = false;
    if ((type == "float" || type == "int") &&
        (expected_type == "float" || expected_type == "int")) {
        matches = compareResults(output, expected, rules_.baselines.tolerance);
    } else {
        matches = (output == expected);
    }

    if (matches) {
        // Update runs counter and last_seen
        int runs = entry.contains("runs") ? entry["runs"].get<int>() : 0;
        entry["runs"] = runs + 1;
        auto now = std::chrono::system_clock::now();
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        entry["last_seen"] = ts;
        baselines_dirty_ = true;
        return "";
    }

    // Mismatch
    return enforce("baselines", rules_.baselines.level,
        formatError(rules_.baselines.level,
            fmt::format("Baseline mismatch for '{}': expected '{}', got '{}'",
                key, expected, output),
            line > 0 ? fmt::format("line {}", line) : "",
            "baselines",
            "Output has changed from previously recorded baseline",
            "", ""));
}

// --- Drift Tracking ---

void GovernanceEngine::writeDriftEvent(
    const std::string& language, const std::string& task_type,
    const std::string& code_hash, const std::string& expected,
    const std::string& got, int line, int consensus, int total,
    const std::string& file) {

    auto& dtc = rules_.polyglot_optimization.verification.drift_tracking;
    if (!dtc.enabled) return;

    // Expand ~ in path
    std::string path = dtc.path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    // Ensure parent directory exists
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }

    // Build JSONL entry
    auto now = std::chrono::system_clock::now();
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    nlohmann::json entry;
    entry["ts"] = ts;
    entry["lang"] = language;
    entry["task"] = task_type;
    if (dtc.include_code_hash && !code_hash.empty()) entry["hash"] = code_hash;
    entry["expected"] = expected;
    entry["got"] = got;
    entry["line"] = line;
    entry["consensus"] = consensus;
    entry["total"] = total;
    if (!file.empty()) entry["file"] = file;

    // Append-only write (O(1) per event instead of O(n) read+write)
    {
        std::ofstream out(path, std::ios::app);
        if (out.is_open()) {
            out << entry.dump() << "\n";
        }
    }

    // Periodic ring buffer trim — only when writes exceed max_entries
    drift_write_count_++;
    if (dtc.max_entries > 0 && drift_write_count_ >= dtc.max_entries) {
        drift_write_count_ = 0;
        std::vector<std::string> lines;
        {
            std::ifstream in(path);
            if (in.is_open()) {
                std::string l;
                while (std::getline(in, l)) {
                    if (!l.empty()) lines.push_back(l);
                }
            }
        }
        if (static_cast<int>(lines.size()) > dtc.max_entries) {
            int excess = static_cast<int>(lines.size()) - dtc.max_entries;
            lines.erase(lines.begin(), lines.begin() + excess);
            std::ofstream out(path);
            if (out.is_open()) {
                for (const auto& l : lines) {
                    out << l << "\n";
                }
            }
        }
    }
}

void GovernanceEngine::analyzeDriftTrend(const std::string& language) {
    auto& dtc = rules_.polyglot_optimization.verification.drift_tracking;
    if (!dtc.enabled) return;

    // Expand ~ in path
    std::string path = dtc.path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    // Read JSONL and filter by language
    std::vector<nlohmann::json> events;
    {
        std::ifstream in(path);
        if (!in.is_open()) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line);
                if (j.contains("lang") && j["lang"].get<std::string>() == language) {
                    events.push_back(std::move(j));
                }
            } catch (...) {
                // Skip corrupt lines
            }
        }
    }

    // Look at last trend_window events
    int window = dtc.trend_window;
    int start = static_cast<int>(events.size()) > window
        ? static_cast<int>(events.size()) - window : 0;

    // Count events in the window — every logged event IS a drift event
    int events_in_window = 0;
    for (int i = start; i < static_cast<int>(events.size()); i++) {
        events_in_window++;
    }

    if (events_in_window == 0) return;

    // Rate = drift_events / window_size
    // e.g., 3 drift events in a window of 10 → 30% → triggers at threshold 0.3
    double drift_rate = static_cast<double>(events_in_window) / static_cast<double>(window);

    if (drift_rate >= dtc.escalation_threshold) {
        fprintf(stderr,
            "\n  [governance] DRIFT TREND WARNING: %s has %.0f%% drift rate "
            "(%d events in last %d window)\n"
            "    Threshold: %.0f%% — consider investigating %s block consistency\n\n",
            language.c_str(), drift_rate * 100.0,
            events_in_window, window,
            dtc.escalation_threshold * 100.0, language.c_str());
    }
}

std::string GovernanceEngine::verifyPolyglotResult(
    const std::string& language,
    const std::string& code,
    const std::string& result_str,
    int line)
{
    if (!isVerificationEnabled()) return "";

    auto& cfg = rules_.polyglot_optimization.verification;

    // 1. Classify the task
    std::string task_type = classifyTaskForVerification(code, language);

    // 2. Check if this task type should be verified
    if (!cfg.verify_task_types.empty()) {
        bool found = false;
        for (const auto& vt : cfg.verify_task_types) {
            if (task_type.find(vt) != std::string::npos || vt.find(task_type) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found) return "";
    }

    // 3. Filter consensus languages to installed only, skip original language
    auto& registry = runtime::LanguageRegistry::instance();
    std::vector<std::string> available_langs;
    std::string norm_lang = normalizeLanguage(language);
    for (const auto& lang : cfg.consensus_languages) {
        std::string norm = normalizeLanguage(lang);
        if (norm == norm_lang) continue;
        if (registry.getExecutor(norm) != nullptr) {
            available_langs.push_back(norm);
        }
    }

    if (available_langs.empty()) return "";

    // 4. Run verification in each language
    std::vector<VerificationResult> results;
    results.push_back({norm_lang, result_str, 0, true, ""});  // Original

    for (const auto& target_lang : available_langs) {
        VerificationResult vr;
        vr.language = target_lang;

        std::string verif_code = generateVerificationCode(
            task_type, code, result_str, norm_lang, target_lang);

        if (verif_code.empty()) {
            vr.success = false;
            vr.error = "No template available";
            results.push_back(vr);
            continue;
        }

        auto start_time = std::chrono::steady_clock::now();
        try {
            auto* verif_executor = registry.getExecutor(target_lang);
            if (!verif_executor) {
                vr.success = false;
                vr.error = "Executor not found";
                results.push_back(vr);
                continue;
            }

            auto verif_value = verif_executor->executeWithReturn(verif_code);
            vr.result = verif_value ? verif_value->toString() : "";
            vr.success = true;
            // Drain captured output to prevent leaking into subsequent real executions
            verif_executor->getCapturedOutput();
        } catch (const std::exception& e) {
            vr.success = false;
            vr.error = e.what();
        }

        auto end_time = std::chrono::steady_clock::now();
        vr.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        results.push_back(vr);
    }

    // 5. Compare all results against original
    int agree_count = 0;
    int total_count = 0;
    std::vector<std::string> drift_details;

    for (const auto& vr : results) {
        if (!vr.success) continue;
        total_count++;
        if (compareResults(result_str, vr.result, cfg.tolerance)) {
            agree_count++;
        } else {
            drift_details.push_back(fmt::format("{}={}", vr.language, vr.result));
        }
    }

    // 6. Format and output governance message
    bool consensus = (agree_count >= cfg.min_consensus) && drift_details.empty();

    if (consensus) {
        std::string lang_vals;
        for (const auto& vr : results) {
            if (!vr.success) continue;
            if (!lang_vals.empty()) lang_vals += "  ";
            lang_vals += fmt::format("{}={}", vr.language, vr.result);
        }
        fmt::print("\n  [governance] Verification: {} block (line {})\n", language, line);
        if (cfg.show_drift_details) {
            fmt::print("    \xe2\x9c\x93 {}  ({}/{} agree)\n\n", lang_vals, agree_count, total_count);
        } else {
            fmt::print("    \xe2\x9c\x93 {}/{} agree\n\n", agree_count, total_count);
        }
        return "";
    }

    // Drift detected
    std::string level_str = cfg.enforcement_level;
    std::string level_upper = level_str;
    std::transform(level_upper.begin(), level_upper.end(), level_upper.begin(), ::toupper);

    fmt::print("\n  [governance] Verification MISMATCH: {} block (line {})  [{}]\n",
        language, line, level_upper);

    if (cfg.show_drift_details) {
        for (const auto& vr : results) {
            if (!vr.success) {
                fmt::print("    {} = ERROR: {}\n", vr.language, vr.error);
                continue;
            }
            bool matches = compareResults(result_str, vr.result, cfg.tolerance);
            fmt::print("    {}{}={}\n", matches ? "\xe2\x9c\x93 " : "\xe2\x9c\x97 ", vr.language, vr.result);
        }

        if (isNumericString(result_str) && !drift_details.empty()) {
            for (const auto& vr : results) {
                if (vr.success && !compareResults(result_str, vr.result, cfg.tolerance) &&
                    isNumericString(vr.result)) {
                    double diff = std::abs(std::stod(result_str) - std::stod(vr.result));
                    fmt::print("    Drift: {:.2e} (tolerance: {:.2e})\n", diff, cfg.tolerance);
                    break;
                }
            }
        }
    }

    fmt::print("    Task: {} | Consensus: {}/{}\n\n", task_type, agree_count, total_count);

    // Drift tracking: write event and analyze trend
    {
        std::size_t hash_val = std::hash<std::string>{}(code);
        char hash_buf[16];
        snprintf(hash_buf, sizeof(hash_buf), "%06zx", hash_val & 0xFFFFFF);
        writeDriftEvent(language, task_type, hash_buf, result_str,
            drift_details.empty() ? "" : drift_details[0],
            line, agree_count, total_count, "");
        analyzeDriftTrend(language);
    }

    // Audit logging for soft/hard enforcement
    if (level_str == "soft" || level_str == "hard") {
        fprintf(stderr, "[governance] AUDIT DRIFT: %s block at line %d — %d/%d consensus (%s)\n",
            language.c_str(), line, agree_count, total_count, task_type.c_str());
    }

    // Hard enforcement: block execution
    if (level_str == "hard") {
        std::string details;
        for (const auto& vr : results) {
            if (vr.success) details += fmt::format("  {}={}\n", vr.language, vr.result);
        }
        return fmt::format(
            "Verification HARD violation: Cross-language drift detected at line {}\n"
            "  Task: {}\n"
            "  Results:\n{}"
            "  Consensus: {}/{} (minimum: {})\n\n"
            "  Use --governance-override to bypass, or adjust verification.tolerance in govern.json",
            line, task_type, details, agree_count, total_count, cfg.min_consensus);
    }

    return "";
}

} // namespace governance
} // namespace naab
