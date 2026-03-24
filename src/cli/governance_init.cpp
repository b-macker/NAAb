// governance_init.cpp — Interactive governance config generator for `naab-lang init`
// Generates a complete govern.json covering all 83 sections from govern-template.json

#include "governance_init.h"
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

using json = nlohmann::json;
using namespace naab::cli;

// ---------------------------------------------------------------------------
// Helper: fixed level per preset
static std::string fixedLevel(StrictnessPreset s,
                               const char* relaxed, const char* standard,
                               const char* strict, const char* paranoid) {
    switch (s) {
        case StrictnessPreset::RELAXED:  return relaxed;
        case StrictnessPreset::STANDARD: return standard;
        case StrictnessPreset::STRICT:   return strict;
        case StrictnessPreset::PARANOID: return paranoid;
    }
    return standard;
}

static bool isEnabled(StrictnessPreset s, int threshold) {
    return static_cast<int>(s) >= threshold;
}

// ---------------------------------------------------------------------------
// Helper: prompt user
// ---------------------------------------------------------------------------
static bool promptYesNo(const std::string& msg, bool default_yes, bool is_tty) {
    if (!is_tty) return default_yes;
    std::string hint = default_yes ? "[Y/n]" : "[y/N]";
    fmt::print("{} {}: ", msg, hint);
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) return default_yes;
    return (line[0] == 'y' || line[0] == 'Y');
}

static int promptChoice(const std::string& /*msg*/, int default_choice, int max_choice, bool is_tty) {
    if (!is_tty) return default_choice;
    fmt::print("   Choice [{}]: ", default_choice);
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) return default_choice;
    try {
        int v = std::stoi(line);
        if (v >= 1 && v <= max_choice) return v;
    } catch (...) {}
    return default_choice;
}

// ---------------------------------------------------------------------------
// runInteractiveSetup
// ---------------------------------------------------------------------------
GovernanceInitConfig naab::cli::runInteractiveSetup(bool is_tty) {
    GovernanceInitConfig config;

    if (is_tty) {
        fmt::print("\n");
        fmt::print("───────────────────────────────────────\n");
        fmt::print("  Governance Setup (govern.json)\n");
        fmt::print("───────────────────────────────────────\n\n");
    }

    // Q1: Languages
    if (is_tty) {
        fmt::print("1. What languages will your polyglot blocks use?\n");
        fmt::print("   [1] Python only\n");
        fmt::print("   [2] Python + Shell\n");
        fmt::print("   [3] Python + Shell + JavaScript\n");
        fmt::print("   [4] Python + Shell + Go + Nim\n");
        fmt::print("   [5] All supported (python, shell, javascript, go, nim, ruby, php, rust, cpp, csharp)\n");
        fmt::print("   [6] Custom (enter comma-separated list)\n");
    }
    int lang_choice = promptChoice("Languages", 2, 6, is_tty);
    switch (lang_choice) {
        case 1: config.languages = {"python"}; break;
        case 2: config.languages = {"python", "shell"}; break;
        case 3: config.languages = {"python", "shell", "javascript"}; break;
        case 4: config.languages = {"python", "shell", "go", "nim"}; break;
        case 5: config.languages = {"python", "shell", "javascript", "go", "nim", "ruby", "php", "rust", "cpp", "csharp"}; break;
        case 6: {
            fmt::print("   Enter languages (comma-separated): ");
            std::string line;
            std::getline(std::cin, line);
            config.languages.clear();
            std::istringstream ss(line);
            std::string lang;
            while (std::getline(ss, lang, ',')) {
                // trim
                lang.erase(0, lang.find_first_not_of(" \t"));
                lang.erase(lang.find_last_not_of(" \t") + 1);
                if (!lang.empty()) config.languages.push_back(lang);
            }
            if (config.languages.empty()) config.languages = {"python", "shell"};
            break;
        }
        default: config.languages = {"python", "shell"}; break;
    }
    if (is_tty) fmt::print("\n");

    // Q2: Strictness
    if (is_tty) {
        fmt::print("2. How strict should governance be?\n");
        fmt::print("   [1] Relaxed   — advisory warnings only, nothing blocked\n");
        fmt::print("   [2] Standard  — block secrets & dangerous code, warn on quality\n");
        fmt::print("   [3] Strict    — block everything suspicious, enforce quality\n");
        fmt::print("   [4] Paranoid  — maximum security, all checks at hard\n");
    }
    int strict_choice = promptChoice("Strictness", 2, 4, is_tty);
    switch (strict_choice) {
        case 1: config.strictness = StrictnessPreset::RELAXED; break;
        case 2: config.strictness = StrictnessPreset::STANDARD; break;
        case 3: config.strictness = StrictnessPreset::STRICT; break;
        case 4: config.strictness = StrictnessPreset::PARANOID; break;
    }
    if (is_tty) fmt::print("\n");

    // Q3: Taint tracking
    if (is_tty) fmt::print("3. Enable taint tracking? (tracks untrusted data from sources to sinks)\n");
    config.taint_enabled = promptYesNo("   Enable taint tracking?", true, is_tty);
    if (is_tty) fmt::print("\n");

    // Q4: Taint sources (only if taint enabled)
    if (config.taint_enabled) {
        if (is_tty) {
            fmt::print("4. What taint sources does your code use?\n");
            fmt::print("   [1] Environment variables only (env.get)\n");
            fmt::print("   [2] Environment + file input (env.get, file.read, io.read_line)\n");
            fmt::print("   [3] Environment + file + network (+ http responses, polyglot output)\n");
            fmt::print("   [4] Custom (enter comma-separated list)\n");
        }
        int source_choice = promptChoice("Taint sources", 2, 4, is_tty);
        switch (source_choice) {
            case 1:
                config.taint_sources = {"env.get"};
                break;
            case 2:
                config.taint_sources = {"env.get", "file.read", "io.read_line"};
                break;
            case 3:
                config.taint_sources = {"env.get", "file.read", "io.read_line", "polyglot_output", "http.response"};
                break;
            case 4: {
                fmt::print("   Enter sources (comma-separated): ");
                std::string line;
                std::getline(std::cin, line);
                std::istringstream ss(line);
                std::string src;
                while (std::getline(ss, src, ',')) {
                    src.erase(0, src.find_first_not_of(" \t"));
                    src.erase(src.find_last_not_of(" \t") + 1);
                    if (!src.empty()) config.taint_sources.push_back(src);
                }
                break;
            }
        }
        // Auto-populate sinks based on languages
        config.taint_sinks = {"shell_exec"};
        for (const auto& lang : config.languages) {
            if (lang == "python") config.taint_sinks.push_back("python_exec");
            else if (lang == "javascript") config.taint_sinks.push_back("javascript_exec");
            else if (lang == "go") config.taint_sinks.push_back("go_exec");
            else if (lang == "nim") config.taint_sinks.push_back("nim_exec");
            else if (lang == "ruby") config.taint_sinks.push_back("ruby_exec");
            else if (lang == "php") config.taint_sinks.push_back("php_exec");
            else if (lang == "rust") config.taint_sinks.push_back("rust_exec");
            else if (lang == "cpp") config.taint_sinks.push_back("cpp_exec");
            else if (lang == "csharp") config.taint_sinks.push_back("csharp_exec");
        }
        if (source_choice >= 2) {
            config.taint_sinks.push_back("file.write");
            config.taint_sinks.push_back("file.append");
        }
        if (source_choice >= 3) {
            config.taint_sinks.push_back("http.");
            config.taint_sinks.push_back("env.set_var");
        }
        if (is_tty) fmt::print("\n");
    }

    // Q5: Project type
    if (is_tty) {
        fmt::print("5. Project type?\n");
        fmt::print("   [1] Application  — requires main block, standard limits\n");
        fmt::print("   [2] Library/Module — no main required, stricter quality\n");
        fmt::print("   [3] Script       — relaxed limits, quick iteration\n");
        fmt::print("   [4] Test Suite   — relaxed quality, allow mocks/stubs\n");
    }
    int proj_choice = promptChoice("Project type", 1, 4, is_tty);
    switch (proj_choice) {
        case 1: config.project_type = ProjectType::APPLICATION; break;
        case 2: config.project_type = ProjectType::LIBRARY; break;
        case 3: config.project_type = ProjectType::SCRIPT; break;
        case 4: config.project_type = ProjectType::TEST_SUITE; break;
    }
    if (is_tty) fmt::print("\n");

    // Q6: Capabilities
    if (is_tty) {
        fmt::print("6. Capabilities — what should polyglot code be allowed to do?\n");
        fmt::print("   [1] Minimal    — no network, no filesystem, no shell\n");
        fmt::print("   [2] Standard   — read files, run shell, no network\n");
        fmt::print("   [3] Full       — network + filesystem + shell\n");
    }
    int cap_choice = promptChoice("Capabilities", 2, 3, is_tty);
    switch (cap_choice) {
        case 1: config.capabilities = CapabilityLevel::MINIMAL; break;
        case 2: config.capabilities = CapabilityLevel::STANDARD; break;
        case 3: config.capabilities = CapabilityLevel::FULL; break;
    }
    if (is_tty) fmt::print("\n");

    // Q7: Scanner
    if (is_tty) fmt::print("7. Enable the code quality scanner? (static analysis on polyglot blocks)\n");
    config.scanner_enabled = promptYesNo("   Enable scanner?", true, is_tty);
    if (is_tty) fmt::print("\n");

    // Q8: Contracts
    if (is_tty) fmt::print("8. Enable function contracts? (runtime assertions on return values)\n");
    config.contracts_enabled = promptYesNo("   Enable contracts?", false, is_tty);
    if (is_tty) fmt::print("\n");

    return config;
}

// ---------------------------------------------------------------------------
// presetToConfig
// ---------------------------------------------------------------------------
GovernanceInitConfig naab::cli::presetToConfig(const std::string& preset,
                                                const std::string& languages_override,
                                                bool taint_flag) {
    GovernanceInitConfig config;

    // Parse preset name
    if (preset == "relaxed") config.strictness = StrictnessPreset::RELAXED;
    else if (preset == "standard") config.strictness = StrictnessPreset::STANDARD;
    else if (preset == "strict") config.strictness = StrictnessPreset::STRICT;
    else if (preset == "paranoid") config.strictness = StrictnessPreset::PARANOID;
    else {
        fmt::print("Warning: Unknown preset '{}', using 'standard'\n", preset);
        config.strictness = StrictnessPreset::STANDARD;
    }

    // Parse language override
    if (!languages_override.empty()) {
        config.languages.clear();
        std::istringstream ss(languages_override);
        std::string lang;
        while (std::getline(ss, lang, ',')) {
            lang.erase(0, lang.find_first_not_of(" \t"));
            lang.erase(lang.find_last_not_of(" \t") + 1);
            if (!lang.empty()) config.languages.push_back(lang);
        }
    }

    config.taint_enabled = taint_flag;
    if (taint_flag) {
        config.taint_sources = {"env.get", "file.read", "io.read_line"};
        config.taint_sinks = {"shell_exec"};
        for (const auto& lang : config.languages) {
            if (lang == "python") config.taint_sinks.push_back("python_exec");
            else if (lang == "javascript") config.taint_sinks.push_back("javascript_exec");
            else if (lang == "go") config.taint_sinks.push_back("go_exec");
            else if (lang == "nim") config.taint_sinks.push_back("nim_exec");
        }
        config.taint_sinks.push_back("file.write");
        config.taint_sinks.push_back("file.append");
    }

    return config;
}

// ---------------------------------------------------------------------------
// Per-language config builder
// ---------------------------------------------------------------------------
static json buildPerLanguage(const std::string& lang, StrictnessPreset s) {
    json pl;
    bool strict_plus = (s >= StrictnessPreset::STRICT);

    if (lang == "python") {
        pl["timeout"] = strict_plus ? 10 : 15;
        pl["max_lines"] = strict_plus ? 150 : 200;
        pl["banned_functions"] = json::array({"eval(", "exec(", "compile(", "__import__("});
        pl["imports"] = {
            {"mode", "blocklist"},
            {"blocked", json::array({"subprocess", "ctypes", "os.system"})},
            {"blocked_from", json::array({"os:system,popen,exec"})}
        };
        pl["no_star_imports"] = true;
        pl["no_star_imports_level"] = strict_plus ? "soft" : "advisory";
    } else if (lang == "javascript") {
        pl["timeout"] = strict_plus ? 8 : 10;
        pl["max_lines"] = strict_plus ? 100 : 150;
        pl["banned_functions"] = json::array({"eval(", "Function(", "setTimeout("});
        pl["imports"] = {
            {"mode", "blocklist"},
            {"blocked", json::array({"child_process", "fs", "net"})}
        };
        pl["strict_mode"] = true;
        pl["strict_mode_level"] = strict_plus ? "soft" : "advisory";
        pl["no_var"] = true;
        pl["no_var_level"] = strict_plus ? "soft" : "advisory";
    } else if (lang == "shell") {
        pl["timeout"] = strict_plus ? 8 : 10;
        pl["max_lines"] = strict_plus ? 30 : 50;
        pl["banned_commands"] = json::array({"rm -rf /", "mkfs", "dd if=/dev/zero"});
        pl["shell_injection"] = true;
        pl["shell_injection_level"] = "hard";
        pl["require_set_e"] = strict_plus;
        pl["require_set_e_level"] = strict_plus ? "soft" : "advisory";
        pl["no_curl_pipe_sh"] = true;
        pl["no_curl_pipe_sh_level"] = "hard";
        pl["no_wget_pipe_bash"] = true;
        pl["no_wget_pipe_bash_level"] = "hard";
    } else if (lang == "go") {
        pl["timeout"] = 30;
        pl["max_lines"] = strict_plus ? 150 : 200;
        pl["banned_imports"] = json::array({"unsafe", "syscall"});
        pl["require_package_main"] = true;
    } else if (lang == "nim") {
        pl["timeout"] = 30;
        pl["max_lines"] = strict_plus ? 150 : 200;
    } else if (lang == "ruby") {
        pl["timeout"] = 10;
        pl["max_lines"] = strict_plus ? 80 : 100;
        pl["banned_functions"] = json::array({"eval(", "system(", "exec("});
    } else if (lang == "php") {
        pl["timeout"] = 10;
        pl["max_lines"] = strict_plus ? 80 : 100;
        pl["banned_functions"] = json::array({"eval(", "exec(", "system(", "shell_exec(", "passthru("});
    } else if (lang == "rust") {
        pl["timeout"] = 60;
        pl["max_lines"] = strict_plus ? 200 : 300;
    } else if (lang == "cpp") {
        pl["timeout"] = 60;
        pl["max_lines"] = strict_plus ? 200 : 300;
        pl["banned_includes"] = json::array({"<cstdlib>"});
        pl["banned_functions"] = json::array({"system(", "exec("});
    } else if (lang == "csharp") {
        pl["timeout"] = 60;
        pl["max_lines"] = strict_plus ? 200 : 300;
        pl["banned_namespaces"] = json::array({"System.Diagnostics.Process"});
    } else {
        // Unknown language — basic defaults
        pl["timeout"] = 15;
        pl["max_lines"] = 200;
    }
    return pl;
}

// ---------------------------------------------------------------------------
// Helper: check if language is in the config
// ---------------------------------------------------------------------------
static bool hasLang(const GovernanceInitConfig& c, const std::string& lang) {
    return std::find(c.languages.begin(), c.languages.end(), lang) != c.languages.end();
}

// ---------------------------------------------------------------------------
// generateGovernJson — builds ALL 83 sections
// ---------------------------------------------------------------------------
std::string naab::cli::generateGovernJson(const GovernanceInitConfig& config) {
    auto s = config.strictness;
    auto pt = config.project_type;
    auto cap = config.capabilities;
    bool strict_plus = (s >= StrictnessPreset::STRICT);
    bool paranoid = (s == StrictnessPreset::PARANOID);
    bool is_script = (pt == ProjectType::SCRIPT);
    bool is_test = (pt == ProjectType::TEST_SUITE);
    bool is_lib = (pt == ProjectType::LIBRARY);

    json root;

    // ── Top-level ──────────────────────────────────────────────────
    root["version"] = "4.0";
    root["mode"] = "enforce";
    root["_comment"] = fmt::format("Generated by naab-lang init (preset: {})",
        fixedLevel(s, "relaxed", "standard", "strict", "paranoid"));

    // ── languages ──────────────────────────────────────────────────
    root["languages"]["allowed"] = config.languages;
    root["languages"]["blocked"] = json::array();
    json per_lang;
    for (const auto& lang : config.languages) {
        per_lang[lang] = buildPerLanguage(lang, s);
    }
    root["languages"]["per_language"] = per_lang;

    // ── capabilities ───────────────────────────────────────────────
    // network
    {
        json net;
        bool net_enabled = (cap == CapabilityLevel::FULL);
        net["enabled"] = net_enabled;
        net["blocked_hosts"] = json::array({"169.254.169.254", "metadata.google.internal"});
        net["https_only"] = false;
        net["allow_websockets"] = net_enabled;
        net["allow_raw_sockets"] = false;
        root["capabilities"]["network"] = net;
    }
    // filesystem
    {
        json fs;
        if (cap == CapabilityLevel::MINIMAL) fs["mode"] = "none";
        else if (cap == CapabilityLevel::STANDARD) fs["mode"] = "read";
        else fs["mode"] = "write";
        fs["blocked_paths"] = json::array({"/etc/shadow", "/etc/passwd"});
        fs["blocked_extensions"] = json::array({".exe", ".dll", ".so"});
        fs["allow_symlinks"] = true;
        fs["allow_hidden_files"] = true;
        fs["allow_absolute_paths"] = true;
        root["capabilities"]["filesystem"] = fs;
    }
    // shell
    {
        json sh;
        sh["enabled"] = (cap != CapabilityLevel::MINIMAL);
        sh["blocked_commands"] = json::array({"sudo", "su", "chmod +s"});
        sh["allow_pipes"] = true;
        sh["allow_redirects"] = true;
        sh["allow_backgrounding"] = false;
        root["capabilities"]["shell"] = sh;
    }
    // env_vars
    {
        json ev;
        ev["read"] = true;
        ev["write"] = (cap == CapabilityLevel::FULL);
        ev["blocked_read"] = json::array({"AWS_SECRET_ACCESS_KEY", "GITHUB_TOKEN"});
        ev["blocked_write"] = json::array({"PATH", "HOME", "LD_PRELOAD"});
        root["capabilities"]["env_vars"] = ev;
    }
    // process
    {
        json pr;
        pr["spawn"] = (cap != CapabilityLevel::MINIMAL);
        pr["signals"] = (cap == CapabilityLevel::FULL);
        int max_procs = (cap == CapabilityLevel::MINIMAL) ? 0 :
                        (cap == CapabilityLevel::FULL) ? 10 : 5;
        pr["max_processes"] = max_procs;
        pr["allow_daemon"] = false;
        root["capabilities"]["process"] = pr;
    }
    // time
    {
        json tm;
        tm["allow_sleep"] = true;
        int max_sleep = (cap == CapabilityLevel::MINIMAL) ? 5 :
                        (cap == CapabilityLevel::FULL) ? 30 : 10;
        tm["max_sleep_seconds"] = max_sleep;
        tm["allow_timers"] = true;
        root["capabilities"]["time"] = tm;
    }
    // memory
    {
        json mem;
        int max_mb = (cap == CapabilityLevel::MINIMAL) ? 128 :
                     (cap == CapabilityLevel::FULL) ? 1024 : 512;
        mem["max_allocation_mb"] = max_mb;
        mem["allow_mmap"] = (cap == CapabilityLevel::FULL);
        mem["allow_shared_memory"] = false;
        root["capabilities"]["memory"] = mem;
    }

    // ── taint_tracking ─────────────────────────────────────────────
    {
        json tt;
        tt["enabled"] = config.taint_enabled;
        tt["level"] = config.taint_enabled ?
            fixedLevel(s, "advisory", "soft", "hard", "hard") : "soft";
        tt["sources"] = config.taint_enabled ?
            json(config.taint_sources) : json::array({"env.get", "io.read_line", "file.read", "polyglot_output"});
        tt["sinks"] = config.taint_enabled ?
            json(config.taint_sinks) : json::array({"shell_exec", "python_exec", "file.write"});
        tt["sanitizers"] = json::array({"validate_", "sanitize_", "escape_"});
        tt["propagation"] = {
            {"string_concat", true},
            {"string_interpolation", true},
            {"function_returns", true},
            {"for_loop_iterators", true},
            {"dict_array_access", true}
        };
        root["taint_tracking"] = tt;
    }

    // ── limits ─────────────────────────────────────────────────────
    {
        int global_timeout = (is_script || is_test) ? 120 : 60;
        root["limits"]["timeout"] = {
            {"global", global_timeout},
            {"per_block", 30},
            {"total_polyglot", global_timeout * 2}
        };
    }
    {
        int per_block_mb = strict_plus ? 128 : 256;
        int total_mb = strict_plus ? 512 : 1024;
        root["limits"]["memory"] = {
            {"per_block_mb", per_block_mb},
            {"total_mb", total_mb}
        };
    }
    {
        int poly_blocks = is_lib ? 30 : (is_script || is_test) ? 100 : 50;
        int loop_iter = (is_script || is_test) ? 10000000 : 1000000;
        int total_exec = is_lib ? 100 : (is_script || is_test) ? 500 : 200;
        root["limits"]["execution"] = {
            {"call_depth", 100},
            {"loop_iterations", loop_iter},
            {"polyglot_blocks", poly_blocks},
            {"parallel_blocks", 4},
            {"total_executions", total_exec}
        };
    }
    {
        int arr_size = (is_script || is_test) ? 1000000 : 100000;
        root["limits"]["data"] = {
            {"array_size", arr_size},
            {"dict_size", 50000},
            {"string_length", 1000000},
            {"nesting_depth", 20},
            {"output_size", 1000000},
            {"input_size", 1000000}
        };
    }
    {
        int max_lines = strict_plus ? 300 : 500;
        root["limits"]["code"] = {
            {"max_lines_per_block", max_lines},
            {"max_total_polyglot_lines", max_lines * 10},
            {"max_functions", 100},
            {"max_variables", 500},
            {"max_nesting_depth", strict_plus ? 8 : 10}
        };
    }
    {
        root["limits"]["rate"] = {
            {"max_polyglot_per_second", strict_plus ? 5 : 10},
            {"max_stdlib_calls_per_second", 100},
            {"max_file_ops_per_second", 20},
            {"cooldown_on_limit_ms", 100}
        };
    }

    // ── requirements ───────────────────────────────────────────────
    {
        bool main_needed = (pt == ProjectType::APPLICATION);
        root["requirements"]["main_block"] = {
            {"enabled", main_needed},
            {"level", main_needed ? fixedLevel(s, "off", "soft", "soft", "hard") : "off"},
            {"message", "Programs should have a main block"}
        };
    }
    {
        root["requirements"]["error_handling"] = {
            {"enabled", isEnabled(s, 2)},
            {"level", fixedLevel(s, "off", "off", "soft", "hard")},
            {"require_try_catch", strict_plus},
            {"require_catch_body", strict_plus}
        };
    }
    {
        root["requirements"]["strict_types"] = {
            {"enabled", paranoid},
            {"level", paranoid ? "advisory" : "off"}
        };
    }
    {
        root["requirements"]["no_global_state"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "advisory", "soft")},
            {"allow_global_constants", true},
            {"allow_global_functions", true},
            {"block_global_variables", strict_plus}
        };
    }
    {
        root["requirements"]["naming_conventions"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "advisory", "soft")},
            {"variables", "snake_case"},
            {"functions", "snake_case"},
            {"constants", "UPPER_SNAKE_CASE"}
        };
    }
    {
        root["requirements"]["documentation"] = {
            {"enabled", paranoid},
            {"level", paranoid ? "advisory" : "off"}
        };
    }
    {
        root["requirements"]["version_pinning"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "advisory", "soft")}
        };
    }

    // ── restrictions ───────────────────────────────────────────────
    {
        root["restrictions"]["dangerous_calls"] = {
            {"enabled", true},
            {"level", fixedLevel(s, "advisory", "hard", "hard", "hard")},
            {"check_chained_calls", strict_plus},
            {"check_string_formatting", paranoid}
        };
    }
    {
        root["restrictions"]["shell_injection"] = {
            {"enabled", true},
            {"level", fixedLevel(s, "advisory", "hard", "hard", "hard")},
            {"check_variable_expansion", strict_plus},
            {"require_quoting", paranoid}
        };
    }
    {
        root["restrictions"]["privilege_escalation"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "hard", "hard", "hard")},
            {"block_sudo", true},
            {"block_su", true},
            {"block_chmod_suid", true},
            {"block_setuid", strict_plus}
        };
    }
    // imports
    {
        json blocked;
        for (const auto& lang : config.languages) {
            if (lang == "python") blocked["python"] = json::array({"subprocess", "ctypes", "pickle", "os.system"});
            else if (lang == "javascript") blocked["javascript"] = json::array({"child_process", "fs", "net", "cluster"});
            else if (lang == "ruby") blocked["ruby"] = json::array({"open3", "pty"});
        }
        root["restrictions"]["imports"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")},
            {"mode", "blocklist"},
            {"blocked", blocked}
        };
    }
    {
        root["restrictions"]["data_exfiltration"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "soft", "hard")},
            {"block_base64_encode_secrets", true},
            {"block_hex_encode_secrets", true},
            {"block_url_encode_secrets", strict_plus}
        };
    }
    {
        root["restrictions"]["resource_abuse"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")},
            {"block_fork_bomb", true},
            {"block_recursive_file_ops", true},
            {"block_disk_filling", true}
        };
    }
    {
        root["restrictions"]["information_disclosure"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "soft", "hard")},
            {"block_env_dump", true},
            {"block_process_listing", true},
            {"block_system_info_leak", true}
        };
    }
    {
        root["restrictions"]["code_injection"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")},
            {"block_dynamic_code_gen", true},
            {"block_template_injection", strict_plus},
            {"block_sql_injection_patterns", true},
            {"block_command_injection", true}
        };
    }
    {
        root["restrictions"]["crypto"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "advisory", "soft")},
            {"block_weak_hashing", true},
            {"weak_hashes", json::array({"md5", "sha1"})},
            {"block_weak_encryption", true},
            {"block_hardcoded_keys", true}
        };
    }
    {
        root["restrictions"]["polyglot_output"] = {
            {"format", "any"},
            {"max_size", 0},
            {"require_structured", paranoid},
            {"validate_json", paranoid}
        };
    }

    // ── code_quality ───────────────────────────────────────────────
    // no_secrets
    {
        root["code_quality"]["no_secrets"] = {
            {"enabled", true},
            {"level", fixedLevel(s, "advisory", "hard", "hard", "hard")},
            {"entropy_check", {
                {"enabled", true},
                {"threshold", 4.5},
                {"min_length", 20}
            }},
            {"suspicious_variable_names", {
                {"enabled", true},
                {"names", json::array({"password", "secret", "api_key", "token", "private_key"})}
            }},
            {"allowlist", json::array({"example.com", "localhost", "127.0.0.1"})}
        };
    }
    // no_placeholders
    {
        root["code_quality"]["no_placeholders"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")},
            {"markers", json::array({"TODO", "FIXME", "STUB", "PLACEHOLDER", "HACK", "XXX", "TEMP"})}
        };
    }
    // no_hardcoded_results
    {
        root["code_quality"]["no_hardcoded_results"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "advisory", "soft", "hard")},
            {"check_return_true_false", true},
            {"check_return_none_null", true},
            {"check_return_empty_collections", true},
            {"check_dict_success_fields", true},
            {"check_dict_status_fields", true},
            {"check_perfect_scores", true}
        };
    }
    // no_pii
    {
        root["code_quality"]["no_pii"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "advisory", "soft", "hard")},
            {"detect_ssn", true},
            {"detect_credit_card", true},
            {"detect_email", true},
            {"detect_phone", true},
            {"detect_ip_address", strict_plus}
        };
    }
    // no_temporary_code
    {
        root["code_quality"]["no_temporary_code"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")}
        };
    }
    // no_simulation_markers
    {
        root["code_quality"]["no_simulation_markers"] = {
            {"enabled", true},
            {"level", fixedLevel(s, "advisory", "hard", "hard", "hard")}
        };
    }
    // no_mock_data
    {
        std::string mock_level;
        if (is_test) mock_level = "off";
        else mock_level = fixedLevel(s, "off", "advisory", "soft", "hard");
        root["code_quality"]["no_mock_data"] = {
            {"enabled", !is_test && isEnabled(s, 1)},
            {"level", mock_level},
            {"variable_prefixes", json::array({"mock_", "fake_", "dummy_", "test_"})},
            {"function_prefixes", json::array({"mock_", "fake_", "stub_"})},
            {"ignore_in_test_context", true}
        };
    }
    // no_apologetic_language
    {
        root["code_quality"]["no_apologetic_language"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "advisory", "soft")},
            {"scan_comments_only", true}
        };
    }
    // no_dead_code
    {
        root["code_quality"]["no_dead_code"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "advisory", "soft", "hard")},
            {"detect_unreachable_after_return", true},
            {"detect_always_true_conditions", strict_plus},
            {"detect_always_false_conditions", strict_plus},
            {"detect_empty_except_blocks", true},
            {"detect_commented_out_code", strict_plus}
        };
    }
    // no_debug_artifacts
    {
        root["code_quality"]["no_debug_artifacts"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")},
            {"check_polyglot_only", true}
        };
    }
    // no_unsafe_deserialization
    {
        root["code_quality"]["no_unsafe_deserialization"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")}
        };
    }
    // no_sql_injection
    {
        root["code_quality"]["no_sql_injection"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")}
        };
    }
    // no_path_traversal
    {
        root["code_quality"]["no_path_traversal"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "soft", "hard", "hard")}
        };
    }
    // no_hardcoded_urls
    {
        root["code_quality"]["no_hardcoded_urls"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "advisory", "soft")},
            {"allowlist", json::array({"https://example.com", "http://localhost"})}
        };
    }
    // no_hardcoded_ips
    {
        root["code_quality"]["no_hardcoded_ips"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "advisory", "soft")},
            {"allowlist", json::array({"127.0.0.1", "0.0.0.0", "::1"})}
        };
    }
    // max_complexity
    {
        root["code_quality"]["max_complexity"] = {
            {"enabled", strict_plus},
            {"level", fixedLevel(s, "off", "off", "advisory", "soft")},
            {"max_lines_per_block", 100},
            {"max_nesting_depth", 5},
            {"max_parameters", 6},
            {"max_cyclomatic_complexity", 10},
            {"max_cognitive_complexity", 15}
        };
    }
    // no_oversimplification
    {
        std::string os_level;
        if (is_script) os_level = fixedLevel(s, "off", "advisory", "soft", "hard");
        else if (is_lib) os_level = fixedLevel(s, "soft", "hard", "hard", "hard");
        else os_level = fixedLevel(s, "advisory", "hard", "hard", "hard");
        root["code_quality"]["no_oversimplification"] = {
            {"enabled", true},
            {"level", os_level},
            {"check_empty_bodies", true},
            {"check_trivial_returns", true},
            {"check_identity_functions", true},
            {"check_not_implemented", true},
            {"check_comment_only_bodies", true},
            {"check_fabricated_results", true},
            {"min_function_lines", 2}
        };
    }
    // no_incomplete_logic
    {
        root["code_quality"]["no_incomplete_logic"] = {
            {"enabled", true},
            {"level", fixedLevel(s, "advisory", "hard", "hard", "hard")},
            {"check_empty_catch", true},
            {"check_swallowed_exceptions", true},
            {"check_generic_errors", strict_plus},
            {"check_vague_error_messages", strict_plus},
            {"check_single_iteration_loops", true},
            {"check_missing_validation", strict_plus}
        };
    }
    // no_hallucinated_apis
    {
        root["code_quality"]["no_hallucinated_apis"] = {
            {"enabled", true},
            {"level", fixedLevel(s, "advisory", "soft", "hard", "hard")},
            {"check_cross_language", true},
            {"check_made_up_functions", true},
            {"check_wrong_syntax", strict_plus}
        };
    }
    // complexity_floor
    {
        std::string cf_level = fixedLevel(s, "off", "off", "soft", "hard");
        json rules = json::array();
        if (strict_plus) {
            rules.push_back({
                {"names", json::array({"test_"})},
                {"min_score", 3},
                {"require_branching_or_loops", false},
                {"message", "Test functions have lower complexity requirements"}
            });
            rules.push_back({
                {"names", json::array({"analyze", "compute", "calculate", "process", "transform", "evaluate", "optimize", "classify", "detect", "predict"})},
                {"min_score", 25},
                {"require_branching_or_loops", true},
                {"message", "Functions named analyze/compute/process must contain non-trivial logic"}
            });
            rules.push_back({
                {"names", json::array({"sort", "search", "filter", "merge", "parse", "serialize", "encode", "decode", "compress", "encrypt"})},
                {"min_score", 20},
                {"require_branching_or_loops", true}
            });
            rules.push_back({
                {"names", json::array({"get", "set", "is_", "has_", "to_", "from_", "format", "wrap", "apply_", "move_", "check_", "destroy_", "find_", "render_", "load_", "spawn_", "format_", "make_"})},
                {"min_score", 3},
                {"require_branching_or_loops", false}
            });
        }
        root["code_quality"]["complexity_floor"] = {
            {"level", cf_level},
            {"min_score", 10},
            {"check_polyglot", false},
            {"check_naab", true},
            {"skip_if_has_polyglot_block", true},
            {"rules", rules}
        };
    }
    // encoding
    {
        root["code_quality"]["encoding"] = {
            {"enabled", isEnabled(s, 1)},
            {"level", fixedLevel(s, "off", "advisory", "soft", "hard")},
            {"require_utf8", true},
            {"block_null_bytes", true},
            {"block_control_characters", strict_plus},
            {"block_unicode_bidi", true},
            {"block_homoglyph_attacks", strict_plus}
        };
    }
    // duplicate_calls
    {
        root["code_quality"]["duplicate_calls"] = {
            {"enabled", true},
            {"threshold", 3},
            {"max_entries", 5}
        };
    }
    // polyglot_try_catch
    {
        root["code_quality"]["polyglot_try_catch"] = {
            {"enabled", true},
            {"max_entries", 3}
        };
    }

    // ── contracts ──────────────────────────────────────────────────
    {
        root["contracts"] = {
            {"level", "soft"},
            {"validate_inputs", config.contracts_enabled},
            {"functions", json::object()}
        };
        if (config.contracts_enabled) {
            root["contracts"]["_comment"] = "Add function contracts here: {\"fn_name\": {\"return_type\": \"string\", ...}}";
        }
    }

    // ── baselines ──────────────────────────────────────────────────
    {
        root["baselines"] = {
            {"enabled", false},
            {"level", "advisory"},
            {"path", ".naab/baselines.json"},
            {"tolerance", 1e-6},
            {"auto_record", false},
            {"hash_keys", true}
        };
    }

    // ── custom_rules ───────────────────────────────────────────────
    {
        root["_comment_custom_rules"] = "Add custom regex rules: [{\"id\": \"CUSTOM-001\", \"pattern\": \"...\", \"level\": \"hard\", ...}]";
        root["custom_rules"] = json::array();
    }

    // ── scopes ─────────────────────────────────────────────────────
    {
        json scopes = json::array();
        // tests scope — relax quality checks
        json test_scope;
        test_scope["glob"] = "tests/**";
        test_scope["overrides"] = {
            {"code_quality.no_placeholders", "false"},
            {"code_quality.no_mock_data", "false"},
            {"code_quality.no_oversimplification.level", "advisory"}
        };
        scopes.push_back(test_scope);

        // examples scope — relax hardcoded results
        json example_scope;
        example_scope["glob"] = "examples/**";
        example_scope["overrides"] = {
            {"code_quality.no_hardcoded_results", "false"},
            {"requirements.documentation.enabled", "true"}
        };
        scopes.push_back(example_scope);

        root["scopes"] = scopes;
    }

    // ── output ─────────────────────────────────────────────────────
    {
        root["output"]["summary"] = {
            {"enabled", true},
            {"format", "detailed"},
            {"show_passing", true},
            {"show_skipped", false},
            {"group_by", "category"},
            {"sort_by", "severity"}
        };
        root["output"]["errors"] = {
            {"verbose", true},
            {"show_line_preview", true},
            {"show_code_context", 3},
            {"show_help", true},
            {"show_examples", true},
            {"show_rule_path", true},
            {"show_fix_suggestions", true},
            {"max_errors_per_rule", 5},
            {"max_total_errors", 50}
        };
        root["output"]["formatting"] = {
            {"color", true},
            {"unicode_symbols", true},
            {"width", 80},
            {"indent", 2}
        };
        root["output"]["file_output"] = {
            {"report_json", ""},
            {"report_sarif", ""},
            {"report_junit", ""},
            {"report_csv", ""},
            {"report_html", ""}
        };
        int max_adv = paranoid ? 5 : strict_plus ? 10 :
                      (s == StrictnessPreset::STANDARD) ? 15 : 50;
        root["output"]["max_advisories"] = max_adv;
        root["output"]["advisory_summary"] = true;
    }

    // ── audit ──────────────────────────────────────────────────────
    {
        root["audit"]["level"] = strict_plus ? "detailed" : "basic";
        root["audit"]["output_file"] = ".governance-audit.jsonl";
        root["audit"]["tamper_evidence"] = {
            {"enabled", paranoid},
            {"algorithm", "sha256"},
            {"chain_genesis", "NAAB-GOVERNANCE-GENESIS"}
        };
        root["audit"]["log_events"] = {
            {"checks_passed", strict_plus},
            {"checks_failed", true},
            {"checks_warned", true},
            {"overrides", true},
            {"config_loaded", true},
            {"execution_start", strict_plus},
            {"execution_end", strict_plus},
            {"polyglot_executed", strict_plus},
            {"polyglot_timing", strict_plus},
            {"taint_decisions", strict_plus},
            {"contract_checks", config.contracts_enabled}
        };
        root["audit"]["retention"] = {
            {"max_file_size_mb", 100},
            {"rotate_at_mb", 50},
            {"keep_rotated", 5}
        };
        root["audit"]["provenance"] = {
            {"enabled", paranoid},
            {"record_proof_objects", paranoid},
            {"record_attestations", paranoid},
            {"record_decisions", paranoid}
        };
    }

    // ── meta ───────────────────────────────────────────────────────
    {
        root["meta"]["schema_validation"] = {
            {"warn_unknown_keys", true},
            {"suggest_corrections", true},
            {"strict_types", true}
        };
        root["meta"]["config_immutability"] = {
            {"hash", ""},
            {"verify_on_load", paranoid},
            {"block_on_mismatch", false}
        };
        root["meta"]["inheritance"] = {
            {"max_depth", 5},
            {"merge_strategy", "child_wins"},
            {"merge_arrays", "replace"},
            {"allow_circular", false}
        };
        root["meta"]["feature_flags"] = {
            {"experimental_checks", false},
            {"legacy_compatibility", true},
            {"verbose_parsing", false}
        };
        root["meta"]["environment"] = {
            {"allow_env_var_substitution", false},
            {"env_prefix", "NAAB_GOV_"},
            {"allow_cli_override", true}
        };
    }

    // ── hooks ──────────────────────────────────────────────────────
    {
        auto empty_hook = [](){ return json{{"command", ""}, {"args", json::array()}, {"timeout", 5}}; };
        root["hooks"]["on_violation"] = empty_hook();
        root["hooks"]["on_override"] = empty_hook();
        root["hooks"]["on_complete"] = empty_hook();
        root["hooks"]["pre_check"] = empty_hook();
        root["hooks"]["post_check"] = empty_hook();
    }

    // ── polyglot ───────────────────────────────────────────────────
    {
        root["polyglot"]["variable_binding"] = {
            {"require_explicit", strict_plus},
            {"require_explicit_level", strict_plus ? "soft" : "advisory"},
            {"max_bound_variables", 0},
            {"validate_types", paranoid}
        };
        root["polyglot"]["output"] = {
            {"require_json_pipe", false},
            {"require_naab_return", false},
            {"max_output_lines", 0},
            {"strip_whitespace", false},
            {"validate_encoding", true}
        };
        root["polyglot"]["context_isolation"] = {
            {"enabled", strict_plus},
            {"level", strict_plus ? "soft" : "advisory"},
            {"no_shared_state", paranoid},
            {"no_env_pollution", true},
            {"clean_temp_files", true}
        };
        root["polyglot"]["parallel"] = {
            {"max_parallel_blocks", 4},
            {"timeout_per_block", 30},
            {"fail_strategy", "fail_fast"},
            {"allow_shared_variables", false}
        };
        root["polyglot"]["persistent_runtime"] = {
            {"max_sessions", 5},
            {"session_timeout", 300},
            {"max_memory_per_session_mb", 256},
            {"allow_state_persistence", true}
        };
    }

    // ── polyglot_optimization ──────────────────────────────────────
    {
        json po;
        po["enabled"] = true;
        po["enforcement_level"] = strict_plus ? "soft" : "advisory";

        // pattern_detection with all 8 task types
        po["pattern_detection"]["enabled"] = true;
        po["pattern_detection"]["task_inference"]["numerical_operations"] = {
            {"patterns", json::array({"\\bnp\\.", "\\bnumpy\\.", "matrix", "\\bsum\\(", "\\bmean\\("})},
            {"optimal_languages", json::array({"python", "nim"})},
            {"suboptimal_languages", json::array({"javascript", "shell"})}
        };
        po["pattern_detection"]["task_inference"]["string_processing"] = {
            {"patterns", json::array({"\\bsplit\\(", "\\bjoin\\(", "\\breplace\\(", "\\bregex\\b"})},
            {"optimal_languages", json::array({"python", "ruby", "nim"})},
            {"suboptimal_languages", json::array({"go", "shell"})}
        };
        po["pattern_detection"]["task_inference"]["file_operations"] = {
            {"patterns", json::array({"\\bfind\\b", "\\bgrep\\b", "\\bsed\\b", "\\bawk\\b"})},
            {"optimal_languages", json::array({"shell"})},
            {"suboptimal_languages", json::array({"javascript"})}
        };
        po["pattern_detection"]["task_inference"]["systems_calls"] = {
            {"patterns", json::array({"\\bmalloc\\b", "\\bfree\\b", "pointer", "\\bunsafe\\b"})},
            {"optimal_languages", json::array({"nim", "go", "rust", "cpp"})},
            {"suboptimal_languages", json::array({"python", "javascript", "ruby"})}
        };
        po["pattern_detection"]["task_inference"]["web_apis"] = {
            {"patterns", json::array({"\\bfetch\\(", "\\bhttp\\.", "\\brequest\\("})},
            {"optimal_languages", json::array({"javascript", "python", "go"})},
            {"suboptimal_languages", json::array({"shell", "nim"})}
        };
        po["pattern_detection"]["task_inference"]["json_processing"] = {
            {"patterns", json::array({"JSON\\.parse", "json\\.", "json\\.dumps"})},
            {"optimal_languages", json::array({"javascript", "python", "nim"})},
            {"suboptimal_languages", json::array({"cpp", "shell"})}
        };
        po["pattern_detection"]["task_inference"]["concurrency"] = {
            {"patterns", json::array({"\\bgoroutine\\b", "\\basync\\b", "\\bawait\\b", "\\bthread\\b"})},
            {"optimal_languages", json::array({"go", "javascript"})},
            {"suboptimal_languages", json::array({"python", "ruby"})}
        };
        po["pattern_detection"]["task_inference"]["data_parsing"] = {
            {"patterns", json::array({"\\bcsv\\.", "\\bxml\\.", "BeautifulSoup", "DictReader"})},
            {"optimal_languages", json::array({"python", "javascript"})},
            {"suboptimal_languages", json::array({"shell", "go"})}
        };

        po["profiling"] = {
            {"enabled", true},
            {"profile_path", "~/.naab/profile.json"},
            {"max_entries", 10000}
        };
        po["calibration"] = {
            {"enabled", true},
            {"calibration_path", "~/.naab/calibration.json"},
            {"max_age_days", 30},
            {"iterations", 3}
        };
        po["confidence"] = {
            {"min_display_level", "estimated"},
            {"suppress_unknown", true},
            {"show_measurement_details", true}
        };
        po["verification"] = {
            {"enabled", false},
            {"enforcement_level", "advisory"}
        };
        po["language_diversity"] = {
            {"enabled", true},
            {"min_languages", 2},
            {"max_single_language_percent", 80}
        };
        po["helper_errors"] = {
            {"enabled", true},
            {"show_alternative_language", true},
            {"show_example_code", true}
        };
        po["ai_guidance"] = {
            {"enabled", true},
            {"include_in_errors", true},
            {"suggest_refactoring", true},
            {"show_benchmarks", false}
        };

        root["polyglot_optimization"] = po;
    }

    // ── scanner ────────────────────────────────────────────────────
    if (config.scanner_enabled) {
        json sc;
        sc["version"] = "1.0";
        sc["mode"] = (s == StrictnessPreset::RELAXED) ? "audit" : "enforce";

        sc["scan"] = {
            {"max_files", 200},
            {"max_file_size_kb", 500},
            {"include_tests", false},
            {"follow_symlinks", false},
            {"exclude_patterns", json::array({"node_modules", ".git", "__pycache__", "build", "dist"})}
        };

        // Scanner level helper — bumps template defaults by preset
        auto scanLevel = [&](const char* template_level) -> std::string {
            // template_level is the govern-template.json default
            // We bump it based on strictness
            if (paranoid) return "hard";
            if (s == StrictnessPreset::STRICT) {
                if (std::string(template_level) == "advisory") return "soft";
                return template_level;
            }
            if (s == StrictnessPreset::RELAXED) return "advisory";
            return template_level; // STANDARD uses template defaults
        };

        // redundancy (18 checks)
        sc["redundancy"] = {
            {"obvious_comments",       {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"over_abstraction",       {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"redundant_try_catch",    {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"suspicious_try_catch",   {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"generic_variable_names", {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"excessive_comments",     {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"apologetic_comments",    {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"placeholder_code",       {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"gaming_comments",        {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"trivial_constant_alias", {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"restating_docstrings",   {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"unnecessary_else_after_return", {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"wrapper_classes",        {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"unused_imports",         {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"single_use_variable",    {{"enabled", false}, {"level", scanLevel("advisory")}}},
            {"copy_paste_signatures",  {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"excessive_type_checks",  {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"decorative_separators",  {{"enabled", true}, {"level", scanLevel("advisory")}}}
        };

        // code_quality (15 checks)
        sc["code_quality"] = {
            {"empty_catch",            {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"catch_and_ignore",       {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"magic_numbers",          {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"magic_strings",          {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"dead_code_after_return", {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"dead_conditional",       {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"god_functions",          {{"enabled", true}, {"level", scanLevel("soft")}, {"max_lines", 80}}},
            {"god_classes",            {{"enabled", true}, {"level", scanLevel("soft")}, {"max_lines", 300}}},
            {"deep_nesting",           {{"enabled", true}, {"level", scanLevel("soft")}, {"max_depth", 4}}},
            {"long_parameter_list",    {{"enabled", true}, {"level", scanLevel("soft")}, {"max_params", 5}}},
            {"complex_boolean_expr",   {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"mutable_global_state",   {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"boolean_function_returns", {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"string_concat_in_loop",  {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"recursive_no_base_case", {{"enabled", true}, {"level", scanLevel("soft")}}}
        };

        // complexity (8 checks)
        sc["complexity"] = {
            {"cyclomatic_complexity",  {{"enabled", true}, {"level", scanLevel("soft")}, {"max", 15}}},
            {"cognitive_complexity",   {{"enabled", true}, {"level", scanLevel("soft")}, {"max", 20}}},
            {"file_length",            {{"enabled", true}, {"level", scanLevel("soft")}, {"max_lines", 500}}},
            {"function_count",         {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"class_count",            {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"import_count",           {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"return_count",           {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"nested_ternary",         {{"enabled", true}, {"level", scanLevel("soft")}}}
        };

        // style (9 checks)
        sc["style"] = {
            {"inconsistent_naming",    {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"debug_leftovers",        {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"commented_out_code",     {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"inconsistent_spacing",   {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"long_lines",             {{"enabled", true}, {"level", scanLevel("advisory")}, {"max_length", 120}}},
            {"missing_final_newline",  {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"multiple_blank_lines",   {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"inconsistent_quotes",    {{"enabled", false}, {"level", scanLevel("advisory")}}},
            {"import_ordering",        {{"enabled", true}, {"level", scanLevel("advisory")}}}
        };

        // security (10 checks)
        sc["security"] = {
            {"hardcoded_credentials",    {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"sql_string_concat",        {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"shell_injection",          {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"insecure_random",          {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"weak_crypto",              {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"hardcoded_ip",             {{"enabled", strict_plus}, {"level", scanLevel("advisory")}}},
            {"hardcoded_url",            {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"path_traversal",           {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"insecure_deserialization", {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"exposed_stack_traces",     {{"enabled", true}, {"level", scanLevel("advisory")}}}
        };

        // lang_rules — only include for selected languages + naab always
        json lang_rules;

        // naab always
        lang_rules["naab"] = {
            {"value_semantics_bug",    {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"missing_null_check",     {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"polyglot_no_binding",    {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"throw_raw_string",       {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"oversized_polyglot",     {{"enabled", true}, {"level", scanLevel("soft")}, {"max_lines", 100}}},
            {"top_level_let",          {{"enabled", true}, {"level", scanLevel("hard")}}},
            {"arrow_lambda",           {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"missing_export",         {{"enabled", true}, {"level", scanLevel("advisory")}}},
            {"dict_bracket_access",    {{"enabled", true}, {"level", scanLevel("soft")}}},
            {"python_return_in_block", {{"enabled", true}, {"level", scanLevel("hard")}}}
        };

        if (hasLang(config, "python")) {
            lang_rules["python"] = {
                {"bare_except",           {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"mutable_default_args",  {{"enabled", true}, {"level", scanLevel("hard")}}},
                {"star_imports",          {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"assert_for_validation", {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"global_keyword",        {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"nested_comprehension",  {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"broad_exception_type",  {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"open_without_with",     {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"type_check_isinstance", {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"import_inside_function", {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"mutable_class_vars",    {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"string_format_mix",     {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"duplicate_dict_key",    {{"enabled", true}, {"level", scanLevel("hard")}}},
                {"f_string_without_expression", {{"enabled", true}, {"level", scanLevel("advisory")}}}
            };
        }

        if (hasLang(config, "javascript")) {
            lang_rules["javascript"] = {
                {"loose_equality",       {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"var_usage",            {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"eval_usage",           {{"enabled", true}, {"level", scanLevel("hard")}}},
                {"for_in_array",         {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"prototype_pollution",  {{"enabled", true}, {"level", scanLevel("hard")}}},
                {"document_write",       {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"innerhtml_assignment", {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"promise_no_catch",     {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"async_no_await",       {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"callback_hell",        {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"no_strict_mode",       {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"implicit_globals",     {{"enabled", true}, {"level", scanLevel("soft")}}}
            };
        }

        if (hasLang(config, "cpp")) {
            lang_rules["cpp"] = {
                {"raw_new_delete",        {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"using_namespace_std_header", {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"c_style_casts",         {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"goto_usage",            {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"manual_memory",         {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"macro_functions",       {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"missing_virtual_dtor",  {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"exception_by_value",    {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"string_c_str_misuse",   {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"include_guard_missing", {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"array_decay",           {{"enabled", true}, {"level", scanLevel("advisory")}}}
            };
        }

        if (hasLang(config, "go")) {
            lang_rules["go"] = {
                {"ignored_error",        {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"panic_in_library",     {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"empty_interface_abuse", {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"error_string_format",  {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"shadow_err",           {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"naked_return",         {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"goroutine_leak",       {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"defer_in_loop",        {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"init_function",        {{"enabled", true}, {"level", scanLevel("advisory")}}}
            };
        }

        if (hasLang(config, "rust")) {
            lang_rules["rust"] = {
                {"unsafe_blocks",       {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"todo_unimplemented",  {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"string_push",         {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"unwrap_in_prod",      {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"needless_lifetimes",  {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"clone_abuse",         {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"manual_map",          {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"expect_without_msg",  {{"enabled", true}, {"level", scanLevel("soft")}}},
                {"return_impl_trait",   {{"enabled", true}, {"level", scanLevel("advisory")}}},
                {"box_vec",             {{"enabled", true}, {"level", scanLevel("advisory")}}}
            };
        }

        sc["lang_rules"] = lang_rules;

        sc["output"] = {
            {"format", "text"},
            {"max_issues_per_file", 50},
            {"max_total_issues", 500},
            {"group_by", "file"},
            {"sort_by", "severity"},
            {"show_line_preview", true},
            {"show_fix_suggestion", true},
            {"save_json", true},
            {"save_text", true}
        };

        root["scanner"] = sc;
    }

    // ── project_context ────────────────────────────────────────────
    {
        root["project_context"] = {
            {"enabled", false},
            {"enforcement_level", "advisory"},
            {"sources", {
                {"llm", true},
                {"linters", true},
                {"manifests", true}
            }},
            {"extract", {
                {"language_preferences", true},
                {"banned_patterns", true},
                {"style_rules", true},
                {"custom_directives", true}
            }},
            {"feed_optimization", true},
            {"show_extractions", true},
            {"dry_run", false},
            {"max_file_size_kb", 100}
        };
    }

    return root.dump(2);
}

// ---------------------------------------------------------------------------
// writeGovernJson
// ---------------------------------------------------------------------------
bool naab::cli::writeGovernJson(const std::string& path,
                                 const GovernanceInitConfig& config,
                                 bool force) {
    namespace fs = std::filesystem;

    if (fs::exists(path) && !force) {
        fmt::print("  govern.json already exists. Use --force to overwrite.\n");
        return false;
    }

    std::string content = generateGovernJson(config);

    std::ofstream out(path);
    if (!out.is_open()) {
        fmt::print("  Failed to create {}\n", path);
        return false;
    }
    out << content << "\n";
    out.close();

    // Print summary
    std::string preset_name = fixedLevel(config.strictness, "relaxed", "standard", "strict", "paranoid");
    std::string lang_list;
    for (size_t i = 0; i < config.languages.size(); i++) {
        if (i > 0) lang_list += ", ";
        lang_list += config.languages[i];
    }
    std::string cap_desc;
    switch (config.capabilities) {
        case CapabilityLevel::MINIMAL: cap_desc = "minimal (sandbox)"; break;
        case CapabilityLevel::STANDARD: cap_desc = "read files, shell"; break;
        case CapabilityLevel::FULL: cap_desc = "network + filesystem + shell"; break;
    }

    fmt::print("  Created govern.json\n");
    fmt::print("  Profile: {} | Languages: {} | Taint: {}\n",
        preset_name, lang_list, config.taint_enabled ? "enabled" : "disabled");
    fmt::print("  Capabilities: {} | Scanner: {}\n",
        cap_desc, config.scanner_enabled ? "enabled" : "disabled");

    return true;
}
