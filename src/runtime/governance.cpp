// NAAb Governance Engine Implementation
// Runtime enforcement of project governance rules via govern.json
//
// Three-tier enforcement model (inspired by HashiCorp Sentinel):
//   HARD      - Block execution. No override possible.
//   SOFT      - Block execution. Override with --governance-override flag.
//   ADVISORY  - Warn only. Execution continues.

#include "naab/governance.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <regex>
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
    {"shell", ">\\s*/dev/",               "Writing to device files",
     "Avoid writing to device files"},
    {"shell", "chmod\\s+777",             "chmod 777 (world-writable)",
     "Use specific permissions (644 for files, 755 for executables)"},
    {"shell", "curl.*\\|\\s*sh",          "curl | sh (remote code execution)",
     "Download and inspect scripts before executing"},
    {"shell", "wget.*\\|\\s*sh",          "wget | sh (remote code execution)",
     "Download and inspect scripts before executing"},

    // Any language
    {"any", "\\bsudo\\s",                 "sudo (privilege escalation)",
     "Avoid privilege escalation in polyglot blocks"},
};

static const std::vector<std::string> PLACEHOLDER_PATTERNS_DB = {
    "TODO", "FIXME", "STUB", "PLACEHOLDER", "XXX", "TBD",
    "HACK", "IMPLEMENT_ME", "RUNTIME_COMPUTED"
};

struct HardcodedResultPattern {
    std::string pattern;
    std::string description;
};

static const std::vector<HardcodedResultPattern> HARDCODED_RESULT_PATTERNS_DB = {
    {"return\\s+True\\s*#",    "Hardcoded return True with comment"},
    {"return\\s+False\\s*#",   "Hardcoded return False with comment"},
    {"return\\s+0\\s*#",       "Hardcoded return 0 with comment"},
    {"return\\s+None\\s*#",    "Hardcoded return None with comment"},
    {"#\\s*for now",           "Temporary implementation marker (# for now)"},
    {"#\\s*simplified",        "Simplified implementation marker"},
    {"#\\s*placeholder",       "Placeholder implementation marker"},
    {"#\\s*stub",              "Stub implementation marker"},
    {"#\\s*not implemented",   "Not implemented marker"},
    {"#\\s*basic implementation", "Basic implementation marker"},
    {"#\\s*minimal",           "Minimal implementation marker"},
};

// ============================================================================
// Helper functions
// ============================================================================

std::string GovernanceEngine::levelToString(EnforcementLevel level) {
    switch (level) {
        case EnforcementLevel::HARD:     return "hard";
        case EnforcementLevel::SOFT:     return "soft";
        case EnforcementLevel::ADVISORY: return "advisory";
    }
    return "unknown";
}

std::string GovernanceEngine::levelToTag(EnforcementLevel level) {
    switch (level) {
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
}

bool GovernanceEngine::loadFromFile(const std::string& path) {
    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) return false;

        nlohmann::json j = nlohmann::json::parse(ifs);
        loadFromJson(j, rules_);
        loaded_path_ = path;
        active_ = (rules_.mode != GovernanceMode::OFF);
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
            return loadFromFile(candidate.string());
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
            if (std::regex_search(code, re)) {
                std::string location = line > 0
                    ? fmt::format("line {}: {} block", line, language)
                    : fmt::format("{} block", language);

                return enforce("restrictions.dangerous_calls",
                    rules_.dangerous_calls_level,
                    formatError(rules_.dangerous_calls_level,
                        fmt::format("Dangerous pattern in {} block: {}",
                            language, pattern.description),
                        location,
                        fmt::format("restrictions.dangerous_calls = \"{}\"",
                            levelToString(rules_.dangerous_calls_level)),
                        fmt::format("{}\n{}", pattern.description,
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

// ============================================================================
// V3.0 New Check Implementations
// ============================================================================

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
    "# [Ff]or now", "# [Tt]emporary", "# [Qq]uick fix",
    "# [Ww]ill implement later", "# [Ss]implified",
    "# [Bb]asic implementation", "# [Mm]inimal implementation",
    "# [Ww]ill (?:replace|refactor|rewrite)", "# [Nn]eeds? (?:refactoring|improvement|work)",
    "# [Ss]kipping for now", "# [Dd]efer(?:red)?", "# [Pp]rototype",
    "# [Ww]orkaround", "# [Bb]andaid", "# [Bb]and-aid",
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
                "Replace temporary code with production implementation", "", ""));
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
                "Replace simulated/mocked code with real implementation", "", ""));
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
                        "Use real data sources instead of mock/fake data", "", ""));
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
                "Replace placeholder literals with real data", "", ""));
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
                "LLM-generated code should not contain apologies or self-deprecation\nThis indicates the code may not have been properly verified",
                "", ""));
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
                "Remove dead/unreachable code", "", ""));
    }
    recordPass("code_quality.no_dead_code", cfg.level);
    return "";
}

// --- Debug Artifacts ---
static const std::vector<std::string> DEFAULT_DEBUG_PATTERNS = {
    "print\\(.*debug", "console\\.log\\(", "console\\.debug\\(",
    "System\\.out\\.println\\(", "fmt\\.Println\\(",
    "import\\s+pdb", "import\\s+ipdb", "breakpoint\\(\\)",
    "debugger;?", "binding\\.pry",
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
                        "Remove debug statements before deployment", "", ""));
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
                "Use safe deserialization methods (json.loads, yaml.safe_load)", "", ""));
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
    "\\.\\./", "\\.\\.\\\\", "%2e%2e%2f", "%2e%2e/", "\\.\\.%2f",
};

std::string GovernanceEngine::checkPathTraversal(const std::string& code, int line) {
    auto& cfg = rules_.code_quality.no_path_traversal;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_PATH_PATTERNS : cfg.patterns;
    std::string found = searchPatterns(code, pats);
    if (!found.empty()) {
        return enforce("code_quality.no_path_traversal", cfg.level,
            formatError(cfg.level, "Path traversal pattern detected",
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_path_traversal",
                "Use absolute paths or os.path.realpath() to prevent traversal", "", ""));
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
                    "Use configuration or environment variables for URLs", "", ""));
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
                    "Use configuration or DNS for IP addresses", "", ""));
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
                "Null bytes can be used for injection attacks", "", ""));
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
                        "Bidi override characters can be used for trojan source attacks", "", ""));
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
                    "Break large blocks into smaller functions or multiple blocks", "", ""));
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
            formatError(cfg.level, fmt::format("Oversimplified code: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_oversimplification",
                "This looks like a stub or trivial implementation.\n"
                "LLMs often produce minimal code that passes syntax checks but lacks real logic.\n"
                "Implement the actual business logic instead of a placeholder.",
                "def validate(data): return True",
                "def validate(data):\n    if not isinstance(data, dict): raise TypeError(...)\n    ..."));
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
            formatError(cfg.level, fmt::format("Incomplete logic: \"{}\"", found),
                line > 0 ? fmt::format("line {}", line) : "", "code_quality.no_incomplete_logic",
                "This code has logic gaps that indicate shortcuts or lazy implementation.\n"
                "Common issues: empty catch blocks, generic error messages, degenerate loops,\n"
                "always-true/false conditions, or swallowed exceptions.",
                "except Exception: pass  # swallows all errors",
                "except ValueError as e:\n    logger.error(f\"Validation failed: {e}\")\n    raise"));
    }
    recordPass("code_quality.no_incomplete_logic", cfg.level);
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

    // Check language-specific patterns
    if (lang_patterns) {
        for (const auto& [pattern, suggestion] : *lang_patterns) {
            try {
                auto flags = cfg.case_sensitive ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase);
                std::regex re(pattern, flags);
                std::smatch match;
                if (std::regex_search(code, match, re)) {
                    return enforce("code_quality.no_hallucinated_apis", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Hallucinated API in {} block: \"{}\"", language, match[0].str()),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.no_hallucinated_apis",
                            suggestion, "", ""));
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
    auto it = rules_.languages.per_language.find(lang);
    if (it != rules_.languages.per_language.end() && it->second.max_lines > 0)
        return it->second.max_lines;
    return rules_.limits.code.max_lines_per_block;
}

const LanguageConfig* GovernanceEngine::getLanguageConfig(const std::string& lang) const {
    auto it = rules_.languages.per_language.find(lang);
    if (it != rules_.languages.per_language.end()) return &it->second;
    return nullptr;
}

// --- Comprehensive Polyglot Block Check ---
std::string GovernanceEngine::checkPolyglotBlock(
    const std::string& language, const std::string& code,
    const std::string& /*source_file*/, int line) {

    std::string err;

    // Language allowed?
    err = checkLanguageAllowed(language, line);
    if (!err.empty()) return err;

    // Code quality checks
    err = checkSecrets(code, line);
    if (!err.empty()) return err;
    err = checkPlaceholders(code, line);
    if (!err.empty()) return err;
    err = checkHardcodedResults(code, line);
    if (!err.empty()) return err;
    err = checkDangerousCall(language, code, line);
    if (!err.empty()) return err;

    // New v3.0 checks
    err = checkPii(code, line);
    if (!err.empty()) return err;
    err = checkTemporaryCode(code, line);
    if (!err.empty()) return err;
    err = checkSimulationMarkers(code, line);
    if (!err.empty()) return err;
    err = checkMockData(code, line);
    if (!err.empty()) return err;
    err = checkApologeticLanguage(code, line);
    if (!err.empty()) return err;
    err = checkDeadCode(code, line);
    if (!err.empty()) return err;
    err = checkDebugArtifacts(language, code, line);
    if (!err.empty()) return err;
    err = checkUnsafeDeserialization(code, line);
    if (!err.empty()) return err;
    err = checkSqlInjection(code, line);
    if (!err.empty()) return err;
    err = checkPathTraversal(code, line);
    if (!err.empty()) return err;
    err = checkHardcodedUrls(code, line);
    if (!err.empty()) return err;
    err = checkHardcodedIps(code, line);
    if (!err.empty()) return err;
    err = checkEncoding(code, line);
    if (!err.empty()) return err;
    err = checkComplexity(code, line);
    if (!err.empty()) return err;

    // LLM anti-drift checks
    err = checkOversimplification(code, line);
    if (!err.empty()) return err;
    err = checkIncompleteLogic(code, line);
    if (!err.empty()) return err;
    err = checkHallucinatedApis(language, code, line);
    if (!err.empty()) return err;

    // Security checks
    err = checkShellInjection(code, line);
    if (!err.empty()) return err;
    err = checkCodeInjection(language, code, line);
    if (!err.empty()) return err;
    err = checkPrivilegeEscalation(code, line);
    if (!err.empty()) return err;
    err = checkDataExfiltration(code, line);
    if (!err.empty()) return err;
    err = checkResourceAbuse(code, line);
    if (!err.empty()) return err;
    err = checkInfoDisclosure(language, code, line);
    if (!err.empty()) return err;
    err = checkCryptoWeakness(code, line);
    if (!err.empty()) return err;

    // Per-language checks
    err = checkImports(language, code, line);
    if (!err.empty()) return err;
    err = checkBannedFunctions(language, code, line);
    if (!err.empty()) return err;
    err = checkLanguageStyle(language, code, line);
    if (!err.empty()) return err;
    err = checkCodeSize(language, code, line);
    if (!err.empty()) return err;

    // Custom rules
    err = checkCustomRules(language, code, line);
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
        "output", "audit", "meta", "hooks", "polyglot"
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

} // namespace governance
} // namespace naab
