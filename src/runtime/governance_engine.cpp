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
#ifndef _WIN32
#  include <sys/file.h>
#endif
#include <fmt/core.h>

namespace naab {

namespace governance {
    // Feature 1: Process-level flag for exit code determination
    bool g_governance_hard_block = false;
}

// Extern: thread_local interpreter pointer set in interpreter.cpp
namespace interpreter {
    extern thread_local Interpreter* g_current_interpreter;
}

// Feature 3: CWE/OWASP mapping for standard compliance reporting
static const std::unordered_map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>>
    g_cwe_owasp_map = {
    // rule_name_suffix -> {cwe_ids, owasp_ids}
    {"no_sql_injection",        {{"CWE-89"},  {"A03:2021"}}},
    {"sql_string_concat",       {{"CWE-89"},  {"A03:2021"}}},
    {"no_path_traversal",       {{"CWE-22"},  {"A01:2021"}}},
    {"path_traversal",          {{"CWE-22"},  {"A01:2021"}}},
    {"shell_injection",         {{"CWE-78"},  {"A03:2021"}}},
    {"code_injection",          {{"CWE-94"},  {"A03:2021"}}},
    {"no_unsafe_deserialization",{{"CWE-502"}, {"A08:2021"}}},
    {"insecure_deserialization", {{"CWE-502"}, {"A08:2021"}}},
    {"no_secrets",              {{"CWE-798"}, {"A07:2021"}}},
    {"hardcoded_credentials",   {{"CWE-798"}, {"A07:2021"}}},
    {"privilege_escalation",    {{"CWE-269"}, {"A04:2021"}}},
    {"data_exfiltration",       {{"CWE-200"}, {"A01:2021"}}},
    {"information_disclosure",  {{"CWE-200"}, {"A01:2021"}}},
    {"crypto",                  {{"CWE-327"}, {"A02:2021"}}},
    {"weak_crypto",             {{"CWE-327"}, {"A02:2021"}}},
    {"no_pii",                  {{"CWE-359"}, {}}},
    {"resource_abuse",          {{"CWE-400"}, {}}},
    {"encoding",                {{"CWE-116"}, {}}},
    {"insecure_random",         {{"CWE-330"}, {"A02:2021"}}},
    {"prototype_pollution",     {{"CWE-1321"},{}}},
    {"eval_usage",              {{"CWE-95"},  {"A03:2021"}}},
    {"dangerous_calls",         {{"CWE-676"}, {}}},
    {"no_hardcoded_urls",       {{"CWE-798"}, {}}},
    {"no_hardcoded_ips",        {{"CWE-798"}, {}}},
};

std::pair<std::vector<std::string>, std::vector<std::string>>
lookupCweOwasp(const std::string& rule_name) {
    auto it = g_cwe_owasp_map.find(rule_name);
    if (it != g_cwe_owasp_map.end()) return it->second;
    // Try suffix match (e.g., "code_quality.no_sql_injection" matches "no_sql_injection")
    auto dot = rule_name.rfind('.');
    if (dot != std::string::npos) {
        auto suffix = rule_name.substr(dot + 1);
        it = g_cwe_owasp_map.find(suffix);
        if (it != g_cwe_owasp_map.end()) return it->second;
    }
    return {{}, {}};
}

namespace governance {

// ============================================================================
// Pattern Databases (used by legacy check methods below)
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

// Filesystem operation patterns (for capabilities.filesystem enforcement in polyglot blocks)
static const std::vector<DangerousPattern> FILESYSTEM_IMPORT_PATTERNS = {
    // Python — builtin file I/O
    {"python", "\\bopen\\s*\\(",
     "open() call (direct filesystem access)",
     "Use NAAb stdlib file.read()/file.write() instead"},
    {"python", "\\bio\\.open\\s*\\(",
     "io.open() call (direct filesystem access)",
     "Use NAAb stdlib file.read()/file.write() instead"},
    // Python — pathlib
    {"python", "\\bpathlib\\b",
     "pathlib import/usage (filesystem access)",
     "Use NAAb stdlib file module instead of pathlib"},
    {"python", "\\bPath\\s*\\(",
     "pathlib.Path() constructor (filesystem access)",
     "Use NAAb stdlib file module instead of pathlib"},
    // Python — os filesystem operations
    {"python", "\\bos\\.listdir\\s*\\(",
     "os.listdir() (filesystem enumeration)",
     "Use NAAb stdlib file.list() instead"},
    {"python", "\\bos\\.walk\\s*\\(",
     "os.walk() (recursive filesystem traversal)",
     "Use NAAb stdlib file.list() instead"},
    {"python", "\\bos\\.remove\\s*\\(",
     "os.remove() (file deletion)",
     "Use NAAb stdlib file.delete() instead"},
    {"python", "\\bos\\.unlink\\s*\\(",
     "os.unlink() (file deletion)",
     "Use NAAb stdlib file.delete() instead"},
    {"python", "\\bos\\.rename\\s*\\(",
     "os.rename() (file move/rename)",
     "Avoid direct filesystem mutations in polyglot blocks"},
    {"python", "\\bos\\.makedirs?\\s*\\(",
     "os.makedirs() (directory creation)",
     "Use NAAb stdlib file.create_dir() instead"},
    {"python", "\\bos\\.path\\b",
     "os.path (filesystem path operations)",
     "Use NAAb stdlib path helpers instead"},
    // Python — shutil
    {"python", "\\bshutil\\.",
     "shutil (high-level filesystem operations)",
     "Use NAAb stdlib file module for safe file operations"},
    // Python — glob
    {"python", "\\bglob\\.(?:i)?glob\\s*\\(",
     "glob.glob() (filesystem pattern matching)",
     "Use NAAb stdlib file.list() instead"},
    // JavaScript — Node.js fs
    {"javascript",
     "\\bfs\\.(?:read|write|open|unlink|rmdir|mkdir|readdir|exists|stat|copyFile|rename)\\b",
     "Node.js fs module call (direct filesystem access)",
     "Use NAAb stdlib file module instead of fs"},
    {"javascript", "require\\s*\\(\\s*['\"]fs['\"]",
     "require('fs') (filesystem module import)",
     "Use NAAb stdlib file module instead of Node.js fs"},
    // Shell — recursive/forced deletion
    {"shell", "\\brm\\s+-[rRf]",
     "rm -r/-f (recursive/forced file deletion)",
     "Use NAAb stdlib file.delete() for single-file deletion"},
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

// --- Core Engine Implementation (extracted from governance.cpp) ---

bool GovernanceEngine::discoverAndLoad(const std::string& start_dir) {
    namespace fs = std::filesystem;

    last_error_.clear();
    fs::path dir(start_dir);
    while (true) {
        fs::path candidate = dir / "govern.json";
        if (fs::exists(candidate)) {
            govern_json_dir_ = dir.string();
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
    last_error_ = "not_found";
    return false;
}

// ============================================================================
// Core Enforcement Logic
// ============================================================================

void GovernanceEngine::setCheckContext(const std::string& file, int line) {
    current_check_file_ = file;
    current_check_line_ = line;
}

void GovernanceEngine::recordPass(const std::string& rule_name,
                                   EnforcementLevel level) {
    std::string cat = rule_name.substr(0, rule_name.find('.'));
    auto [cwes, owasps] = lookupCweOwasp(rule_name);
    // V-CONC-007: mutex-guard concurrent access from async threads
    std::lock_guard<std::mutex> lock(results_mutex_);
    check_results_.push_back({rule_name, level, true, "", cat, "",
                              current_check_line_, current_check_file_, cwes, owasps});
    // V-GOV-024: cap telemetry to prevent unbounded memory growth
    if (check_results_.size() > MAX_CHECK_RESULTS) {
        check_results_.erase(check_results_.begin());
    }
}

std::string GovernanceEngine::enforce(
    const std::string& rule_name,
    EnforcementLevel level,
    const std::string& violation_message) {

    // Record the failing check with full context
    std::string cat = rule_name.substr(0, rule_name.find('.'));
    std::string sev = (level == EnforcementLevel::HARD) ? "critical" :
                      (level == EnforcementLevel::SOFT) ? "high" : "medium";
    auto [cwes, owasps] = lookupCweOwasp(rule_name);
    {
        // V-CONC-007: mutex-guard concurrent access from async threads
        std::lock_guard<std::mutex> lock(results_mutex_);
        check_results_.push_back({rule_name, level, false, violation_message, cat, sev,
                                  current_check_line_, current_check_file_, cwes, owasps});
        // V-GOV-024: cap telemetry
        if (check_results_.size() > MAX_CHECK_RESULTS) {
            check_results_.erase(check_results_.begin());
        }
    }

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
            g_governance_hard_block = true;
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

    // V-GOV-020: Per-agent language enforcement (defense-in-depth beyond applyAgentRole)
    for (const auto& role : rules_.agent_roles) {
        if (role.name == agent_id_) {
            // Check per-agent blocked languages
            for (const auto& bl : role.blocked_languages) {
                if (bl == language) {
                    return enforce("agent_role.language", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            fmt::format("Agent '{}' is blocked from using language \"{}\"",
                                agent_id_, language),
                            "",
                            fmt::format("agent_roles.{}.blocked_languages contains \"{}\"",
                                agent_id_, language),
                            "Your agent role does not permit this language",
                            fmt::format("let result = <<{}\n...\n>>", language),
                            "Request access from the project governance config"));
                }
            }
            // Check per-agent allowed languages (if non-empty, must be in list)
            if (!role.allowed_languages.empty()) {
                bool found = false;
                for (const auto& al : role.allowed_languages) {
                    if (al == language) { found = true; break; }
                }
                if (!found) {
                    std::string al_list;
                    for (const auto& al : role.allowed_languages) {
                        if (!al_list.empty()) al_list += ", ";
                        al_list += al;
                    }
                    return enforce("agent_role.language", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            fmt::format("Agent '{}' is not allowed to use language \"{}\"",
                                agent_id_, language),
                            "",
                            fmt::format("agent_roles.{}.allowed_languages = [{}]",
                                agent_id_, al_list),
                            fmt::format("Your agent role only permits: {}", al_list),
                            fmt::format("let result = <<{}\n...\n>>", language),
                            fmt::format("let result = <<{}\n...\n>>",
                                role.allowed_languages.front())));
                }
            }
            break;
        }
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

std::string GovernanceEngine::checkFilesystemImports(
    const std::string& language, const std::string& code, int line) {
    // Only enforce when filesystem access is restricted (not the default "write" mode)
    if (rules_.filesystem_mode == "write" || rules_.filesystem_mode.empty()) {
        recordPass("capabilities.filesystem", EnforcementLevel::HARD);
        return "";
    }
    for (const auto& pat : FILESYSTEM_IMPORT_PATTERNS) {
        if (pat.language != language && pat.language != "any") continue;
        try {
            std::regex re(pat.pattern, std::regex::icase);
            if (std::regex_search(code, re)) {
                return enforce("capabilities.filesystem", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        fmt::format("Filesystem access blocked: {} in {} block",
                                    pat.description, language),
                        fmt::format("line {}", line),
                        fmt::format("capabilities.filesystem = \"{}\"",
                                    rules_.filesystem_mode),
                        "Filesystem operations are restricted by governance policy.\n"
                        "Use NAAb stdlib file module for controlled file access.",
                        "", pat.safe_alternative));
            }
        } catch (const std::regex_error&) {
            // Invalid pattern — skip
        }
    }
    recordPass("capabilities.filesystem", EnforcementLevel::HARD);
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

std::string GovernanceEngine::checkPathAccess(const std::string& filepath, const std::string& mode) {
    // Canonicalize path for consistent prefix matching
    std::string canon;
    try {
        canon = std::filesystem::weakly_canonical(filepath).string();
    } catch (...) {
        canon = filepath;
    }

    // Normalize path separators: replace backslashes with forward slashes so that
    // govern.json paths (which may use either separator) compare correctly on Windows.
    auto normSep = [](std::string p) {
        std::replace(p.begin(), p.end(), '\\', '/');
        return p;
    };
    const std::string canon_n = normSep(canon);

    // V-GOV-022: Path prefix match with directory boundary validation.
    // Ensures /data/safe doesn't match /data/safe_malicious.
    auto pathPrefixMatch = [](const std::string& path, const std::string& prefix) -> bool {
        if (path.find(prefix) != 0) return false;
        // Exact match or next char is '/' (directory boundary)
        return path.size() == prefix.size() ||
               prefix.back() == '/' ||
               path[prefix.size()] == '/';
    };

    // Layer 1: Base filesystem blocked_paths (deny wins)
    for (const auto& bp : rules_.capabilities.filesystem.blocked_paths) {
        if (pathPrefixMatch(canon_n, normSep(bp))) {
            return enforce("capabilities.filesystem.path", EnforcementLevel::HARD,
                formatError(EnforcementLevel::HARD,
                    "File path blocked by governance: " + filepath,
                    "",
                    "capabilities.filesystem.blocked_paths contains \"" + bp + "\"",
                    "This path is blocked by the project's governance configuration",
                    "file." + mode + "(\"" + filepath + "\", ...)",
                    "Use an allowed path instead"));
        }
    }

    // Layer 2: Base filesystem allowed_paths (if non-empty, must match one)
    if (!rules_.capabilities.filesystem.allowed_paths.empty()) {
        bool base_allowed = false;
        for (const auto& ap : rules_.capabilities.filesystem.allowed_paths) {
            if (pathPrefixMatch(canon_n, normSep(ap))) {
                base_allowed = true;
                break;
            }
        }
        if (!base_allowed) {
            return enforce("capabilities.filesystem.path", EnforcementLevel::HARD,
                formatError(EnforcementLevel::HARD,
                    "File path not in allowed paths: " + filepath,
                    "",
                    "capabilities.filesystem.allowed_paths does not match",
                    "Only paths matching the allowed list are accessible",
                    "file." + mode + "(\"" + filepath + "\", ...)",
                    "Use a path within the allowed directories"));
        }
    }

    // Layer 3+4: Agent role path restrictions
    for (const auto& role : rules_.agent_roles) {
        if (role.name == agent_id_) {
            // Agent blocked_paths
            for (const auto& bp : role.blocked_paths) {
                if (pathPrefixMatch(canon_n, normSep(bp))) {
                    return enforce("agent_role.path", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            "Agent '" + agent_id_ + "' blocked from path: " + filepath,
                            "",
                            "agent_roles." + agent_id_ + ".blocked_paths contains \"" + bp + "\"",
                            "Your agent role does not permit access to this path",
                            "file." + mode + "(\"" + filepath + "\", ...)",
                            "Request access from the project governance config"));
                }
            }
            // Agent allowed_paths (if non-empty, must match one)
            if (!role.allowed_paths.empty()) {
                bool agent_allowed = false;
                for (const auto& ap : role.allowed_paths) {
                    if (pathPrefixMatch(canon_n, normSep(ap))) {
                        agent_allowed = true;
                        break;
                    }
                }
                if (!agent_allowed) {
                    return enforce("agent_role.path", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            "Agent '" + agent_id_ + "' not allowed to access: " + filepath,
                            "",
                            "agent_roles." + agent_id_ + ".allowed_paths",
                            "Your agent role restricts file access to specific paths",
                            "file." + mode + "(\"" + filepath + "\", ...)",
                            "Use a path within your agent's allowed directories"));
                }
            }
            break;
        }
    }

    recordPass("capabilities.filesystem.path", EnforcementLevel::HARD);
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
    // V-GOV-020: Per-agent shell enforcement (defense-in-depth beyond applyAgentRole)
    for (const auto& role : rules_.agent_roles) {
        if (role.name == agent_id_) {
            if (role.shell_allowed_set && !role.shell_allowed) {
                return enforce("agent_role.shell", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        "Agent '" + agent_id_ + "' is not allowed to execute shell blocks",
                        "",
                        "agent_roles." + agent_id_ + ".shell_allowed = false",
                        "Your agent role does not permit shell execution",
                        "let result = <<shell\nls -la\n>>",
                        "Use NAAb stdlib or request shell access from governance config"));
            }
            break;
        }
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
        size > static_cast<size_t>(rules_.max_array_size)) {
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
// Advisory Output Control
// ============================================================================

void GovernanceEngine::emitAdvisory(const std::string& msg) {
    int max = rules_.output.max_advisories;
    if (max > 0 && advisory_count_ >= max) {
        advisory_suppressed_++;
        return;
    }
    advisory_count_++;
    fmt::print(stderr, "{}\n", msg);
}

void GovernanceEngine::flushGroupedAdvisories() {
    // 1. Grouped duplicate call warnings
    if (!dup_call_summary_.empty()) {
        std::string msg = "[ADVISORY] Duplicate calls (store results in variables):";
        int shown = 0;
        for (const auto& [call, entries] : dup_call_summary_) {
            if (shown >= rules_.code_quality.duplicate_calls.max_entries) {
                msg += fmt::format("\n  ... and {} more unique calls",
                    static_cast<int>(dup_call_summary_.size()) - shown);
                break;
            }
            // Build compact location list
            std::string locs;
            for (size_t i = 0; i < entries.size() && i < 3; i++) {
                if (i > 0) locs += ", ";
                locs += fmt::format("{}:{}", entries[i].function_name, entries[i].line);
            }
            if (entries.size() > 3) {
                locs += fmt::format(", +{} more", entries.size() - 3);
            }
            msg += fmt::format("\n  {}  — {}x in: {}", call, entries[0].count, locs);
            shown++;
        }
        emitAdvisory(msg);
    }

    // 2. Grouped polyglot try/catch warnings
    if (!ptc_functions_.empty()) {
        std::string msg = "[ADVISORY] Polyglot blocks without try/catch:";
        int shown = 0;
        for (const auto& [name, line] : ptc_functions_) {
            if (shown >= rules_.code_quality.polyglot_try_catch.max_entries) {
                msg += fmt::format(", +{} more",
                    static_cast<int>(ptc_functions_.size()) - shown);
                break;
            }
            msg += (shown == 0 ? " " : ", ") + fmt::format("{}:{}", name, line);
            shown++;
        }
        msg += "\n  Wrap polyglot blocks in try/catch for graceful error handling.";
        emitAdvisory(msg);
    }

    // 3. Suppression summary
    if (advisory_suppressed_ > 0 && rules_.output.advisory_summary) {
        fmt::print(stderr, "[ADVISORY] ... and {} more advisories suppressed "
                   "(increase output.max_advisories to see all)\n", advisory_suppressed_);
    }

    // Reset
    dup_call_summary_.clear();
    ptc_functions_.clear();
    advisory_count_ = 0;
    advisory_suppressed_ = 0;
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
// Dashboard Summary (--governance-dashboard)
// ============================================================================

void GovernanceEngine::printDashboard() const {
    if (check_results_.empty()) return;

    int passed = 0, blocked = 0;
    std::map<std::string, int> block_counts;
    for (const auto& r : check_results_) {
        if (r.passed) {
            passed++;
        } else {
            blocked++;
            block_counts[r.rule_name]++;
        }
    }

    // Find top violation
    std::string top_rule;
    int top_count = 0;
    for (const auto& [rule, count] : block_counts) {
        if (count > top_count) {
            top_count = count;
            top_rule = rule;
        }
    }

    fprintf(stderr, "\n─── Agent Governance Summary ───\n");
    fprintf(stderr, "Agent:      %s\n", agent_id_.c_str());
    fprintf(stderr, "Checks:     %d passed, %d blocked\n", passed, blocked);
    if (!top_rule.empty())
        fprintf(stderr, "Top block:  %s (%d violation%s)\n",
                top_rule.c_str(), top_count, top_count != 1 ? "s" : "");
    if (rules_.telemetry_output.enabled)
        fprintf(stderr, "Telemetry:  %zu events → %s\n",
                check_results_.size(), rules_.telemetry_output.output_file.c_str());
    fprintf(stderr, "────────────────────────────────\n");
}

// ============================================================================
// Feature 1: wasBlocked()
// ============================================================================

bool GovernanceEngine::wasBlocked() const {
    for (const auto& r : check_results_) {
        if (!r.passed && r.level == EnforcementLevel::HARD) return true;
    }
    return false;
}

// ============================================================================
// Feature 2: Quality Gate Evaluation
// ============================================================================

std::string GovernanceEngine::evaluateQualityGate() const {
    if (!rules_.quality_gate.enabled) return "";

    int hard = 0, soft = 0, advisory = 0, security = 0, total_violations = 0;
    for (const auto& r : check_results_) {
        if (r.passed) continue;
        total_violations++;
        if (r.level == EnforcementLevel::HARD) hard++;
        else if (r.level == EnforcementLevel::SOFT) soft++;
        else advisory++;
        if (r.severity == "critical" || r.severity == "high") security++;
    }

    for (const auto& cond : rules_.quality_gate.conditions) {
        int value = 0;
        if (cond.metric == "hard_violations") value = hard;
        else if (cond.metric == "soft_violations") value = soft;
        else if (cond.metric == "advisory_violations") value = advisory;
        else if (cond.metric == "security_findings") value = security;
        else if (cond.metric == "total_violations") value = total_violations;
        else if (cond.metric == "total_checks") value = static_cast<int>(check_results_.size());
        else continue;

        bool failed = false;
        if (cond.op == ">" && value > cond.threshold) failed = true;
        else if (cond.op == ">=" && value >= cond.threshold) failed = true;
        else if (cond.op == "<" && value < cond.threshold) failed = true;
        else if (cond.op == "<=" && value <= cond.threshold) failed = true;
        else if (cond.op == "==" && value == cond.threshold) failed = true;

        if (failed) {
            return fmt::format(
                "[governance] Quality gate FAILED: {} {} {} (actual: {})\n",
                cond.metric, cond.op, cond.threshold, value);
        }
    }
    return "";
}

// ============================================================================
// Feature 4: Governance Baseline
// ============================================================================

void GovernanceEngine::saveGovernanceBaseline() const {
    if (rules_.governance_baseline.path.empty()) return;

    int hard = 0, soft = 0, advisory = 0, security = 0;
    for (const auto& r : check_results_) {
        if (r.passed) continue;
        if (r.level == EnforcementLevel::HARD) hard++;
        else if (r.level == EnforcementLevel::SOFT) soft++;
        else advisory++;
        if (r.severity == "critical" || r.severity == "high") security++;
    }

    std::filesystem::path p(rules_.governance_baseline.path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    nlohmann::json baseline;
    baseline["version"] = 1;
    baseline["timestamp"] = std::string(ts);
    baseline["hard_violations"] = hard;
    baseline["soft_violations"] = soft;
    baseline["advisory_violations"] = advisory;
    baseline["security_findings"] = security;
    baseline["total_checks"] = static_cast<int>(check_results_.size());

    std::ofstream ofs(rules_.governance_baseline.path);
    if (ofs.is_open()) {
        ofs << baseline.dump(2) << "\n";
        fprintf(stderr, "[governance] Baseline saved to %s\n",
                rules_.governance_baseline.path.c_str());
    }
}

std::string GovernanceEngine::checkGovernanceBaseline() const {
    if (!rules_.governance_baseline.enabled) return "";

    std::ifstream ifs(rules_.governance_baseline.path);
    if (!ifs.is_open()) return "";  // No baseline yet — skip

    nlohmann::json prev;
    try { prev = nlohmann::json::parse(ifs); }
    catch (...) { return ""; }

    int hard = 0, soft = 0, advisory = 0, security = 0;
    for (const auto& r : check_results_) {
        if (r.passed) continue;
        if (r.level == EnforcementLevel::HARD) hard++;
        else if (r.level == EnforcementLevel::SOFT) soft++;
        else advisory++;
        if (r.severity == "critical" || r.severity == "high") security++;
    }

    std::vector<std::string> regressions;
    auto check = [&](const char* name, int current, const char* key) {
        if (prev.contains(key) && current > prev[key].get<int>()) {
            regressions.push_back(fmt::format(
                "  {} increased: {} -> {} (+{})",
                name, prev[key].get<int>(), current, current - prev[key].get<int>()));
        }
    };

    check("hard_violations", hard, "hard_violations");
    check("soft_violations", soft, "soft_violations");
    check("advisory_violations", advisory, "advisory_violations");
    check("security_findings", security, "security_findings");

    if (regressions.empty()) return "";

    std::string msg = "[governance] Baseline REGRESSION detected:\n";
    for (const auto& r : regressions) msg += r + "\n";
    msg += fmt::format("  Baseline from: {}\n",
        prev.contains("timestamp") ? prev["timestamp"].get<std::string>() : "unknown");
    return msg;
}

// ============================================================================
// Feature 5: Environment Selector
// ============================================================================

void GovernanceEngine::applyEnvironment(const std::string& env_name) {
    auto it = rules_.environments.find(env_name);
    if (it == rules_.environments.end()) {
        fprintf(stderr, "[governance] WARNING: Environment '%s' not found in govern.json. Available:",
                env_name.c_str());
        for (const auto& [name, _] : rules_.environments) {
            fprintf(stderr, " %s", name.c_str());
        }
        fprintf(stderr, "\n");
        return;
    }

    for (const auto& [key, value] : it->second) {
        if (key == "mode") {
            if (value == "enforce") rules_.mode = GovernanceMode::ENFORCE;
            else if (value == "audit") rules_.mode = GovernanceMode::AUDIT;
            else if (value == "off") rules_.mode = GovernanceMode::OFF;
        } else if (key == "quality_gate.enabled") {
            rules_.quality_gate.enabled = (value == "true");
        } else if (key == "governance_baseline.enabled") {
            rules_.governance_baseline.enabled = (value == "true");
        } else if (key == "governance_baseline.fail_on_regression") {
            rules_.governance_baseline.fail_on_regression = (value == "true");
        } else if (key == "output.file_output.report_sarif") {
            rules_.output.file_output.report_sarif = value;
        } else if (key == "output.file_output.report_json") {
            rules_.output.file_output.report_json = value;
        } else if (key == "output.file_output.report_junit") {
            rules_.output.file_output.report_junit = value;
        }
        // Additional dot-path overrides can be added as needed
    }
    fprintf(stderr, "[governance] Applied environment: %s\n", env_name.c_str());
}

// ============================================================================
// Phase 8.4: Runtime Version Pinning
// ============================================================================

// Compare two version strings numerically (e.g., "3.11.2" >= "3.10")
// Returns true if 'observed' is >= 'required' component-by-component.
static bool semverGe(const std::string& observed, const std::string& required) {
    auto parseComponents = [](const std::string& v) {
        std::vector<int> parts;
        std::istringstream ss(v);
        std::string tok;
        while (std::getline(ss, tok, '.')) {
            try { parts.push_back(std::stoi(tok)); }
            catch (...) { parts.push_back(0); }
        }
        return parts;
    };
    auto obs = parseComponents(observed);
    auto req = parseComponents(required);
    size_t n = std::max(obs.size(), req.size());
    for (size_t i = 0; i < n; ++i) {
        int o = (i < obs.size()) ? obs[i] : 0;
        int r = (i < req.size()) ? req[i] : 0;
        if (o > r) return true;
        if (o < r) return false;
    }
    return true;  // equal
}

// Extract numeric version from a runtime version string.
// e.g., "Python 3.11.2" -> "3.11.2", "go1.21.0" -> "1.21.0"
static std::string extractVersionNumber(const std::string& version_str) {
    std::regex version_re(R"((\d+\.\d+(?:\.\d+)*))");
    std::smatch m;
    if (std::regex_search(version_str, m, version_re)) {
        return m[1].str();
    }
    return version_str;
}

static bool versionSatisfies(const std::string& observed_raw,
                              const std::string& required) {
    std::string observed = extractVersionNumber(observed_raw);
    if (required.substr(0, 2) == ">=") {
        return semverGe(observed, required.substr(2));
    }
    if (required.substr(0, 1) == ">") {
        // strict greater: not equal AND >=
        std::string base = required.substr(1);
        if (!semverGe(observed, base)) return false;
        return observed != base;
    }
    // Prefix match: "3.11" matches "3.11.0", "3.11.2", etc.
    // Strip to prefix length and compare
    std::string obs_prefix = observed.substr(0, std::min(observed.size(), required.size()));
    return obs_prefix == required;
}

void GovernanceEngine::checkRuntimeVersions(const std::string& language,
                                             const std::string& observed_version) {
    if (!active_) return;
    if (rules_.runtime_versions.empty()) return;
    if (observed_version.empty()) return;

    for (const auto& pin : rules_.runtime_versions) {
        if (pin.language != language) continue;

        bool ok = versionSatisfies(observed_version, pin.required_version);
        std::string rule_name = "runtime_version." + language;

        if (!ok) {
            std::string msg = pin.message.empty()
                ? fmt::format("Runtime version mismatch for {}: required '{}', got '{}'",
                    language, pin.required_version, observed_version)
                : pin.message;
            enforce(rule_name, pin.level,
                formatError(pin.level, msg,
                    fmt::format("{}", observed_version),
                    fmt::format("runtime_versions[language=\"{}\"].required = \"{}\"",
                        language, pin.required_version),
                    fmt::format("Pin your runtime: add to govern.json:\n"
                        "  \"runtime_versions\": [{{\"language\": \"{}\", "
                        "\"required\": \"{}\", \"level\": \"advisory\"}}]",
                        language, pin.required_version),
                    "", ""));
        } else {
            recordPass(rule_name, pin.level);
        }
        break;  // Only one pin per language
    }
}

} // namespace governance
} // namespace naab
