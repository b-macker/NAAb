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
#include "naab/crypto_utils.h"       // run-identity fingerprint (E3)
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
    if (scoring_calibration_dirty_ && rules().scoring_calibration.auto_save) {
        saveScoringCalibration();
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
        case EnforcementLevel::DETECT:            return "detect";
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
        case EnforcementLevel::DETECT:            return "DETECT";
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
    oss << "  Rule: " << rule << "\n\n";

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
        std::string s = (value.is_string() ? value.get<std::string>() : std::string());
        if (s == "hard")              return {true, EnforcementLevel::HARD};
        if (s == "approval_required") return {true, EnforcementLevel::APPROVAL_REQUIRED};
        if (s == "soft")              return {true, EnforcementLevel::SOFT};
        if (s == "advisory")          return {true, EnforcementLevel::ADVISORY};
        if (s == "detect")            return {true, EnforcementLevel::DETECT};
        // A1c: any OTHER string falls through to {false, HARD} below -- i.e. it
        // silently DISABLES the check. So "level": "off", "none", "warn", or a
        // plain typo turns a check off with no diagnostic, which is the same
        // silence as the ignored "enabled" flag, in the very key the warning for
        // that flag tells operators to reach for.
        //
        // The value is still not honoured differently -- rejecting unknown levels
        // and defaulting them to enabled are both tightenings that could fail
        // configs which load today. Only the silence is fixed, matching the
        // precedent set for the enabled flag.
        // Warn once per distinct unknown level. Several checks are parsed by two
        // paths (no_secrets, no_placeholders, no_hardcoded_results each reach
        // parseEnforcementLevel twice), so a single bad value printed per call
        // site produces duplicate lines that carry no extra information -- the
        // message does not name the key, so the second line is pure noise.
        {
            static std::mutex warned_mu;
            static std::set<std::string> warned;
            std::lock_guard<std::mutex> lk(warned_mu);
            if (warned.insert(s).second) {
                fprintf(stderr,
                        "[governance] Warning: unknown enforcement level \"%s\" - "
                        "this check is DISABLED. Valid levels: hard, soft, "
                        "advisory, detect, approval_required.\n",
                        s.c_str());
            }
        }
    }
    if (value.is_object()) {
        bool enabled = value.value("enabled", true);
        EnforcementLevel level = EnforcementLevel::HARD;
        if (value.contains("level") && value["level"].is_string()) {
            std::string s = value["level"].get<std::string>();
            if (s == "approval_required") level = EnforcementLevel::APPROVAL_REQUIRED;
            else if (s == "soft") level = EnforcementLevel::SOFT;
            else if (s == "advisory") level = EnforcementLevel::ADVISORY;
            else if (s == "detect") level = EnforcementLevel::DETECT;
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

// A check whose object form enables itself from the mere PRESENCE of the block,
// and never reads "enabled". Writing {"enabled": false} therefore turns it ON.
//
// The value is deliberately not honoured -- see the long note at the
// restrictions.* block: honouring it is a loosening, because every config
// carrying "enabled": false today is being enforced and would silently stop on
// upgrade. What is wrong is the silence, so the silence is what this fixes, with
// no change to what is enforced. Shared so code_quality, requirements and
// restrictions all say the same thing about the same JSON.
// A check whose block enables itself only when "level" is present, and which
// never reads "enabled" at all -- so BOTH {"enabled": true} and
// {"enabled": false} are no-ops there. The first is the more dangerous reading:
// an operator who writes {"enabled": true} with no level believes they turned a
// requirement on, and it stays off. Distinct message from
// warnIgnoredEnableFlag(), whose subject is enabled-by-presence.
static void warnEnableNeedsLevel(const nlohmann::json& blk, const char* section,
                                 const char* name) {
    if (!blk.contains("enabled") || !blk["enabled"].is_boolean()) return;
    const bool asked   = blk["enabled"].get<bool>();
    const bool actual  = blk.contains("level");   // "level" is the real trigger
    // Only the MISMATCH is worth a warning. Warning on every block carrying an
    // "enabled" key fires on the two combinations that already do what the
    // operator meant -- {enabled:true, level:X} is on and stays on,
    // {enabled:false} with no level is off and stays off -- and in the first of
    // those the advice ("add a level") is actively wrong, because a level is
    // right there. A warning an operator learns to ignore is worse than none.
    if (asked == actual) return;
    if (asked) {
        fprintf(stderr,
                "[governance] Warning: \"%s.%s.enabled\": true does not enable this "
                "check - it is enabled by setting \"level\" on the \"%s\" block, "
                "which is absent, so the check is OFF. Add a \"level\" to enable it.\n",
                section, name, name);
    } else {
        fprintf(stderr,
                "[governance] Warning: \"%s.%s.enabled\": false has no effect - the "
                "\"level\" key on the \"%s\" block is what enables this check, so it "
                "is ON. Remove the block to leave it disabled.\n",
                section, name, name);
    }
}

static void warnIgnoredEnableFlag(const nlohmann::json& blk, const char* section,
                                  const char* name) {
    if (blk.contains("enabled") && blk["enabled"].is_boolean() &&
        !blk["enabled"].get<bool>()) {
        fprintf(stderr,
                "[governance] Warning: \"%s.%s.enabled\": false has no effect - "
                "this check is enabled by the presence of its block. "
                "Remove the \"%s\" block to leave it disabled.\n",
                section, name, name);
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
        if (gov.contains("verbose") && gov["verbose"].is_boolean()) rules_.verbose = gov["verbose"].get<bool>();
        if (gov.contains("dashboard") && gov["dashboard"].is_boolean()) rules_.dashboard = gov["dashboard"].get<bool>();
        if (gov.contains("baseline_save") && gov["baseline_save"].is_boolean()) rules_.baseline_save = gov["baseline_save"].get<bool>();
        if (gov.contains("override") && gov["override"].is_boolean()) rules_.allow_override = gov["override"].get<bool>();
        if (gov.contains("lint_only") && gov["lint_only"].is_boolean()) rules_.lint_only_config = gov["lint_only"].get<bool>();
        if (gov.contains("record_baselines") && gov["record_baselines"].is_boolean()) rules_.record_baselines = gov["record_baselines"].get<bool>();
        if (gov.contains("check_baselines") && gov["check_baselines"].is_boolean()) rules_.check_baselines = gov["check_baselines"].get<bool>();
        if (gov.contains("quiet") && gov["quiet"].is_boolean()) rules_.quiet_config = gov["quiet"].get<bool>();
        if (gov.contains("no_color") && gov["no_color"].is_boolean()) rules_.no_color_config = gov["no_color"].get<bool>();
        if (gov.contains("report_json") && gov["report_json"].is_string()) rules_.report_json = gov["report_json"].get<std::string>();
        if (gov.contains("report_sarif") && gov["report_sarif"].is_string()) rules_.report_sarif = gov["report_sarif"].get<std::string>();
        if (gov.contains("report_junit") && gov["report_junit"].is_string()) rules_.report_junit = gov["report_junit"].get<std::string>();
        if (gov.contains("telemetry") && gov["telemetry"].is_string()) rules_.telemetry_path = gov["telemetry"].get<std::string>();
        if (gov.contains("agent_id") && gov["agent_id"].is_string()) rules_.agent_id_config = gov["agent_id"].get<std::string>();
        if (gov.contains("default_env") && gov["default_env"].is_string()) rules_.default_env = gov["default_env"].get<std::string>();
        if (gov.contains("strict_types") && gov["strict_types"].is_boolean()) rules_.strict_types_config = gov["strict_types"].get<bool>();
        if (gov.contains("gc_threshold") && gov["gc_threshold"].is_number_unsigned()) rules_.runtime.gc_threshold = gov["gc_threshold"].get<size_t>();
        if (gov.contains("gc_stats") && gov["gc_stats"].is_boolean()) rules_.runtime.gc_stats = gov["gc_stats"].get<bool>();
        if (gov.contains("sandbox_level") && gov["sandbox_level"].is_string()) rules_.sandbox_level_config = gov["sandbox_level"].get<std::string>();
        if (gov.contains("explanations") && gov["explanations"].is_boolean()) rules_.explanations_enabled = gov["explanations"].get<bool>();
        if (gov.contains("require_override_reason") && gov["require_override_reason"].is_boolean()) rules_.require_override_reason = gov["require_override_reason"].get<bool>();
    }

    // Runtime limits section
    if (j.contains("runtime") && j["runtime"].is_object()) {
        auto& rt = j["runtime"];
        if (rt.contains("timeout") && rt["timeout"].is_number_integer()) rules_.runtime.timeout = rt["timeout"].get<int>();
        if (rt.contains("memory_limit") && rt["memory_limit"].is_number_unsigned()) rules_.runtime.memory_limit = rt["memory_limit"].get<size_t>();
        if (rt.contains("gc_threshold") && rt["gc_threshold"].is_number_unsigned()) rules_.runtime.gc_threshold = rt["gc_threshold"].get<size_t>();
        if (rt.contains("gc_stats") && rt["gc_stats"].is_boolean()) rules_.runtime.gc_stats = rt["gc_stats"].get<bool>();
    }

    // Security section
    if (j.contains("security") && j["security"].is_object()) {
        auto& sec = j["security"];
        if (sec.contains("sandbox_level") && sec["sandbox_level"].is_string()) rules_.sandbox_level_config = sec["sandbox_level"].get<std::string>();
        if (sec.contains("allow_network") && sec["allow_network"].is_boolean()) rules_.allow_network_config = sec["allow_network"].get<bool>();
        if (sec.contains("strict_types") && sec["strict_types"].is_boolean()) rules_.strict_types_config = sec["strict_types"].get<bool>();

        // naab-46: Warn on misplaced keys that have no effect in the security section
        if (sec.contains("blocked_commands")) {
            fprintf(stderr, "[governance] Warning: \"security.blocked_commands\" has no effect — "
                            "use \"capabilities.shell.blocked_commands\" instead\n");
        }
        if (sec.contains("blocked_paths")) {
            fprintf(stderr, "[governance] Warning: \"security.blocked_paths\" has no effect — "
                            "use \"capabilities.filesystem.blocked_paths\" instead\n");
        }
    }

    // API section
    if (j.contains("api") && j["api"].is_object()) {
        auto& api = j["api"];
        if (api.contains("key") && api["key"].is_string()) rules_.api.key = api["key"].get<std::string>();
        if (api.contains("timeout") && api["timeout"].is_number_integer()) rules_.api.timeout = api["timeout"].get<int>();
        if (api.contains("rate_limit") && api["rate_limit"].is_number_integer()) rules_.api.rate_limit = api["rate_limit"].get<int>();
        if (api.contains("max_body") && api["max_body"].is_number_unsigned()) rules_.api.max_body = api["max_body"].get<size_t>();
        if (api.contains("tls_cert") && api["tls_cert"].is_string()) rules_.api.tls_cert_path = api["tls_cert"].get<std::string>();
        if (api.contains("tls_key") && api["tls_key"].is_string()) rules_.api.tls_key_path = api["tls_key"].get<std::string>();
        if (api.contains("keys") && api["keys"].is_array()) {
            for (auto& k : api["keys"]) {
                if (!k.is_object() || !k.contains("key") || !k["key"].is_string()) continue;
                GovernanceRules::ApiKeyEntry entry;
                entry.key = k["key"].get<std::string>();
                if (k.contains("name") && k["name"].is_string()) entry.name = k["name"].get<std::string>();
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
        // naab-29 EO-08: Resolve contradictory config — blocked takes precedence.
        // Record the overlap BEFORE erasing it: the erase is what makes CONTRA-007
        // unable to observe the contradiction it exists to report.
        for (const auto& blocked : rules_.blocked_languages) {
            if (rules_.allowed_languages.erase(blocked) > 0) {
                rules_.languages.allowed_and_blocked.insert(blocked);
            }
        }
    }

    // Capabilities (supports both legacy flat and v3.0 object formats)
    if (j.contains("capabilities")) {
        auto& cap = j["capabilities"];
        if (cap.contains("network") && cap["network"].is_boolean()) {
            if (cap["network"].is_boolean())
                rules_.network_allowed = cap["network"].get<bool>();
            else if (cap["network"].is_object() && cap["network"].contains("enabled") && cap["network"]["enabled"].is_boolean())
                rules_.network_allowed = cap["network"]["enabled"].get<bool>();
        }
        if (cap.contains("filesystem")) {
            if (cap["filesystem"].is_string())
                rules_.filesystem_mode = cap["filesystem"].get<std::string>();
            else if (cap["filesystem"].is_object() && cap["filesystem"].contains("mode") && cap["filesystem"]["mode"].is_string())
                rules_.filesystem_mode = cap["filesystem"]["mode"].get<std::string>();
        }
        if (cap.contains("shell") && cap["shell"].is_boolean()) {
            if (cap["shell"].is_boolean())
                rules_.shell_allowed = cap["shell"].get<bool>();
            else if (cap["shell"].is_object() && cap["shell"].contains("enabled") && cap["shell"]["enabled"].is_boolean())
                rules_.shell_allowed = cap["shell"]["enabled"].get<bool>();
        }
    }

    // Limits (supports both legacy flat and v3.0 nested formats)
    if (j.contains("limits")) {
        auto& lim = j["limits"];
        if (lim.contains("timeout") && lim["timeout"].is_number_integer()) {
            if (lim["timeout"].is_number()) {
                rules_.timeout_seconds = lim["timeout"].get<int>();
                rules_.explicitly_set.insert("timeout_seconds");
            } else if (lim["timeout"].is_object()) {
                if (lim["timeout"].contains("global") && lim["timeout"]["global"].is_number_integer()) {
                    rules_.timeout_seconds = lim["timeout"]["global"].get<int>();
                    rules_.explicitly_set.insert("timeout_seconds");
                    rules_.explicitly_set.insert("limits.timeout.global");
                }
            }
        }
        if (lim.contains("memory") && lim["memory"].is_number_integer()) {
            if (lim["memory"].is_number()) {
                rules_.memory_limit_mb = lim["memory"].get<int>();
                rules_.explicitly_set.insert("memory_limit_mb");
            } else if (lim["memory"].is_object()) {
                if (lim["memory"].contains("total_mb") && lim["memory"]["total_mb"].is_number_integer()) {
                    rules_.memory_limit_mb = lim["memory"]["total_mb"].get<int>();
                    rules_.explicitly_set.insert("memory_limit_mb");
                    rules_.explicitly_set.insert("limits.memory.total_mb");
                }
            }
        }
        if (lim.contains("call_depth") && lim["call_depth"].is_number_integer()) {
            rules_.max_call_depth = lim["call_depth"].get<int>();
            rules_.explicitly_set.insert("max_call_depth");
        }
        if (lim.contains("array_size") && lim["array_size"].is_number_integer()) {
            rules_.max_array_size = lim["array_size"].get<int>();
            rules_.explicitly_set.insert("max_array_size");
        }
        // V3.0 nested paths
        if (lim.contains("execution") && lim["execution"].is_object()) {
            auto& exec = lim["execution"];
            if (exec.contains("call_depth") && exec["call_depth"].is_number_integer())
                rules_.max_call_depth = exec["call_depth"].get<int>();
        }
        if (lim.contains("data") && lim["data"].is_object()) {
            auto& data = lim["data"];
            if (data.contains("array_size") && data["array_size"].is_number_integer())
                rules_.max_array_size = data["array_size"].get<int>();
        }
    }

    // Requirements (supports both legacy string/bool and v3.0 object formats)
    if (j.contains("requirements")) {
        auto& req = j["requirements"];
        if (req.contains("error_handling")) {
            rules_.explicitly_set.insert("require_error_handling");
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
            rules_.explicitly_set.insert("require_main_block");
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
            else if (res["polyglot_output"].is_object() && res["polyglot_output"].contains("format") && res["polyglot_output"]["format"].is_string())
                rules_.polyglot_output = res["polyglot_output"]["format"].get<std::string>();
        }
        if (res.contains("dangerous_calls")) {
            rules_.explicitly_set.insert("restrict_dangerous_calls");
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
            rules_.explicitly_set.insert("no_secrets");
            auto [en, lv] = parseEnforcementLevel(res["no_secrets"]);
            rules_.no_secrets = en; rules_.no_secrets_level = lv;
            rules_.code_quality.no_secrets.enabled = en;
            rules_.code_quality.no_secrets.level = lv;
        }
        if (res.contains("no_placeholders")) {
            rules_.explicitly_set.insert("no_placeholders");
            auto [en, lv] = parseEnforcementLevel(res["no_placeholders"]);
            rules_.no_placeholders = en; rules_.no_placeholders_level = lv;
            rules_.code_quality.no_placeholders.enabled = en;
            rules_.code_quality.no_placeholders.level = lv;
        }
        if (res.contains("no_hardcoded_results")) {
            rules_.explicitly_set.insert("no_hardcoded_results");
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
        auto parseCodeQualityField = [](const nlohmann::json& val, bool& out_enabled,
                                        EnforcementLevel& out_level, const char* name) {
            if (val.is_object()) {
                // An object without "level" enables the check AT HARD, and the
                // inner "enabled" is not read -- so {"enabled": false} on
                // no_secrets turns secret scanning ON as an uncatchable exit-3
                // block, identical to writing true. Verified: block absent exits
                // 0, {"enabled": false} exits 3, true exits 3. Not honoured here
                // for the reason at the restrictions.* block (honouring is a
                // loosening); warned instead.
                warnIgnoredEnableFlag(val, "code_quality", name);
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
            parseCodeQualityField(cq["no_secrets"], rules_.no_secrets, rules_.no_secrets_level, "no_secrets");
        if (cq.contains("no_placeholders"))
            parseCodeQualityField(cq["no_placeholders"], rules_.no_placeholders, rules_.no_placeholders_level, "no_placeholders");
        if (cq.contains("no_hardcoded_results"))
            parseCodeQualityField(cq["no_hardcoded_results"], rules_.no_hardcoded_results, rules_.no_hardcoded_results_level, "no_hardcoded_results");
    }

    // Audit (legacy simple format)
    if (j.contains("audit")) {
        auto& aud = j["audit"];
        if (aud.is_object()) {
            if (aud.contains("level") && aud["level"].is_string())
                rules_.audit_level = aud["level"].get<std::string>();
            if (aud.contains("tamper_evidence") && aud["tamper_evidence"].is_boolean()) {
                if (aud["tamper_evidence"].is_boolean())
                    rules_.tamper_evidence = aud["tamper_evidence"].get<bool>();
            }
        }
    }

    // --- V3.0 Expanded Sections ---
    if (j.contains("version") && j["version"].is_string())
        rules_.version = j["version"].get<std::string>();
    if (j.contains("extends") && j["extends"].is_string())
        rules_.extends_path = j["extends"].get<std::string>();
    if (j.contains("description") && j["description"].is_string())
        rules_.description = j["description"].get<std::string>();

    // V3 Languages: per_language configs
    if (j.contains("languages") && j["languages"].is_object()) {
        auto& lang = j["languages"];
        if (lang.contains("require_explicit") && lang["require_explicit"].is_boolean()) {
            rules_.languages.require_explicit = lang["require_explicit"].get<bool>(); rules_.explicitly_set.insert("languages.require_explicit"); }

        // Sync to new struct too
        rules_.languages.allowed = rules_.allowed_languages;
        rules_.languages.blocked = rules_.blocked_languages;

        if (lang.contains("per_language") && lang["per_language"].is_object()) {
            for (auto& [lang_name, cfg] : lang["per_language"].items()) {
                LanguageConfig lc;
                if (cfg.contains("timeout") && cfg["timeout"].is_number_integer()) lc.timeout = cfg["timeout"].get<int>();
                if (cfg.contains("max_lines") && cfg["max_lines"].is_number_integer()) lc.max_lines = cfg["max_lines"].get<int>();
                if (cfg.contains("max_output_size") && cfg["max_output_size"].is_number_integer()) lc.max_output_size = cfg["max_output_size"].get<int>();
                if (cfg.contains("version_hint") && cfg["version_hint"].is_string()) lc.version_hint = cfg["version_hint"].get<std::string>();

                if (cfg.contains("dangerous_calls")) {
                    auto [en, lv] = parseEnforcementLevel(cfg["dangerous_calls"]);
                    lc.dangerous_calls_enabled = en;
                    lc.dangerous_calls = lv;
                }
                if (cfg.contains("banned_functions")) {
                    for (auto& f : cfg["banned_functions"])
                        if (f.is_string()) lc.banned_functions.push_back(f.get<std::string>());
                }
                if (cfg.contains("banned_globals")) {
                    for (auto& g : cfg["banned_globals"])
                        if (g.is_string()) lc.banned_globals.push_back(g.get<std::string>());
                }
                if (cfg.contains("banned_keywords")) {
                    for (auto& k : cfg["banned_keywords"])
                        if (k.is_string()) lc.banned_keywords.push_back(k.get<std::string>());
                }
                if (cfg.contains("banned_imports")) {
                    for (auto& i : cfg["banned_imports"])
                        if (i.is_string()) lc.banned_imports.push_back(i.get<std::string>());
                }
                if (cfg.contains("banned_namespaces")) {
                    for (auto& n : cfg["banned_namespaces"])
                        if (n.is_string()) lc.banned_namespaces.push_back(n.get<std::string>());
                }
                if (cfg.contains("banned_commands")) {
                    for (auto& c : cfg["banned_commands"])
                        if (c.is_string()) lc.banned_commands.push_back(c.get<std::string>());
                }
                if (cfg.contains("imports") && cfg["imports"].is_object()) {
                    auto& imp = cfg["imports"];
                    if (imp.contains("mode") && imp["mode"].is_string()) lc.imports.mode = imp["mode"].get<std::string>();
                    if (imp.contains("blocked"))
                        for (auto& b : imp["blocked"]) if (b.is_string()) lc.imports.blocked.push_back(b.get<std::string>());
                    if (imp.contains("allowed"))
                        for (auto& a : imp["allowed"]) if (a.is_string()) lc.imports.allowed.push_back(a.get<std::string>());
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
                if (cfg.contains("require_package_main") && cfg["require_package_main"].is_boolean())
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
            if (net.contains("enabled") && net["enabled"].is_boolean()) { nc.enabled = net["enabled"].get<bool>(); rules_.network_allowed = nc.enabled; rules_.explicitly_set.insert("capabilities.network.enabled"); }
            if (net.contains("https_only") && net["https_only"].is_boolean()) nc.https_only = net["https_only"].get<bool>();
            if (net.contains("allowed_hosts"))
                for (auto& h : net["allowed_hosts"]) if (h.is_string()) nc.allowed_hosts.push_back(h.get<std::string>());
            if (net.contains("blocked_hosts"))
                for (auto& h : net["blocked_hosts"]) if (h.is_string()) nc.blocked_hosts.push_back(h.get<std::string>());
            if (net.contains("allowed_ports"))
                for (auto& p : net["allowed_ports"]) { if (p.is_number_integer()) nc.allowed_ports.push_back(p.get<int>()); }
            if (net.contains("allow_websockets") && net["allow_websockets"].is_boolean()) nc.allow_websockets = net["allow_websockets"].get<bool>();
            if (net.contains("allow_raw_sockets") && net["allow_raw_sockets"].is_boolean()) nc.allow_raw_sockets = net["allow_raw_sockets"].get<bool>();
            parseRationale(net, nc.rationale);
        }
        if (cap.contains("filesystem") && cap["filesystem"].is_object()) {
            auto& fs = cap["filesystem"];
            auto& fc = rules_.capabilities.filesystem;
            if (fs.contains("mode") && fs["mode"].is_string()) { fc.mode = fs["mode"].get<std::string>(); rules_.filesystem_mode = fc.mode; rules_.explicitly_set.insert("capabilities.filesystem.mode"); }
            if (fs.contains("allowed_paths"))
                for (auto& p : fs["allowed_paths"]) if (p.is_string()) fc.allowed_paths.push_back(p.get<std::string>());
            if (fs.contains("blocked_paths"))
                for (auto& p : fs["blocked_paths"]) if (p.is_string()) fc.blocked_paths.push_back(p.get<std::string>());
            if (fs.contains("allowed_extensions"))
                for (auto& e : fs["allowed_extensions"]) if (e.is_string()) fc.allowed_extensions.push_back(e.get<std::string>());
            if (fs.contains("max_file_size") && fs["max_file_size"].is_number_integer()) fc.max_file_size = fs["max_file_size"].get<int>();
            if (fs.contains("max_files") && fs["max_files"].is_number_integer()) fc.max_files = fs["max_files"].get<int>();
            if (fs.contains("allow_symlinks") && fs["allow_symlinks"].is_boolean()) fc.allow_symlinks = fs["allow_symlinks"].get<bool>();
            if (fs.contains("allow_hidden_files") && fs["allow_hidden_files"].is_boolean()) fc.allow_hidden_files = fs["allow_hidden_files"].get<bool>();
            if (fs.contains("allow_absolute_paths") && fs["allow_absolute_paths"].is_boolean()) fc.allow_absolute_paths = fs["allow_absolute_paths"].get<bool>();
            parseRationale(fs, fc.rationale);
        }
        if (cap.contains("shell") && cap["shell"].is_object()) {
            auto& sh = cap["shell"];
            auto& sc = rules_.capabilities.shell;
            if (sh.contains("enabled") && sh["enabled"].is_boolean()) { sc.enabled = sh["enabled"].get<bool>(); rules_.shell_allowed = sc.enabled; rules_.explicitly_set.insert("capabilities.shell.enabled"); }
            if (sh.contains("allowed_commands"))
                for (auto& c : sh["allowed_commands"]) if (c.is_string()) sc.allowed_commands.push_back(c.get<std::string>());
            if (sh.contains("blocked_commands"))
                for (auto& c : sh["blocked_commands"]) if (c.is_string()) sc.blocked_commands.push_back(c.get<std::string>());
            if (sh.contains("allow_pipes") && sh["allow_pipes"].is_boolean()) sc.allow_pipes = sh["allow_pipes"].get<bool>();
            if (sh.contains("allow_redirects") && sh["allow_redirects"].is_boolean()) sc.allow_redirects = sh["allow_redirects"].get<bool>();
            if (sh.contains("max_execution_time") && sh["max_execution_time"].is_number_integer()) sc.max_execution_time = sh["max_execution_time"].get<int>();
            parseRationale(sh, sc.rationale);
        }
        if (cap.contains("env_vars") && cap["env_vars"].is_object()) {
            auto& ev = cap["env_vars"];
            auto& ec = rules_.capabilities.env_vars;
            if (ev.contains("read") && ev["read"].is_boolean()) ec.read = ev["read"].get<bool>();
            if (ev.contains("write") && ev["write"].is_boolean()) ec.write = ev["write"].get<bool>();
            if (ev.contains("allowed_read"))
                for (auto& v : ev["allowed_read"]) if (v.is_string()) ec.allowed_read.push_back(v.get<std::string>());
            if (ev.contains("blocked_read"))
                for (auto& v : ev["blocked_read"]) if (v.is_string()) ec.blocked_read.push_back(v.get<std::string>());
            if (ev.contains("allowed_write"))
                for (auto& v : ev["allowed_write"]) if (v.is_string()) ec.allowed_write.push_back(v.get<std::string>());
            if (ev.contains("blocked_write"))
                for (auto& v : ev["blocked_write"]) if (v.is_string()) ec.blocked_write.push_back(v.get<std::string>());
            // V-SC-006-ext: Polyglot subprocess environment scrubbing
            if (ev.contains("subprocess_scrub_mode") && ev["subprocess_scrub_mode"].is_string())
                ec.subprocess_scrub_mode = ev["subprocess_scrub_mode"].get<std::string>();
            if (ev.contains("blocked_subprocess_prefixes"))
                for (auto& v : ev["blocked_subprocess_prefixes"]) if (v.is_string()) ec.blocked_subprocess_prefixes.push_back(v.get<std::string>());
            if (ev.contains("blocked_subprocess_vars"))
                for (auto& v : ev["blocked_subprocess_vars"]) if (v.is_string()) ec.blocked_subprocess_vars.push_back(v.get<std::string>());
            if (ev.contains("allowed_subprocess_vars"))
                for (auto& v : ev["allowed_subprocess_vars"]) if (v.is_string()) ec.allowed_subprocess_vars.push_back(v.get<std::string>());
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
            if (t.contains("global") && t["global"].is_number_integer()) { rules_.limits.timeout.global = t["global"].get<int>(); rules_.timeout_seconds = rules_.limits.timeout.global; rules_.runtime.timeout = rules_.limits.timeout.global; rules_.explicitly_set.insert("limits.timeout.global"); rules_.explicitly_set.insert("timeout_seconds"); }
            if (t.contains("per_block") && t["per_block"].is_number_integer()) rules_.limits.timeout.per_block = t["per_block"].get<int>();
            if (t.contains("total_polyglot") && t["total_polyglot"].is_number_integer()) { rules_.limits.timeout.total_polyglot = t["total_polyglot"].get<int>(); rules_.explicitly_set.insert("limits.timeout.total_polyglot"); }
        }
        if (lim.contains("memory") && lim["memory"].is_object()) {
            auto& m = lim["memory"];
            if (m.contains("per_block_mb") && m["per_block_mb"].is_number_integer()) { rules_.limits.memory.per_block_mb = m["per_block_mb"].get<int>(); rules_.explicitly_set.insert("limits.memory.per_block_mb"); }
            if (m.contains("total_mb") && m["total_mb"].is_number_integer()) { rules_.limits.memory.total_mb = m["total_mb"].get<int>(); rules_.memory_limit_mb = rules_.limits.memory.total_mb; rules_.explicitly_set.insert("limits.memory.total_mb"); rules_.explicitly_set.insert("memory_limit_mb"); }
        }
        if (lim.contains("execution") && lim["execution"].is_object()) {
            auto& e = lim["execution"];
            if (e.contains("call_depth") && e["call_depth"].is_number_integer()) { rules_.limits.execution.call_depth = e["call_depth"].get<int>(); rules_.max_call_depth = rules_.limits.execution.call_depth; rules_.explicitly_set.insert("limits.execution.call_depth"); rules_.explicitly_set.insert("max_call_depth"); }
            if (e.contains("loop_iterations") && e["loop_iterations"].is_number_integer()) { rules_.limits.execution.loop_iterations = e["loop_iterations"].get<int>(); rules_.explicitly_set.insert("limits.execution.loop_iterations"); }
            if (e.contains("polyglot_blocks") && e["polyglot_blocks"].is_number_integer()) { rules_.limits.execution.polyglot_blocks = e["polyglot_blocks"].get<int>(); rules_.explicitly_set.insert("limits.execution.polyglot_blocks"); }
            if (e.contains("parallel_blocks") && e["parallel_blocks"].is_number_integer()) rules_.limits.execution.parallel_blocks = e["parallel_blocks"].get<int>();
            if (e.contains("total_executions") && e["total_executions"].is_number_integer()) rules_.limits.execution.total_executions = e["total_executions"].get<int>();
            // limits.execution.timeout_seconds is the canonical key for overall script timeout.
            // Wire it to runtime.timeout so main.cpp enforcement picks it up.
            if (e.contains("timeout_seconds") && e["timeout_seconds"].is_number_integer()) { rules_.runtime.timeout = e["timeout_seconds"].get<int>(); }
        }
        if (lim.contains("data") && lim["data"].is_object()) {
            auto& d = lim["data"];

            // limits.data.string_length / nesting_depth / dict_size are parsed,
            // recorded in explicitly_set (so they take part in the ratchet and in
            // inheritance) and clamped -- and then read by nothing. Their only
            // readers are GovernanceEngine::checkStringLength / checkNestingDepth /
            // checkDictSize, which have zero call sites anywhere in src/: defined
            // once, declared once in governance.h, never invoked. So an operator
            // can set a HARD data limit, see it survive validation, and get no
            // enforcement at all.
            //
            // NOT warned, because they are live and would be false alarms:
            //   * output_size    -- read at polyglot.cpp:718, which enforces it
            //                       directly (as a plain runtime_error rather than
            //                       through enforce(), so it produces no finding or
            //                       telemetry -- a separate gap, tracked in
            //                       docs/open-investigations.md, not fixed here).
            //   * array_size     -- mirrored to rules_.max_array_size, which has
            //                       live consumers.
            //   * max_json_depth -- calls setMaxJsonDepth(), read by json_impl.cpp.
            //
            // That last one is why nesting_depth's message names it: the two
            // spellings are documented as the same setting and behave differently,
            // so "your key does nothing" is only half the help an operator needs.
            //
            // The checks are deliberately NOT wired in instead. They enforce at
            // HARD (exit 3, uncatchable), and 32 config sites in this repo already
            // set these keys -- including both govern-template.json copies and
            // tests/gorilla/naab-32/phases/phase1-hardening.json, which sets
            // dict_size 50 and nesting_depth 8, tight enough to block ordinary
            // data. Wiring them in would start hard-blocking configs that pass
            // today, which is a behaviour change wearing a bug fix's clothes.
            auto warnInertLimit = [&d](const char* key, const char* extra) {
                if (d.contains(key) && d[key].is_number_integer()) {
                    fprintf(stderr,
                            "[governance] Warning: \"limits.data.%s\" is parsed but not enforced — "
                            "no check reads it.%s\n", key, extra);
                }
            };
            warnInertLimit("string_length", "");
            warnInertLimit("dict_size", "");
            warnInertLimit("nesting_depth",
                           " Use \"limits.data.max_json_depth\" for JSON parse depth,"
                           " which is enforced.");

            if (d.contains("array_size") && d["array_size"].is_number_integer()) { rules_.limits.data.array_size = d["array_size"].get<int>(); rules_.max_array_size = rules_.limits.data.array_size; rules_.explicitly_set.insert("limits.data.array_size"); rules_.explicitly_set.insert("max_array_size"); }
            if (d.contains("dict_size") && d["dict_size"].is_number_integer()) { rules_.limits.data.dict_size = d["dict_size"].get<int>(); rules_.explicitly_set.insert("limits.data.dict_size"); }
            if (d.contains("string_length") && d["string_length"].is_number_integer()) { rules_.limits.data.string_length = d["string_length"].get<int>(); rules_.explicitly_set.insert("limits.data.string_length"); }
            if (d.contains("nesting_depth") && d["nesting_depth"].is_number_integer()) { rules_.limits.data.nesting_depth = d["nesting_depth"].get<int>(); rules_.explicitly_set.insert("limits.data.nesting_depth"); }
            if (d.contains("max_json_depth") && d["max_json_depth"].is_number_integer()) {
                int jd = d["max_json_depth"].get<int>();
                rules_.limits.data.nesting_depth = jd;  // alias: max_json_depth -> nesting_depth
                naab::limits::setMaxJsonDepth(jd);
            }
            if (d.contains("output_size") && d["output_size"].is_number_integer()) { rules_.limits.data.output_size = d["output_size"].get<int>(); rules_.explicitly_set.insert("limits.data.output_size"); }
            if (d.contains("input_size") && d["input_size"].is_number_integer()) { rules_.limits.data.input_size = d["input_size"].get<int>(); rules_.explicitly_set.insert("limits.data.input_size"); }
        }
        if (lim.contains("code") && lim["code"].is_object()) {
            auto& c = lim["code"];
            if (c.contains("max_lines_per_block") && c["max_lines_per_block"].is_number_integer()) { rules_.limits.code.max_lines_per_block = c["max_lines_per_block"].get<int>(); rules_.explicitly_set.insert("limits.code.max_lines_per_block"); }
            if (c.contains("max_total_polyglot_lines") && c["max_total_polyglot_lines"].is_number_integer()) { rules_.limits.code.max_total_polyglot_lines = c["max_total_polyglot_lines"].get<int>(); rules_.explicitly_set.insert("limits.code.max_total_polyglot_lines"); }
            if (c.contains("max_nesting_depth") && c["max_nesting_depth"].is_number_integer()) { rules_.limits.code.max_nesting_depth = c["max_nesting_depth"].get<int>(); rules_.explicitly_set.insert("limits.code.max_nesting_depth"); }
        }
        if (lim.contains("rate") && lim["rate"].is_object()) {
            auto& r = lim["rate"];
            if (r.contains("max_polyglot_per_second") && r["max_polyglot_per_second"].is_number_integer()) { rules_.limits.rate.max_polyglot_per_second = r["max_polyglot_per_second"].get<int>(); rules_.explicitly_set.insert("limits.rate.max_polyglot_per_second"); }
            if (r.contains("max_stdlib_calls_per_second") && r["max_stdlib_calls_per_second"].is_number_integer()) { rules_.limits.rate.max_stdlib_calls_per_second = r["max_stdlib_calls_per_second"].get<int>(); rules_.explicitly_set.insert("limits.rate.max_stdlib_calls_per_second"); }
            if (r.contains("max_file_ops_per_second") && r["max_file_ops_per_second"].is_number_integer()) { rules_.limits.rate.max_file_ops_per_second = r["max_file_ops_per_second"].get<int>(); rules_.explicitly_set.insert("limits.rate.max_file_ops_per_second"); }
            if (r.contains("cooldown_on_limit_ms") && r["cooldown_on_limit_ms"].is_number_integer()) { rules_.limits.rate.cooldown_on_limit_ms = r["cooldown_on_limit_ms"].get<int>(); rules_.explicitly_set.insert("limits.rate.cooldown_on_limit_ms"); }
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
            warnEnableNeedsLevel(mb, "requirements", "main_block");
            rules_.explicitly_set.insert("requirements.main_block");
            if (mb.contains("level")) {
                auto [en, lv] = parseEnforcementLevel(mb["level"]);
                rules_.requirements.main_block.enabled = true;
                rules_.requirements.main_block.level = lv;
                rules_.require_main_block = true;
                rules_.main_block_level = lv;
            }
            if (mb.contains("message") && mb["message"].is_string()) rules_.requirements.main_block.message = mb["message"].get<std::string>();
            parseRationale(mb, rules_.requirements.main_block.rationale);
        }
        if (req.contains("error_handling") && req["error_handling"].is_object()) {
            auto& eh = req["error_handling"];
            warnEnableNeedsLevel(eh, "requirements", "error_handling");
            rules_.explicitly_set.insert("requirements.error_handling");
            if (eh.contains("level")) {
                auto [en, lv] = parseEnforcementLevel(eh["level"]);
                rules_.requirements.error_handling.enabled = true;
                rules_.requirements.error_handling.level = lv;
                rules_.require_error_handling = true;
                rules_.error_handling_level = lv;
            }
            if (eh.contains("require_try_catch") && eh["require_try_catch"].is_boolean()) rules_.requirements.error_handling.require_try_catch = eh["require_try_catch"].get<bool>();
            if (eh.contains("require_catch_body") && eh["require_catch_body"].is_boolean()) rules_.requirements.error_handling.require_catch_body = eh["require_catch_body"].get<bool>();
            parseRationale(eh, rules_.requirements.error_handling.rationale);
        }
        if (req.contains("naming_conventions") && req["naming_conventions"].is_object()) {
            auto& nc = req["naming_conventions"];
            warnIgnoredEnableFlag(nc, "requirements", "naming_conventions");
            rules_.requirements.naming_conventions.enabled = true;
            rules_.explicitly_set.insert("requirements.naming_conventions");
            if (nc.contains("level")) { auto [en, lv] = parseEnforcementLevel(nc["level"]); rules_.requirements.naming_conventions.level = lv; }
            if (nc.contains("variables") && nc["variables"].is_string()) rules_.requirements.naming_conventions.variables = nc["variables"].get<std::string>();
            if (nc.contains("functions") && nc["functions"].is_string()) rules_.requirements.naming_conventions.functions = nc["functions"].get<std::string>();
            if (nc.contains("check_naab_code") && nc["check_naab_code"].is_boolean()) rules_.requirements.naming_conventions.check_naab_code = nc["check_naab_code"].get<bool>();
            if (nc.contains("check_polyglot_code") && nc["check_polyglot_code"].is_boolean()) rules_.requirements.naming_conventions.check_polyglot_code = nc["check_polyglot_code"].get<bool>();
        }
    }

    // V3 Restrictions (expanded objects)
    if (j.contains("restrictions") && j["restrictions"].is_object()) {
        auto& res = j["restrictions"];

        // Six of the twelve restrictions.* sub-blocks enable themselves from the
        // mere PRESENCE of the block and never read "enabled" at all, so writing
        //     "restrictions": {"crypto": {"enabled": false}}
        // does not disable the check -- it turns it ON, identically to writing
        // true. The only way to leave one off is to omit the block. The other
        // five (vcs_secret_extraction, obfuscation, data_exfiltration,
        // resource_abuse, information_disclosure) DO read the key and disable
        // properly, so the same JSON means opposite things depending on which
        // sibling it is written under, and an operator cannot learn the rule
        // from one example.
        //
        // The value is deliberately NOT honoured here. Making all twelve read
        // the key is a loosening: every config carrying "enabled": false today
        // is being enforced, and would silently stop being enforced on upgrade.
        // That is the same trade rejected for default-on secret scanning, in
        // the other direction. What is actually wrong is the silence -- the
        // operator believes they turned something off -- so the silence is what
        // gets fixed, additively, with no change to what is enforced.
        auto warnIgnoredEnable = [](const nlohmann::json& blk, const char* name) {
            if (blk.contains("enabled") && blk["enabled"].is_boolean() &&
                !blk["enabled"].get<bool>()) {
                fprintf(stderr,
                        "[governance] Warning: \"restrictions.%s.enabled\": false has no effect — "
                        "this check is enabled by the presence of its block. "
                        "Remove the \"%s\" block to leave it disabled.\n",
                        name, name);
            }
        };

        if (res.contains("polyglot_output") && res["polyglot_output"].is_object()) {
            auto& po = res["polyglot_output"];
            if (po.contains("format") && po["format"].is_string()) { rules_.restrictions.polyglot_output.format = po["format"].get<std::string>(); rules_.polyglot_output = rules_.restrictions.polyglot_output.format; rules_.explicitly_set.insert("restrictions.polyglot_output.format"); }
            if (po.contains("max_size") && po["max_size"].is_number_integer()) { rules_.restrictions.polyglot_output.max_size = po["max_size"].get<int>(); rules_.explicitly_set.insert("restrictions.polyglot_output.max_size"); }
            if (po.contains("validate_json") && po["validate_json"].is_boolean()) { rules_.restrictions.polyglot_output.validate_json = po["validate_json"].get<bool>(); rules_.explicitly_set.insert("restrictions.polyglot_output.validate_json"); }
            if (po.contains("require_structured") && po["require_structured"].is_boolean()) { rules_.restrictions.polyglot_output.require_structured = po["require_structured"].get<bool>(); rules_.explicitly_set.insert("restrictions.polyglot_output.require_structured"); }
        }
        if (res.contains("dangerous_calls") && res["dangerous_calls"].is_object()) {
            auto& dc = res["dangerous_calls"];
            warnIgnoredEnable(dc, "dangerous_calls");
            rules_.restrictions.dangerous_calls.enabled = true;
            rules_.explicitly_set.insert("restrictions.dangerous_calls");
            rules_.restrict_dangerous_calls = true;
            if (dc.contains("level")) { auto [en, lv] = parseEnforcementLevel(dc["level"]); rules_.restrictions.dangerous_calls.level = lv; rules_.dangerous_calls_level = lv; }
            if (dc.contains("allowlist")) for (auto& a : dc["allowlist"]) if (a.is_string()) rules_.restrictions.dangerous_calls.allowlist.push_back(a.get<std::string>());
            if (dc.contains("blocklist_extra")) for (auto& b : dc["blocklist_extra"]) if (b.is_string()) rules_.restrictions.dangerous_calls.blocklist_extra.push_back(b.get<std::string>());
            parseRationale(dc, rules_.restrictions.dangerous_calls.rationale);
        }
        if (res.contains("shell_injection") && res["shell_injection"].is_object()) {
            auto& si = res["shell_injection"];
            warnIgnoredEnable(si, "shell_injection");
            rules_.restrictions.shell_injection.enabled = true;
            rules_.explicitly_set.insert("restrictions.shell_injection");
            if (si.contains("level")) { auto [en, lv] = parseEnforcementLevel(si["level"]); rules_.restrictions.shell_injection.level = lv; }
            if (si.contains("patterns")) for (auto& p : si["patterns"]) if (p.is_string()) rules_.restrictions.shell_injection.patterns.push_back(p.get<std::string>());
            parseRationale(si, rules_.restrictions.shell_injection.rationale);
        }
        if (res.contains("privilege_escalation") && res["privilege_escalation"].is_object()) {
            auto& pe = res["privilege_escalation"];
            warnIgnoredEnable(pe, "privilege_escalation");
            rules_.restrictions.privilege_escalation.enabled = true;
            rules_.explicitly_set.insert("restrictions.privilege_escalation");
            if (pe.contains("level")) { auto [en, lv] = parseEnforcementLevel(pe["level"]); rules_.restrictions.privilege_escalation.level = lv; }
            if (pe.contains("block_sudo") && pe["block_sudo"].is_boolean()) rules_.restrictions.privilege_escalation.block_sudo = pe["block_sudo"].get<bool>();
            if (pe.contains("block_su") && pe["block_su"].is_boolean()) rules_.restrictions.privilege_escalation.block_su = pe["block_su"].get<bool>();
            parseRationale(pe, rules_.restrictions.privilege_escalation.rationale);
        }
        if (res.contains("code_injection") && res["code_injection"].is_object()) {
            auto& ci = res["code_injection"];
            warnIgnoredEnable(ci, "code_injection");
            rules_.restrictions.code_injection.enabled = true;
            rules_.explicitly_set.insert("restrictions.code_injection");
            if (ci.contains("level")) { auto [en, lv] = parseEnforcementLevel(ci["level"]); rules_.restrictions.code_injection.level = lv; }
            if (ci.contains("block_dynamic_code_gen") && ci["block_dynamic_code_gen"].is_boolean()) rules_.restrictions.code_injection.block_dynamic_code_gen = ci["block_dynamic_code_gen"].get<bool>();
            if (ci.contains("block_sql_injection_patterns") && ci["block_sql_injection_patterns"].is_boolean()) rules_.restrictions.code_injection.block_sql_injection_patterns = ci["block_sql_injection_patterns"].get<bool>();
            if (ci.contains("block_command_injection") && ci["block_command_injection"].is_boolean()) rules_.restrictions.code_injection.block_command_injection = ci["block_command_injection"].get<bool>();
            if (ci.contains("block_template_injection") && ci["block_template_injection"].is_boolean()) rules_.restrictions.code_injection.block_template_injection = ci["block_template_injection"].get<bool>();
            if (ci.contains("block_xpath_injection") && ci["block_xpath_injection"].is_boolean()) rules_.restrictions.code_injection.block_xpath_injection = ci["block_xpath_injection"].get<bool>();
            if (ci.contains("block_ldap_injection") && ci["block_ldap_injection"].is_boolean()) rules_.restrictions.code_injection.block_ldap_injection = ci["block_ldap_injection"].get<bool>();
            parseRationale(ci, rules_.restrictions.code_injection.rationale);
        }
        if (res.contains("crypto") && res["crypto"].is_object()) {
            auto& cr = res["crypto"];
            warnIgnoredEnable(cr, "crypto");
            rules_.restrictions.crypto.enabled = true;
            rules_.explicitly_set.insert("restrictions.crypto");
            if (cr.contains("level")) { auto [en, lv] = parseEnforcementLevel(cr["level"]); rules_.restrictions.crypto.level = lv; }
            if (cr.contains("weak_hashes")) for (auto& h : cr["weak_hashes"]) if (h.is_string()) rules_.restrictions.crypto.weak_hashes.push_back(h.get<std::string>());
            if (cr.contains("weak_ciphers")) for (auto& c : cr["weak_ciphers"]) if (c.is_string()) rules_.restrictions.crypto.weak_ciphers.push_back(c.get<std::string>());
            parseRationale(cr, rules_.restrictions.crypto.rationale);
        }
        if (res.contains("vcs_secret_extraction") && res["vcs_secret_extraction"].is_object()) {
            auto& vs = res["vcs_secret_extraction"];
            rules_.explicitly_set.insert("restrictions.vcs_secret_extraction");
            if (vs.contains("enabled") && vs["enabled"].is_boolean()) rules_.restrictions.vcs_secret_extraction.enabled = vs["enabled"].get<bool>();
            if (vs.contains("level")) { auto [en, lv] = parseEnforcementLevel(vs["level"]); rules_.restrictions.vcs_secret_extraction.level = lv; }
            parseRationale(vs, rules_.restrictions.vcs_secret_extraction.rationale);
        }
        if (res.contains("obfuscation") && res["obfuscation"].is_object()) {
            auto& ob = res["obfuscation"];
            rules_.explicitly_set.insert("restrictions.obfuscation");
            if (ob.contains("enabled") && ob["enabled"].is_boolean()) rules_.restrictions.obfuscation.enabled = ob["enabled"].get<bool>();
            if (ob.contains("level")) { auto [en, lv] = parseEnforcementLevel(ob["level"]); rules_.restrictions.obfuscation.level = lv; }
            parseRationale(ob, rules_.restrictions.obfuscation.rationale);
        }
        if (res.contains("imports") && res["imports"].is_object()) {
            auto& im = res["imports"];
            warnIgnoredEnable(im, "imports");
            rules_.restrictions.imports.enabled = true;
            rules_.explicitly_set.insert("restrictions.imports");
            if (im.contains("level")) { auto [en, lv] = parseEnforcementLevel(im["level"]); rules_.restrictions.imports.level = lv; }
            if (im.contains("mode") && im["mode"].is_string()) rules_.restrictions.imports.mode = im["mode"].get<std::string>();
            if (im.contains("blocked") && im["blocked"].is_object())
                for (auto& [lang, arr] : im["blocked"].items())
                    for (auto& v : arr) if (v.is_string()) rules_.restrictions.imports.blocked[lang].push_back(v.get<std::string>());
            if (im.contains("allowed") && im["allowed"].is_object())
                for (auto& [lang, arr] : im["allowed"].items())
                    for (auto& v : arr) if (v.is_string()) rules_.restrictions.imports.allowed[lang].push_back(v.get<std::string>());
            parseRationale(im, rules_.restrictions.imports.rationale);
        }
        if (res.contains("data_exfiltration") && res["data_exfiltration"].is_object()) {
            auto& de = res["data_exfiltration"];
            rules_.explicitly_set.insert("restrictions.data_exfiltration");
            if (de.contains("enabled") && de["enabled"].is_boolean()) rules_.restrictions.data_exfiltration.enabled = de["enabled"].get<bool>();
            if (de.contains("level")) { auto [en, lv] = parseEnforcementLevel(de["level"]); rules_.restrictions.data_exfiltration.level = lv; }
            if (de.contains("block_base64_encode_secrets") && de["block_base64_encode_secrets"].is_boolean()) rules_.restrictions.data_exfiltration.block_base64_encode_secrets = de["block_base64_encode_secrets"].get<bool>();
            if (de.contains("block_hex_encode_secrets") && de["block_hex_encode_secrets"].is_boolean()) rules_.restrictions.data_exfiltration.block_hex_encode_secrets = de["block_hex_encode_secrets"].get<bool>();
            if (de.contains("block_network_exfil") && de["block_network_exfil"].is_boolean()) rules_.restrictions.data_exfiltration.block_network_exfil = de["block_network_exfil"].get<bool>();
            if (de.contains("block_socket_exfil") && de["block_socket_exfil"].is_boolean()) rules_.restrictions.data_exfiltration.block_socket_exfil = de["block_socket_exfil"].get<bool>();
            if (de.contains("block_encoding_chains") && de["block_encoding_chains"].is_boolean()) rules_.restrictions.data_exfiltration.block_encoding_chains = de["block_encoding_chains"].get<bool>();
            if (de.contains("patterns") && de["patterns"].is_array()) {
                for (const auto& p : de["patterns"]) {
                    if (p.is_string()) rules_.restrictions.data_exfiltration.patterns.push_back(p.get<std::string>());
                }
            }
            parseRationale(de, rules_.restrictions.data_exfiltration.rationale);
        }
        if (res.contains("resource_abuse") && res["resource_abuse"].is_object()) {
            auto& ra = res["resource_abuse"];
            rules_.explicitly_set.insert("restrictions.resource_abuse");
            if (ra.contains("enabled") && ra["enabled"].is_boolean()) rules_.restrictions.resource_abuse.enabled = ra["enabled"].get<bool>();
            if (ra.contains("level")) { auto [en, lv] = parseEnforcementLevel(ra["level"]); rules_.restrictions.resource_abuse.level = lv; }
            if (ra.contains("block_fork_bomb") && ra["block_fork_bomb"].is_boolean()) rules_.restrictions.resource_abuse.block_fork_bomb = ra["block_fork_bomb"].get<bool>();
            if (ra.contains("block_disk_filling") && ra["block_disk_filling"].is_boolean()) rules_.restrictions.resource_abuse.block_disk_filling = ra["block_disk_filling"].get<bool>();
            parseRationale(ra, rules_.restrictions.resource_abuse.rationale);
        }
        if (res.contains("information_disclosure") && res["information_disclosure"].is_object()) {
            auto& id = res["information_disclosure"];
            rules_.explicitly_set.insert("restrictions.information_disclosure");
            if (id.contains("enabled") && id["enabled"].is_boolean()) rules_.restrictions.information_disclosure.enabled = id["enabled"].get<bool>();
            if (id.contains("level")) { auto [en, lv] = parseEnforcementLevel(id["level"]); rules_.restrictions.information_disclosure.level = lv; }
            if (id.contains("block_env_dump") && id["block_env_dump"].is_boolean()) rules_.restrictions.information_disclosure.block_env_dump = id["block_env_dump"].get<bool>();
            if (id.contains("block_process_listing") && id["block_process_listing"].is_boolean()) rules_.restrictions.information_disclosure.block_process_listing = id["block_process_listing"].get<bool>();
            if (id.contains("block_system_info_leak") && id["block_system_info_leak"].is_boolean()) rules_.restrictions.information_disclosure.block_system_info_leak = id["block_system_info_leak"].get<bool>();
            parseRationale(id, rules_.restrictions.information_disclosure.rationale);
        }
    }

    // V3 Code Quality (expanded objects with per-check configs)
    if (j.contains("code_quality") && j["code_quality"].is_object()) {
        auto& cq = j["code_quality"];

        // no_secrets (expanded)
        if (cq.contains("no_secrets") && cq["no_secrets"].is_object()) { rules_.explicitly_set.insert("code_quality.no_secrets");
            auto& ns = cq["no_secrets"];
            rules_.code_quality.no_secrets.enabled = true;
            rules_.no_secrets = true;
            if (ns.contains("level")) { auto [en, lv] = parseEnforcementLevel(ns["level"]); rules_.code_quality.no_secrets.level = lv; rules_.no_secrets_level = lv; }
            if (ns.contains("allowlist")) for (auto& a : ns["allowlist"]) if (a.is_string()) rules_.code_quality.no_secrets.allowlist.push_back(a.get<std::string>());
            if (ns.contains("entropy_check") && ns["entropy_check"].is_object()) {
                auto& ec = ns["entropy_check"];
                rules_.code_quality.no_secrets.entropy_check.enabled = true;
                if (ec.contains("threshold") && ec["threshold"].is_number()) rules_.code_quality.no_secrets.entropy_check.threshold = ec["threshold"].get<double>();
                if (ec.contains("min_length") && ec["min_length"].is_number_integer()) rules_.code_quality.no_secrets.entropy_check.min_length = ec["min_length"].get<int>();
            }
            if (ns.contains("suspicious_variable_names") && ns["suspicious_variable_names"].is_object()) {
                auto& sv = ns["suspicious_variable_names"];
                if (sv.contains("enabled") && sv["enabled"].is_boolean()) rules_.code_quality.no_secrets.suspicious_variable_names.enabled = sv["enabled"].get<bool>();
                if (sv.contains("names")) for (auto& n : sv["names"]) if (n.is_string()) rules_.code_quality.no_secrets.suspicious_variable_names.names.push_back(n.get<std::string>());
            }
            parseRationale(ns, rules_.code_quality.no_secrets.rationale);
        }

        // no_placeholders (expanded)
        if (cq.contains("no_placeholders") && cq["no_placeholders"].is_object()) { rules_.explicitly_set.insert("code_quality.no_placeholders");
            auto& np = cq["no_placeholders"];
            rules_.code_quality.no_placeholders.enabled = true;
            rules_.no_placeholders = true;
            if (np.contains("level")) { auto [en, lv] = parseEnforcementLevel(np["level"]); rules_.code_quality.no_placeholders.level = lv; rules_.no_placeholders_level = lv; }
            if (np.contains("markers")) { rules_.code_quality.no_placeholders.markers.clear(); for (auto& m : np["markers"]) if (m.is_string()) rules_.code_quality.no_placeholders.markers.push_back(m.get<std::string>()); }
            if (np.contains("custom_markers")) for (auto& m : np["custom_markers"]) if (m.is_string()) rules_.code_quality.no_placeholders.custom_markers.push_back(m.get<std::string>());
            if (np.contains("case_sensitive") && np["case_sensitive"].is_boolean()) rules_.code_quality.no_placeholders.case_sensitive = np["case_sensitive"].get<bool>();
            parseRationale(np, rules_.code_quality.no_placeholders.rationale);
        }

        // New code quality checks
        auto loadSimpleCheck = [&](const std::string& key, auto& config) {
            if (cq.contains(key)) {
                rules_.explicitly_set.insert("code_quality." + key);
                if (cq[key].is_boolean() || cq[key].is_string()) {
                    auto [en, lv] = parseEnforcementLevel(cq[key]);
                    config.enabled = en;
                    config.level = lv;
                } else if (cq[key].is_object()) {
                    // Presence of an object means "configure and enable", and an
                    // explicit `enabled` inside it is NOT read -- so
                    // `{"enabled": false}` turns the check ON, the inverse of what
                    // the operator wrote. Both copies of govern-template.json ship
                    // `no_hardcoded_urls: {"enabled": false}` and
                    // `no_hardcoded_ips: {"enabled": false}`, so the shipped
                    // template enables the two checks it documents as off.
                    //
                    // The value is deliberately NOT honoured, for the reason given
                    // at the restrictions.* block above: honouring it is a
                    // LOOSENING -- every config carrying "enabled": false today is
                    // being enforced and would silently stop on upgrade. This was
                    // briefly fixed the other way, which disabled two checks in
                    // both templates and made the same JSON mean opposite things in
                    // code_quality and restrictions -- the exact confusion that
                    // comment exists to complain about. The silence is the defect;
                    // the silence is what gets fixed.
                    config.enabled = true;
                    auto& obj = cq[key];
                    warnIgnoredEnableFlag(obj, "code_quality", key.c_str());
                    if (obj.contains("level")) { auto [en, lv] = parseEnforcementLevel(obj["level"]); config.level = lv; }
                    if (obj.contains("patterns"))
                        for (auto& p : obj["patterns"]) if (p.is_string()) config.patterns.push_back(p.get<std::string>());
                    if (obj.contains("custom_patterns"))
                        for (auto& p : obj["custom_patterns"]) if (p.is_string()) config.patterns.push_back(p.get<std::string>());
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
        if (cq.contains("semantic_checks")) { rules_.explicitly_set.insert("code_quality.semantic_checks");
            if (cq["semantic_checks"].is_boolean() || cq["semantic_checks"].is_string()) {
                auto [en, lv] = parseEnforcementLevel(cq["semantic_checks"]);
                rules_.code_quality.semantic_checks.enabled = en;
                rules_.code_quality.semantic_checks.level = lv;
            } else if (cq["semantic_checks"].is_object()) {
                auto& sc = cq["semantic_checks"];
                rules_.code_quality.semantic_checks.enabled = true;
                if (sc.contains("level")) { auto [en, lv] = parseEnforcementLevel(sc["level"]); rules_.code_quality.semantic_checks.level = lv; }
                if (sc.contains("check_imports") && sc["check_imports"].is_boolean()) rules_.code_quality.semantic_checks.check_imports = sc["check_imports"].get<bool>();
                if (sc.contains("check_api_signatures") && sc["check_api_signatures"].is_boolean()) rules_.code_quality.semantic_checks.check_api_signatures = sc["check_api_signatures"].get<bool>();
                if (sc.contains("check_shell_syntax") && sc["check_shell_syntax"].is_boolean()) rules_.code_quality.semantic_checks.check_shell_syntax = sc["check_shell_syntax"].get<bool>();
                if (sc.contains("check_dangerous_eval") && sc["check_dangerous_eval"].is_boolean()) rules_.code_quality.semantic_checks.check_dangerous_eval = sc["check_dangerous_eval"].get<bool>();
                parseRationale(sc, rules_.code_quality.semantic_checks.rationale);
            }
        }

        // intent_validation
        if (cq.contains("intent_validation") && cq["intent_validation"].is_object()) {
            auto& iv = cq["intent_validation"];
            rules_.code_quality.intent_validation.enabled = true;
            rules_.explicitly_set.insert("code_quality.intent_validation");
            if (iv.contains("enabled") && iv["enabled"].is_boolean()) rules_.code_quality.intent_validation.enabled = iv["enabled"].get<bool>();
            if (iv.contains("required") && iv["required"].is_boolean()) rules_.code_quality.intent_validation.required = iv["required"].get<bool>();
            if (iv.contains("level")) { auto [en, lv] = parseEnforcementLevel(iv["level"]); rules_.code_quality.intent_validation.level = lv; }
            if (iv.contains("missing_level")) { auto [en, lv] = parseEnforcementLevel(iv["missing_level"]); rules_.code_quality.intent_validation.missing_level = lv; }
            if (iv.contains("mode") && iv["mode"].is_string()) rules_.code_quality.intent_validation.mode = iv["mode"].get<std::string>();
            if (iv.contains("min_function_lines") && iv["min_function_lines"].is_number_integer()) rules_.code_quality.intent_validation.min_function_lines = iv["min_function_lines"].get<int>();
            if (iv.contains("exempt_functions")) {
                for (auto& f : iv["exempt_functions"])
                    if (f.is_string()) rules_.code_quality.intent_validation.exempt_functions.push_back(f.get<std::string>());
            }
            if (iv.contains("project_intent") && iv["project_intent"].is_string())
                rules_.code_quality.intent_validation.project_intent = iv["project_intent"].get<std::string>();
            if (iv.contains("function_intents") && iv["function_intents"].is_object()) {
                for (auto& [name, intent] : iv["function_intents"].items())
                    rules_.code_quality.intent_validation.function_intents[name] = (intent.is_string() ? intent.get<std::string>() : std::string());
            }
            parseRationale(iv, rules_.code_quality.intent_validation.rationale);
        }

        // no_pii
        if (cq.contains("no_pii")) { rules_.explicitly_set.insert("code_quality.no_pii");
            if (cq["no_pii"].is_boolean() || cq["no_pii"].is_string()) {
                auto [en, lv] = parseEnforcementLevel(cq["no_pii"]);
                rules_.code_quality.no_pii.enabled = en;
                rules_.code_quality.no_pii.level = lv;
            } else if (cq["no_pii"].is_object()) {
                auto& pii = cq["no_pii"];
                rules_.code_quality.no_pii.enabled = true;
                warnIgnoredEnableFlag(pii, "code_quality", "no_pii");
                if (pii.contains("level")) { auto [en, lv] = parseEnforcementLevel(pii["level"]); rules_.code_quality.no_pii.level = lv; }
                if (pii.contains("detect_ssn") && pii["detect_ssn"].is_boolean()) rules_.code_quality.no_pii.detect_ssn = pii["detect_ssn"].get<bool>();
                if (pii.contains("detect_credit_card") && pii["detect_credit_card"].is_boolean()) rules_.code_quality.no_pii.detect_credit_card = pii["detect_credit_card"].get<bool>();
                if (pii.contains("detect_email") && pii["detect_email"].is_boolean()) rules_.code_quality.no_pii.detect_email = pii["detect_email"].get<bool>();
                if (pii.contains("detect_phone") && pii["detect_phone"].is_boolean()) rules_.code_quality.no_pii.detect_phone = pii["detect_phone"].get<bool>();
                if (pii.contains("detect_ip_address") && pii["detect_ip_address"].is_boolean()) rules_.code_quality.no_pii.detect_ip_address = pii["detect_ip_address"].get<bool>();
                if (pii.contains("mask_in_errors") && pii["mask_in_errors"].is_boolean()) rules_.code_quality.no_pii.mask_in_errors = pii["mask_in_errors"].get<bool>();
                if (pii.contains("allowlist_patterns")) for (auto& a : pii["allowlist_patterns"]) if (a.is_string()) rules_.code_quality.no_pii.allowlist_patterns.push_back(a.get<std::string>());
                parseRationale(pii, rules_.code_quality.no_pii.rationale);
            }
        }

        // no_mock_data
        if (cq.contains("no_mock_data") && cq["no_mock_data"].is_object()) { rules_.explicitly_set.insert("code_quality.no_mock_data");
            auto& md = cq["no_mock_data"];
            rules_.code_quality.no_mock_data.enabled = true;
            warnIgnoredEnableFlag(md, "code_quality", "no_mock_data");
            if (md.contains("level")) { auto [en, lv] = parseEnforcementLevel(md["level"]); rules_.code_quality.no_mock_data.level = lv; }
            if (md.contains("variable_prefixes")) for (auto& p : md["variable_prefixes"]) if (p.is_string()) rules_.code_quality.no_mock_data.variable_prefixes.push_back(p.get<std::string>());
            if (md.contains("function_prefixes")) for (auto& p : md["function_prefixes"]) if (p.is_string()) rules_.code_quality.no_mock_data.function_prefixes.push_back(p.get<std::string>());
            if (md.contains("literal_patterns")) for (auto& p : md["literal_patterns"]) if (p.is_string()) rules_.code_quality.no_mock_data.literal_patterns.push_back(p.get<std::string>());
            if (md.contains("ignore_in_test_context") && md["ignore_in_test_context"].is_boolean()) rules_.code_quality.no_mock_data.ignore_in_test_context = md["ignore_in_test_context"].get<bool>();
            parseRationale(md, rules_.code_quality.no_mock_data.rationale);
        } else if (cq.contains("no_mock_data")) { rules_.explicitly_set.insert("code_quality.no_mock_data");
            auto [en, lv] = parseEnforcementLevel(cq["no_mock_data"]);
            rules_.code_quality.no_mock_data.enabled = en;
            rules_.code_quality.no_mock_data.level = lv;
        }

        // no_apologetic_language
        if (cq.contains("no_apologetic_language")) { rules_.explicitly_set.insert("code_quality.no_apologetic_language");
            if (cq["no_apologetic_language"].is_boolean() || cq["no_apologetic_language"].is_string()) {
                auto [en, lv] = parseEnforcementLevel(cq["no_apologetic_language"]);
                rules_.code_quality.no_apologetic_language.enabled = en;
                rules_.code_quality.no_apologetic_language.level = lv;
            } else if (cq["no_apologetic_language"].is_object()) {
                auto& al = cq["no_apologetic_language"];
                rules_.code_quality.no_apologetic_language.enabled = true;
                warnIgnoredEnableFlag(al, "code_quality", "no_apologetic_language");
                if (al.contains("level")) { auto [en, lv] = parseEnforcementLevel(al["level"]); rules_.code_quality.no_apologetic_language.level = lv; }
                if (al.contains("scan_comments_only") && al["scan_comments_only"].is_boolean()) rules_.code_quality.no_apologetic_language.scan_comments_only = al["scan_comments_only"].get<bool>();
                if (al.contains("scan_strings") && al["scan_strings"].is_boolean()) rules_.code_quality.no_apologetic_language.scan_strings = al["scan_strings"].get<bool>();
                parseRationale(al, rules_.code_quality.no_apologetic_language.rationale);
            }
        }

        // max_complexity
        if (cq.contains("max_complexity") && cq["max_complexity"].is_object()) { rules_.explicitly_set.insert("code_quality.max_complexity");
            auto& mc = cq["max_complexity"];
            rules_.code_quality.max_complexity.enabled = true;
            warnIgnoredEnableFlag(mc, "code_quality", "max_complexity");
            if (mc.contains("level")) { auto [en, lv] = parseEnforcementLevel(mc["level"]); rules_.code_quality.max_complexity.level = lv; }
            if (mc.contains("max_lines_per_block") && mc["max_lines_per_block"].is_number_integer()) rules_.code_quality.max_complexity.max_lines_per_block = mc["max_lines_per_block"].get<int>();
            if (mc.contains("max_nesting_depth") && mc["max_nesting_depth"].is_number_integer()) rules_.code_quality.max_complexity.max_nesting_depth = mc["max_nesting_depth"].get<int>();
            if (mc.contains("max_parameters") && mc["max_parameters"].is_number_integer()) rules_.code_quality.max_complexity.max_parameters = mc["max_parameters"].get<int>();
            parseRationale(mc, rules_.code_quality.max_complexity.rationale);
        }

        // encoding
        if (cq.contains("encoding") && cq["encoding"].is_object()) { rules_.explicitly_set.insert("code_quality.encoding");
            auto& enc = cq["encoding"];
            rules_.code_quality.encoding.enabled = true;
            warnIgnoredEnableFlag(enc, "code_quality", "encoding");
            if (enc.contains("level")) { auto [en, lv] = parseEnforcementLevel(enc["level"]); rules_.code_quality.encoding.level = lv; }
            if (enc.contains("block_null_bytes") && enc["block_null_bytes"].is_boolean()) rules_.code_quality.encoding.block_null_bytes = enc["block_null_bytes"].get<bool>();
            if (enc.contains("block_unicode_bidi") && enc["block_unicode_bidi"].is_boolean()) rules_.code_quality.encoding.block_unicode_bidi = enc["block_unicode_bidi"].get<bool>();
            parseRationale(enc, rules_.code_quality.encoding.rationale);
        }

        // no_hardcoded_results (expanded)
        if (cq.contains("no_hardcoded_results") && cq["no_hardcoded_results"].is_object()) { rules_.explicitly_set.insert("code_quality.no_hardcoded_results");
            auto& hr = cq["no_hardcoded_results"];
            rules_.code_quality.no_hardcoded_results.enabled = true;
            rules_.no_hardcoded_results = true;
            if (hr.contains("level")) { auto [en, lv] = parseEnforcementLevel(hr["level"]); rules_.code_quality.no_hardcoded_results.level = lv; rules_.no_hardcoded_results_level = lv; }
            if (hr.contains("check_return_true_false") && hr["check_return_true_false"].is_boolean()) rules_.code_quality.no_hardcoded_results.check_return_true_false = hr["check_return_true_false"].get<bool>();
            if (hr.contains("check_dict_success_fields") && hr["check_dict_success_fields"].is_boolean()) rules_.code_quality.no_hardcoded_results.check_dict_success_fields = hr["check_dict_success_fields"].get<bool>();
            parseRationale(hr, rules_.code_quality.no_hardcoded_results.rationale);
        }

        // no_oversimplification
        if (cq.contains("no_oversimplification")) { rules_.explicitly_set.insert("code_quality.no_oversimplification");
            auto& val = cq["no_oversimplification"];
            auto& os = rules_.code_quality.no_oversimplification;
            if (val.is_string()) {
                auto [en, lv] = parseEnforcementLevel(val);
                os.enabled = en; os.level = lv;
            } else if (val.is_object()) {
                os.enabled = true;
                if (val.contains("level")) { auto [en, lv] = parseEnforcementLevel(val["level"]); os.level = lv; }
                if (val.contains("enabled") && val["enabled"].is_boolean()) os.enabled = val["enabled"].get<bool>();
                if (val.contains("check_empty_bodies") && val["check_empty_bodies"].is_boolean()) os.check_empty_bodies = val["check_empty_bodies"].get<bool>();
                if (val.contains("check_trivial_returns") && val["check_trivial_returns"].is_boolean()) os.check_trivial_returns = val["check_trivial_returns"].get<bool>();
                if (val.contains("check_identity_functions") && val["check_identity_functions"].is_boolean()) os.check_identity_functions = val["check_identity_functions"].get<bool>();
                if (val.contains("check_not_implemented") && val["check_not_implemented"].is_boolean()) os.check_not_implemented = val["check_not_implemented"].get<bool>();
                if (val.contains("check_comment_only_bodies") && val["check_comment_only_bodies"].is_boolean()) os.check_comment_only_bodies = val["check_comment_only_bodies"].get<bool>();
                if (val.contains("check_fabricated_results") && val["check_fabricated_results"].is_boolean()) os.check_fabricated_results = val["check_fabricated_results"].get<bool>();
                if (val.contains("case_sensitive") && val["case_sensitive"].is_boolean()) os.case_sensitive = val["case_sensitive"].get<bool>();
                if (val.contains("min_function_lines") && val["min_function_lines"].is_number_integer()) os.min_function_lines = val["min_function_lines"].get<int>();
                if (val.contains("custom_patterns")) {
                    for (auto& p : val["custom_patterns"]) if (p.is_string()) os.custom_patterns.push_back(p.get<std::string>());
                }
                parseRationale(val, os.rationale);
            }
        }

        // no_incomplete_logic
        if (cq.contains("no_incomplete_logic")) { rules_.explicitly_set.insert("code_quality.no_incomplete_logic");
            auto& val = cq["no_incomplete_logic"];
            auto& il = rules_.code_quality.no_incomplete_logic;
            if (val.is_string()) {
                auto [en, lv] = parseEnforcementLevel(val);
                il.enabled = en; il.level = lv;
            } else if (val.is_object()) {
                il.enabled = true;
                if (val.contains("level")) { auto [en, lv] = parseEnforcementLevel(val["level"]); il.level = lv; }
                if (val.contains("enabled") && val["enabled"].is_boolean()) il.enabled = val["enabled"].get<bool>();
                if (val.contains("check_empty_catch") && val["check_empty_catch"].is_boolean()) il.check_empty_catch = val["check_empty_catch"].get<bool>();
                if (val.contains("check_swallowed_exceptions") && val["check_swallowed_exceptions"].is_boolean()) il.check_swallowed_exceptions = val["check_swallowed_exceptions"].get<bool>();
                if (val.contains("check_generic_errors") && val["check_generic_errors"].is_boolean()) il.check_generic_errors = val["check_generic_errors"].get<bool>();
                if (val.contains("check_vague_error_messages") && val["check_vague_error_messages"].is_boolean()) il.check_vague_error_messages = val["check_vague_error_messages"].get<bool>();
                if (val.contains("check_single_iteration_loops") && val["check_single_iteration_loops"].is_boolean()) il.check_single_iteration_loops = val["check_single_iteration_loops"].get<bool>();
                if (val.contains("check_bare_raise") && val["check_bare_raise"].is_boolean()) il.check_bare_raise = val["check_bare_raise"].get<bool>();
                if (val.contains("check_always_true_false") && val["check_always_true_false"].is_boolean()) il.check_always_true_false = val["check_always_true_false"].get<bool>();
                if (val.contains("check_missing_validation") && val["check_missing_validation"].is_boolean()) il.check_missing_validation = val["check_missing_validation"].get<bool>();
                if (val.contains("case_sensitive") && val["case_sensitive"].is_boolean()) il.case_sensitive = val["case_sensitive"].get<bool>();
                if (val.contains("custom_patterns")) {
                    for (auto& p : val["custom_patterns"]) if (p.is_string()) il.custom_patterns.push_back(p.get<std::string>());
                }
                if (val.contains("suppressions") && val["suppressions"].is_array()) {
                    for (auto& s : val["suppressions"]) if (s.is_string()) il.suppressions.push_back(s.get<std::string>());
                }
                parseRationale(val, il.rationale);
            }
        }

        // no_hallucinated_apis
        if (cq.contains("no_hallucinated_apis")) { rules_.explicitly_set.insert("code_quality.no_hallucinated_apis");
            auto& val = cq["no_hallucinated_apis"];
            auto& ha = rules_.code_quality.no_hallucinated_apis;
            if (val.is_string()) {
                auto [en, lv] = parseEnforcementLevel(val);
                ha.enabled = en; ha.level = lv;
            } else if (val.is_object()) {
                ha.enabled = true;
                if (val.contains("level")) { auto [en, lv] = parseEnforcementLevel(val["level"]); ha.level = lv; }
                if (val.contains("enabled") && val["enabled"].is_boolean()) ha.enabled = val["enabled"].get<bool>();
                if (val.contains("check_cross_language") && val["check_cross_language"].is_boolean()) ha.check_cross_language = val["check_cross_language"].get<bool>();
                if (val.contains("check_made_up_functions") && val["check_made_up_functions"].is_boolean()) ha.check_made_up_functions = val["check_made_up_functions"].get<bool>();
                if (val.contains("check_wrong_syntax") && val["check_wrong_syntax"].is_boolean()) ha.check_wrong_syntax = val["check_wrong_syntax"].get<bool>();
                if (val.contains("case_sensitive") && val["case_sensitive"].is_boolean()) ha.case_sensitive = val["case_sensitive"].get<bool>();
                auto loadPatterns = [](const nlohmann::json& obj, const std::string& key, std::vector<std::string>& out) {
                    if (obj.contains(key)) for (auto& p : obj[key]) if (p.is_string()) out.push_back(p.get<std::string>());
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
        if (cq.contains("complexity_floor")) { rules_.explicitly_set.insert("code_quality.complexity_floor");
            auto& val = cq["complexity_floor"];
            auto& cf = rules_.code_quality.complexity_floor;
            cf.enabled = true;  // Presence of section enables it
            if (val.is_object()) warnIgnoredEnableFlag(val, "code_quality", "complexity_floor");
            if (val.is_string()) {
                auto [en, lv] = parseEnforcementLevel(val);
                if (en) cf.level = lv;
            } else if (val.is_object()) {
                if (val.contains("level")) { auto [en, lv] = parseEnforcementLevel(val["level"]); cf.level = lv; }
                if (val.contains("min_score") && val["min_score"].is_number_integer()) cf.min_score = val["min_score"].get<int>();
                if (val.contains("check_polyglot") && val["check_polyglot"].is_boolean()) cf.check_polyglot = val["check_polyglot"].get<bool>();
                if (val.contains("check_naab") && val["check_naab"].is_boolean()) cf.check_naab = val["check_naab"].get<bool>();
                if (val.contains("skip_if_has_polyglot_block") && val["skip_if_has_polyglot_block"].is_boolean()) cf.skip_if_has_polyglot_block = val["skip_if_has_polyglot_block"].get<bool>();
                if (val.contains("min_lines_for_check") && val["min_lines_for_check"].is_number_integer()) cf.min_lines_for_check = val["min_lines_for_check"].get<int>();
                if (val.contains("rules") && val["rules"].is_array()) {
                    for (auto& rule_json : val["rules"]) {
                        ComplexityFloorRule rule;
                        if (rule_json.contains("names") && rule_json["names"].is_array()) {
                            for (auto& n : rule_json["names"]) if (n.is_string()) rule.names.push_back(n.get<std::string>());
                        }
                        if (rule_json.contains("min_score") && rule_json["min_score"].is_number_integer()) rule.min_score = rule_json["min_score"].get<int>();
                        if (rule_json.contains("require_branching_or_loops") && rule_json["require_branching_or_loops"].is_boolean()) rule.require_branching_or_loops = rule_json["require_branching_or_loops"].get<bool>();
                        if (rule_json.contains("message") && rule_json["message"].is_string()) rule.message = rule_json["message"].get<std::string>();
                        cf.rules.push_back(std::move(rule));
                    }
                }
                // Bridge: convert target_prefixes to a rules entry if no explicit rules defined
                if (val.contains("target_prefixes") && val["target_prefixes"].is_array() && cf.rules.empty()) {
                    ComplexityFloorRule prefix_rule;
                    for (auto& p : val["target_prefixes"]) {
                        if (p.is_string()) prefix_rule.names.push_back(p.get<std::string>());
                    }
                    prefix_rule.min_score = cf.min_score;
                    prefix_rule.require_branching_or_loops = false;
                    cf.rules.push_back(std::move(prefix_rule));
                }
                if (val.contains("weights") && val["weights"].is_object()) {
                    auto& w = val["weights"];
                    cf.weights.custom = true;
                    if (w.contains("loop") && w["loop"].is_number_integer()) cf.weights.loop = std::max(0, w["loop"].get<int>());
                    if (w.contains("padding_loop") && w["padding_loop"].is_number_integer()) cf.weights.padding_loop = std::max(0, w["padding_loop"].get<int>());
                    if (w.contains("nested_loops") && w["nested_loops"].is_number_integer()) cf.weights.nested_loops = std::max(0, w["nested_loops"].get<int>());
                    if (w.contains("large_iterations") && w["large_iterations"].is_number_integer()) cf.weights.large_iterations = std::max(0, w["large_iterations"].get<int>());
                    if (w.contains("function") && w["function"].is_number_integer()) cf.weights.function = std::max(0, w["function"].get<int>());
                    if (w.contains("recursion") && w["recursion"].is_number_integer()) cf.weights.recursion = std::max(0, w["recursion"].get<int>());
                    if (w.contains("array_ops") && w["array_ops"].is_number_integer()) cf.weights.array_ops = std::max(0, w["array_ops"].get<int>());
                    if (w.contains("pipeline") && w["pipeline"].is_number_integer()) cf.weights.pipeline = std::max(0, w["pipeline"].get<int>());
                    if (w.contains("pipeline_cap") && w["pipeline_cap"].is_number_integer()) cf.weights.pipeline_cap = std::max(0, w["pipeline_cap"].get<int>());
                    if (w.contains("comprehension") && w["comprehension"].is_number_integer()) cf.weights.comprehension = std::max(0, w["comprehension"].get<int>());
                    if (w.contains("memory_alloc") && w["memory_alloc"].is_number_integer()) cf.weights.memory_alloc = std::max(0, w["memory_alloc"].get<int>());
                    if (w.contains("lifetime") && w["lifetime"].is_number_integer()) cf.weights.lifetime = std::max(0, w["lifetime"].get<int>());
                    if (w.contains("pointers") && w["pointers"].is_number_integer()) cf.weights.pointers = std::max(0, w["pointers"].get<int>());
                    if (w.contains("try_catch") && w["try_catch"].is_number_integer()) cf.weights.try_catch = std::max(0, w["try_catch"].get<int>());
                    if (w.contains("error_propagation") && w["error_propagation"].is_number_integer()) cf.weights.error_propagation = std::max(0, w["error_propagation"].get<int>());
                    if (w.contains("import") && w["import"].is_number_integer()) cf.weights.import = std::max(0, w["import"].get<int>());
                    if (w.contains("external_call") && w["external_call"].is_number_integer()) cf.weights.external_call = std::max(0, w["external_call"].get<int>());
                }
                parseRationale(val, cf.rationale);
            }
        }

        // Duplicate calls config
        if (cq.contains("duplicate_calls") && cq["duplicate_calls"].is_object()) { rules_.explicitly_set.insert("code_quality.duplicate_calls");
            auto& dc = cq["duplicate_calls"];
            if (dc.contains("enabled") && dc["enabled"].is_boolean()) rules_.code_quality.duplicate_calls.enabled = dc["enabled"].get<bool>();
            if (dc.contains("threshold") && dc["threshold"].is_number_integer()) rules_.code_quality.duplicate_calls.threshold = dc["threshold"].get<int>();
            if (dc.contains("max_entries") && dc["max_entries"].is_number_integer()) rules_.code_quality.duplicate_calls.max_entries = dc["max_entries"].get<int>();
            parseRationale(dc, rules_.code_quality.duplicate_calls.rationale);
        }

        // Polyglot try/catch config
        if (cq.contains("polyglot_try_catch") && cq["polyglot_try_catch"].is_object()) { rules_.explicitly_set.insert("code_quality.polyglot_try_catch");
            auto& ptc = cq["polyglot_try_catch"];
            if (ptc.contains("enabled") && ptc["enabled"].is_boolean()) rules_.code_quality.polyglot_try_catch.enabled = ptc["enabled"].get<bool>();
            if (ptc.contains("max_entries") && ptc["max_entries"].is_number_integer()) rules_.code_quality.polyglot_try_catch.max_entries = ptc["max_entries"].get<int>();
        }

        // Drift detection: structural regression gate
        if (cq.contains("drift_detection") && cq["drift_detection"].is_object()) { rules_.explicitly_set.insert("code_quality.drift_detection");
            auto& dd = cq["drift_detection"];
            rules_.code_quality.drift_detection.enabled = dd.value("enabled", false);
            if (dd.contains("level")) {
                auto [en, lv] = parseEnforcementLevel(dd["level"]);
                rules_.code_quality.drift_detection.level = lv;
            }
            if (dd.contains("baseline_path") && dd["baseline_path"].is_string()) rules_.code_quality.drift_detection.baseline_path = dd["baseline_path"].get<std::string>();
            if (dd.contains("max_function_loss") && dd["max_function_loss"].is_number()) rules_.code_quality.drift_detection.max_function_loss = dd["max_function_loss"].get<double>();
            if (dd.contains("max_loc_loss") && dd["max_loc_loss"].is_number()) rules_.code_quality.drift_detection.max_loc_loss = dd["max_loc_loss"].get<double>();
            if (dd.contains("max_export_loss") && dd["max_export_loss"].is_number()) rules_.code_quality.drift_detection.max_export_loss = dd["max_export_loss"].get<double>();
            if (dd.contains("max_struct_loss") && dd["max_struct_loss"].is_number()) rules_.code_quality.drift_detection.max_struct_loss = dd["max_struct_loss"].get<double>();
            if (dd.contains("auto_save") && dd["auto_save"].is_boolean()) rules_.code_quality.drift_detection.auto_save = dd["auto_save"].get<bool>();
            // Gate 1: Signature stability
            if (dd.contains("check_signatures") && dd["check_signatures"].is_boolean()) rules_.code_quality.drift_detection.check_signatures = dd["check_signatures"].get<bool>();
            if (dd.contains("max_param_loss") && dd["max_param_loss"].is_number()) rules_.code_quality.drift_detection.max_param_loss = dd["max_param_loss"].get<double>();
            // Gate 2: Import regression
            if (dd.contains("check_imports") && dd["check_imports"].is_boolean()) rules_.code_quality.drift_detection.check_imports = dd["check_imports"].get<bool>();
            if (dd.contains("max_import_loss") && dd["max_import_loss"].is_number()) rules_.code_quality.drift_detection.max_import_loss = dd["max_import_loss"].get<double>();
            // Gate 3: Complexity regression
            if (dd.contains("check_complexity") && dd["check_complexity"].is_boolean()) rules_.code_quality.drift_detection.check_complexity = dd["check_complexity"].get<bool>();
            if (dd.contains("max_complexity_loss") && dd["max_complexity_loss"].is_number()) rules_.code_quality.drift_detection.max_complexity_loss = dd["max_complexity_loss"].get<double>();
            if (dd.contains("min_complexity_baseline") && dd["min_complexity_baseline"].is_number_integer()) rules_.code_quality.drift_detection.min_complexity_baseline = dd["min_complexity_baseline"].get<int>();
            // Gate 4: Comment inflation
            if (dd.contains("check_comment_ratio") && dd["check_comment_ratio"].is_boolean()) rules_.code_quality.drift_detection.check_comment_ratio = dd["check_comment_ratio"].get<bool>();
            if (dd.contains("max_comment_ratio") && dd["max_comment_ratio"].is_number()) rules_.code_quality.drift_detection.max_comment_ratio = dd["max_comment_ratio"].get<double>();
            if (dd.contains("max_comment_only_ratio") && dd["max_comment_only_ratio"].is_number()) rules_.code_quality.drift_detection.max_comment_only_ratio = dd["max_comment_only_ratio"].get<double>();
            // Gate 5: Dead export
            if (dd.contains("check_hollow_exports") && dd["check_hollow_exports"].is_boolean()) rules_.code_quality.drift_detection.check_hollow_exports = dd["check_hollow_exports"].get<bool>();
            if (dd.contains("min_hollow_export_complexity") && dd["min_hollow_export_complexity"].is_number_integer()) rules_.code_quality.drift_detection.min_hollow_export_complexity = dd["min_hollow_export_complexity"].get<int>();
            // Gate 6: Polyglot regression
            if (dd.contains("check_polyglot") && dd["check_polyglot"].is_boolean()) rules_.code_quality.drift_detection.check_polyglot = dd["check_polyglot"].get<bool>();
            if (dd.contains("max_polyglot_loss") && dd["max_polyglot_loss"].is_number()) rules_.code_quality.drift_detection.max_polyglot_loss = dd["max_polyglot_loss"].get<double>();
            // Gate 7: Struct field stability
            if (dd.contains("check_struct_fields") && dd["check_struct_fields"].is_boolean()) rules_.code_quality.drift_detection.check_struct_fields = dd["check_struct_fields"].get<bool>();
            if (dd.contains("max_field_loss") && dd["max_field_loss"].is_number()) rules_.code_quality.drift_detection.max_field_loss = dd["max_field_loss"].get<double>();
            // Gate 8: Test function regression
            if (dd.contains("check_test_functions") && dd["check_test_functions"].is_boolean()) rules_.code_quality.drift_detection.check_test_functions = dd["check_test_functions"].get<bool>();
            if (dd.contains("max_test_loss") && dd["max_test_loss"].is_number()) rules_.code_quality.drift_detection.max_test_loss = dd["max_test_loss"].get<double>();
            // Gate 9: Function name stability
            if (dd.contains("check_function_names") && dd["check_function_names"].is_boolean()) rules_.code_quality.drift_detection.check_function_names = dd["check_function_names"].get<bool>();
            if (dd.contains("max_function_name_loss") && dd["max_function_name_loss"].is_number()) rules_.code_quality.drift_detection.max_function_name_loss = dd["max_function_name_loss"].get<double>();
            // Gate 10: Baseline tamper protection
            if (dd.contains("require_baseline") && dd["require_baseline"].is_boolean()) rules_.code_quality.drift_detection.require_baseline = dd["require_baseline"].get<bool>();
            // Gate 11: Function body hash
            if (dd.contains("check_body_hash") && dd["check_body_hash"].is_boolean()) rules_.code_quality.drift_detection.check_body_hash = dd["check_body_hash"].get<bool>();
            // Gate 12: Parameter utilization
            if (dd.contains("check_param_utilization") && dd["check_param_utilization"].is_boolean()) rules_.code_quality.drift_detection.check_param_utilization = dd["check_param_utilization"].get<bool>();
            if (dd.contains("min_param_utilization") && dd["min_param_utilization"].is_number()) rules_.code_quality.drift_detection.min_param_utilization = dd["min_param_utilization"].get<double>();
            // Gate 13: Config presence
            if (dd.contains("check_config_presence") && dd["check_config_presence"].is_boolean()) rules_.code_quality.drift_detection.check_config_presence = dd["check_config_presence"].get<bool>();
            // Gate 14: Script location
            if (dd.contains("check_script_location") && dd["check_script_location"].is_boolean()) rules_.code_quality.drift_detection.check_script_location = dd["check_script_location"].get<bool>();
            // Gate 16: Signature presence
            if (dd.contains("check_signature_presence") && dd["check_signature_presence"].is_boolean()) rules_.code_quality.drift_detection.check_signature_presence = dd["check_signature_presence"].get<bool>();
            // Gate 17: Polyglot content regression
            if (dd.contains("check_polyglot_content") && dd["check_polyglot_content"].is_boolean()) rules_.code_quality.drift_detection.check_polyglot_content = dd["check_polyglot_content"].get<bool>();
            if (dd.contains("max_polyglot_shrink") && dd["max_polyglot_shrink"].is_number()) rules_.code_quality.drift_detection.max_polyglot_shrink = dd["max_polyglot_shrink"].get<double>();
            // Gate 0 extension: Function gain detection
            if (dd.contains("max_function_gain") && dd["max_function_gain"].is_number()) rules_.code_quality.drift_detection.max_function_gain = dd["max_function_gain"].get<double>();
            // Gate 18: New function detection
            if (dd.contains("check_new_functions") && dd["check_new_functions"].is_boolean()) rules_.code_quality.drift_detection.check_new_functions = dd["check_new_functions"].get<bool>();
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
                if (f.is_string()) rules_.integrity.blocked_flags.push_back(f.get<std::string>());
            }
        }
    }

    // V3 Custom Rules
    if (j.contains("custom_rules") && j["custom_rules"].is_array()) {
        for (auto& cr : j["custom_rules"]) {
            CustomRule rule;
            if (cr.contains("id") && cr["id"].is_string()) rule.id = cr["id"].get<std::string>();
            if (cr.contains("name") && cr["name"].is_string()) rule.name = cr["name"].get<std::string>();
            if (cr.contains("description") && cr["description"].is_string()) rule.description = cr["description"].get<std::string>();
            if (cr.contains("pattern") && cr["pattern"].is_string()) rule.pattern = cr["pattern"].get<std::string>();
            if (cr.contains("languages")) for (auto& l : cr["languages"]) if (l.is_string()) rule.languages.push_back(l.get<std::string>());
            if (cr.contains("level")) { auto [en, lv] = parseEnforcementLevel(cr["level"]); rule.level = lv; }
            if (cr.contains("message") && cr["message"].is_string()) rule.message = cr["message"].get<std::string>();
            if (cr.contains("help") && cr["help"].is_string()) rule.help = cr["help"].get<std::string>();
            if (cr.contains("good_example") && cr["good_example"].is_string()) rule.good_example = cr["good_example"].get<std::string>();
            if (cr.contains("bad_example") && cr["bad_example"].is_string()) rule.bad_example = cr["bad_example"].get<std::string>();
            if (cr.contains("enabled") && cr["enabled"].is_boolean()) rule.enabled = cr["enabled"].get<bool>();
            if (cr.contains("case_sensitive") && cr["case_sensitive"].is_boolean()) rule.case_sensitive = cr["case_sensitive"].get<bool>();
            if (cr.contains("tags")) for (auto& t : cr["tags"]) if (t.is_string()) rule.tags.push_back(t.get<std::string>());
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
                    if (pr.contains("id") && pr["id"].is_string()) rule.id = pr["id"].get<std::string>();
                    if (pr.contains("function") && pr["function"].is_string()) rule.function_name = pr["function"].get<std::string>();
                    if (pr.contains("description") && pr["description"].is_string()) rule.description = pr["description"].get<std::string>();
                    if (pr.contains("level")) { auto [en, lv] = parseEnforcementLevel(pr["level"]); rule.level = lv; }
                    if (pr.contains("languages")) for (auto& l : pr["languages"]) if (l.is_string()) rule.languages.push_back(l.get<std::string>());
                    if (pr.contains("trigger") && pr["trigger"].is_string()) rule.trigger = pr["trigger"].get<std::string>();
                    if (pr.contains("message") && pr["message"].is_string()) rule.message = pr["message"].get<std::string>();
                    if (pr.contains("help") && pr["help"].is_string()) rule.help = pr["help"].get<std::string>();
                    if (pr.contains("good_example") && pr["good_example"].is_string()) rule.good_example = pr["good_example"].get<std::string>();
                    if (pr.contains("bad_example") && pr["bad_example"].is_string()) rule.bad_example = pr["bad_example"].get<std::string>();
                    if (pr.contains("enabled") && pr["enabled"].is_boolean()) rule.enabled = pr["enabled"].get<bool>();
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
            if (s.contains("enabled") && s["enabled"].is_boolean()) rules_.output.summary.enabled = s["enabled"].get<bool>();
            if (s.contains("format") && s["format"].is_string()) rules_.output.summary.format = s["format"].get<std::string>();
            if (s.contains("show_passing") && s["show_passing"].is_boolean()) rules_.output.summary.show_passing = s["show_passing"].get<bool>();
            if (s.contains("group_by") && s["group_by"].is_string()) rules_.output.summary.group_by = s["group_by"].get<std::string>();
        }
        if (out.contains("errors") && out["errors"].is_object()) {
            auto& e = out["errors"];
            if (e.contains("verbose") && e["verbose"].is_boolean()) rules_.output.errors.verbose = e["verbose"].get<bool>();
            if (e.contains("show_help") && e["show_help"].is_boolean()) rules_.output.errors.show_help = e["show_help"].get<bool>();
            if (e.contains("show_examples") && e["show_examples"].is_boolean()) rules_.output.errors.show_examples = e["show_examples"].get<bool>();
            if (e.contains("max_errors_per_rule") && e["max_errors_per_rule"].is_number_integer()) rules_.output.errors.max_errors_per_rule = e["max_errors_per_rule"].get<int>();
            if (e.contains("max_total_errors") && e["max_total_errors"].is_number_integer()) rules_.output.errors.max_total_errors = e["max_total_errors"].get<int>();
            if (e.contains("show_code_context") && e["show_code_context"].is_number_integer()) rules_.output.errors.show_code_context = e["show_code_context"].get<int>();
        }
        if (out.contains("formatting") && out["formatting"].is_object()) {
            auto& f = out["formatting"];
            if (f.contains("color") && f["color"].is_boolean()) rules_.output.formatting.color = f["color"].get<bool>();
            if (f.contains("unicode_symbols") && f["unicode_symbols"].is_boolean()) rules_.output.formatting.unicode_symbols = f["unicode_symbols"].get<bool>();
            if (f.contains("width") && f["width"].is_number_integer()) rules_.output.formatting.width = f["width"].get<int>();
        }
        if (out.contains("file_output") && out["file_output"].is_object()) {
            auto& fo = out["file_output"];
            if (fo.contains("report_json") && fo["report_json"].is_string() && !fo["report_json"].is_null()) rules_.output.file_output.report_json = fo["report_json"].get<std::string>();
            if (fo.contains("report_sarif") && fo["report_sarif"].is_string() && !fo["report_sarif"].is_null()) rules_.output.file_output.report_sarif = fo["report_sarif"].get<std::string>();
            if (fo.contains("report_junit") && fo["report_junit"].is_string() && !fo["report_junit"].is_null()) rules_.output.file_output.report_junit = fo["report_junit"].get<std::string>();
        }
        if (out.contains("max_advisories") && out["max_advisories"].is_number_integer()) { rules_.output.max_advisories = out["max_advisories"].get<int>(); rules_.explicitly_set.insert("output.max_advisories"); }
        if (out.contains("advisory_summary") && out["advisory_summary"].is_boolean()) rules_.output.advisory_summary = out["advisory_summary"].get<bool>();
        if (out.contains("quiet") && out["quiet"].is_boolean()) rules_.quiet_config = out["quiet"].get<bool>();
        if (out.contains("no_color") && out["no_color"].is_boolean()) rules_.no_color_config = out["no_color"].get<bool>();
        if (out.contains("voice") && out["voice"].is_string()) rules_.output.voice = out["voice"].get<std::string>();
        if (out.contains("voice_cache") && out["voice_cache"].is_boolean()) rules_.output.voice_cache = out["voice_cache"].get<bool>();
    }

    // V3 Audit (expanded)
    if (j.contains("audit") && j["audit"].is_object()) {
        auto& aud = j["audit"];
        if (aud.contains("level") && aud["level"].is_string()) { rules_.audit.level = aud["level"].get<std::string>(); rules_.explicitly_set.insert("audit.level"); }
        if (aud.contains("output_file") && aud["output_file"].is_string()) { rules_.audit.output_file = aud["output_file"].get<std::string>(); rules_.explicitly_set.insert("audit.output_file"); }
        if (aud.contains("tamper_evidence") && aud["tamper_evidence"].is_object()) { rules_.explicitly_set.insert("audit.tamper_evidence");
            auto& te = aud["tamper_evidence"];
            if (te.contains("enabled") && te["enabled"].is_boolean()) { rules_.audit.tamper_evidence.enabled = te["enabled"].get<bool>(); rules_.tamper_evidence = rules_.audit.tamper_evidence.enabled; }
            if (te.contains("algorithm") && te["algorithm"].is_string()) rules_.audit.tamper_evidence.algorithm = te["algorithm"].get<std::string>();
            if (te.contains("chain_genesis") && te["chain_genesis"].is_string()) rules_.audit.tamper_evidence.chain_genesis = te["chain_genesis"].get<std::string>();
            if (te.contains("hmac_key") && te["hmac_key"].is_string()) rules_.audit.tamper_evidence.hmac_key = te["hmac_key"].get<std::string>();
            if (te.contains("hmac_key_env") && te["hmac_key_env"].is_string()) {
                std::string env_name = te["hmac_key_env"].get<std::string>();
                const char* env_val = std::getenv(env_name.c_str());
                if (env_val && env_val[0] != '\0') {
                    rules_.audit.tamper_evidence.hmac_key = env_val;
                }
            }
        }
        if (aud.contains("log_events") && aud["log_events"].is_object()) {
            auto& le = aud["log_events"];
            if (le.contains("checks_passed") && le["checks_passed"].is_boolean()) rules_.audit.log_events.checks_passed = le["checks_passed"].get<bool>();
            if (le.contains("checks_failed") && le["checks_failed"].is_boolean()) rules_.audit.log_events.checks_failed = le["checks_failed"].get<bool>();
            if (le.contains("overrides") && le["overrides"].is_boolean()) rules_.audit.log_events.overrides = le["overrides"].get<bool>();
            if (le.contains("polyglot_executed") && le["polyglot_executed"].is_boolean()) rules_.audit.log_events.polyglot_executed = le["polyglot_executed"].get<bool>();
            if (le.contains("polyglot_timing") && le["polyglot_timing"].is_boolean()) rules_.audit.log_events.polyglot_timing = le["polyglot_timing"].get<bool>();
            if (le.contains("taint_decisions") && le["taint_decisions"].is_boolean()) rules_.audit.log_events.taint_decisions = le["taint_decisions"].get<bool>();
            if (le.contains("contract_checks") && le["contract_checks"].is_boolean()) rules_.audit.log_events.contract_checks = le["contract_checks"].get<bool>();
        }
        if (aud.contains("provenance") && aud["provenance"].is_object()) { rules_.explicitly_set.insert("audit.provenance");
            auto& prov = aud["provenance"];
            if (prov.contains("enabled") && prov["enabled"].is_boolean()) rules_.audit.provenance.enabled = prov["enabled"].get<bool>();
            if (prov.contains("record_proof_objects") && prov["record_proof_objects"].is_boolean()) rules_.audit.provenance.record_proof_objects = prov["record_proof_objects"].get<bool>();
            if (prov.contains("record_attestations") && prov["record_attestations"].is_boolean()) rules_.audit.provenance.record_attestations = prov["record_attestations"].get<bool>();
            if (prov.contains("record_decisions") && prov["record_decisions"].is_boolean()) rules_.audit.provenance.record_decisions = prov["record_decisions"].get<bool>();
            if (prov.contains("sign_records") && prov["sign_records"].is_boolean()) rules_.audit.provenance.sign_records = prov["sign_records"].get<bool>();
            if (prov.contains("signing_key") && prov["signing_key"].is_string()) rules_.audit.provenance.signing_key = prov["signing_key"].get<std::string>();
            if (prov.contains("signing_key_env") && prov["signing_key_env"].is_string()) {
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
            if (sv.contains("warn_unknown_keys") && sv["warn_unknown_keys"].is_boolean()) rules_.meta.schema_validation.warn_unknown_keys = sv["warn_unknown_keys"].get<bool>();
            if (sv.contains("suggest_corrections") && sv["suggest_corrections"].is_boolean()) rules_.meta.schema_validation.suggest_corrections = sv["suggest_corrections"].get<bool>();
        }
        if (meta.contains("allow_agent_addition_mid_run") &&
            meta["allow_agent_addition_mid_run"].is_boolean()) {
            rules_.meta.allow_agent_addition_mid_run =
                meta["allow_agent_addition_mid_run"].get<bool>();
        }
        if (meta.contains("inheritance") && meta["inheritance"].is_object()) {
            auto& inh = meta["inheritance"];
            if (inh.contains("max_depth") && inh["max_depth"].is_number_integer()) rules_.meta.inheritance.max_depth = inh["max_depth"].get<int>();
            if (inh.contains("merge_strategy") && inh["merge_strategy"].is_string()) rules_.meta.inheritance.merge_strategy = inh["merge_strategy"].get<std::string>();
            if (inh.contains("merge_arrays") && inh["merge_arrays"].is_string()) rules_.meta.inheritance.merge_arrays = inh["merge_arrays"].get<std::string>();
        }
        if (meta.contains("environment") && meta["environment"].is_object()) {
            auto& env = meta["environment"];
            if (env.contains("allow_env_var_substitution") && env["allow_env_var_substitution"].is_boolean()) rules_.meta.environment.allow_env_var_substitution = env["allow_env_var_substitution"].get<bool>();
            if (env.contains("env_prefix") && env["env_prefix"].is_string()) rules_.meta.environment.env_prefix = env["env_prefix"].get<std::string>();
            if (env.contains("allow_cli_override") && env["allow_cli_override"].is_boolean()) rules_.meta.environment.allow_cli_override = env["allow_cli_override"].get<bool>();
        }
        if (meta.contains("feature_flags") && meta["feature_flags"].is_object()) {
            auto& ff = meta["feature_flags"];
            if (ff.contains("experimental_checks") && ff["experimental_checks"].is_boolean()) rules_.meta.feature_flags.experimental_checks = ff["experimental_checks"].get<bool>();
            if (ff.contains("verbose_parsing") && ff["verbose_parsing"].is_boolean()) rules_.meta.feature_flags.verbose_parsing = ff["verbose_parsing"].get<bool>();
        }
    }

    // V3 Polyglot rules
    if (j.contains("polyglot") && j["polyglot"].is_object()) {
        auto& pg = j["polyglot"];
        if (pg.contains("variable_binding") && pg["variable_binding"].is_object()) {
            auto& vb = pg["variable_binding"];
            if (vb.contains("require_explicit")) { auto [en, lv] = parseEnforcementLevel(vb["require_explicit"]); rules_.polyglot.variable_binding.require_explicit = en; rules_.polyglot.variable_binding.require_explicit_level = lv; rules_.explicitly_set.insert("polyglot.variable_binding.require_explicit"); }
            if (vb.contains("max_bound_variables") && vb["max_bound_variables"].is_number_integer()) { rules_.polyglot.variable_binding.max_bound_variables = vb["max_bound_variables"].get<int>(); rules_.explicitly_set.insert("polyglot.variable_binding.max_bound_variables"); }
        }
        if (pg.contains("output") && pg["output"].is_object()) {
            auto& po = pg["output"];
            if (po.contains("require_json_pipe") && po["require_json_pipe"].is_boolean()) { rules_.polyglot.output.require_json_pipe = po["require_json_pipe"].get<bool>(); rules_.explicitly_set.insert("polyglot.output.require_json_pipe"); }
            if (po.contains("require_naab_return") && po["require_naab_return"].is_boolean()) { rules_.polyglot.output.require_naab_return = po["require_naab_return"].get<bool>(); rules_.explicitly_set.insert("polyglot.output.require_naab_return"); }
            if (po.contains("max_output_lines") && po["max_output_lines"].is_number_integer()) { rules_.polyglot.output.max_output_lines = po["max_output_lines"].get<int>(); rules_.explicitly_set.insert("polyglot.output.max_output_lines"); }
            if (po.contains("validate_encoding") && po["validate_encoding"].is_boolean()) rules_.polyglot.output.validate_encoding = po["validate_encoding"].get<bool>();
        }
        if (pg.contains("parallel") && pg["parallel"].is_object()) {
            auto& par = pg["parallel"];
            if (par.contains("max_parallel_blocks") && par["max_parallel_blocks"].is_number_integer()) { rules_.polyglot.parallel.max_parallel_blocks = par["max_parallel_blocks"].get<int>(); rules_.explicitly_set.insert("polyglot.parallel.max_parallel_blocks"); }
            if (par.contains("timeout_per_block") && par["timeout_per_block"].is_number_integer()) { rules_.polyglot.parallel.timeout_per_block = par["timeout_per_block"].get<int>(); rules_.explicitly_set.insert("polyglot.parallel.timeout_per_block"); }
            if (par.contains("fail_strategy") && par["fail_strategy"].is_string()) { rules_.polyglot.parallel.fail_strategy = par["fail_strategy"].get<std::string>(); rules_.explicitly_set.insert("polyglot.parallel.fail_strategy"); }
        }
        if (pg.contains("persistent_runtime") && pg["persistent_runtime"].is_object()) {
            auto& pr = pg["persistent_runtime"];
            if (pr.contains("max_sessions") && pr["max_sessions"].is_number_integer()) { rules_.polyglot.persistent_runtime.max_sessions = pr["max_sessions"].get<int>(); rules_.explicitly_set.insert("polyglot.persistent_runtime.max_sessions"); }
            if (pr.contains("session_timeout") && pr["session_timeout"].is_number_integer()) { rules_.polyglot.persistent_runtime.session_timeout = pr["session_timeout"].get<int>(); rules_.explicitly_set.insert("polyglot.persistent_runtime.session_timeout"); }
            if (pr.contains("max_memory_per_session_mb") && pr["max_memory_per_session_mb"].is_number_integer()) { rules_.polyglot.persistent_runtime.max_memory_per_session_mb = pr["max_memory_per_session_mb"].get<int>(); rules_.explicitly_set.insert("polyglot.persistent_runtime.max_memory_per_session_mb"); }
        }
    }

    // V3 Polyglot Optimization (Section 14)
    if (j.contains("polyglot_optimization") && j["polyglot_optimization"].is_object()) {
        auto& po = j["polyglot_optimization"];

        if (po.contains("enabled") && po["enabled"].is_boolean()) rules_.polyglot_optimization.enabled = po["enabled"].get<bool>();
        if (po.contains("enforcement_level") && po["enforcement_level"].is_string()) rules_.polyglot_optimization.enforcement_level = po["enforcement_level"].get<std::string>();

        // Pattern detection
        if (po.contains("pattern_detection") && po["pattern_detection"].is_object()) {
            auto& pd = po["pattern_detection"];
            if (pd.contains("enabled") && pd["enabled"].is_boolean()) rules_.polyglot_optimization.pattern_detection.enabled = pd["enabled"].get<bool>();

            // Task inference patterns
            if (pd.contains("task_inference") && pd["task_inference"].is_object()) {
                for (auto& [task_name, task_config] : pd["task_inference"].items()) {
                    if (!task_config.is_object()) continue;

                    TaskInferencePattern pattern;
                    if (task_config.contains("patterns") && task_config["patterns"].is_array()) {
                        for (auto& p : task_config["patterns"]) {
                            if (p.is_string()) pattern.patterns.push_back(p.get<std::string>());
                        }
                    }
                    if (task_config.contains("optimal_languages") && task_config["optimal_languages"].is_array()) {
                        for (auto& lang : task_config["optimal_languages"]) {
                            if (lang.is_string()) pattern.optimal_languages.push_back(lang.get<std::string>());
                        }
                    }
                    if (task_config.contains("suboptimal_languages") && task_config["suboptimal_languages"].is_array()) {
                        for (auto& lang : task_config["suboptimal_languages"]) {
                            if (lang.is_string()) pattern.suboptimal_languages.push_back(lang.get<std::string>());
                        }
                    }
                    if (task_config.contains("message") && task_config["message"].is_string()) pattern.message = task_config["message"].get<std::string>();

                    rules_.polyglot_optimization.pattern_detection.task_inference[task_name] = pattern;
                }
            }
        }

        // Language diversity
        if (po.contains("language_diversity") && po["language_diversity"].is_object()) {
            auto& ld = po["language_diversity"];
            if (ld.contains("enabled") && ld["enabled"].is_boolean()) rules_.polyglot_optimization.language_diversity.enabled = ld["enabled"].get<bool>();
            if (ld.contains("min_languages") && ld["min_languages"].is_number_integer()) rules_.polyglot_optimization.language_diversity.min_languages = ld["min_languages"].get<int>();
            if (ld.contains("max_single_language_percent") && ld["max_single_language_percent"].is_number_integer()) rules_.polyglot_optimization.language_diversity.max_single_language_percent = ld["max_single_language_percent"].get<int>();
            if (ld.contains("message") && ld["message"].is_string()) rules_.polyglot_optimization.language_diversity.message = ld["message"].get<std::string>();
        }

        // Helper errors
        if (po.contains("helper_errors") && po["helper_errors"].is_object()) {
            auto& he = po["helper_errors"];
            if (he.contains("enabled") && he["enabled"].is_boolean()) rules_.polyglot_optimization.helper_errors.enabled = he["enabled"].get<bool>();
            if (he.contains("show_alternative_language") && he["show_alternative_language"].is_boolean()) rules_.polyglot_optimization.helper_errors.show_alternative_language = he["show_alternative_language"].get<bool>();
            if (he.contains("show_example_code") && he["show_example_code"].is_boolean()) rules_.polyglot_optimization.helper_errors.show_example_code = he["show_example_code"].get<bool>();
            if (he.contains("fuzzy_match_threshold") && he["fuzzy_match_threshold"].is_number()) rules_.polyglot_optimization.helper_errors.fuzzy_match_threshold = he["fuzzy_match_threshold"].get<double>();
        }

        // AI guidance
        if (po.contains("ai_guidance") && po["ai_guidance"].is_object()) {
            auto& ag = po["ai_guidance"];
            if (ag.contains("enabled") && ag["enabled"].is_boolean()) rules_.polyglot_optimization.ai_guidance.enabled = ag["enabled"].get<bool>();
            if (ag.contains("include_in_errors") && ag["include_in_errors"].is_boolean()) rules_.polyglot_optimization.ai_guidance.include_in_errors = ag["include_in_errors"].get<bool>();
            if (ag.contains("suggest_refactoring") && ag["suggest_refactoring"].is_boolean()) rules_.polyglot_optimization.ai_guidance.suggest_refactoring = ag["suggest_refactoring"].get<bool>();
            if (ag.contains("show_benchmarks") && ag["show_benchmarks"].is_boolean()) rules_.polyglot_optimization.ai_guidance.show_benchmarks = ag["show_benchmarks"].get<bool>();
        }

        // Empirical profiling
        if (po.contains("profiling") && po["profiling"].is_object()) {
            auto& pf = po["profiling"];
            if (pf.contains("enabled") && pf["enabled"].is_boolean()) rules_.polyglot_optimization.profiling.enabled = pf["enabled"].get<bool>();
            if (pf.contains("profile_path") && pf["profile_path"].is_string()) rules_.polyglot_optimization.profiling.profile_path = pf["profile_path"].get<std::string>();
            if (pf.contains("max_entries") && pf["max_entries"].is_number_integer()) rules_.polyglot_optimization.profiling.max_entries = pf["max_entries"].get<int>();
            if (pf.contains("include_code_hash") && pf["include_code_hash"].is_boolean()) rules_.polyglot_optimization.profiling.include_code_hash = pf["include_code_hash"].get<bool>();
        }

        // Calibration
        if (po.contains("calibration") && po["calibration"].is_object()) {
            auto& cb = po["calibration"];
            if (cb.contains("enabled") && cb["enabled"].is_boolean()) rules_.polyglot_optimization.calibration.enabled = cb["enabled"].get<bool>();
            if (cb.contains("auto_calibrate") && cb["auto_calibrate"].is_boolean()) rules_.polyglot_optimization.calibration.auto_calibrate = cb["auto_calibrate"].get<bool>();
            if (cb.contains("calibration_path") && cb["calibration_path"].is_string()) rules_.polyglot_optimization.calibration.calibration_path = cb["calibration_path"].get<std::string>();
            if (cb.contains("max_age_days") && cb["max_age_days"].is_number_integer()) rules_.polyglot_optimization.calibration.max_age_days = cb["max_age_days"].get<int>();
            if (cb.contains("iterations") && cb["iterations"].is_number_integer()) rules_.polyglot_optimization.calibration.iterations = cb["iterations"].get<int>();
        }

        // Confidence labels
        if (po.contains("confidence") && po["confidence"].is_object()) {
            auto& cf = po["confidence"];
            if (cf.contains("min_display_level") && cf["min_display_level"].is_string()) rules_.polyglot_optimization.confidence.min_display_level = cf["min_display_level"].get<std::string>();
            if (cf.contains("suppress_unknown") && cf["suppress_unknown"].is_boolean()) rules_.polyglot_optimization.confidence.suppress_unknown = cf["suppress_unknown"].get<bool>();
            if (cf.contains("show_measurement_details") && cf["show_measurement_details"].is_boolean()) rules_.polyglot_optimization.confidence.show_measurement_details = cf["show_measurement_details"].get<bool>();
        }

        // Polyglot consensus verification
        if (po.contains("verification") && po["verification"].is_object()) {
            auto& vf = po["verification"];
            if (vf.contains("enabled") && vf["enabled"].is_boolean())
                rules_.polyglot_optimization.verification.enabled = vf["enabled"].get<bool>();
            if (vf.contains("enforcement_level") && vf["enforcement_level"].is_string())
                rules_.polyglot_optimization.verification.enforcement_level = vf["enforcement_level"].get<std::string>();
            if (vf.contains("tolerance") && vf["tolerance"].is_number())
                rules_.polyglot_optimization.verification.tolerance = vf["tolerance"].get<double>();
            if (vf.contains("min_consensus") && vf["min_consensus"].is_number_integer())
                rules_.polyglot_optimization.verification.min_consensus = vf["min_consensus"].get<int>();
            if (vf.contains("max_verification_time_ms") && vf["max_verification_time_ms"].is_number_integer())
                rules_.polyglot_optimization.verification.max_verification_time_ms = vf["max_verification_time_ms"].get<int>();
            if (vf.contains("show_drift_details") && vf["show_drift_details"].is_boolean())
                rules_.polyglot_optimization.verification.show_drift_details = vf["show_drift_details"].get<bool>();
            if (vf.contains("consensus_languages") && vf["consensus_languages"].is_array()) {
                for (auto& lang : vf["consensus_languages"]) {
                    if (lang.is_string()) rules_.polyglot_optimization.verification.consensus_languages.push_back(
                        lang.get<std::string>());
                }
            }
            if (vf.contains("verify_task_types") && vf["verify_task_types"].is_array()) {
                for (auto& tt : vf["verify_task_types"]) {
                    if (tt.is_string()) rules_.polyglot_optimization.verification.verify_task_types.push_back(
                        tt.get<std::string>());
                }
            }
            // Drift tracking sub-config
            if (vf.contains("drift_tracking") && vf["drift_tracking"].is_object()) {
                auto& dt = vf["drift_tracking"];
                auto& dtc = rules_.polyglot_optimization.verification.drift_tracking;
                if (dt.contains("enabled") && dt["enabled"].is_boolean()) dtc.enabled = dt["enabled"].get<bool>();
                if (dt.contains("path") && dt["path"].is_string()) dtc.path = dt["path"].get<std::string>();
                if (dt.contains("max_entries") && dt["max_entries"].is_number_integer()) dtc.max_entries = dt["max_entries"].get<int>();
                if (dt.contains("trend_window") && dt["trend_window"].is_number_integer()) dtc.trend_window = dt["trend_window"].get<int>();
                if (dt.contains("escalation_threshold") && dt["escalation_threshold"].is_number()) dtc.escalation_threshold = dt["escalation_threshold"].get<double>();
                if (dt.contains("include_code_hash") && dt["include_code_hash"].is_boolean()) dtc.include_code_hash = dt["include_code_hash"].get<bool>();
            }
        }

        // Task→Language scoring matrix
        if (po.contains("task_language_matrix") && po["task_language_matrix"].is_object()) {
            for (auto& [task_name, lang_scores] : po["task_language_matrix"].items()) {
                if (!lang_scores.is_object()) continue;

                for (auto& [lang_name, score_obj] : lang_scores.items()) {
                    TaskLanguageScore score;
                    if (score_obj.is_object()) {
                        if (score_obj.contains("score") && score_obj["score"].is_number_integer()) score.score = score_obj["score"].get<int>();
                        if (score_obj.contains("reason") && score_obj["reason"].is_string()) score.reason = score_obj["reason"].get<std::string>();
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
            if (hj.contains("command") && hj["command"].is_string() && !hj["command"].is_null()) hc.command = hj["command"].get<std::string>();
            if (hj.contains("args") && hj["args"].is_array()) for (auto& a : hj["args"]) if (a.is_string()) hc.args.push_back(a.get<std::string>());
            if (hj.contains("timeout") && hj["timeout"].is_number_integer()) hc.timeout = hj["timeout"].get<int>();
            if (hj.contains("inherit_governance_keys") && hj["inherit_governance_keys"].is_boolean()) hc.inherit_governance_keys = hj["inherit_governance_keys"].get<bool>();
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
        if (pc.contains("enabled") && pc["enabled"].is_boolean()) rules_.project_context.enabled = pc["enabled"].get<bool>();
        if (pc.contains("enforcement_level") && pc["enforcement_level"].is_string()) rules_.project_context.enforcement_level = pc["enforcement_level"].get<std::string>();
        if (pc.contains("priority_source") && pc["priority_source"].is_string()) rules_.project_context.priority_source = pc["priority_source"].get<std::string>();
        if (pc.contains("sources") && pc["sources"].is_object()) {
            auto& src = pc["sources"];
            if (src.contains("llm") && src["llm"].is_boolean()) rules_.project_context.sources.llm = src["llm"].get<bool>();
            if (src.contains("linters") && src["linters"].is_boolean()) rules_.project_context.sources.linters = src["linters"].get<bool>();
            if (src.contains("manifests") && src["manifests"].is_boolean()) rules_.project_context.sources.manifests = src["manifests"].get<bool>();
        }
        if (pc.contains("watch_files")) {
            for (auto& f : pc["watch_files"]) if (f.is_string()) rules_.project_context.watch_files.push_back(f.get<std::string>());
        }
        if (pc.contains("ignore_files")) {
            for (auto& f : pc["ignore_files"]) if (f.is_string()) rules_.project_context.ignore_files.push_back(f.get<std::string>());
        }
        if (pc.contains("suppress_rules")) {
            for (auto& r : pc["suppress_rules"]) if (r.is_string()) rules_.project_context.suppress_rules.push_back(r.get<std::string>());
        }
        if (pc.contains("extract") && pc["extract"].is_object()) {
            auto& ex = pc["extract"];
            if (ex.contains("language_preferences") && ex["language_preferences"].is_boolean()) rules_.project_context.extract_language_prefs = ex["language_preferences"].get<bool>();
            if (ex.contains("banned_patterns") && ex["banned_patterns"].is_boolean()) rules_.project_context.extract_banned_patterns = ex["banned_patterns"].get<bool>();
            if (ex.contains("style_rules") && ex["style_rules"].is_boolean()) rules_.project_context.extract_style_rules = ex["style_rules"].get<bool>();
            if (ex.contains("custom_directives") && ex["custom_directives"].is_boolean()) rules_.project_context.extract_custom_directives = ex["custom_directives"].get<bool>();
        }
        if (pc.contains("feed_optimization") && pc["feed_optimization"].is_boolean()) rules_.project_context.feed_optimization = pc["feed_optimization"].get<bool>();
        if (pc.contains("show_extractions") && pc["show_extractions"].is_boolean()) rules_.project_context.show_extractions = pc["show_extractions"].get<bool>();
        if (pc.contains("dry_run") && pc["dry_run"].is_boolean()) rules_.project_context.dry_run = pc["dry_run"].get<bool>();
        if (pc.contains("max_file_size_kb") && pc["max_file_size_kb"].is_number_integer()) rules_.project_context.max_file_size_kb = pc["max_file_size_kb"].get<int>();
    }

    // Contracts
    if (j.contains("contracts") && j["contracts"].is_object()) {
        auto& ct = j["contracts"];
        if (ct.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(ct["level"]);
            if (en) rules_.contracts.level = lv;
        }
        if (ct.contains("validate_inputs") && ct["validate_inputs"].is_boolean()) rules_.contracts.validate_inputs = ct["validate_inputs"].get<bool>();
        if (ct.contains("functions") && ct["functions"].is_object()) {
            for (auto& [fn_name, fn_obj] : ct["functions"].items()) {
                if (!fn_obj.is_object()) continue;
                FunctionContract fc;
                if (fn_obj.contains("description") && fn_obj["description"].is_string()) fc.description = fn_obj["description"].get<std::string>();
                if (fn_obj.contains("level")) {
                    auto [en, lv] = parseEnforcementLevel(fn_obj["level"]);
                    if (en) fc.level = lv;
                }
                if (fn_obj.contains("return_type") && fn_obj["return_type"].is_string()) fc.return_type = fn_obj["return_type"].get<std::string>();
                if (fn_obj.contains("return_range") && fn_obj["return_range"].is_array() && fn_obj["return_range"].size() == 2) {
                    fc.has_return_range = true;
                    fc.return_range_min = fn_obj["return_range"][0].is_number() ? fn_obj["return_range"][0].get<double>() : 0.0;
                    fc.return_range_max = fn_obj["return_range"][1].is_number() ? fn_obj["return_range"][1].get<double>() : 0.0;
                }
                if (fn_obj.contains("return_min") && fn_obj["return_min"].is_number()) { fc.has_return_min = true; fc.return_min = fn_obj["return_min"].get<double>(); }
                if (fn_obj.contains("return_max") && fn_obj["return_max"].is_number()) { fc.has_return_max = true; fc.return_max = fn_obj["return_max"].get<double>(); }
                if (fn_obj.contains("return_one_of") && fn_obj["return_one_of"].is_array()) {
                    for (auto& v : fn_obj["return_one_of"]) {
                        // Handle non-string values (ints, bools) by converting to string
                        if (v.is_string()) {
                            if (v.is_string()) fc.return_one_of.push_back(v.get<std::string>());
                        } else {
                            fc.return_one_of.push_back(v.dump());
                        }
                    }
                }
                if (fn_obj.contains("return_non_empty") && fn_obj["return_non_empty"].is_boolean()) fc.return_non_empty = fn_obj["return_non_empty"].get<bool>();
                if (fn_obj.contains("return_keys") && fn_obj["return_keys"].is_array()) {
                    for (auto& k : fn_obj["return_keys"]) if (k.is_string()) fc.return_keys.push_back(k.get<std::string>());
                }
                if (fn_obj.contains("return_keys_non_null") && fn_obj["return_keys_non_null"].is_boolean()) fc.return_keys_non_null = fn_obj["return_keys_non_null"].get<bool>();
                if (fn_obj.contains("return_keys_non_empty") && fn_obj["return_keys_non_empty"].is_boolean()) fc.return_keys_non_empty = fn_obj["return_keys_non_empty"].get<bool>();
                if (fn_obj.contains("return_length_min") && fn_obj["return_length_min"].is_number_integer()) fc.return_length_min = fn_obj["return_length_min"].get<int>();
                if (fn_obj.contains("return_length_max") && fn_obj["return_length_max"].is_number_integer()) fc.return_length_max = fn_obj["return_length_max"].get<int>();
                if (fn_obj.contains("return_not_null") && fn_obj["return_not_null"].is_boolean()) fc.return_not_null = fn_obj["return_not_null"].get<bool>();
                if (fn_obj.contains("return_matches") && fn_obj["return_matches"].is_string()) fc.return_matches = fn_obj["return_matches"].get<std::string>();
                if (fn_obj.contains("params") && fn_obj["params"].is_array()) {
                    for (auto& p : fn_obj["params"]) if (p.is_string()) fc.params.push_back(p.get<std::string>());
                }
                if (fn_obj.contains("must_call") && fn_obj["must_call"].is_array()) {
                    for (auto& mc : fn_obj["must_call"]) if (mc.is_string()) fc.must_call.push_back(mc.get<std::string>());
                }
                if (fn_obj.contains("must_contain") && fn_obj["must_contain"].is_array()) {
                    for (auto& mc : fn_obj["must_contain"]) if (mc.is_string()) fc.must_contain.push_back(mc.get<std::string>());
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
                            for (auto& p : params) if (p.is_string()) spec.params.push_back(p.get<std::string>());
                        }
                        fc.must_derive_from.push_back(std::move(spec));
                    }
                }
                // v6: must_vary — anti-hardcoding (array of {key, across, fixtures?})
                if (fn_obj.contains("must_vary") && fn_obj["must_vary"].is_array()) {
                    for (auto& mv : fn_obj["must_vary"]) {
                        if (!mv.is_object()) continue;
                        FunctionContract::MustVarySpec spec;
                        if (mv.contains("key") && mv["key"].is_string()) spec.key = mv["key"].get<std::string>();
                        if (mv.contains("across") && mv["across"].is_string()) spec.across = mv["across"].get<std::string>();
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
                        if (md.contains("key") && md["key"].is_string()) dc.key = md["key"].get<std::string>();
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
                        if (mh.contains("expect") && mh["expect"].is_string()) spec.expect = mh["expect"].get<std::string>();
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
        rules_.explicitly_set.insert("baselines");
        if (bl.contains("enabled") && bl["enabled"].is_boolean()) rules_.baselines.enabled = bl["enabled"].get<bool>();
        if (bl.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(bl["level"]);
            if (en) rules_.baselines.level = lv;
        }
        if (bl.contains("path") && bl["path"].is_string()) rules_.baselines.path = bl["path"].get<std::string>();
        if (bl.contains("tolerance") && bl["tolerance"].is_number()) rules_.baselines.tolerance = bl["tolerance"].get<double>();
        if (bl.contains("auto_record") && bl["auto_record"].is_boolean()) rules_.baselines.auto_record = bl["auto_record"].get<bool>();
        if (bl.contains("hash_keys") && bl["hash_keys"].is_boolean()) rules_.baselines.hash_keys = bl["hash_keys"].get<bool>();
    }

    // Taint Tracking
    if (j.contains("taint_tracking") && j["taint_tracking"].is_object()) {
        auto& tt = j["taint_tracking"];
        if (tt.contains("enabled") && tt["enabled"].is_boolean()) rules_.taint_tracking.enabled = tt["enabled"].get<bool>();
        if (tt.contains("lineage") && tt["lineage"].is_boolean()) rules_.taint_tracking.lineage = tt["lineage"].get<bool>();
        if (tt.contains("level") && tt["level"].is_string()) { rules_.taint_tracking.level = tt["level"].get<std::string>(); rules_.explicitly_set.insert("taint_tracking.level"); }
        if (tt.contains("sources") && tt["sources"].is_array()) {
            for (const auto& s : tt["sources"]) {
                if (s.is_string()) rules_.taint_tracking.sources.push_back(s.get<std::string>());
            }
        }
        if (tt.contains("sinks") && tt["sinks"].is_array()) {
            for (const auto& s : tt["sinks"]) {
                if (s.is_string()) rules_.taint_tracking.sinks.push_back(s.get<std::string>());
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
                if (s.is_string()) rules_.taint_tracking.sanitizers.push_back(s.get<std::string>());
            }
        }
        parseRationale(tt, rules_.taint_tracking.rationale);
        if (tt.contains("gate_cross_block") && tt["gate_cross_block"].is_boolean()) rules_.taint_tracking.gate_cross_block = tt["gate_cross_block"].get<bool>();
        if (tt.contains("cross_block_level")) {
            auto [en, lv] = parseEnforcementLevel(tt["cross_block_level"]);
            rules_.taint_tracking.cross_block_level = lv;
        }
    }

    // --- Approval config (APPROVAL_REQUIRED tier) ---
    if (j.contains("approval") && j["approval"].is_object()) {
        auto& ap = j["approval"];
        if (ap.contains("store_path") && ap["store_path"].is_string()) rules_.approval.store_path = ap["store_path"].get<std::string>();
        if (ap.contains("approver_keys") && ap["approver_keys"].is_array()) {
            for (const auto& k : ap["approver_keys"]) {
                if (k.is_string()) rules_.approval.approver_keys.push_back(k.get<std::string>());
            }
        }
        if (ap.contains("default_expiry_hours") && ap["default_expiry_hours"].is_number_integer()) { rules_.approval.default_expiry_hours = ap["default_expiry_hours"].get<int>(); rules_.explicitly_set.insert("approval.default_expiry_hours"); }
    }

    // --- Trust Policy (Authority Decay) ---
    if (j.contains("trust") && j["trust"].is_object()) {
        auto& tp = j["trust"];
        if (tp.contains("max_signature_age_days") && tp["max_signature_age_days"].is_number_integer()) {
            rules_.trust_policy.max_signature_age_days = tp["max_signature_age_days"].get<int>(); rules_.explicitly_set.insert("trust_policy.max_signature_age_days"); }
        if (tp.contains("require_fresh_signature") && tp["require_fresh_signature"].is_boolean()) {
            rules_.trust_policy.require_fresh_signature = tp["require_fresh_signature"].get<bool>(); rules_.explicitly_set.insert("trust_policy.require_fresh_signature"); }
        if (tp.contains("stale_signature_level") && tp["stale_signature_level"].is_string()) {
            rules_.explicitly_set.insert("trust_policy.stale_signature_level");
            std::string lev = tp["stale_signature_level"].get<std::string>();
            if (lev == "hard") rules_.trust_policy.stale_signature_level = EnforcementLevel::HARD;
            else if (lev == "soft") rules_.trust_policy.stale_signature_level = EnforcementLevel::SOFT;
            else rules_.trust_policy.stale_signature_level = EnforcementLevel::ADVISORY;
        }
        if (tp.contains("check_key_expiry") && tp["check_key_expiry"].is_boolean()) {
            rules_.trust_policy.check_key_expiry = tp["check_key_expiry"].get<bool>(); rules_.explicitly_set.insert("trust_policy.check_key_expiry"); }
        if (tp.contains("check_revocation") && tp["check_revocation"].is_boolean()) {
            rules_.trust_policy.check_revocation = tp["check_revocation"].get<bool>(); rules_.explicitly_set.insert("trust_policy.check_revocation"); }
    }

    // --- Prerequisites (Environment Attestation) ---
    if (j.contains("prerequisites") && j["prerequisites"].is_object()) {
        auto& pr = j["prerequisites"];
        rules_.explicitly_set.insert("prerequisites");
        if (pr.contains("enabled") && pr["enabled"].is_boolean()) rules_.prerequisites.enabled = pr["enabled"].get<bool>();
        if (pr.contains("checks") && pr["checks"].is_array()) {
            for (const auto& chk : pr["checks"]) {
                PrerequisiteCheck pc;
                if (chk.contains("type") && chk["type"].is_string()) pc.type = chk["type"].get<std::string>();
                if (chk.contains("name") && chk["name"].is_string()) pc.name = chk["name"].get<std::string>();
                if (chk.contains("required") && chk["required"].is_string()) pc.required = chk["required"].get<std::string>();
                if (chk.contains("level") && chk["level"].is_string()) {
                    std::string lev = chk["level"].get<std::string>();
                    if (lev == "hard") pc.level = EnforcementLevel::HARD;
                    else if (lev == "soft") pc.level = EnforcementLevel::SOFT;
                    else pc.level = EnforcementLevel::ADVISORY;
                }
                if (chk.contains("message") && chk["message"].is_string()) pc.message = chk["message"].get<std::string>();
                rules_.prerequisites.checks.push_back(std::move(pc));
            }
        }
    }

    // --- Contradiction Detection ---
    if (j.contains("contradiction_detection") && j["contradiction_detection"].is_object()) {
        auto& cd = j["contradiction_detection"];
        rules_.explicitly_set.insert("contradiction_detection");
        if (cd.contains("enabled") && cd["enabled"].is_boolean()) rules_.contradiction_detection.enabled = cd["enabled"].get<bool>();
        if (cd.contains("max_level") && cd["max_level"].is_string()) {
            std::string lev = cd["max_level"].get<std::string>();
            if (lev == "hard") rules_.contradiction_detection.max_level = EnforcementLevel::HARD;
            else if (lev == "soft") rules_.contradiction_detection.max_level = EnforcementLevel::SOFT;
            else rules_.contradiction_detection.max_level = EnforcementLevel::ADVISORY;
        }
    }

    // --- Dynamic Code Generation (codegen) ---
    if (j.contains("codegen") && j["codegen"].is_object()) {
        auto& cg = j["codegen"];
        rules_.explicitly_set.insert("codegen");
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
        rules_.explicitly_set.insert("telemetry");
        if (tel.contains("enabled") && tel["enabled"].is_boolean()) rules_.telemetry_output.enabled = tel["enabled"].get<bool>();
        if (tel.contains("output_file") && tel["output_file"].is_string()) rules_.telemetry_output.output_file = tel["output_file"].get<std::string>();
        if (tel.contains("tamper_evidence") && tel["tamper_evidence"].is_object()) {
            auto& te = tel["tamper_evidence"];
            if (te.contains("enabled") && te["enabled"].is_boolean()) {
                rules_.telemetry_output.tamper_evidence.enabled = te["enabled"].get<bool>();
                rules_.explicitly_set.insert("telemetry.tamper_evidence.enabled");
            }
            if (te.contains("algorithm") && te["algorithm"].is_string()) rules_.telemetry_output.tamper_evidence.algorithm = te["algorithm"].get<std::string>();
            if (te.contains("chain_genesis") && te["chain_genesis"].is_string()) rules_.telemetry_output.tamper_evidence.chain_genesis = te["chain_genesis"].get<std::string>();
            if (te.contains("hmac_key") && te["hmac_key"].is_string()) rules_.telemetry_output.tamper_evidence.hmac_key = te["hmac_key"].get<std::string>();
            if (te.contains("hmac_key_env") && te["hmac_key_env"].is_string()) {
                std::string env_name = te["hmac_key_env"].get<std::string>();
                const char* env_val = std::getenv(env_name.c_str());
                if (env_val) rules_.telemetry_output.tamper_evidence.hmac_key = env_val;
            }
        }
        if (tel.contains("decision_snapshots") && tel["decision_snapshots"].is_boolean())
            rules_.telemetry_output.decision_snapshots = tel["decision_snapshots"].get<bool>();

        // Forwarding config
        if (tel.contains("webhook_url") && tel["webhook_url"].is_string())
            rules_.telemetry_output.webhook_url = tel["webhook_url"].get<std::string>();
        if (tel.contains("webhook_auth_header") && tel["webhook_auth_header"].is_string())
            rules_.telemetry_output.webhook_auth_header = tel["webhook_auth_header"].get<std::string>();
        if (tel.contains("webhook_auth_env") && tel["webhook_auth_env"].is_string()) {
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
        if (tel.contains("forward_batch_size") && tel["forward_batch_size"].is_number_integer()) {
            int v = tel["forward_batch_size"].get<int>();
            rules_.telemetry_output.forward_batch_size = v < 1 ? 1 : v;
        }
        if (tel.contains("forward_timeout_ms") && tel["forward_timeout_ms"].is_number_integer()) {
            int v = tel["forward_timeout_ms"].get<int>();
            rules_.telemetry_output.forward_timeout_ms = v < 1 ? 1 : v;  // 0 = infinite in libcurl
        }
        if (tel.contains("forward_retry_count") && tel["forward_retry_count"].is_number_integer()) {
            int v = tel["forward_retry_count"].get<int>();
            rules_.telemetry_output.forward_retry_count = v < 0 ? 0 : v;
        }
        if (tel.contains("forward_buffer_max") && tel["forward_buffer_max"].is_number_integer()) {
            int v = tel["forward_buffer_max"].get<int>();
            rules_.telemetry_output.forward_buffer_max = v < 0 ? 0 : v;
        }
        if (tel.contains("forward_shutdown_drain_ms") && tel["forward_shutdown_drain_ms"].is_number_integer()) {
            int v = tel["forward_shutdown_drain_ms"].get<int>();
            rules_.telemetry_output.forward_shutdown_drain_ms = v < 0 ? 0 : v;
        }
        // Fix 4B: opt-in telemetry check deduplication
        if (tel.contains("deduplicate_checks") && tel["deduplicate_checks"].is_boolean()) {
            rules_.telemetry_output.deduplicate_checks = tel["deduplicate_checks"].get<bool>();
        }

        // Agent interaction transcript
        if (tel.contains("transcript") && tel["transcript"].is_object()) {
            auto& tr = tel["transcript"];
            if (tr.contains("enabled") && tr["enabled"].is_boolean())
                rules_.transcript.enabled = tr["enabled"].get<bool>();
            if (tr.contains("output_file") && tr["output_file"].is_string())
                rules_.transcript.output_file = tr["output_file"].get<std::string>();
            if (tr.contains("agents") && tr["agents"].is_array()) {
                for (const auto& a : tr["agents"]) {
                    if (a.is_string())
                        rules_.transcript.agents.push_back(a.get<std::string>());
                }
            }
        }
    }

    // Auto-enable tamper-evident hash chain when output_file is set
    // but tamper_evidence.enabled was not explicitly configured.
    // Matches pulse monitor expectation (DEGRADED when chain not advancing).
    if (!rules_.telemetry_output.output_file.empty()
            && !rules_.explicitly_set.count("telemetry.tamper_evidence.enabled")) {
        rules_.telemetry_output.tamper_evidence.enabled = true;
    }

    // --- Agents (unified config: permissions + LLM) ---
    // Prefer "agents" key; fall back to legacy "agent_roles" for backward compat
    std::string agents_key = j.contains("agents") ? "agents" : "agent_roles";
    std::unordered_set<std::string> agents_with_explicit_timeout;
    if (j.contains(agents_key) && j[agents_key].is_object()) {
        for (auto& [name, cfg_json] : j[agents_key].items()) {
            AgentConfig agent;
            agent.name = name;

            // --- Permissions ---
            if (cfg_json.contains("allowed_languages") && cfg_json["allowed_languages"].is_array())
                for (const auto& l : cfg_json["allowed_languages"])
                    if (l.is_string()) agent.allowed_languages.push_back(l.get<std::string>());
            if (cfg_json.contains("blocked_languages") && cfg_json["blocked_languages"].is_array())
                for (const auto& l : cfg_json["blocked_languages"])
                    if (l.is_string()) agent.blocked_languages.push_back(l.get<std::string>());
            if (cfg_json.contains("blocked_paths") && cfg_json["blocked_paths"].is_array())
                for (const auto& p : cfg_json["blocked_paths"])
                    if (p.is_string()) agent.blocked_paths.push_back(p.get<std::string>());
            if (cfg_json.contains("allowed_paths") && cfg_json["allowed_paths"].is_array())
                for (const auto& p : cfg_json["allowed_paths"])
                    if (p.is_string()) agent.allowed_paths.push_back(p.get<std::string>());
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
            // Opt out of the CONTENT half of shell_allowed (response-text scan)
            // while leaving the EXECUTION half in force. Unset = scan still runs.
            if (cfg_json.contains("shell_content_allowed") &&
                cfg_json["shell_content_allowed"].is_boolean()) {
                agent.shell_content_allowed = cfg_json["shell_content_allowed"].get<bool>();
                agent.shell_content_allowed_set = true;
            }
            // Fine-grained action matrix
            if (cfg_json.contains("allowed_actions") && cfg_json["allowed_actions"].is_array()) {
                for (const auto& a : cfg_json["allowed_actions"]) {
                    if (a.is_string()) agent.allowed_actions.push_back(a.get<std::string>());
                }
            }

            // --- LLM config (only present in "agents" key, not legacy "agent_roles") ---
            if (agents_key == "agents") {
                if (cfg_json.contains("provider") && cfg_json["provider"].is_string())
                    agent.provider = cfg_json["provider"].get<std::string>();
                // model: string or array (fallback chain)
                if (cfg_json.contains("model")) {
                    if (cfg_json["model"].is_array()) {
                        agent.model_chain.clear();
                        for (const auto& m : cfg_json["model"])
                            if (m.is_string()) agent.model_chain.push_back(m.get<std::string>());
                        if (!agent.model_chain.empty())
                            agent.model = agent.model_chain[0];
                    } else if (cfg_json["model"].is_string()) {
                        agent.model = cfg_json["model"].get<std::string>();
                        agent.model_chain = {agent.model};
                    }
                }
                // api_key_env: string or array (key rotation)
                if (cfg_json.contains("api_key_env")) {
                    if (cfg_json["api_key_env"].is_array()) {
                        agent.api_key_envs.clear();
                        for (const auto& k : cfg_json["api_key_env"])
                            if (k.is_string()) agent.api_key_envs.push_back(k.get<std::string>());
                        if (!agent.api_key_envs.empty())
                            agent.api_key_env = agent.api_key_envs[0];
                    } else if (cfg_json["api_key_env"].is_string()) {
                        agent.api_key_env = cfg_json["api_key_env"].get<std::string>();
                        agent.api_key_envs = {agent.api_key_env};
                    }
                }
                if (cfg_json.contains("max_tokens") && cfg_json["max_tokens"].is_number_integer())
                    agent.max_tokens = cfg_json["max_tokens"].get<int>();
                if (cfg_json.contains("min_tokens") && cfg_json["min_tokens"].is_number_integer())
                    agent.min_tokens = std::max(0, cfg_json["min_tokens"].get<int>());
                if (cfg_json.contains("system_prompt") && cfg_json["system_prompt"].is_string())
                    agent.system_prompt = cfg_json["system_prompt"].get<std::string>();
                if (cfg_json.contains("tools") && cfg_json["tools"].is_array())
                    for (const auto& t : cfg_json["tools"])
                        if (t.is_string()) agent.tools.push_back(t.get<std::string>());
                // Tool execution configuration
                if (cfg_json.contains("tools_enabled") && cfg_json["tools_enabled"].is_boolean())
                    agent.tools_enabled = cfg_json["tools_enabled"].get<bool>();
                if (cfg_json.contains("max_tool_calls_per_turn") && cfg_json["max_tool_calls_per_turn"].is_number_integer())
                    agent.max_tool_calls_per_turn = std::max(1, cfg_json["max_tool_calls_per_turn"].get<int>());
                if (cfg_json.contains("max_tool_loop_turns") && cfg_json["max_tool_loop_turns"].is_number_integer())
                    agent.max_tool_loop_turns = std::max(1, cfg_json["max_tool_loop_turns"].get<int>());
                if (cfg_json.contains("tool_result_max_chars") && cfg_json["tool_result_max_chars"].is_number_integer())
                    agent.tool_result_max_chars = std::max(0, cfg_json["tool_result_max_chars"].get<int>());
                if (cfg_json.contains("tool_result_max_total_chars") && cfg_json["tool_result_max_total_chars"].is_number_integer())
                    agent.tool_result_max_total_chars = std::max(0, cfg_json["tool_result_max_total_chars"].get<int>());
                if (cfg_json.contains("tool_timeout_seconds") && cfg_json["tool_timeout_seconds"].is_number_integer())
                    agent.tool_timeout_seconds = std::max(1, cfg_json["tool_timeout_seconds"].get<int>());
                if (cfg_json.contains("max_turns") && cfg_json["max_turns"].is_number_integer())
                    agent.max_turns = cfg_json["max_turns"].get<int>();
                if (cfg_json.contains("max_total_tokens") && cfg_json["max_total_tokens"].is_number_integer())
                    agent.max_total_tokens = cfg_json["max_total_tokens"].get<int>();
                // agent.propose() candidate cap (0 = propose disabled)
                if (cfg_json.contains("propose_candidates_max") && cfg_json["propose_candidates_max"].is_number_integer())
                    agent.propose_candidates_max = std::max(0, cfg_json["propose_candidates_max"].get<int>());
                if (cfg_json.contains("temperature") && cfg_json["temperature"].is_number())
                    agent.temperature = cfg_json["temperature"].get<double>();
                if (cfg_json.contains("stop_reason_action") && cfg_json["stop_reason_action"].is_string())
                    agent.stop_reason_action = cfg_json["stop_reason_action"].get<std::string>();
                if (cfg_json.contains("stream") && cfg_json["stream"].is_boolean())
                    agent.stream = cfg_json["stream"].get<bool>();
                if (cfg_json.contains("timeout") && cfg_json["timeout"].is_number_integer()) {
                    agent.timeout_seconds = cfg_json["timeout"].get<int>();
                    agents_with_explicit_timeout.insert(name);
                }
                if (cfg_json.contains("response_format") && cfg_json["response_format"].is_string())
                    agent.response_format = cfg_json["response_format"].get<std::string>();
                if (cfg_json.contains("risk_budget") && cfg_json["risk_budget"].is_number_integer()) {
                    agent.risk_budget = cfg_json["risk_budget"].get<int>();
                    if (agent.risk_budget < 0) agent.risk_budget = 0;
                }
                // API endpoint base override — https only, except loopback (test stubs)
                if (cfg_json.contains("api_base") && cfg_json["api_base"].is_string()) {
                    std::string base = cfg_json["api_base"].get<std::string>();
                    bool https = base.rfind("https://", 0) == 0;
                    bool loopback_http = base.rfind("http://127.0.0.1", 0) == 0 ||
                                         base.rfind("http://localhost", 0) == 0 ||
                                         base.rfind("http://[::1]", 0) == 0;
                    if (https || loopback_http) {
                        agent.api_base = base;
                    } else if (!base.empty()) {
                        fprintf(stderr, "[governance] agent '%s': api_base ignored "
                                "(must be https:// or loopback http://)\n", name.c_str());
                    }
                }
                // Thinking budget — -1=provider default, 0=disable, >0=budget
                if (cfg_json.contains("thinking_budget") && cfg_json["thinking_budget"].is_number_integer()) {
                    agent.thinking_budget = cfg_json["thinking_budget"].get<int>();
                    if (agent.thinking_budget < -1) agent.thinking_budget = -1;
                }
                // Standing Lease — TTL on agent authorization
                if (cfg_json.contains("standing_lease_turns") && cfg_json["standing_lease_turns"].is_number_integer())
                    agent.standing_lease_turns = std::max(0, cfg_json["standing_lease_turns"].get<int>());
                if (cfg_json.contains("standing_lease_seconds") && cfg_json["standing_lease_seconds"].is_number_integer())
                    agent.standing_lease_seconds = std::max(0, cfg_json["standing_lease_seconds"].get<int>());
                // Context windowing
                if (cfg_json.contains("context_window") && cfg_json["context_window"].is_number_integer())
                    agent.context_window = std::max(0, cfg_json["context_window"].get<int>());
                if (cfg_json.contains("context_strategy") && cfg_json["context_strategy"].is_string()) {
                    std::string val = cfg_json["context_strategy"].get<std::string>();
                    if (val == "full" || val == "recent" || val == "summary")
                        agent.context_strategy = val;
                }
                // Retry configuration
                if (cfg_json.contains("retry") && cfg_json["retry"].is_object()) {
                    auto& r = cfg_json["retry"];
                    if (r.contains("max_attempts") && r["max_attempts"].is_number_integer())
                        agent.retry.max_attempts = std::max(1, r["max_attempts"].get<int>());
                    if (r.contains("backoff_ms") && r["backoff_ms"].is_number_integer())
                        agent.retry.backoff_ms = std::max(0, r["backoff_ms"].get<int>());
                    if (r.contains("backoff_multiplier") && r["backoff_multiplier"].is_number())
                        agent.retry.backoff_multiplier = std::max(1.0, r["backoff_multiplier"].get<double>());
                    if (r.contains("jitter") && r["jitter"].is_boolean())
                        agent.retry.jitter = r["jitter"].get<bool>();
                    if (r.contains("retry_on") && r["retry_on"].is_array()) {
                        agent.retry.retry_on.clear();
                        for (const auto& c : r["retry_on"]) { if (c.is_number_integer()) agent.retry.retry_on.push_back(c.get<int>()); }
                    }
                    if (r.contains("skip_key_on") && r["skip_key_on"].is_array()) {
                        agent.retry.skip_key_on.clear();
                        for (const auto& c : r["skip_key_on"]) { if (c.is_number_integer()) agent.retry.skip_key_on.push_back(c.get<int>()); }
                    }
                    if (r.contains("fallback_model_on") && r["fallback_model_on"].is_array()) {
                        agent.retry.fallback_model_on.clear();
                        for (const auto& c : r["fallback_model_on"]) { if (c.is_number_integer()) agent.retry.fallback_model_on.push_back(c.get<int>()); }
                    }
                    if (r.contains("never_retry") && r["never_retry"].is_array()) {
                        agent.retry.never_retry.clear();
                        for (const auto& c : r["never_retry"]) { if (c.is_number_integer()) agent.retry.never_retry.push_back(c.get<int>()); }
                    }
                    if (r.contains("key_retry_after_seconds") && r["key_retry_after_seconds"].is_number_integer())
                        agent.retry.key_retry_after_seconds = std::max(0, r["key_retry_after_seconds"].get<int>());
                }
                // Rate limit configuration
                if (cfg_json.contains("rate_limit") && cfg_json["rate_limit"].is_object()) {
                    auto& rl = cfg_json["rate_limit"];
                    if (rl.contains("requests_per_minute") && rl["requests_per_minute"].is_number_integer())
                        agent.rate_limit.requests_per_minute = std::max(0, rl["requests_per_minute"].get<int>());
                    if (rl.contains("delay_between_calls_ms") && rl["delay_between_calls_ms"].is_number_integer())
                        agent.rate_limit.delay_between_calls_ms = std::max(0, rl["delay_between_calls_ms"].get<int>());
                }
                // Output Contract — validation schema for LLM responses (Phase 7)
                if (cfg_json.contains("output_contract") && cfg_json["output_contract"].is_object()) {
                    auto& oc = cfg_json["output_contract"];
                    if (oc.contains("format") && oc["format"].is_string())
                        agent.output_contract.format = oc["format"].get<std::string>();
                    if (oc.contains("required_fields") && oc["required_fields"].is_array()) {
                        agent.output_contract.required_fields.clear();
                        for (const auto& f : oc["required_fields"])
                            if (f.is_string()) agent.output_contract.required_fields.push_back(f.get<std::string>());
                    }
                    if (oc.contains("field_types") && oc["field_types"].is_object()) {
                        agent.output_contract.field_types.clear();
                        for (auto& [field, type] : oc["field_types"].items())
                            if (type.is_string()) agent.output_contract.field_types[field] = type.get<std::string>();
                    }
                    if (oc.contains("regex_checks") && oc["regex_checks"].is_object()) {
                        agent.output_contract.regex_checks.clear();
                        for (auto& [field, pattern] : oc["regex_checks"].items())
                            if (pattern.is_string()) agent.output_contract.regex_checks[field] = pattern.get<std::string>();
                    }
                }

                // Per-agent CDD signal overrides (keys = canonical signal
                // config names; unknown keys warn instead of silently
                // fail-opening on a typo)
                if (cfg_json.contains("context_drift_signals") &&
                    cfg_json["context_drift_signals"].is_object()) {
                    for (auto& [sig_key, sig_val] : cfg_json["context_drift_signals"].items()) {
                        bool known = false;
                        for (int si = 0; si < NUM_CDD_SIGNALS; si++) {
                            if (sig_key == kCddSignalKeys[si]) { known = true; break; }
                        }
                        if (!known) {
                            // The name a reader is most likely to type here is
                            // the one they just saw firing in telemetry -- and
                            // for four signals that is NOT the config key:
                            //
                            //   config key                   telemetry label
                            //   circular_actions             circular
                            //   intent_contradictions        contradictions
                            //   vocabulary_contraction       vocab_contraction
                            //   capability_underutilization  capability_underutil
                            //
                            // penalties_detail/signals_detail print signalName();
                            // this parser matches kCddSignalKeys. Copying a name
                            // across therefore disables NOTHING: the signal keeps
                            // firing and paying, and the only symptom is one
                            // stderr line that says "unknown" about a signal the
                            // operator can plainly see working.
                            //
                            // The alias is deliberately NOT accepted. A second
                            // spelling silently working would be config surface
                            // the ratchet comparison and the override bitmask do
                            // not know about. Naming the canonical key turns a
                            // dead-end warning into the fix.
                            const char* suggestion = nullptr;
                            for (int si = 0; si < NUM_CDD_SIGNALS; si++) {
                                if (sig_key == ContextDriftAnalyzer::signalName(si)) {
                                    suggestion = kCddSignalKeys[si];
                                    break;
                                }
                            }
                            if (suggestion) {
                                fprintf(stderr,
                                    "[governance] Warning: unknown context_drift_signals key \"%s\" "
                                    "for agent \"%s\" — that is the telemetry label; "
                                    "the config key is \"%s\" (signal NOT overridden)\n",
                                    sig_key.c_str(), agent.name.c_str(), suggestion);
                            } else {
                                fprintf(stderr,
                                    "[governance] Warning: unknown context_drift_signals key \"%s\" for agent \"%s\"\n",
                                    sig_key.c_str(), agent.name.c_str());
                            }
                            continue;
                        }
                        if (sig_val.is_boolean()) {
                            agent.context_drift_signals[sig_key] = sig_val.get<bool>();
                        }
                    }
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
        rules_.explicitly_set.insert("quality_gate");
        if (qg.contains("enabled") && qg["enabled"].is_boolean()) rules_.quality_gate.enabled = qg["enabled"].get<bool>();
        if (qg.contains("conditions") && qg["conditions"].is_array()) {
            for (const auto& cond : qg["conditions"]) {
                QualityGateCondition c;
                if (cond.contains("metric") && cond["metric"].is_string()) c.metric = cond["metric"].get<std::string>();
                if (cond.contains("operator") && cond["operator"].is_string()) c.op = cond["operator"].get<std::string>();
                if (cond.contains("threshold") && cond["threshold"].is_number_integer()) c.threshold = cond["threshold"].get<int>();
                rules_.quality_gate.conditions.push_back(c);
            }
        }
    }

    // --- Cumulative Risk Scoring ---
    if (j.contains("scoring") && j["scoring"].is_object()) {
        auto& sc = j["scoring"];
        rules_.explicitly_set.insert("scoring");
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
        if (sc.contains("yellow_threshold")) rules_.explicitly_set.insert("scoring.yellow_threshold");
        if (sc.contains("red_threshold")) rules_.explicitly_set.insert("scoring.red_threshold");
        rules_.scoring.threshold_mode   = sc.value("threshold_mode", "fixed");
        parseRationale(sc, rules_.scoring.rationale);
        if (rules_.scoring.yellow_threshold > rules_.scoring.red_threshold) {
            fmt::print(stderr, "[WARN] scoring.yellow_threshold ({}) > red_threshold ({}) — "
                       "clamping yellow to red\n",
                       rules_.scoring.yellow_threshold, rules_.scoring.red_threshold);
            rules_.scoring.yellow_threshold = rules_.scoring.red_threshold;
        }
    }

    // --- Scoring Calibration (operator-driven weight overrides) ---
    if (j.contains("scoring_calibration") && j["scoring_calibration"].is_object()) {
        auto& sc = j["scoring_calibration"];
        rules_.scoring_calibration.enabled = sc.value("enabled", false);
        if (sc.contains("path") && sc["path"].is_string())
            rules_.scoring_calibration.path = sc["path"].get<std::string>();
        rules_.scoring_calibration.auto_save = sc.value("auto_save", true);
        parseRationale(sc, rules_.scoring_calibration.rationale);
    }

    // --- Agent Review (LLM-based governance phase) ---
    if (j.contains("agent_review") && j["agent_review"].is_object()) {
        auto& ar = j["agent_review"];
        rules_.explicitly_set.insert("agent_review");
        rules_.agent_review.enabled = ar.value("enabled", false);
        rules_.agent_review.scorer = ar.value("scorer", "");
        rules_.agent_review.validation = ar.value("validation", "");
        rules_.agent_review.voice = ar.value("voice", "");
        rules_.agent_review.cache = ar.value("cache", false);
        rules_.agent_review.hints = ar.value("hints", false);
        // Default: "open" (warn-only). agent_review itself defaults to disabled.
        // Set to "closed" for fail-safe behavior when agent_review is enabled.
        rules_.agent_review.fail_policy = ar.value("fail_policy", "open");
        rules_.agent_review.dispatch_mode = ar.value("dispatch_mode", "sequential");
        rules_.agent_review.fail_strategy = ar.value("fail_strategy", "fail_fast");
        if (ar.contains("max_parallel") && ar["max_parallel"].is_number_integer()) {
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
        if (ad.contains("max_concurrent") && ad["max_concurrent"].is_number_integer()) {
            rules_.agent_dispatch.max_concurrent = ad["max_concurrent"].get<int>(); rules_.explicitly_set.insert("agent_dispatch.max_concurrent"); }
        if (ad.contains("pool_size") && ad["pool_size"].is_number_integer()) {
            rules_.agent_dispatch.pool_size = ad["pool_size"].get<int>(); rules_.explicitly_set.insert("agent_dispatch.pool_size"); }
        if (ad.contains("pool_queue_max") && ad["pool_queue_max"].is_number_integer()) {
            rules_.agent_dispatch.pool_queue_max = ad["pool_queue_max"].get<int>(); rules_.explicitly_set.insert("agent_dispatch.pool_queue_max"); }
        if (ad.contains("max_retries_per_run") && ad["max_retries_per_run"].is_number_integer()) {
            rules_.agent_dispatch.max_retries_per_run = std::max(0, ad["max_retries_per_run"].get<int>()); rules_.explicitly_set.insert("agent_dispatch.max_retries_per_run"); }
        if (ad.contains("default_timeout_seconds") && ad["default_timeout_seconds"].is_number_integer()) {
            rules_.agent_dispatch.default_timeout_seconds = std::max(0, ad["default_timeout_seconds"].get<int>()); rules_.explicitly_set.insert("agent_dispatch.default_timeout_seconds"); }
        if (ad.contains("hard_stop") && ad["hard_stop"].is_object()) {
            auto& hs = ad["hard_stop"];
            if (hs.contains("max_calls_per_run") && hs["max_calls_per_run"].is_number_integer()) {
                rules_.agent_dispatch.hard_stop.max_calls_per_run = std::max(0, hs["max_calls_per_run"].get<int>()); rules_.explicitly_set.insert("agent_dispatch.hard_stop.max_calls_per_run"); }
            if (hs.contains("max_tokens_per_run") && hs["max_tokens_per_run"].is_number_integer()) {
                rules_.agent_dispatch.hard_stop.max_tokens_per_run = std::max(0, hs["max_tokens_per_run"].get<int>()); rules_.explicitly_set.insert("agent_dispatch.hard_stop.max_tokens_per_run"); }
            if (hs.contains("max_agent_time_ms") && hs["max_agent_time_ms"].is_number_integer()) {
                rules_.agent_dispatch.hard_stop.max_agent_time_ms = std::max(0, hs["max_agent_time_ms"].get<int>()); rules_.explicitly_set.insert("agent_dispatch.hard_stop.max_agent_time_ms"); }
            if (hs.contains("consecutive_failure_limit") && hs["consecutive_failure_limit"].is_number_integer()) {
                rules_.agent_dispatch.hard_stop.consecutive_failure_limit = std::max(0, hs["consecutive_failure_limit"].get<int>()); rules_.explicitly_set.insert("agent_dispatch.hard_stop.consecutive_failure_limit"); }
            if (hs.contains("action") && hs["action"].is_string()) {
                rules_.agent_dispatch.hard_stop.action = hs["action"].get<std::string>(); rules_.explicitly_set.insert("agent_dispatch.hard_stop.action"); }
        }
    }

    // Apply agent_dispatch.default_timeout_seconds to agents without explicit timeout
    if (rules_.agent_dispatch.default_timeout_seconds > 0) {
        for (auto& agent : rules_.agents) {
            if (!agents_with_explicit_timeout.count(agent.name)) {
                agent.timeout_seconds = rules_.agent_dispatch.default_timeout_seconds;
            }
        }
    }

    // --- Behavioral Sequence Detection ---
    if (j.contains("behavioral_sequences") && j["behavioral_sequences"].is_object()) { rules_.explicitly_set.insert("behavioral_sequences");
        auto& bs = j["behavioral_sequences"];
        auto& cfg = rules_.behavioral_sequences;
        if (bs.contains("enabled") && bs["enabled"].is_boolean()) cfg.enabled = bs["enabled"].get<bool>();
        if (bs.contains("window_size") && bs["window_size"].is_number_unsigned()) cfg.window_size = bs["window_size"].get<size_t>();
        if (bs.contains("default_pattern_enforcement") && bs["default_pattern_enforcement"].is_string()) {
            std::string dpe = bs["default_pattern_enforcement"].get<std::string>();
            if (dpe == "observe" || dpe == "declared") {
                cfg.default_pattern_enforcement = dpe;
            } else {
                fmt::print(stderr, "[governance] Warning: unknown "
                    "behavioral_sequences.default_pattern_enforcement \"{}\" — valid values are "
                    "\"observe\" and \"declared\"; keeping \"{}\"\n", dpe, cfg.default_pattern_enforcement);
            }
        }
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
                if (pat.contains("rationale") && pat["rationale"].is_string()) sp.rationale = pat["rationale"].get<std::string>();
                if (pat.contains("cross_agent") && pat["cross_agent"].is_boolean()) sp.cross_agent = pat["cross_agent"].get<bool>();
                if (pat.contains("sequence") && pat["sequence"].is_array()) {
                    for (auto& step_str : pat["sequence"]) {
                        SequenceStep step;
                        std::string s = (step_str.is_string() ? step_str.get<std::string>() : std::string());
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
    if (j.contains("context_drift") && j["context_drift"].is_object()) { rules_.explicitly_set.insert("context_drift");
        auto& cd = j["context_drift"];
        auto& cfg = rules_.context_drift;
        if (cd.contains("enabled") && cd["enabled"].is_boolean()) cfg.enabled = cd["enabled"].get<bool>();
        if (cd.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(cd["level"]);
            cfg.level = lv;
        }
        if (cd.contains("coherence_threshold") && cd["coherence_threshold"].is_number()) {
            cfg.coherence_threshold = cd["coherence_threshold"].get<double>();
            cfg.coherence_threshold = std::max(0.0, std::min(1.0, cfg.coherence_threshold));
        }
        if (cd.contains("max_contradictions") && cd["max_contradictions"].is_number_integer()) cfg.max_contradictions = cd["max_contradictions"].get<int>();
        if (cd.contains("check_interval_turns") && cd["check_interval_turns"].is_number_integer()) cfg.check_interval_turns = cd["check_interval_turns"].get<int>();
        if (cd.contains("event_feed") && cd["event_feed"].is_string()) {
            std::string feed = cd["event_feed"].get<std::string>();
            if (feed == "turn_bucket" || feed == "since_last_check") {
                cfg.event_feed = feed;
            } else {
                // Named rather than silently ignored: an unrecognised value here
                // would otherwise leave the operator believing they had switched
                // the feed, which is the A1c failure shape.
                fmt::print(stderr, "[governance] Warning: unknown context_drift.event_feed "
                                   "\"{}\" — valid values are \"turn_bucket\" and "
                                   "\"since_last_check\"; keeping \"{}\"\n",
                           feed, cfg.event_feed);
            }
        }
        if (cd.contains("fingerprint_window") && cd["fingerprint_window"].is_number_integer()) cfg.fingerprint_window = cd["fingerprint_window"].get<int>();
        if (cd.contains("rate_normalized") && cd["rate_normalized"].is_boolean()) cfg.rate_normalized = cd["rate_normalized"].get<bool>();
        if (cd.contains("rate_normalized_floor") && cd["rate_normalized_floor"].is_number()) cfg.rate_normalized_floor = std::max(0.0, std::min(1.0, cd["rate_normalized_floor"].get<double>()));
        if (cd.contains("coherence_recovery_amount") && cd["coherence_recovery_amount"].is_number()) cfg.coherence_recovery_amount = cd["coherence_recovery_amount"].get<double>();
        if (cd.contains("coherence_recovery_cap") && cd["coherence_recovery_cap"].is_number()) cfg.coherence_recovery_cap = cd["coherence_recovery_cap"].get<double>();
        if (cd.contains("coherence_natural_healing") && cd["coherence_natural_healing"].is_number()) cfg.coherence_natural_healing = cd["coherence_natural_healing"].get<double>();
        if (cd.contains("coherence_healing_damage_fraction") && cd["coherence_healing_damage_fraction"].is_number()) cfg.coherence_healing_damage_fraction = cd["coherence_healing_damage_fraction"].get<double>();
        if (cd.contains("coherence_damage_window") && cd["coherence_damage_window"].is_number_integer()) cfg.coherence_damage_window = cd["coherence_damage_window"].get<int>();
        if (cd.contains("validation_recovery_amount") && cd["validation_recovery_amount"].is_number()) cfg.validation_recovery_amount = std::max(0.0, std::min(1.0, cd["validation_recovery_amount"].get<double>()));
        if (cd.contains("adaptive_absorption_limit") && cd["adaptive_absorption_limit"].is_number_integer()) cfg.adaptive_absorption_limit = std::max(0, cd["adaptive_absorption_limit"].get<int>());
        if (cd.contains("response_min_output_tokens") && cd["response_min_output_tokens"].is_number_integer()) cfg.thresholds.response_min_output_tokens = std::max(1, cd["response_min_output_tokens"].get<int>());
        if (cd.contains("temporal_decay_enabled") && cd["temporal_decay_enabled"].is_boolean()) cfg.temporal_decay_enabled = cd["temporal_decay_enabled"].get<bool>();
        if (cd.contains("temporal_decay_per_minute") && cd["temporal_decay_per_minute"].is_number()) cfg.temporal_decay_per_minute = std::max(0.0, cd["temporal_decay_per_minute"].get<double>());
        if (cd.contains("temporal_decay_grace_minutes") && cd["temporal_decay_grace_minutes"].is_number()) cfg.temporal_decay_grace_minutes = std::max(0.0, cd["temporal_decay_grace_minutes"].get<double>());
        if (cd.contains("adaptive_baseline_enabled") && cd["adaptive_baseline_enabled"].is_boolean()) cfg.adaptive_baseline_enabled = cd["adaptive_baseline_enabled"].get<bool>();
        if (cd.contains("adaptive_baseline_window") && cd["adaptive_baseline_window"].is_number_integer()) cfg.adaptive_baseline_window = std::max(1, cd["adaptive_baseline_window"].get<int>());
        // escalation_effectiveness_window was declared, documented and set by a
        // shipped test config (tests/gorilla/naab-53/src/govern.json) while never
        // being parsed — every sibling key in ContextDriftConfig is read here and
        // this one alone was not, so it stayed hardcoded at its default of 5
        // whatever govern.json said. Verified by setting it to 3 and watching the
        // dashboard report a 5-turn window.
        if (cd.contains("escalation_effectiveness_window") && cd["escalation_effectiveness_window"].is_number_integer()) cfg.escalation_effectiveness_window = std::max(0, cd["escalation_effectiveness_window"].get<int>());
        if (cd.contains("adaptive_baseline_sensitivity") && cd["adaptive_baseline_sensitivity"].is_number()) cfg.adaptive_baseline_sensitivity = std::max(0.0, cd["adaptive_baseline_sensitivity"].get<double>());
        if (cd.contains("thresholds") && cd["thresholds"].is_object()) {
            auto& th = cd["thresholds"];
            if (th.contains("velocity_drop") && th["velocity_drop"].is_number()) cfg.thresholds.velocity_drop = th["velocity_drop"].get<double>();
            if (th.contains("circular_lookback") && th["circular_lookback"].is_number_integer()) cfg.thresholds.circular_lookback = std::max(1, th["circular_lookback"].get<int>());
            if (th.contains("underutilization_delay") && th["underutilization_delay"].is_number_integer()) cfg.thresholds.underutilization_delay = std::max(0, th["underutilization_delay"].get<int>());
            if (th.contains("scope_creep_min_history") && th["scope_creep_min_history"].is_number_integer()) cfg.thresholds.scope_creep_min_history = std::max(1, th["scope_creep_min_history"].get<int>());
            if (th.contains("scope_creep_min_new_types") && th["scope_creep_min_new_types"].is_number_integer()) cfg.thresholds.scope_creep_min_new_types = std::max(1, th["scope_creep_min_new_types"].get<int>());
            if (th.contains("repeated_failure_count") && th["repeated_failure_count"].is_number_integer()) cfg.thresholds.repeated_failure_count = std::max(1, th["repeated_failure_count"].get<int>());
            if (th.contains("vocab_contraction_window") && th["vocab_contraction_window"].is_number_integer()) cfg.thresholds.vocab_contraction_window = std::max(2, th["vocab_contraction_window"].get<int>());
            if (th.contains("entropy_min_initial") && th["entropy_min_initial"].is_number()) cfg.thresholds.entropy_min_initial = std::max(0.0, th["entropy_min_initial"].get<double>());
            if (th.contains("entropy_contraction_ratio") && th["entropy_contraction_ratio"].is_number()) cfg.thresholds.entropy_contraction_ratio = std::max(0.0, std::min(1.0, th["entropy_contraction_ratio"].get<double>()));
            if (th.contains("coherence_history_size") && th["coherence_history_size"].is_number_integer()) cfg.thresholds.coherence_history_size = std::max(2, th["coherence_history_size"].get<int>());
            if (th.contains("response_quality_min_ratio") && th["response_quality_min_ratio"].is_number()) cfg.thresholds.response_quality_min_ratio = std::max(0.0, std::min(1.0, th["response_quality_min_ratio"].get<double>()));
            if (th.contains("thinking_collapse_ratio") && th["thinking_collapse_ratio"].is_number()) cfg.thresholds.thinking_collapse_ratio = std::max(0.0, std::min(1.0, th["thinking_collapse_ratio"].get<double>()));
            if (th.contains("thinking_history_window") && th["thinking_history_window"].is_number_integer()) cfg.thresholds.thinking_history_window = std::max(2, th["thinking_history_window"].get<int>());
            if (th.contains("semantic_stability_min_overlap") && th["semantic_stability_min_overlap"].is_number()) cfg.thresholds.semantic_stability_min_overlap = std::max(0.0, std::min(1.0, th["semantic_stability_min_overlap"].get<double>()));
            if (th.contains("mandate_alignment_min") && th["mandate_alignment_min"].is_number()) cfg.thresholds.mandate_alignment_min = std::max(0.0, std::min(1.0, th["mandate_alignment_min"].get<double>()));
            if (th.contains("context_growth_factor") && th["context_growth_factor"].is_number()) cfg.thresholds.context_growth_factor = std::max(1.5, th["context_growth_factor"].get<double>());
            if (th.contains("instruction_recall_min") && th["instruction_recall_min"].is_number()) cfg.thresholds.instruction_recall_min = std::max(0.0, std::min(1.0, th["instruction_recall_min"].get<double>()));
            if (th.contains("plan_step_overlap_min") && th["plan_step_overlap_min"].is_number()) cfg.thresholds.plan_step_overlap_min = std::max(0.0, std::min(1.0, th["plan_step_overlap_min"].get<double>()));
            if (th.contains("entity_context_min_overlap") && th["entity_context_min_overlap"].is_number()) cfg.thresholds.entity_context_min_overlap = std::max(0.0, std::min(1.0, th["entity_context_min_overlap"].get<double>()));
            if (th.contains("entity_context_window") && th["entity_context_window"].is_number_integer()) cfg.thresholds.entity_context_window = std::max(1, th["entity_context_window"].get<int>());
            if (th.contains("instruction_conflict_topic_overlap") && th["instruction_conflict_topic_overlap"].is_number()) cfg.thresholds.instruction_conflict_topic_overlap = std::max(0.0, std::min(1.0, th["instruction_conflict_topic_overlap"].get<double>()));
            if (th.contains("instruction_conflict_window") && th["instruction_conflict_window"].is_number_integer()) cfg.thresholds.instruction_conflict_window = std::max(2, th["instruction_conflict_window"].get<int>());
            if (th.contains("persona_deviation_factor") && th["persona_deviation_factor"].is_number()) cfg.thresholds.persona_deviation_factor = std::max(1.0, th["persona_deviation_factor"].get<double>());
            if (th.contains("persona_history_window") && th["persona_history_window"].is_number_integer()) cfg.thresholds.persona_history_window = std::max(2, th["persona_history_window"].get<int>());
                if (th.contains("persona_baseline_adaptive") && th["persona_baseline_adaptive"].is_boolean()) cfg.thresholds.persona_baseline_adaptive = th["persona_baseline_adaptive"].get<bool>();
            if (th.contains("tool_result_recall_min") && th["tool_result_recall_min"].is_number()) cfg.thresholds.tool_result_recall_min = std::max(0.0, std::min(1.0, th["tool_result_recall_min"].get<double>()));
            if (th.contains("claim_accuracy_min") && th["claim_accuracy_min"].is_number()) cfg.thresholds.claim_accuracy_min = std::max(0.0, std::min(1.0, th["claim_accuracy_min"].get<double>()));
            if (th.contains("prompt_compliance_mandate_min") && th["prompt_compliance_mandate_min"].is_number()) cfg.thresholds.prompt_compliance_mandate_min = std::max(0.0, std::min(1.0, th["prompt_compliance_mandate_min"].get<double>()));
            if (th.contains("prompt_compliance_response_min_tokens") && th["prompt_compliance_response_min_tokens"].is_number_integer()) cfg.thresholds.prompt_compliance_response_min_tokens = std::max(0, th["prompt_compliance_response_min_tokens"].get<int>());
            if (th.contains("response_repetition_lookback") && th["response_repetition_lookback"].is_number_integer()) cfg.thresholds.response_repetition_lookback = std::max(1, th["response_repetition_lookback"].get<int>());
        }
        parseRationale(cd, cfg.rationale);
        if (cd.contains("signals") && cd["signals"].is_object()) {
            auto& sig = cd["signals"];
            if (sig.contains("repeated_failures") && sig["repeated_failures"].is_boolean()) cfg.signals.repeated_failures = sig["repeated_failures"].get<bool>();
            if (sig.contains("circular_actions") && sig["circular_actions"].is_boolean()) cfg.signals.circular_actions = sig["circular_actions"].get<bool>();
            if (sig.contains("scope_creep") && sig["scope_creep"].is_boolean()) cfg.signals.scope_creep = sig["scope_creep"].get<bool>();
            if (sig.contains("intent_contradictions") && sig["intent_contradictions"].is_boolean()) cfg.signals.intent_contradictions = sig["intent_contradictions"].get<bool>();
            if (sig.contains("vocabulary_contraction") && sig["vocabulary_contraction"].is_boolean()) cfg.signals.vocabulary_contraction = sig["vocabulary_contraction"].get<bool>();
            if (sig.contains("coherence_velocity") && sig["coherence_velocity"].is_boolean()) cfg.signals.coherence_velocity = sig["coherence_velocity"].get<bool>();
            if (sig.contains("capability_underutilization") && sig["capability_underutilization"].is_boolean()) cfg.signals.capability_underutilization = sig["capability_underutilization"].get<bool>();
            if (sig.contains("semantic_stability") && sig["semantic_stability"].is_boolean()) cfg.signals.semantic_stability = sig["semantic_stability"].get<bool>();
            if (sig.contains("mandate_alignment") && sig["mandate_alignment"].is_boolean()) cfg.signals.mandate_alignment = sig["mandate_alignment"].get<bool>();
            if (sig.contains("response_quality") && sig["response_quality"].is_boolean()) cfg.signals.response_quality = sig["response_quality"].get<bool>();
            if (sig.contains("thinking_collapse") && sig["thinking_collapse"].is_boolean()) cfg.signals.thinking_collapse = sig["thinking_collapse"].get<bool>();
            if (sig.contains("context_growth") && sig["context_growth"].is_boolean()) cfg.signals.context_growth = sig["context_growth"].get<bool>();
            if (sig.contains("instruction_recall") && sig["instruction_recall"].is_boolean()) cfg.signals.instruction_recall = sig["instruction_recall"].get<bool>();
            if (sig.contains("plan_drift") && sig["plan_drift"].is_boolean()) cfg.signals.plan_drift = sig["plan_drift"].get<bool>();
            if (sig.contains("entity_consistency") && sig["entity_consistency"].is_boolean()) cfg.signals.entity_consistency = sig["entity_consistency"].get<bool>();
            if (sig.contains("instruction_conflict") && sig["instruction_conflict"].is_boolean()) cfg.signals.instruction_conflict = sig["instruction_conflict"].get<bool>();
            if (sig.contains("persona_fingerprint") && sig["persona_fingerprint"].is_boolean()) cfg.signals.persona_fingerprint = sig["persona_fingerprint"].get<bool>();
            if (sig.contains("tool_chain_integrity") && sig["tool_chain_integrity"].is_boolean()) cfg.signals.tool_chain_integrity = sig["tool_chain_integrity"].get<bool>();
            if (sig.contains("claim_result_reconciliation") && sig["claim_result_reconciliation"].is_boolean()) cfg.signals.claim_result_reconciliation = sig["claim_result_reconciliation"].get<bool>();
            if (sig.contains("prompt_compliance") && sig["prompt_compliance"].is_boolean()) cfg.signals.prompt_compliance = sig["prompt_compliance"].get<bool>();
            if (sig.contains("response_repetition") && sig["response_repetition"].is_boolean()) cfg.signals.response_repetition = sig["response_repetition"].get<bool>();
            if (sig.contains("validation_outcome") && sig["validation_outcome"].is_boolean()) cfg.signals.validation_outcome = sig["validation_outcome"].get<bool>();
            if (sig.contains("response_degenerate") && sig["response_degenerate"].is_boolean()) cfg.signals.response_degenerate = sig["response_degenerate"].get<bool>();
            if (sig.contains("exclude_infrastructure_errors") && sig["exclude_infrastructure_errors"].is_boolean()) cfg.signals.exclude_infrastructure_errors = sig["exclude_infrastructure_errors"].get<bool>();
        }
        if (cd.contains("weights") && cd["weights"].is_object()) {
            auto& w = cd["weights"];
            if (w.contains("circular") && w["circular"].is_number()) cfg.weights.circular = w["circular"].get<double>();
            if (w.contains("scope_creep") && w["scope_creep"].is_number()) cfg.weights.scope_creep = w["scope_creep"].get<double>();
            if (w.contains("contradiction") && w["contradiction"].is_number()) cfg.weights.contradiction = w["contradiction"].get<double>();
            if (w.contains("repeated_failure") && w["repeated_failure"].is_number()) cfg.weights.repeated_failure = w["repeated_failure"].get<double>();
            if (w.contains("vocabulary_contraction") && w["vocabulary_contraction"].is_number()) cfg.weights.vocabulary_contraction = w["vocabulary_contraction"].get<double>();
            if (w.contains("coherence_velocity") && w["coherence_velocity"].is_number()) cfg.weights.coherence_velocity = w["coherence_velocity"].get<double>();
            if (w.contains("capability_underutilization") && w["capability_underutilization"].is_number()) cfg.weights.capability_underutilization = w["capability_underutilization"].get<double>();
            if (w.contains("semantic_stability") && w["semantic_stability"].is_number()) cfg.weights.semantic_stability = w["semantic_stability"].get<double>();
            if (w.contains("mandate_alignment") && w["mandate_alignment"].is_number()) cfg.weights.mandate_alignment = w["mandate_alignment"].get<double>();
            if (w.contains("response_quality") && w["response_quality"].is_number()) cfg.weights.response_quality = w["response_quality"].get<double>();
            if (w.contains("thinking_collapse") && w["thinking_collapse"].is_number()) cfg.weights.thinking_collapse = w["thinking_collapse"].get<double>();
            if (w.contains("context_growth") && w["context_growth"].is_number()) cfg.weights.context_growth = w["context_growth"].get<double>();
            if (w.contains("instruction_recall") && w["instruction_recall"].is_number()) cfg.weights.instruction_recall = w["instruction_recall"].get<double>();
            if (w.contains("plan_drift") && w["plan_drift"].is_number()) cfg.weights.plan_drift = w["plan_drift"].get<double>();
            if (w.contains("entity_consistency") && w["entity_consistency"].is_number()) cfg.weights.entity_consistency = w["entity_consistency"].get<double>();
            if (w.contains("instruction_conflict") && w["instruction_conflict"].is_number()) cfg.weights.instruction_conflict = w["instruction_conflict"].get<double>();
            if (w.contains("persona_fingerprint") && w["persona_fingerprint"].is_number()) cfg.weights.persona_fingerprint = w["persona_fingerprint"].get<double>();
            if (w.contains("tool_chain_integrity") && w["tool_chain_integrity"].is_number()) cfg.weights.tool_chain_integrity = w["tool_chain_integrity"].get<double>();
            if (w.contains("claim_result_reconciliation") && w["claim_result_reconciliation"].is_number()) cfg.weights.claim_result_reconciliation = w["claim_result_reconciliation"].get<double>();
            if (w.contains("prompt_compliance") && w["prompt_compliance"].is_number()) cfg.weights.prompt_compliance = w["prompt_compliance"].get<double>();
            if (w.contains("response_repetition") && w["response_repetition"].is_number()) cfg.weights.response_repetition = std::clamp(w["response_repetition"].get<double>(), 0.0, 1.0);
            if (w.contains("validation_outcome") && w["validation_outcome"].is_number()) cfg.weights.validation_outcome = std::clamp(w["validation_outcome"].get<double>(), 0.0, 1.0);
            if (w.contains("response_degenerate") && w["response_degenerate"].is_number()) cfg.weights.response_degenerate = std::clamp(w["response_degenerate"].get<double>(), 0.0, 1.0);
        }
        if (cd.contains("reality_checkpoint") && cd["reality_checkpoint"].is_object()) {
            auto& rc = cd["reality_checkpoint"];
            auto& rccfg = cfg.reality_checkpoint;
            if (rc.contains("enabled") && rc["enabled"].is_boolean()) rccfg.enabled = rc["enabled"].get<bool>();
            if (rc.contains("level")) {
                auto [en, lv] = parseEnforcementLevel(rc["level"]);
                rccfg.level = lv;
            }
            if (rc.contains("pressure_threshold") && rc["pressure_threshold"].is_number()) rccfg.pressure_threshold = rc["pressure_threshold"].get<double>();
            if (rc.contains("sustained_turns_required") && rc["sustained_turns_required"].is_number_integer()) rccfg.sustained_turns_required = rc["sustained_turns_required"].get<int>();
            if (rc.contains("min_turns_between_checkpoints") && rc["min_turns_between_checkpoints"].is_number_integer()) rccfg.min_turns_between_checkpoints = rc["min_turns_between_checkpoints"].get<int>();
            if (rc.contains("expected_conversation_depth") && rc["expected_conversation_depth"].is_number_integer()) rccfg.expected_conversation_depth = rc["expected_conversation_depth"].get<int>();
            parseRationale(rc, rccfg.rationale);
            if (rc.contains("weights") && rc["weights"].is_object()) {
                auto& rw = rc["weights"];
                if (rw.contains("coherence_proximity") && rw["coherence_proximity"].is_number()) rccfg.weights.coherence_proximity = rw["coherence_proximity"].get<double>();
                if (rw.contains("risk_score_proximity") && rw["risk_score_proximity"].is_number()) rccfg.weights.risk_score_proximity = rw["risk_score_proximity"].get<double>();
                if (rw.contains("signal_density") && rw["signal_density"].is_number()) rccfg.weights.signal_density = rw["signal_density"].get<double>();
                if (rw.contains("conversation_depth") && rw["conversation_depth"].is_number()) rccfg.weights.conversation_depth = rw["conversation_depth"].get<double>();
                if (rw.contains("bsd_partial_progress") && rw["bsd_partial_progress"].is_number()) rccfg.weights.bsd_partial_progress = rw["bsd_partial_progress"].get<double>();
                if (rw.contains("pipeline_inherited") && rw["pipeline_inherited"].is_number()) rccfg.weights.pipeline_inherited = rw["pipeline_inherited"].get<double>();
                if (rw.contains("coherence_acceleration") && rw["coherence_acceleration"].is_number()) rccfg.weights.coherence_acceleration = rw["coherence_acceleration"].get<double>();
                if (rw.contains("codegen_pressure") && rw["codegen_pressure"].is_number()) rccfg.weights.codegen_pressure = rw["codegen_pressure"].get<double>();
                if (rw.contains("bsd_eviction_pressure") && rw["bsd_eviction_pressure"].is_number()) rccfg.weights.bsd_eviction_pressure = rw["bsd_eviction_pressure"].get<double>();
                if (rw.contains("semantic_deviation") && rw["semantic_deviation"].is_number()) rccfg.weights.semantic_deviation = rw["semantic_deviation"].get<double>();
            }
            if (rc.contains("signal_density_divisor") && rc["signal_density_divisor"].is_number()) rccfg.signal_density_divisor = std::max(0.1, rc["signal_density_divisor"].get<double>());
            if (rc.contains("acceleration_multiplier") && rc["acceleration_multiplier"].is_number()) rccfg.acceleration_multiplier = std::max(0.1, rc["acceleration_multiplier"].get<double>());
        }
    }

    // Context drift analysis runs INSIDE the BSD-gated block in agentSend()
    // (agent_impl.cpp:4036), so "context_drift.enabled": true buys nothing on
    // its own: checkContextDrift() is never called, not one of the 23 signals
    // fires, and no CDD_TURN is emitted. The output admissibility gate is
    // gated separately (circuit_breaker.*) and keeps running regardless --
    // judging a coherence score that nothing ever wrote, and reporting
    // baseline_state "calibrating", which reads as "window still filling"
    // rather than "no turn was ever recorded".
    //
    // Two independent investigations misdiagnosed the same live run through
    // that gap before the coupling was found, so it must not stay silent.
    // Warn rather than auto-enable: turning BSD on for these configs would
    // start firing detections on runs that pass today.
    //
    // Every in-tree CDD test sets both flags together, which is exactly why
    // the suite never surfaced this.
    if (rules_.context_drift.enabled && !rules_.behavioral_sequences.enabled) {
        fprintf(stderr,
            "[governance] Warning: \"context_drift.enabled\": true has no effect - "
            "context drift analysis runs only when \"behavioral_sequences.enabled\" "
            "is also true\n");
    }

    // --- Exposure Tracking ---
    if (j.contains("exposure_tracking") && j["exposure_tracking"].is_object()) { rules_.explicitly_set.insert("exposure_tracking");
        auto& et = j["exposure_tracking"];
        auto& cfg = rules_.exposure_tracking;
        if (et.contains("enabled") && et["enabled"].is_boolean()) cfg.enabled = et["enabled"].get<bool>();
        if (et.contains("max_autonomous_actions") && et["max_autonomous_actions"].is_number_integer()) {
            cfg.max_autonomous_actions = et["max_autonomous_actions"].get<int>();
            if (cfg.max_autonomous_actions < 0) cfg.max_autonomous_actions = 0;
        }
        if (et.contains("max_unique_agents") && et["max_unique_agents"].is_number_integer()) cfg.max_unique_agents = et["max_unique_agents"].get<int>();
        if (et.contains("coherence_floor") && et["coherence_floor"].is_number()) cfg.coherence_floor = et["coherence_floor"].get<double>();
        if (et.contains("max_pipeline_depth") && et["max_pipeline_depth"].is_number_integer()) {
            cfg.max_pipeline_depth = et["max_pipeline_depth"].get<int>();
            if (cfg.max_pipeline_depth < 0) cfg.max_pipeline_depth = 0;
        }
        if (et.contains("checkpoint_cooldown_turns") && et["checkpoint_cooldown_turns"].is_number_integer()) {
            cfg.checkpoint_cooldown_turns = et["checkpoint_cooldown_turns"].get<int>();
            if (cfg.checkpoint_cooldown_turns < 0) cfg.checkpoint_cooldown_turns = 0;
        }
        if (et.contains("min_capability_utilization") && et["min_capability_utilization"].is_number()) cfg.min_capability_utilization = et["min_capability_utilization"].get<double>();
        if (et.contains("utilization_check_after_turns") && et["utilization_check_after_turns"].is_number_integer()) cfg.utilization_check_after_turns = et["utilization_check_after_turns"].get<int>();
        if (et.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(et["level"]);
            cfg.level = lv;
        }
        parseRationale(et, cfg.rationale);
    }

    // --- Temporal Coupling (F10) ---
    if (j.contains("temporal_coupling") && j["temporal_coupling"].is_object()) { rules_.explicitly_set.insert("temporal_coupling");
        auto& tc = j["temporal_coupling"];
        auto& cfg = rules_.temporal_coupling;
        if (tc.contains("enabled") && tc["enabled"].is_boolean()) cfg.enabled = tc["enabled"].get<bool>();
        if (tc.contains("max_correlation") && tc["max_correlation"].is_number()) cfg.max_correlation = tc["max_correlation"].get<double>();
        if (tc.contains("min_events") && tc["min_events"].is_number_integer()) cfg.min_events = tc["min_events"].get<int>();
        if (tc.contains("level") && tc["level"].is_string()) {
            auto [en, lv] = parseEnforcementLevel(tc["level"]);
            cfg.level = lv;
        }
        parseRationale(tc, cfg.rationale);
    }

    // --- Circuit Breaker (F6) ---
    if (j.contains("circuit_breaker") && j["circuit_breaker"].is_object()) { rules_.explicitly_set.insert("circuit_breaker");
        auto& cbj = j["circuit_breaker"];
        auto& cfg = rules_.circuit_breaker;
        if (cbj.contains("enabled") && cbj["enabled"].is_boolean()) cfg.enabled = cbj["enabled"].get<bool>();
        if (cbj.contains("elevated_threshold") && cbj["elevated_threshold"].is_number()) cfg.elevated_threshold = cbj["elevated_threshold"].get<double>();
        if (cbj.contains("high_threshold") && cbj["high_threshold"].is_number()) cfg.high_threshold = cbj["high_threshold"].get<double>();
        if (cbj.contains("critical_threshold") && cbj["critical_threshold"].is_number()) cfg.critical_threshold = cbj["critical_threshold"].get<double>();
        if (cbj.contains("elevated_sustained") && cbj["elevated_sustained"].is_number_integer()) cfg.elevated_sustained = cbj["elevated_sustained"].get<int>();
        if (cbj.contains("high_sustained") && cbj["high_sustained"].is_number_integer()) cfg.high_sustained = cbj["high_sustained"].get<int>();
        if (cbj.contains("critical_sustained") && cbj["critical_sustained"].is_number_integer()) cfg.critical_sustained = cbj["critical_sustained"].get<int>();
        if (cbj.contains("deescalate_sustained") && cbj["deescalate_sustained"].is_number_integer()) cfg.deescalate_sustained = std::max(1, cbj["deescalate_sustained"].get<int>());
        // Level effects: what ELEVATED and HIGH actually DO. Default true —
        // these are the documented semantics of the levels, not an opt-in
        // feature, and shipping them off would repeat the pattern that left
        // them inert for years.
        if (cbj.contains("level_effects") && cbj["level_effects"].is_object()) {
            const auto& le = cbj["level_effects"];
            if (le.contains("elevated_cdd_every_turn") && le["elevated_cdd_every_turn"].is_boolean())
                cfg.level_effects.elevated_cdd_every_turn = le["elevated_cdd_every_turn"].get<bool>();
            if (le.contains("high_advisory_to_soft") && le["high_advisory_to_soft"].is_boolean())
                cfg.level_effects.high_advisory_to_soft = le["high_advisory_to_soft"].get<bool>();
        }
        if (cbj.contains("step_up_enabled") && cbj["step_up_enabled"].is_boolean()) cfg.step_up_enabled = cbj["step_up_enabled"].get<bool>();
        if (cbj.contains("step_up_at_level") && cbj["step_up_at_level"].is_string()) cfg.step_up_at_level = cbj["step_up_at_level"].get<std::string>();
        if (cbj.contains("step_up_challenge") && cbj["step_up_challenge"].is_string()) cfg.step_up_challenge = cbj["step_up_challenge"].get<std::string>();
        if (cbj.contains("step_up_min_words") && cbj["step_up_min_words"].is_number_integer()) cfg.step_up_min_words = std::max(1, cbj["step_up_min_words"].get<int>());
        if (cbj.contains("step_up_cooldown_turns") && cbj["step_up_cooldown_turns"].is_number_integer()) cfg.step_up_cooldown_turns = std::max(0, cbj["step_up_cooldown_turns"].get<int>());
        if (cbj.contains("max_challenge_failures") && cbj["max_challenge_failures"].is_number_integer()) cfg.max_challenge_failures = std::max(1, cbj["max_challenge_failures"].get<int>());
        if (cbj.contains("step_up_on_inadmissible") && cbj["step_up_on_inadmissible"].is_boolean()) cfg.step_up_on_inadmissible = cbj["step_up_on_inadmissible"].get<bool>();
        if (cbj.contains("step_up_keyword_threshold") && cbj["step_up_keyword_threshold"].is_number()) {
            double val = std::max(0.0, std::min(1.0, cbj["step_up_keyword_threshold"].get<double>()));
            if (val < 0.2) {
                fprintf(stderr, "[governance] Warning: step_up_keyword_threshold %.2f is below minimum 0.2 — clamped to 0.2\n", val);
                val = 0.2;
            }
            cfg.step_up_keyword_threshold = val;
        }
        if (cbj.contains("step_up_contextual") && cbj["step_up_contextual"].is_boolean()) cfg.step_up_contextual = cbj["step_up_contextual"].get<bool>();
        if (cbj.contains("step_up_contextual_threshold") && cbj["step_up_contextual_threshold"].is_number()) {
            cfg.step_up_contextual_threshold = std::max(0.0, std::min(1.0, cbj["step_up_contextual_threshold"].get<double>()));
        }
        if (cbj.contains("step_up_challenge_history") && cbj["step_up_challenge_history"].is_string()) {
            std::string val = cbj["step_up_challenge_history"].get<std::string>();
            if (val == "full" || val == "recent" || val == "summary") {
                cfg.step_up_challenge_history = val;
            } else {
                fprintf(stderr, "[governance] Warning: step_up_challenge_history '%s' is not valid — using 'recent'\n", val.c_str());
            }
        }
        if (cbj.contains("step_up_history_recent_count") && cbj["step_up_history_recent_count"].is_number_integer()) {
            cfg.step_up_history_recent_count = std::max(2, std::min(200, cbj["step_up_history_recent_count"].get<int>()));
        }
        // Mandate reinforcement — periodic reminder injection
        if (cbj.contains("mandate_reinforcement_enabled") && cbj["mandate_reinforcement_enabled"].is_boolean())
            cfg.mandate_reinforcement_enabled = cbj["mandate_reinforcement_enabled"].get<bool>();
        if (cbj.contains("mandate_reinforcement_interval") && cbj["mandate_reinforcement_interval"].is_number_integer())
            cfg.mandate_reinforcement_interval = std::max(1, cbj["mandate_reinforcement_interval"].get<int>());
        if (cbj.contains("mandate_reinforcement_message") && cbj["mandate_reinforcement_message"].is_string())
            cfg.mandate_reinforcement_message = cbj["mandate_reinforcement_message"].get<std::string>();
        // Coherence correction — CDD-triggered correction injection
        if (cbj.contains("coherence_correction_enabled") && cbj["coherence_correction_enabled"].is_boolean())
            cfg.coherence_correction_enabled = cbj["coherence_correction_enabled"].get<bool>();
        if (cbj.contains("coherence_correction_threshold") && cbj["coherence_correction_threshold"].is_number())
            cfg.coherence_correction_threshold = std::max(0.0, std::min(1.0, cbj["coherence_correction_threshold"].get<double>()));
        if (cbj.contains("coherence_correction_cooldown_turns") && cbj["coherence_correction_cooldown_turns"].is_number_integer())
            cfg.coherence_correction_cooldown_turns = std::max(1, cbj["coherence_correction_cooldown_turns"].get<int>());
        if (cbj.contains("coherence_correction_message") && cbj["coherence_correction_message"].is_string())
            cfg.coherence_correction_message = cbj["coherence_correction_message"].get<std::string>();
        // Output admissibility — post-CDD gate on response coherence
        if (cbj.contains("output_admissibility") && cbj["output_admissibility"].is_object()) {
            auto& oa = cbj["output_admissibility"];
            auto& oac = cfg.output_admissibility;
            if (oa.contains("enabled") && oa["enabled"].is_boolean())
                oac.enabled = oa["enabled"].get<bool>();
            if (oa.contains("threshold") && oa["threshold"].is_number())
                oac.threshold = std::max(0.0, std::min(1.0, oa["threshold"].get<double>()));
            if (oa.contains("action") && oa["action"].is_string()) {
                std::string act = oa["action"].get<std::string>();
                if (act == "block" || act == "quarantine" || act == "attest")
                    oac.action = act;
            }
            // What to do when the gate would PASS but has no basis to judge.
            // Unrecognised values leave the default ("pass") rather than
            // failing the load — same shape as `action` directly above.
            if (oa.contains("on_undetermined") && oa["on_undetermined"].is_string()) {
                std::string ou = oa["on_undetermined"].get<std::string>();
                if (ou == "pass" || ou == "quarantine" || ou == "block")
                    oac.on_undetermined = ou;
            }
            if (oa.contains("level") && oa["level"].is_string()) {
                auto [en, lv] = parseEnforcementLevel(oa["level"]);
                oac.level = lv;
            }
            // Clamp: ADVISORY/NONE can't block — minimum DETECT for "block".
            // on_undetermined="block" enforces at the SAME `level`, so it needs
            // the same clamp: at ADVISORY, enforce() returns a string instead of
            // throwing and the config silently degrades to a quarantine.
            if ((oac.action == "block" || oac.on_undetermined == "block") &&
                (oac.level == EnforcementLevel::ADVISORY ||
                 oac.level == EnforcementLevel::NONE)) {
                oac.level = EnforcementLevel::DETECT;
            }
            // Split commit: history disposition for quarantined/attested responses
            if (oa.contains("inadmissible_history") && oa["inadmissible_history"].is_string()) {
                std::string hist = oa["inadmissible_history"].get<std::string>();
                if (hist == "commit" || hist == "exclude")
                    oac.inadmissible_history = hist;
            }
            // Per-tool-call admissibility gate
            if (oa.contains("gate_tool_calls") && oa["gate_tool_calls"].is_boolean())
                oac.gate_tool_calls = oa["gate_tool_calls"].get<bool>();
            // Maximum consecutive quarantined responses before hard-blocking
            if (oa.contains("max_quarantine_streak") && oa["max_quarantine_streak"].is_number_integer())
                oac.max_quarantine_streak = std::max(0, oa["max_quarantine_streak"].get<int>());
            if (oa.contains("require_corroboration") && oa["require_corroboration"].is_number_integer())
                oac.require_corroboration = std::max(0, oa["require_corroboration"].get<int>());
        }
        parseRationale(cbj, cfg.rationale);
    }

    // --- Advisory Escalation ---
    if (j.contains("advisory_escalation") && j["advisory_escalation"].is_object()) { rules_.explicitly_set.insert("advisory_escalation");
        auto& ae = j["advisory_escalation"];
        auto& cfg = rules_.advisory_escalation;
        if (ae.contains("enabled") && ae["enabled"].is_boolean()) cfg.enabled = ae["enabled"].get<bool>();
        if (ae.contains("soft_after") && ae["soft_after"].is_number_integer()) cfg.soft_after = std::max(2, ae["soft_after"].get<int>());
        if (ae.contains("weight_multiplier") && ae["weight_multiplier"].is_number()) cfg.weight_multiplier = std::max(1.0, ae["weight_multiplier"].get<double>());
        parseRationale(ae, cfg.rationale);
    }

    // --- Governance Health (F4) ---
    if (j.contains("governance_health") && j["governance_health"].is_object()) { rules_.explicitly_set.insert("governance_health");
        auto& gh = j["governance_health"];
        auto& cfg = rules_.governance_health;
        if (gh.contains("enabled") && gh["enabled"].is_boolean()) cfg.enabled = gh["enabled"].get<bool>();
        if (gh.contains("check_after_turns") && gh["check_after_turns"].is_number_integer()) cfg.check_after_turns = gh["check_after_turns"].get<int>();
        if (gh.contains("governance_entropy_warning") && gh["governance_entropy_warning"].is_number()) cfg.governance_entropy_warning = gh["governance_entropy_warning"].get<double>();
        if (gh.contains("coherence_floor_warning") && gh["coherence_floor_warning"].is_number()) cfg.coherence_floor_warning = gh["coherence_floor_warning"].get<double>();
        if (gh.contains("consecutive_passes_suspicion") && gh["consecutive_passes_suspicion"].is_number_integer()) cfg.consecutive_passes_suspicion = std::max(1, gh["consecutive_passes_suspicion"].get<int>());
        if (gh.contains("impaired_degraded_turns") && gh["impaired_degraded_turns"].is_number_integer()) cfg.impaired_degraded_turns = std::max(1, gh["impaired_degraded_turns"].get<int>());
        if (gh.contains("impaired_signal_count") && gh["impaired_signal_count"].is_number_integer()) cfg.impaired_signal_count = std::max(1, gh["impaired_signal_count"].get<int>());
        if (gh.contains("pulse_cooldown_turns") && gh["pulse_cooldown_turns"].is_number_integer()) cfg.pulse_cooldown_turns = std::max(0, gh["pulse_cooldown_turns"].get<int>());
        parseRationale(gh, cfg.rationale);
    }

    // --- Pipeline Separation (F7) ---
    if (j.contains("pipeline_separation") && j["pipeline_separation"].is_object()) { rules_.explicitly_set.insert("pipeline_separation");
        auto& ps = j["pipeline_separation"];
        auto& cfg = rules_.pipeline_separation;
        if (ps.contains("enabled") && ps["enabled"].is_boolean()) cfg.enabled = ps["enabled"].get<bool>();
        if (ps.contains("level")) {
            auto [en, lv] = parseEnforcementLevel(ps["level"]);
            cfg.level = lv;
        }
        parseRationale(ps, cfg.rationale);
    }

    // --- Governance Baseline (Feature 4) ---
    if (j.contains("governance_baseline") && j["governance_baseline"].is_object()) { rules_.explicitly_set.insert("governance_baseline");
        auto& gb = j["governance_baseline"];
        if (gb.contains("enabled") && gb["enabled"].is_boolean()) rules_.governance_baseline.enabled = gb["enabled"].get<bool>();
        if (gb.contains("path") && gb["path"].is_string()) rules_.governance_baseline.path = gb["path"].get<std::string>();
        if (gb.contains("fail_on_regression") && gb["fail_on_regression"].is_boolean()) rules_.governance_baseline.fail_on_regression = gb["fail_on_regression"].get<bool>();
        if (gb.contains("level") && gb["level"].is_string()) {
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
            if (pin_json.contains("language") && pin_json["language"].is_string())
                pin.language = pin_json["language"].get<std::string>();
            if (pin_json.contains("required") && pin_json["required"].is_string())
                pin.required_version = pin_json["required"].get<std::string>();
            if (pin_json.contains("message") && pin_json["message"].is_string())
                pin.message = pin_json["message"].get<std::string>();
            if (pin_json.contains("level") && pin_json["level"].is_string()) {
                auto lvl = pin_json["level"].get<std::string>();
                if (lvl == "hard") pin.level = EnforcementLevel::HARD;
                else if (lvl == "soft") pin.level = EnforcementLevel::SOFT;
                else pin.level = EnforcementLevel::ADVISORY;
            }
            if (!pin.language.empty() && !pin.required_version.empty())
                rules_.runtime_versions.push_back(pin);
        }
    }

    // --- Post-parse warnings ---
    // Reality checkpoint is enabled by default. If explicitly disabled while circuit
    // breaker is active, warn that the blocking intervention layer is missing.
    if (rules_.circuit_breaker.enabled && !rules_.context_drift.reality_checkpoint.enabled) {
        fprintf(stderr, "[governance] Note: reality_checkpoint explicitly disabled — "
                        "checkpoint enforcement is inactive\n");
        // The old wording here was "circuit breaker levels will update", which
        // was only half true and misled at least one shipped config: the
        // circuit breaker used to share the checkpoint's sustained-pressure
        // counter, so every level below reality_checkpoint.pressure_threshold
        // was unreachable. That coupling is gone — the breaker now keeps its own
        // per-level counters, each gated on its own threshold — so there is no
        // longer a set of unreachable levels to warn about. What remains true,
        // and worth saying, is only that the checkpoint's own blocking
        // intervention is switched off.
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

// Read a context_drift Signals toggle by CddSignalId index (kCddSignalKeys order).
static bool cddSignalValue(const ContextDriftConfig::Signals& s, int idx) {
    switch (idx) {
        case SIG_CIRCULAR:             return s.circular_actions;
        case SIG_REPEATED_FAILURES:    return s.repeated_failures;
        case SIG_SCOPE_CREEP:          return s.scope_creep;
        case SIG_CONTRADICTIONS:       return s.intent_contradictions;
        case SIG_VOCAB_CONTRACTION:    return s.vocabulary_contraction;
        case SIG_COHERENCE_VELOCITY:   return s.coherence_velocity;
        case SIG_CAPABILITY_UNDERUTIL: return s.capability_underutilization;
        case SIG_RESPONSE_QUALITY:     return s.response_quality;
        case SIG_THINKING_COLLAPSE:    return s.thinking_collapse;
        case SIG_SEMANTIC_STABILITY:   return s.semantic_stability;
        case SIG_MANDATE_ALIGNMENT:    return s.mandate_alignment;
        case SIG_CONTEXT_GROWTH:       return s.context_growth;
        case SIG_INSTRUCTION_RECALL:   return s.instruction_recall;
        case SIG_PLAN_DRIFT:           return s.plan_drift;
        case SIG_ENTITY_CONSISTENCY:   return s.entity_consistency;
        case SIG_INSTRUCTION_CONFLICT: return s.instruction_conflict;
        case SIG_PERSONA_FINGERPRINT:  return s.persona_fingerprint;
        case SIG_TOOL_CHAIN_INTEGRITY: return s.tool_chain_integrity;
        case SIG_CLAIM_RESULT:         return s.claim_result_reconciliation;
        case SIG_PROMPT_COMPLIANCE:    return s.prompt_compliance;
        case SIG_RESPONSE_REPETITION:  return s.response_repetition;
        case SIG_VALIDATION:           return s.validation_outcome;
        case SIG_RESPONSE_DEGENERATE:  return s.response_degenerate;
        default:                       return true;
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

    // The opt-in must itself ratchet, or it is a one-line escape: turn it on in
    // the same reload that adds the agent and the guard never applies.
    if (new_r.meta.allow_agent_addition_mid_run &&
        !old_r.meta.allow_agent_addition_mid_run) {
        violations.push_back(
            "meta.allow_agent_addition_mid_run: false -> true (loosened — "
            "cannot be enabled mid-run)");
    }
    if (!new_r.meta.allow_agent_addition_mid_run &&
        old_r.meta.allow_agent_addition_mid_run) {
        notices.push_back(
            "meta.allow_agent_addition_mid_run: true -> false (tightened)");
    }

    for (const auto& new_agent : new_r.agents) {
        const auto* old_agent = agentByName(old_r.agents, new_agent.name);
        if (!old_agent) {
            // A new identity carrying no per-agent restrictions is not tightening.
            // Refusing the modification while permitting the addition made the
            // ratchet reachable by renaming: shell_allowed false -> true on an
            // existing agent is a violation, but a NEW agent with
            // shell_allowed: true was accepted, created, and usable.
            if (new_r.meta.allow_agent_addition_mid_run) {
                notices.push_back(fmt::format(
                    "agent.{}: new agent config added (permitted by "
                    "meta.allow_agent_addition_mid_run)", new_agent.name));
            } else {
                violations.push_back(fmt::format(
                    "agent.{}: new agent added mid-run (loosened — set "
                    "meta.allow_agent_addition_mid_run to permit)", new_agent.name));
            }
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
        // thinking_budget: reducing = tightening (notice)
        if (new_agent.thinking_budget >= 0 && new_agent.thinking_budget < old_agent->thinking_budget
                && old_agent->thinking_budget > 0)
            notices.push_back(fmt::format("agent.{}.thinking_budget: {} -> {} (reduced)",
                new_agent.name, old_agent->thinking_budget, new_agent.thinking_budget));
        // -1 → explicit = now explicit (notice)
        if (old_agent->thinking_budget == -1 && new_agent.thinking_budget >= 0)
            notices.push_back(fmt::format("agent.{}.thinking_budget: default -> {} (now explicit)",
                new_agent.name, new_agent.thinking_budget));
        if (new_agent.timeout_seconds < old_agent->timeout_seconds)
            notices.push_back(fmt::format("agent.{}.timeout_seconds: {} -> {} (reduced)",
                new_agent.name, old_agent->timeout_seconds, new_agent.timeout_seconds));
        if (new_agent.timeout_seconds > old_agent->timeout_seconds && old_agent->timeout_seconds > 0)
            violations.push_back(fmt::format("agent.{}.timeout_seconds: {} -> {} (loosened)",
                new_agent.name, old_agent->timeout_seconds, new_agent.timeout_seconds));
        if (new_agent.temperature != old_agent->temperature)
            notices.push_back(fmt::format("agent.{}.temperature: {:.1f} -> {:.1f}",
                new_agent.name, old_agent->temperature, new_agent.temperature));

        // Model chain changes
        if (new_agent.model_chain != old_agent->model_chain || new_agent.model != old_agent->model)
            notices.push_back(fmt::format("agent.{}.model: chain updated", new_agent.name));

        // Per-agent CDD signal overrides: compare EFFECTIVE values (override
        // wins over the global signals toggle), but only where the override
        // itself changed — inherited global changes are ratcheted globally.
        for (int si = 0; si < NUM_CDD_SIGNALS; si++) {
            const char* key = kCddSignalKeys[si];
            auto oo = old_agent->context_drift_signals.find(key);
            auto no = new_agent.context_drift_signals.find(key);
            bool old_has = oo != old_agent->context_drift_signals.end();
            bool new_has = no != new_agent.context_drift_signals.end();
            if (old_has == new_has && (!old_has || oo->second == no->second))
                continue;  // override unchanged
            bool old_eff = old_has ? oo->second : cddSignalValue(old_r.context_drift.signals, si);
            bool new_eff = new_has ? no->second : cddSignalValue(new_r.context_drift.signals, si);
            if (old_eff && !new_eff)
                violations.push_back(fmt::format(
                    "agent.{}.context_drift_signals.{}: enabled -> disabled (loosened)",
                    new_agent.name, key));
            else if (!old_eff && new_eff)
                notices.push_back(fmt::format(
                    "agent.{}.context_drift_signals.{}: disabled -> enabled (tightened)",
                    new_agent.name, key));
        }

        // api_base: any mid-run endpoint change is a violation (traffic redirection)
        if (new_agent.api_base != old_agent->api_base)
            violations.push_back(fmt::format("agent.{}.api_base: endpoint changed mid-run",
                new_agent.name));

        // propose_candidates_max: increasing = loosening
        if (new_agent.propose_candidates_max > old_agent->propose_candidates_max)
            violations.push_back(fmt::format("agent.{}.propose_candidates_max: {} -> {} (loosened)",
                new_agent.name, old_agent->propose_candidates_max, new_agent.propose_candidates_max));
        else if (new_agent.propose_candidates_max < old_agent->propose_candidates_max)
            notices.push_back(fmt::format("agent.{}.propose_candidates_max: {} -> {} (reduced)",
                new_agent.name, old_agent->propose_candidates_max, new_agent.propose_candidates_max));

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
        // thinking_budget: increasing = loosening (violation)
        if (old_agent->thinking_budget >= 0 && new_agent.thinking_budget > old_agent->thinking_budget)
            violations.push_back(fmt::format("agent.{}.thinking_budget: {} -> {} (loosened)",
                new_agent.name, old_agent->thinking_budget, new_agent.thinking_budget));
        // 0 (disabled) → -1 (provider default, may enable thinking) = loosening
        if (old_agent->thinking_budget == 0 && new_agent.thinking_budget == -1)
            violations.push_back(fmt::format("agent.{}.thinking_budget: 0 (disabled) -> -1 (provider default, loosened)",
                new_agent.name));
        // N>0 → -1 (provider default, unknown budget) = loosening
        if (old_agent->thinking_budget > 0 && new_agent.thinking_budget == -1)
            violations.push_back(fmt::format("agent.{}.thinking_budget: {} -> -1 (provider default, loosened)",
                new_agent.name, old_agent->thinking_budget));

        // (agent-scope checks follow)
        // Network allowed loosening
        if (new_agent.network_allowed_set && old_agent->network_allowed_set) {
            if (new_agent.network_allowed && !old_agent->network_allowed)
                violations.push_back(fmt::format("agent.{}.network_allowed: false -> true (loosened)",
                    new_agent.name));
            if (!new_agent.network_allowed && old_agent->network_allowed)
                notices.push_back(fmt::format("agent.{}.network_allowed: true -> false (tightened)",
                    new_agent.name));
        }

        // Shell allowed loosening. network_allowed above has been ratcheted since
        // it was added; its twin has not — a mid-run shell_allowed false -> true
        // was accepted silently, restoring both shell execution AND the response
        // content scan. Same field pair, one guarded. Closing it here because the
        // split below adds a second knob over the same capability, and leaving the
        // first one un-ratcheted would make the new one's guard decorative.
        if (new_agent.shell_allowed_set && old_agent->shell_allowed_set) {
            if (new_agent.shell_allowed && !old_agent->shell_allowed)
                violations.push_back(fmt::format("agent.{}.shell_allowed: false -> true (loosened)",
                    new_agent.name));
            if (!new_agent.shell_allowed && old_agent->shell_allowed)
                notices.push_back(fmt::format("agent.{}.shell_allowed: true -> false (tightened)",
                    new_agent.name));
        }
        // Dropping shell_allowed entirely reverts the role to global policy, which
        // is a loosening whenever it had been restricting.
        if (old_agent->shell_allowed_set && !old_agent->shell_allowed &&
            !new_agent.shell_allowed_set) {
            violations.push_back(fmt::format(
                "agent.{}.shell_allowed: removed (loosened — reverts to global policy)",
                new_agent.name));
        }

        // Shell CONTENT opt-out: enabling it stops the response-text scan, so
        // unset/false -> true is a loosening. Compares effective values, since
        // unset means the scan runs.
        {
            const bool old_content = old_agent->shell_content_allowed_set &&
                                     old_agent->shell_content_allowed;
            const bool new_content = new_agent.shell_content_allowed_set &&
                                     new_agent.shell_content_allowed;
            if (new_content && !old_content)
                violations.push_back(fmt::format(
                    "agent.{}.shell_content_allowed: false -> true (loosened — response shell scan disabled)",
                    new_agent.name));
            if (!new_content && old_content)
                notices.push_back(fmt::format(
                    "agent.{}.shell_content_allowed: true -> false (tightened)",
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
        // Context window ratchet: 0 is unlimited (least strict). Increasing or removing limit = loosening.
        if (new_agent.context_window > old_agent->context_window && old_agent->context_window > 0)
            violations.push_back(fmt::format("agent.{}.context_window: {} -> {} (loosened)",
                new_agent.name, old_agent->context_window, new_agent.context_window));
        else if (new_agent.context_window < old_agent->context_window && new_agent.context_window > 0)
            notices.push_back(fmt::format("agent.{}.context_window: {} -> {} (tightened)",
                new_agent.name, old_agent->context_window, new_agent.context_window));
        if (old_agent->context_window > 0 && new_agent.context_window == 0)
            violations.push_back(fmt::format("agent.{}.context_window: {} -> unlimited (loosened)",
                new_agent.name, old_agent->context_window));
        if (old_agent->context_window == 0 && new_agent.context_window > 0)
            notices.push_back(fmt::format("agent.{}.context_window: unlimited -> {} (tightened)",
                new_agent.name, new_agent.context_window));
        // min_tokens ratchet: a floor on the agent's token budget — raising is
        // tightening, lowering or removing (N→0) is loosening.
        if (new_agent.min_tokens < old_agent->min_tokens && new_agent.min_tokens > 0)
            violations.push_back(fmt::format("agent.{}.min_tokens: {} -> {} (loosened)",
                new_agent.name, old_agent->min_tokens, new_agent.min_tokens));
        if (old_agent->min_tokens > 0 && new_agent.min_tokens == 0)
            violations.push_back(fmt::format("agent.{}.min_tokens: {} -> 0 (loosened — floor removed)",
                new_agent.name, old_agent->min_tokens));
        if (new_agent.min_tokens > old_agent->min_tokens)
            notices.push_back(fmt::format("agent.{}.min_tokens: {} -> {} (tightened)",
                new_agent.name, old_agent->min_tokens, new_agent.min_tokens));
        // Reducing max_tokens below the floor is an attempt to cripple the agent
        // (the floor wins at request time, but flag the attempt explicitly).
        if (new_agent.min_tokens > 0 && new_agent.max_tokens < new_agent.min_tokens
                && old_agent->max_tokens >= new_agent.min_tokens)
            violations.push_back(fmt::format("agent.{}.max_tokens: {} -> {} (below min_tokens floor {})",
                new_agent.name, old_agent->max_tokens, new_agent.max_tokens, new_agent.min_tokens));
    }

    // Detect removed agents — removing constraints is loosening
    for (const auto& old_agent : old_r.agents) {
        if (!agentByName(new_r.agents, old_agent.name)) {
            violations.push_back(fmt::format("agent.{}: config removed (loosened)",
                old_agent.name));
        }
    }

    // --- Agent dispatch ratchet enforcement ---
    if (new_r.agent_dispatch.default_timeout_seconds > old_r.agent_dispatch.default_timeout_seconds
        && old_r.agent_dispatch.default_timeout_seconds > 0)
        violations.push_back(fmt::format("agent_dispatch.default_timeout_seconds: {} -> {} (loosened)",
            old_r.agent_dispatch.default_timeout_seconds, new_r.agent_dispatch.default_timeout_seconds));
    else if (new_r.agent_dispatch.default_timeout_seconds < old_r.agent_dispatch.default_timeout_seconds)
        notices.push_back(fmt::format("agent_dispatch.default_timeout_seconds: {} -> {} (tightened)",
            old_r.agent_dispatch.default_timeout_seconds, new_r.agent_dispatch.default_timeout_seconds));

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

    // --- De-escalation hysteresis ratchet ---
    // Fewer calm turns before stepping the governance level down = loosening
    // Level effects: disabling either mid-run means a level that was biting
    // stops biting, which is the loosening the ratchet exists for.
    {
        const auto& o_le = old_r.circuit_breaker.level_effects;
        const auto& n_le = new_r.circuit_breaker.level_effects;
        if (o_le.elevated_cdd_every_turn && !n_le.elevated_cdd_every_turn)
            violations.push_back(
                "circuit_breaker.level_effects.elevated_cdd_every_turn: true -> false (loosened)");
        else if (!o_le.elevated_cdd_every_turn && n_le.elevated_cdd_every_turn)
            notices.push_back(
                "circuit_breaker.level_effects.elevated_cdd_every_turn: false -> true (tightened)");
        if (o_le.high_advisory_to_soft && !n_le.high_advisory_to_soft)
            violations.push_back(
                "circuit_breaker.level_effects.high_advisory_to_soft: true -> false (loosened)");
        else if (!o_le.high_advisory_to_soft && n_le.high_advisory_to_soft)
            notices.push_back(
                "circuit_breaker.level_effects.high_advisory_to_soft: false -> true (tightened)");
    }
    if (new_r.circuit_breaker.deescalate_sustained < old_r.circuit_breaker.deescalate_sustained)
        violations.push_back(fmt::format("circuit_breaker.deescalate_sustained: {} -> {} (loosened)",
            old_r.circuit_breaker.deescalate_sustained, new_r.circuit_breaker.deescalate_sustained));
    else if (new_r.circuit_breaker.deescalate_sustained > old_r.circuit_breaker.deescalate_sustained)
        notices.push_back(fmt::format("circuit_breaker.deescalate_sustained: {} -> {} (tightened)",
            old_r.circuit_breaker.deescalate_sustained, new_r.circuit_breaker.deescalate_sustained));

    // --- Output Admissibility ratchet ---
    auto& old_oa = old_r.circuit_breaker.output_admissibility;
    auto& new_oa = new_r.circuit_breaker.output_admissibility;
    // Disabling = loosening
    if (!new_oa.enabled && old_oa.enabled)
        violations.push_back("output_admissibility.enabled: true -> false (loosened)");
    else if (new_oa.enabled && !old_oa.enabled)
        notices.push_back("output_admissibility.enabled: false -> true (tightened)");
    // Lowering threshold = loosening (more outputs pass)
    if (new_oa.threshold < old_oa.threshold)
        violations.push_back(fmt::format("output_admissibility.threshold: {:.2f} -> {:.2f} (loosened)",
            old_oa.threshold, new_oa.threshold));
    else if (new_oa.threshold > old_oa.threshold)
        notices.push_back(fmt::format("output_admissibility.threshold: {:.2f} -> {:.2f} (tightened)",
            old_oa.threshold, new_oa.threshold));
    // Action rank: block=3, quarantine=2, attest=1. Lower rank = loosening
    auto actionRank = [](const std::string& a) -> int {
        if (a == "block") return 3;
        if (a == "quarantine") return 2;
        if (a == "attest") return 1;
        return 0;
    };
    int old_rank = actionRank(old_oa.action);
    int new_rank = actionRank(new_oa.action);
    if (new_rank < old_rank)
        violations.push_back(fmt::format("output_admissibility.action: {} -> {} (loosened)",
            old_oa.action, new_oa.action));
    else if (new_rank > old_rank)
        notices.push_back(fmt::format("output_admissibility.action: {} -> {} (tightened)",
            old_oa.action, new_oa.action));
    // Undetermined policy: pass < quarantine < block, same ordering as `action`.
    // Relaxing it mid-run means an unjudgeable response that would have been
    // held is delivered instead, which is the loosening the ratchet exists for.
    auto undeterminedRank = [](const std::string& a) {
        if (a == "block") return 3;
        if (a == "quarantine") return 2;
        return 1;  // "pass"
    };
    int old_ur = undeterminedRank(old_oa.on_undetermined);
    int new_ur = undeterminedRank(new_oa.on_undetermined);
    if (new_ur < old_ur)
        violations.push_back(fmt::format("output_admissibility.on_undetermined: {} -> {} (loosened)",
            old_oa.on_undetermined, new_oa.on_undetermined));
    else if (new_ur > old_ur)
        notices.push_back(fmt::format("output_admissibility.on_undetermined: {} -> {} (tightened)",
            old_oa.on_undetermined, new_oa.on_undetermined));
    // Split commit: exclude is stricter than commit
    if (old_oa.inadmissible_history == "exclude" && new_oa.inadmissible_history == "commit")
        violations.push_back("output_admissibility.inadmissible_history: exclude -> commit (loosened)");
    else if (old_oa.inadmissible_history == "commit" && new_oa.inadmissible_history == "exclude")
        notices.push_back("output_admissibility.inadmissible_history: commit -> exclude (tightened)");
    // Tool-call gate: disabling is loosening
    if (old_oa.gate_tool_calls && !new_oa.gate_tool_calls)
        violations.push_back("output_admissibility.gate_tool_calls: true -> false (loosened)");
    else if (!old_oa.gate_tool_calls && new_oa.gate_tool_calls)
        notices.push_back("output_admissibility.gate_tool_calls: false -> true (tightened)");
    // Quarantine streak: 0=disabled=loosest. Removing limit or increasing N = loosening.
    if (old_oa.max_quarantine_streak > 0 && new_oa.max_quarantine_streak == 0)
        violations.push_back("output_admissibility.max_quarantine_streak: removed (loosened)");
    else if (new_oa.max_quarantine_streak > old_oa.max_quarantine_streak && old_oa.max_quarantine_streak > 0)
        violations.push_back(fmt::format("output_admissibility.max_quarantine_streak: {} -> {} (loosened)",
            old_oa.max_quarantine_streak, new_oa.max_quarantine_streak));
    else if (new_oa.max_quarantine_streak < old_oa.max_quarantine_streak && new_oa.max_quarantine_streak > 0)
        notices.push_back(fmt::format("output_admissibility.max_quarantine_streak: {} -> {} (tightened)",
            old_oa.max_quarantine_streak, new_oa.max_quarantine_streak));
    else if (old_oa.max_quarantine_streak == 0 && new_oa.max_quarantine_streak > 0)
        notices.push_back(fmt::format("output_admissibility.max_quarantine_streak: disabled -> {} (tightened)",
            new_oa.max_quarantine_streak));

    // Corroboration makes the kill HARDER to reach, so enabling it or raising
    // the bar is a loosening. Lowering it or switching it off tightens.
    if (new_oa.require_corroboration > old_oa.require_corroboration)
        violations.push_back(fmt::format("output_admissibility.require_corroboration: {} -> {} (loosened)",
            old_oa.require_corroboration, new_oa.require_corroboration));
    else if (new_oa.require_corroboration < old_oa.require_corroboration)
        notices.push_back(fmt::format("output_admissibility.require_corroboration: {} -> {} (tightened)",
            old_oa.require_corroboration, new_oa.require_corroboration));

    // Coherence-floor challenge trigger: disabling removes the sub-OA recovery
    // ladder = loosening.
    if (old_r.circuit_breaker.step_up_on_inadmissible && !new_r.circuit_breaker.step_up_on_inadmissible)
        violations.push_back("circuit_breaker.step_up_on_inadmissible: true -> false (loosened — sub-OA recovery ladder removed)");
    else if (!old_r.circuit_breaker.step_up_on_inadmissible && new_r.circuit_breaker.step_up_on_inadmissible)
        notices.push_back("circuit_breaker.step_up_on_inadmissible: false -> true (tightened)");

    // Challenge-failure streak: minimum 1 (one-strike). Raising N = loosening
    // (more failed challenges tolerated before termination).
    if (new_r.circuit_breaker.max_challenge_failures > old_r.circuit_breaker.max_challenge_failures)
        violations.push_back(fmt::format("circuit_breaker.max_challenge_failures: {} -> {} (loosened)",
            old_r.circuit_breaker.max_challenge_failures, new_r.circuit_breaker.max_challenge_failures));
    else if (new_r.circuit_breaker.max_challenge_failures < old_r.circuit_breaker.max_challenge_failures)
        notices.push_back(fmt::format("circuit_breaker.max_challenge_failures: {} -> {} (tightened)",
            old_r.circuit_breaker.max_challenge_failures, new_r.circuit_breaker.max_challenge_failures));

    // --- Telemetry evidence ratchet ---
    // Turning off evidence collection mid-run destroys the audit record of
    // whatever follows — all four are one-way switches once enabled.
    if (old_r.telemetry_output.tamper_evidence.enabled && !new_r.telemetry_output.tamper_evidence.enabled)
        violations.push_back("telemetry.tamper_evidence.enabled: true -> false (loosened)");
    else if (!old_r.telemetry_output.tamper_evidence.enabled && new_r.telemetry_output.tamper_evidence.enabled)
        notices.push_back("telemetry.tamper_evidence.enabled: false -> true (tightened)");
    if (old_r.telemetry_output.decision_snapshots && !new_r.telemetry_output.decision_snapshots)
        violations.push_back("telemetry.decision_snapshots: true -> false (loosened)");
    else if (!old_r.telemetry_output.decision_snapshots && new_r.telemetry_output.decision_snapshots)
        notices.push_back("telemetry.decision_snapshots: false -> true (tightened)");
    if (old_r.audit.tamper_evidence.enabled && !new_r.audit.tamper_evidence.enabled)
        violations.push_back("audit.tamper_evidence.enabled: true -> false (loosened)");
    else if (!old_r.audit.tamper_evidence.enabled && new_r.audit.tamper_evidence.enabled)
        notices.push_back("audit.tamper_evidence.enabled: false -> true (tightened)");
    if (old_r.transcript.enabled && !new_r.transcript.enabled)
        violations.push_back("telemetry.transcript.enabled: true -> false (loosened)");
    else if (!old_r.transcript.enabled && new_r.transcript.enabled)
        notices.push_back("telemetry.transcript.enabled: false -> true (tightened)");

    // --- Context Drift ratchet ---
    // rate_normalized: disabling mid-run restores flat per-event penalties
    // (stricter), so false->true is the loosening direction — diluted penalties
    // let a paced adversary drift more cheaply.
    // C1e: adaptive persona baseline. An earlier version of this comment said
    // enabling it "strictly REDUCES how often S17 fires". THAT IS FALSE, and was
    // falsified by a live A/B on living-script_v3 (2026-08-29).
    //
    // The mechanism RECALIBRATES; the direction depends on the workload. The
    // re-derivation recomputes stddev from the rolling window, so:
    //   baseline mis-set low, responses grow richer -> re-derives UPWARD,
    //       band widens, FEWER firings (the original defect case)
    //   responses already uniform -> stddev NARROWS, band tightens,
    //       MORE sensitive
    // Observed: on a uniform-response model, stddev went 2.67 -> 1.00 (the
    // floor), taking the 2-sigma band from +/-5.34 to +/-2.00. A response 12
    // keywords from the mean would fire with the fix ON and not with it OFF.
    //
    // The ratchet still refuses false -> true, and that is deliberate rather
    // than a leftover: when a change can loosen OR tighten depending on
    // workload, refusing the mid-run enable is the fail-safe direction. It
    // blocks the possible loosening; the cost is also blocking a possible
    // tightening, which is the acceptable side to err on.
    if (new_r.context_drift.thresholds.persona_baseline_adaptive &&
        !old_r.context_drift.thresholds.persona_baseline_adaptive)
        violations.push_back("context_drift.thresholds.persona_baseline_adaptive: false -> true (loosened — S17 baseline follows the agent)");

    if (new_r.context_drift.rate_normalized && !old_r.context_drift.rate_normalized)
        violations.push_back("context_drift.rate_normalized: false -> true (loosened — penalties diluted by turn count)");
    else if (!new_r.context_drift.rate_normalized && old_r.context_drift.rate_normalized)
        notices.push_back("context_drift.rate_normalized: true -> false (tightened)");
    // Lowering the floor = loosening (firing signals pay less)
    // event_feed is ratcheted in BOTH directions, unlike every other key here.
    // The usual test is "did this loosen?", but this change is not uniformly
    // signed: switching to since_last_check gives S3 and S5 more events (a
    // tightening) while feeding non-agent components into S1's fingerprint, so
    // two turns carrying an identical response can stop matching (a loosening
    // of an objective signal). Neither direction is safe to make mid-run, so
    // any change is a violation.
    // observe -> declared tightens (allowed); declared -> observe withdraws
    // enforcement from every built-in pattern mid-run.
    if (old_r.behavioral_sequences.default_pattern_enforcement == "declared" &&
        new_r.behavioral_sequences.default_pattern_enforcement != "declared")
        violations.push_back("behavioral_sequences.default_pattern_enforcement: declared -> observe (loosened)");

    if (new_r.context_drift.event_feed != old_r.context_drift.event_feed)
        violations.push_back(fmt::format("context_drift.event_feed: {} -> {} (mid-run change; "
            "the feed changes what every event-reading signal sees, in both directions)",
            old_r.context_drift.event_feed, new_r.context_drift.event_feed));

    if (new_r.context_drift.rate_normalized_floor < old_r.context_drift.rate_normalized_floor)
        violations.push_back(fmt::format("context_drift.rate_normalized_floor: {:.2f} -> {:.2f} (loosened)",
            old_r.context_drift.rate_normalized_floor, new_r.context_drift.rate_normalized_floor));
    else if (new_r.context_drift.rate_normalized_floor > old_r.context_drift.rate_normalized_floor)
        notices.push_back(fmt::format("context_drift.rate_normalized_floor: {:.2f} -> {:.2f} (tightened)",
            old_r.context_drift.rate_normalized_floor, new_r.context_drift.rate_normalized_floor));
    // Adaptive absorption limit: 0 = unlimited absorption = loosest. Removing
    // the limit or raising N = loosening (persistent absorbed drift stays free
    // for longer).
    {
        int old_lim = old_r.context_drift.adaptive_absorption_limit;
        int new_lim = new_r.context_drift.adaptive_absorption_limit;
        if (old_lim > 0 && new_lim == 0)
            violations.push_back("context_drift.adaptive_absorption_limit: removed (loosened)");
        else if (new_lim > old_lim && old_lim > 0)
            violations.push_back(fmt::format("context_drift.adaptive_absorption_limit: {} -> {} (loosened)", old_lim, new_lim));
        else if (new_lim < old_lim && new_lim > 0)
            notices.push_back(fmt::format("context_drift.adaptive_absorption_limit: {} -> {} (tightened)", old_lim, new_lim));
        else if (old_lim == 0 && new_lim > 0)
            notices.push_back(fmt::format("context_drift.adaptive_absorption_limit: unlimited -> {} (tightened)", new_lim));
    }

    // The three coherence recovery knobs were un-ratcheted while their two
    // siblings (validation_recovery_amount below, rate_normalized_floor) were
    // guarded — so raising them mid-run refunded penalties with nothing to stop
    // it. Every one of them makes drift cost less net, which is the same
    // loosening the sibling checks already refuse.
    //
    // coherence_recovery_cap is the ceiling resetCoherence may restore to, so
    // RAISING it permits more recovery and is the loosening direction, matching
    // the other two rather than inverting like exclude_infrastructure_errors.
    {
        const auto& ocd = old_r.context_drift;
        const auto& ncd = new_r.context_drift;
        struct { const char* key; double oldv; double newv; const char* why; } coh[] = {
            {"coherence_natural_healing", ocd.coherence_natural_healing,
             ncd.coherence_natural_healing, "drift heals faster per clean turn"},
            {"coherence_recovery_amount", ocd.coherence_recovery_amount,
             ncd.coherence_recovery_amount, "each recovery event returns more"},
            {"coherence_recovery_cap",    ocd.coherence_recovery_cap,
             ncd.coherence_recovery_cap,    "recovery may climb higher"},
            // R5. Raising the fraction heals a larger share of the agent's own
            // damage rate per clean turn, so it is the loosening direction like
            // the three above. It is the key that carries the safety invariant
            // (< 1.0), which makes re-granting it mid-run exactly the move the
            // ratchet exists to refuse.
            {"coherence_healing_damage_fraction", ocd.coherence_healing_damage_fraction,
             ncd.coherence_healing_damage_fraction, "healing covers more of the damage rate"},
        };
        for (const auto& c : coh) {
            if (c.newv > c.oldv)
                violations.push_back(fmt::format(
                    "context_drift.{}: {:.3f} -> {:.3f} (loosened — {})",
                    c.key, c.oldv, c.newv, c.why));
            else if (c.newv < c.oldv)
                notices.push_back(fmt::format("context_drift.{}: {:.3f} -> {:.3f} (tightened)",
                    c.key, c.oldv, c.newv));
        }
    }

    // Raising the validation recovery credit = loosening (failures cost less net)
    if (new_r.context_drift.validation_recovery_amount > old_r.context_drift.validation_recovery_amount)
        violations.push_back(fmt::format("context_drift.validation_recovery_amount: {:.3f} -> {:.3f} (loosened — validation failures recover more cheaply)",
            old_r.context_drift.validation_recovery_amount, new_r.context_drift.validation_recovery_amount));
    else if (new_r.context_drift.validation_recovery_amount < old_r.context_drift.validation_recovery_amount)
        notices.push_back(fmt::format("context_drift.validation_recovery_amount: {:.3f} -> {:.3f} (tightened)",
            old_r.context_drift.validation_recovery_amount, new_r.context_drift.validation_recovery_amount));
    // Per-signal toggles: disabling a signal mid-run removes a detection channel
    for (int si = 0; si < NUM_CDD_SIGNALS; si++) {
        bool old_on = cddSignalValue(old_r.context_drift.signals, si);
        bool new_on = cddSignalValue(new_r.context_drift.signals, si);
        if (old_on && !new_on)
            violations.push_back(fmt::format(
                "context_drift.signals.{}: enabled -> disabled (loosened)", kCddSignalKeys[si]));
        else if (!old_on && new_on)
            notices.push_back(fmt::format(
                "context_drift.signals.{}: disabled -> enabled (tightened)", kCddSignalKeys[si]));
    }
    // exclude_infrastructure_errors: enabling exclusion shrinks what
    // repeated_failures can see, so false->true is the loosening direction
    if (!old_r.context_drift.signals.exclude_infrastructure_errors &&
        new_r.context_drift.signals.exclude_infrastructure_errors)
        violations.push_back("context_drift.signals.exclude_infrastructure_errors: false -> true (loosened)");
    else if (old_r.context_drift.signals.exclude_infrastructure_errors &&
             !new_r.context_drift.signals.exclude_infrastructure_errors)
        notices.push_back("context_drift.signals.exclude_infrastructure_errors: true -> false (tightened)");

    return violations.empty();
}

// ============================================================================
// Governance Under Survivability — Mid-Run Config Reload
// ============================================================================

// True when behavior-affecting fields differ between two versions of the same
// agent config — the fields whose change makes CDD judge the agent against a
// different mandate/shape than the one its rate window accumulated under.
static bool agentBehaviorFieldsDiffer(const AgentConfig& a, const AgentConfig& b) {
    return a.system_prompt != b.system_prompt ||
           a.model != b.model ||
           a.model_chain != b.model_chain ||
           a.temperature != b.temperature ||
           a.max_tokens != b.max_tokens ||
           a.min_tokens != b.min_tokens ||
           a.thinking_budget != b.thinking_budget ||
           a.context_window != b.context_window ||
           a.context_strategy != b.context_strategy ||
           a.context_drift_signals != b.context_drift_signals;
}

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
            // Suppress duplicate log/telemetry when the same mtime fails repeatedly.
            // The retry is still attempted (verification re-runs each call) but
            // we only emit diagnostics once per mtime to prevent log flooding.
            if (current_mtime != last_sig_fail_mtime_) {
                fmt::print(stderr, "[governance] Reload rejected: signature verification failed\n");
                logAuditEvent("governance_reload_rejected", "governance_config",
                    "Signature verification failed on govern.json reload");
                writeAgentTelemetry("CONFIG_ADJUSTMENT", {
                    {"accepted", "false"},
                    {"reason", "signature"},
                });
                last_sig_fail_mtime_ = current_mtime;
            }
            // NOTE: intentionally NOT caching mtime on signature failure.
            // The .sig sidecar can be updated (by a re-signing subprocess)
            // without changing govern.json's mtime. Caching here would
            // prevent future reloadIfChanged() calls from retrying after
            // the signature becomes valid.
            return false;
        }

        // Signature verified — clear the failure tracker
        last_sig_fail_mtime_ = 0;

        // Apply minimum enforcement levels to new rules
        enforceMinimumLevelsOnRules(new_rules);

        // Re-apply governance self-protection to new rules (survives reload)
        addGovernanceProtectedPaths(new_rules);

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
            writeAgentTelemetry("CONFIG_ADJUSTMENT", {
                {"accepted", "false"},
                {"reason", "ratchet"},
                {"violations", detail},
            });
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

        // Per-agent behavior diff (computed against the still-active old rules)
        // — drives the scoped CDD rate-window reset and CONFIG_ADJUSTMENT
        // telemetry below.
        std::set<std::string> changed_agents;
        std::unordered_map<std::string, std::string> changed_system_prompts;
        {
            const auto& old_r = rules();
            for (const auto& new_agent : new_rules.agents) {
                for (const auto& old_agent : old_r.agents) {
                    if (old_agent.name != new_agent.name) continue;
                    if (agentBehaviorFieldsDiffer(old_agent, new_agent)) {
                        changed_agents.insert(new_agent.name);
                        if (old_agent.system_prompt != new_agent.system_prompt) {
                            changed_system_prompts[new_agent.name] = new_agent.system_prompt;
                        }
                    }
                    break;
                }
            }
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
            if (new_rp->capabilities.filesystem.mode == "none") {
                sb->removeCapability(security::Capability::FS_READ);
                sb->removeCapability(security::Capability::FS_WRITE);
            } else if (new_rp->capabilities.filesystem.mode == "read") {
                sb->removeCapability(security::Capability::FS_WRITE);
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

        // Scoped rate-window reset: operator-driven config changes must not be
        // scored as agent drift. Only the rate window restarts — learned
        // baselines and coherence are preserved (see onAgentConfigChanged).
        if (new_rp->context_drift.enabled && !changed_agents.empty()) {
            std::unordered_map<std::string, std::map<std::string, bool>> changed_overrides;
            for (const auto& na : new_rp->agents) {
                if (changed_agents.count(na.name)) {
                    changed_overrides[na.name] = na.context_drift_signals;
                }
            }
            drift_analyzer_.onAgentConfigChanged(changed_agents, changed_system_prompts,
                                                 changed_overrides);
        }

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
        auto joinStrings = [](const auto& items) {
            std::string s;
            for (const auto& i : items) {
                if (!s.empty()) s += "; ";
                s += i;
            }
            return s;
        };
        size_t notice_count = notices.size();
        std::string notices_joined = joinStrings(notices);
        {
            std::lock_guard<std::mutex> lock(notices_mutex_);
            pending_notices_ = std::move(notices);
        }

        // Audit trail
        logAuditEvent("governance_reloaded", "governance_config",
            fmt::format("Config reloaded: {} change(s){}",
                notice_count,
                update_reason.empty() ? "" : ", reason: " + update_reason));

        // CONFIG_ADJUSTMENT telemetry: link the operator's decision to the
        // config change without transcript diving. Requesting context is
        // best-effort — the reload is picked up inside enforcement entry
        // points, so this identifies whose turn triggered it.
        {
            std::string requesting_config;
            {
                std::lock_guard<std::mutex> lock(agent_config_mutex_);
                requesting_config = current_agent_config_;
            }
            writeAgentTelemetry("CONFIG_ADJUSTMENT", {
                {"accepted", "true"},
                {"reload_count", std::to_string(reload_count_.load(std::memory_order_relaxed))},
                {"governance_epoch", std::to_string(governance_epoch_.load())},
                {"update_reason", update_reason},
                {"changed_agents", joinStrings(changed_agents)},
                {"ratchet_notices", notices_joined},
                {"requesting_handle", std::to_string(current_agent_handle_.load(std::memory_order_relaxed))},
                {"requesting_config", requesting_config},
            });
        }

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
    // With child_wins (default): child keeps its value if explicitly set in JSON.
    // With parent_wins: base value takes precedence if different from default.
    // M3: child.explicitly_set tracks which JSON keys were present in the config.
    // defaults is retained for base-side comparisons and parent_wins branches.
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

    // Shell
    if (parent_wins) {
        if (!bc.shell.enabled) cc.shell.enabled = false;  // parent disables → stays disabled
    } else {
        if (!child.explicitly_set.count("capabilities.shell.enabled") && !bc.shell.enabled) cc.shell.enabled = false;
    }
    // Network
    if (parent_wins) {
        if (!bc.network.enabled) cc.network.enabled = false;
    } else {
        if (!child.explicitly_set.count("capabilities.network.enabled") && !bc.network.enabled) cc.network.enabled = false;
    }
    // Filesystem
    if (parent_wins) {
        if (!bc.filesystem.mode.empty()) cc.filesystem.mode = bc.filesystem.mode;
    } else {
        if (!child.explicitly_set.count("capabilities.filesystem.mode") && !bc.filesystem.mode.empty()) {
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
    // Env vars arrays: merge blocked_read, allowed_read, blocked_write, allowed_write
    auto mergeStringVec = [&](std::vector<std::string>& dst, const std::vector<std::string>& src) {
        if (cfg.merge_arrays == "append") {
            for (const auto& s : src) {
                if (std::find(dst.begin(), dst.end(), s) == dst.end()) {
                    dst.push_back(s);
                }
            }
        } else if (dst.empty()) {
            dst = src;
        }
    };
    mergeStringVec(cc.env_vars.blocked_read, bc.env_vars.blocked_read);
    mergeStringVec(cc.env_vars.allowed_read, bc.env_vars.allowed_read);
    mergeStringVec(cc.env_vars.blocked_write, bc.env_vars.blocked_write);
    mergeStringVec(cc.env_vars.allowed_write, bc.env_vars.allowed_write);

    // --- Limits: child wins or parent wins, with gap-filling ---
    auto& cl = child.limits;
    const auto& bl = base.limits;

    if (parent_wins) {
        if (bl.timeout.global > 0) cl.timeout.global = bl.timeout.global;
        if (bl.timeout.total_polyglot > 0) cl.timeout.total_polyglot = bl.timeout.total_polyglot;
        if (bl.execution.loop_iterations > 0) cl.execution.loop_iterations = bl.execution.loop_iterations;
        if (bl.execution.call_depth > 0) cl.execution.call_depth = bl.execution.call_depth;
        if (bl.execution.polyglot_blocks > 0) cl.execution.polyglot_blocks = bl.execution.polyglot_blocks;
    } else {
        // Child wins: base fills gaps (where child didn't explicitly set)
        if (!child.explicitly_set.count("limits.timeout.global") && bl.timeout.global > 0)
            cl.timeout.global = bl.timeout.global;
        if (!child.explicitly_set.count("limits.timeout.total_polyglot") && bl.timeout.total_polyglot > 0)
            cl.timeout.total_polyglot = bl.timeout.total_polyglot;
        if (!child.explicitly_set.count("limits.execution.loop_iterations") && bl.execution.loop_iterations > 0)
            cl.execution.loop_iterations = bl.execution.loop_iterations;
        if (!child.explicitly_set.count("limits.execution.call_depth") && bl.execution.call_depth > 0)
            cl.execution.call_depth = bl.execution.call_depth;
        if (!child.explicitly_set.count("limits.execution.polyglot_blocks") && bl.execution.polyglot_blocks > 0)
            cl.execution.polyglot_blocks = bl.execution.polyglot_blocks;
    }
    // Memory limits
    if (parent_wins) {
        if (bl.memory.per_block_mb > 0) cl.memory.per_block_mb = bl.memory.per_block_mb;
        if (bl.memory.total_mb > 0) cl.memory.total_mb = bl.memory.total_mb;
    } else {
        if (!child.explicitly_set.count("limits.memory.per_block_mb") && bl.memory.per_block_mb > 0)
            cl.memory.per_block_mb = bl.memory.per_block_mb;
        if (!child.explicitly_set.count("limits.memory.total_mb") && bl.memory.total_mb > 0)
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
        if (!child.explicitly_set.count("limits.data.array_size") && bl.data.array_size > 0)
            cl.data.array_size = bl.data.array_size;
        if (!child.explicitly_set.count("limits.data.dict_size") && bl.data.dict_size > 0)
            cl.data.dict_size = bl.data.dict_size;
        if (!child.explicitly_set.count("limits.data.string_length") && bl.data.string_length > 0)
            cl.data.string_length = bl.data.string_length;
        if (!child.explicitly_set.count("limits.data.nesting_depth") && bl.data.nesting_depth > 0)
            cl.data.nesting_depth = bl.data.nesting_depth;
        if (!child.explicitly_set.count("limits.data.output_size") && bl.data.output_size > 0)
            cl.data.output_size = bl.data.output_size;
        if (!child.explicitly_set.count("limits.data.input_size") && bl.data.input_size > 0)
            cl.data.input_size = bl.data.input_size;
    }
    // Code limits
    if (parent_wins) {
        if (bl.code.max_lines_per_block > 0) cl.code.max_lines_per_block = bl.code.max_lines_per_block;
        if (bl.code.max_total_polyglot_lines > 0) cl.code.max_total_polyglot_lines = bl.code.max_total_polyglot_lines;
        if (bl.code.max_nesting_depth > 0) cl.code.max_nesting_depth = bl.code.max_nesting_depth;
    } else {
        if (!child.explicitly_set.count("limits.code.max_lines_per_block") && bl.code.max_lines_per_block > 0)
            cl.code.max_lines_per_block = bl.code.max_lines_per_block;
        if (!child.explicitly_set.count("limits.code.max_total_polyglot_lines") && bl.code.max_total_polyglot_lines > 0)
            cl.code.max_total_polyglot_lines = bl.code.max_total_polyglot_lines;
        if (!child.explicitly_set.count("limits.code.max_nesting_depth") && bl.code.max_nesting_depth > 0)
            cl.code.max_nesting_depth = bl.code.max_nesting_depth;
    }
    // Rate limits (cooldown_on_limit_ms defaults to 100, rest to 0)
    if (parent_wins) {
        if (bl.rate.max_polyglot_per_second > 0) cl.rate.max_polyglot_per_second = bl.rate.max_polyglot_per_second;
        if (bl.rate.max_stdlib_calls_per_second > 0) cl.rate.max_stdlib_calls_per_second = bl.rate.max_stdlib_calls_per_second;
        if (bl.rate.max_file_ops_per_second > 0) cl.rate.max_file_ops_per_second = bl.rate.max_file_ops_per_second;
        if (bl.rate.cooldown_on_limit_ms != defaults.limits.rate.cooldown_on_limit_ms)
            cl.rate.cooldown_on_limit_ms = bl.rate.cooldown_on_limit_ms;
    } else {
        if (!child.explicitly_set.count("limits.rate.max_polyglot_per_second") && bl.rate.max_polyglot_per_second > 0)
            cl.rate.max_polyglot_per_second = bl.rate.max_polyglot_per_second;
        if (!child.explicitly_set.count("limits.rate.max_stdlib_calls_per_second") && bl.rate.max_stdlib_calls_per_second > 0)
            cl.rate.max_stdlib_calls_per_second = bl.rate.max_stdlib_calls_per_second;
        if (!child.explicitly_set.count("limits.rate.max_file_ops_per_second") && bl.rate.max_file_ops_per_second > 0)
            cl.rate.max_file_ops_per_second = bl.rate.max_file_ops_per_second;
        if (!child.explicitly_set.count("limits.rate.cooldown_on_limit_ms") &&
            bl.rate.cooldown_on_limit_ms != defaults.limits.rate.cooldown_on_limit_ms)
            cl.rate.cooldown_on_limit_ms = bl.rate.cooldown_on_limit_ms;
    }

    // --- Requirements: inherit from base if child didn't set ---
    if (!parent_wins) {
        if (!child.explicitly_set.count("require_error_handling") && base.require_error_handling)
            child.require_error_handling = base.require_error_handling;
        if (!child.explicitly_set.count("require_main_block") && base.require_main_block)
            child.require_main_block = base.require_main_block;
    } else {
        if (base.require_error_handling) child.require_error_handling = true;
        if (base.require_main_block) child.require_main_block = true;
    }

    // --- Restrictions: inherit from base if child didn't set ---
    if (!parent_wins) {
        if (!child.explicitly_set.count("no_placeholders") && base.no_placeholders) child.no_placeholders = true;
        if (!child.explicitly_set.count("no_secrets") && base.no_secrets) child.no_secrets = true;
        if (!child.explicitly_set.count("restrict_dangerous_calls") && base.restrict_dangerous_calls)
            child.restrict_dangerous_calls = true;
        if (!child.explicitly_set.count("no_hardcoded_results") && base.no_hardcoded_results)
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
    if (!child.explicitly_set.count("taint_tracking.level") &&
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
    if (!child.explicitly_set.count("scoring") && base.scoring.enabled) {
        child.scoring = base.scoring;
    } else if (child.scoring.enabled && base.scoring.enabled) {
        // Both set: child wins on thresholds it set, base fills gaps
        if (!parent_wins) {
            if (!child.explicitly_set.count("scoring.yellow_threshold"))
                child.scoring.yellow_threshold = base.scoring.yellow_threshold;
            if (!child.explicitly_set.count("scoring.red_threshold"))
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
    if (!child.explicitly_set.count("behavioral_sequences") && base.behavioral_sequences.enabled) {
        child.behavioral_sequences = base.behavioral_sequences;
    }

    // --- Context drift: inherit if child didn't configure ---
    if (!child.explicitly_set.count("context_drift") && base.context_drift.enabled) {
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
        if (!child.explicitly_set.count("timeout_seconds") && base.timeout_seconds > 0)
            child.timeout_seconds = base.timeout_seconds;
        if (!child.explicitly_set.count("memory_limit_mb") && base.memory_limit_mb > 0)
            child.memory_limit_mb = base.memory_limit_mb;
        if (!child.explicitly_set.count("max_call_depth") && base.max_call_depth > 0)
            child.max_call_depth = base.max_call_depth;
        if (!child.explicitly_set.count("max_array_size") && base.max_array_size > 0)
            child.max_array_size = base.max_array_size;
    } else {
        if (base.timeout_seconds > 0) child.timeout_seconds = base.timeout_seconds;
        if (base.memory_limit_mb > 0) child.memory_limit_mb = base.memory_limit_mb;
        if (base.max_call_depth > 0) child.max_call_depth = base.max_call_depth;
        if (base.max_array_size > 0) child.max_array_size = base.max_array_size;
    }

    // --- Governance depth features: inherit if child didn't configure ---
    if (!child.explicitly_set.count("circuit_breaker") && base.circuit_breaker.enabled)
        child.circuit_breaker = base.circuit_breaker;
    if (!child.explicitly_set.count("advisory_escalation") && base.advisory_escalation.enabled)
        child.advisory_escalation = base.advisory_escalation;
    if (!child.explicitly_set.count("exposure_tracking") && base.exposure_tracking.enabled)
        child.exposure_tracking = base.exposure_tracking;
    if (!child.explicitly_set.count("pipeline_separation") && base.pipeline_separation.enabled)
        child.pipeline_separation = base.pipeline_separation;
    if (!child.explicitly_set.count("governance_health") && base.governance_health.enabled)
        child.governance_health = base.governance_health;
    if (!child.explicitly_set.count("temporal_coupling") && base.temporal_coupling.enabled)
        child.temporal_coupling = base.temporal_coupling;
    if (!child.explicitly_set.count("contradiction_detection") && base.contradiction_detection.enabled)
        child.contradiction_detection = base.contradiction_detection;
    if (!child.explicitly_set.count("codegen") && base.codegen.enabled)
        child.codegen = base.codegen;
    if (!child.explicitly_set.count("telemetry") && base.telemetry_output.enabled)
        child.telemetry_output = base.telemetry_output;
    if (!child.explicitly_set.count("quality_gate") && base.quality_gate.enabled)
        child.quality_gate = base.quality_gate;
    if (!child.explicitly_set.count("governance_baseline") && base.governance_baseline.enabled)
        child.governance_baseline = base.governance_baseline;
    if (!child.explicitly_set.count("baselines") && base.baselines.enabled)
        child.baselines = base.baselines;
    if (!child.explicitly_set.count("prerequisites") && base.prerequisites.enabled)
        child.prerequisites = base.prerequisites;
    if (!child.explicitly_set.count("agent_review") && base.agent_review.enabled)
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
        if (!child.explicitly_set.count("restrictions.dangerous_calls") && base.restrictions.dangerous_calls.enabled)
            child.restrictions.dangerous_calls = base.restrictions.dangerous_calls;
        if (!child.explicitly_set.count("restrictions.shell_injection") && base.restrictions.shell_injection.enabled)
            child.restrictions.shell_injection = base.restrictions.shell_injection;
        if (!child.explicitly_set.count("restrictions.imports") && base.restrictions.imports.enabled)
            child.restrictions.imports = base.restrictions.imports;
        if (!child.explicitly_set.count("restrictions.data_exfiltration") && base.restrictions.data_exfiltration.enabled)
            child.restrictions.data_exfiltration = base.restrictions.data_exfiltration;
        if (!child.explicitly_set.count("restrictions.resource_abuse") && base.restrictions.resource_abuse.enabled)
            child.restrictions.resource_abuse = base.restrictions.resource_abuse;
        if (!child.explicitly_set.count("restrictions.privilege_escalation") && base.restrictions.privilege_escalation.enabled)
            child.restrictions.privilege_escalation = base.restrictions.privilege_escalation;
        if (!child.explicitly_set.count("restrictions.information_disclosure") && base.restrictions.information_disclosure.enabled)
            child.restrictions.information_disclosure = base.restrictions.information_disclosure;
        if (!child.explicitly_set.count("restrictions.code_injection") && base.restrictions.code_injection.enabled)
            child.restrictions.code_injection = base.restrictions.code_injection;
        if (!child.explicitly_set.count("restrictions.crypto") && base.restrictions.crypto.enabled)
            child.restrictions.crypto = base.restrictions.crypto;
        if (!child.explicitly_set.count("restrictions.vcs_secret_extraction") && base.restrictions.vcs_secret_extraction.enabled)
            child.restrictions.vcs_secret_extraction = base.restrictions.vcs_secret_extraction;
        if (!child.explicitly_set.count("restrictions.obfuscation") && base.restrictions.obfuscation.enabled)
            child.restrictions.obfuscation = base.restrictions.obfuscation;
    }
    // PolyglotOutputRestriction (no enabled) — field-by-field gap-fill
    {
        auto& cpo = child.restrictions.polyglot_output;
        const auto& bpo = base.restrictions.polyglot_output;
        if (!child.explicitly_set.count("restrictions.polyglot_output.format") && !bpo.format.empty()) cpo.format = bpo.format;
        if (!child.explicitly_set.count("restrictions.polyglot_output.max_size") && bpo.max_size > 0) cpo.max_size = bpo.max_size;
        if (!child.explicitly_set.count("restrictions.polyglot_output.require_structured") && bpo.require_structured) cpo.require_structured = true;
        if (!child.explicitly_set.count("restrictions.polyglot_output.validate_json") && bpo.validate_json) cpo.validate_json = true;
    }

    // --- Code quality: inherit each check if child didn't configure ---
    if (!child.explicitly_set.count("code_quality.no_secrets") && base.code_quality.no_secrets.enabled)
        child.code_quality.no_secrets = base.code_quality.no_secrets;
    if (!child.explicitly_set.count("code_quality.no_placeholders") && base.code_quality.no_placeholders.enabled)
        child.code_quality.no_placeholders = base.code_quality.no_placeholders;
    if (!child.explicitly_set.count("code_quality.no_hardcoded_results") && base.code_quality.no_hardcoded_results.enabled)
        child.code_quality.no_hardcoded_results = base.code_quality.no_hardcoded_results;
    if (!child.explicitly_set.count("code_quality.no_pii") && base.code_quality.no_pii.enabled)
        child.code_quality.no_pii = base.code_quality.no_pii;
    if (!child.explicitly_set.count("code_quality.no_temporary_code") && base.code_quality.no_temporary_code.enabled)
        child.code_quality.no_temporary_code = base.code_quality.no_temporary_code;
    if (!child.explicitly_set.count("code_quality.no_simulation_markers") && base.code_quality.no_simulation_markers.enabled)
        child.code_quality.no_simulation_markers = base.code_quality.no_simulation_markers;
    if (!child.explicitly_set.count("code_quality.no_mock_data") && base.code_quality.no_mock_data.enabled)
        child.code_quality.no_mock_data = base.code_quality.no_mock_data;
    if (!child.explicitly_set.count("code_quality.no_apologetic_language") && base.code_quality.no_apologetic_language.enabled)
        child.code_quality.no_apologetic_language = base.code_quality.no_apologetic_language;
    if (!child.explicitly_set.count("code_quality.no_dead_code") && base.code_quality.no_dead_code.enabled)
        child.code_quality.no_dead_code = base.code_quality.no_dead_code;
    if (!child.explicitly_set.count("code_quality.no_debug_artifacts") && base.code_quality.no_debug_artifacts.enabled)
        child.code_quality.no_debug_artifacts = base.code_quality.no_debug_artifacts;
    if (!child.explicitly_set.count("code_quality.no_unsafe_deserialization") && base.code_quality.no_unsafe_deserialization.enabled)
        child.code_quality.no_unsafe_deserialization = base.code_quality.no_unsafe_deserialization;
    if (!child.explicitly_set.count("code_quality.no_sql_injection") && base.code_quality.no_sql_injection.enabled)
        child.code_quality.no_sql_injection = base.code_quality.no_sql_injection;
    if (!child.explicitly_set.count("code_quality.no_path_traversal") && base.code_quality.no_path_traversal.enabled)
        child.code_quality.no_path_traversal = base.code_quality.no_path_traversal;
    if (!child.explicitly_set.count("code_quality.no_hardcoded_urls") && base.code_quality.no_hardcoded_urls.enabled)
        child.code_quality.no_hardcoded_urls = base.code_quality.no_hardcoded_urls;
    if (!child.explicitly_set.count("code_quality.no_hardcoded_ips") && base.code_quality.no_hardcoded_ips.enabled)
        child.code_quality.no_hardcoded_ips = base.code_quality.no_hardcoded_ips;
    if (!child.explicitly_set.count("code_quality.max_complexity") && base.code_quality.max_complexity.enabled)
        child.code_quality.max_complexity = base.code_quality.max_complexity;
    if (!child.explicitly_set.count("code_quality.encoding") && base.code_quality.encoding.enabled)
        child.code_quality.encoding = base.code_quality.encoding;
    if (!child.explicitly_set.count("code_quality.no_oversimplification") && base.code_quality.no_oversimplification.enabled)
        child.code_quality.no_oversimplification = base.code_quality.no_oversimplification;
    if (!child.explicitly_set.count("code_quality.no_incomplete_logic") && base.code_quality.no_incomplete_logic.enabled)
        child.code_quality.no_incomplete_logic = base.code_quality.no_incomplete_logic;
    if (!child.explicitly_set.count("code_quality.no_hallucinated_apis") && base.code_quality.no_hallucinated_apis.enabled)
        child.code_quality.no_hallucinated_apis = base.code_quality.no_hallucinated_apis;
    if (!child.explicitly_set.count("code_quality.complexity_floor") && base.code_quality.complexity_floor.enabled)
        child.code_quality.complexity_floor = base.code_quality.complexity_floor;
    if (!child.explicitly_set.count("code_quality.duplicate_calls") && base.code_quality.duplicate_calls.enabled)
        child.code_quality.duplicate_calls = base.code_quality.duplicate_calls;
    if (!child.explicitly_set.count("code_quality.polyglot_try_catch") && base.code_quality.polyglot_try_catch.enabled)
        child.code_quality.polyglot_try_catch = base.code_quality.polyglot_try_catch;
    if (!child.explicitly_set.count("code_quality.semantic_checks") && base.code_quality.semantic_checks.enabled)
        child.code_quality.semantic_checks = base.code_quality.semantic_checks;
    if (!child.explicitly_set.count("code_quality.intent_validation") && base.code_quality.intent_validation.enabled)
        child.code_quality.intent_validation = base.code_quality.intent_validation;
    if (!child.explicitly_set.count("code_quality.drift_detection") && base.code_quality.drift_detection.enabled)
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
        if (!child.explicitly_set.count("requirements.main_block") && base.requirements.main_block.enabled)
            child.requirements.main_block = base.requirements.main_block;
        if (!child.explicitly_set.count("requirements.error_handling") && base.requirements.error_handling.enabled)
            child.requirements.error_handling = base.requirements.error_handling;
        if (!child.explicitly_set.count("requirements.strict_types") && base.requirements.strict_types.enabled)
            child.requirements.strict_types = base.requirements.strict_types;
        if (!child.explicitly_set.count("requirements.no_global_state") && base.requirements.no_global_state.enabled)
            child.requirements.no_global_state = base.requirements.no_global_state;
        if (!child.explicitly_set.count("requirements.naming_conventions") && base.requirements.naming_conventions.enabled)
            child.requirements.naming_conventions = base.requirements.naming_conventions;
        if (!child.explicitly_set.count("requirements.documentation") && base.requirements.documentation.enabled)
            child.requirements.documentation = base.requirements.documentation;
        if (!child.explicitly_set.count("requirements.version_pinning") && base.requirements.version_pinning.enabled)
            child.requirements.version_pinning = base.requirements.version_pinning;
    }

    // --- Trust policy ---
    {
        const auto& bt = base.trust_policy;
        auto& ct = child.trust_policy;
        if (parent_wins) {
            if (bt.max_signature_age_days > 0)
                ct.max_signature_age_days = bt.max_signature_age_days;
            if (bt.require_fresh_signature) ct.require_fresh_signature = true;
            if (bt.stale_signature_level != defaults.trust_policy.stale_signature_level)
                ct.stale_signature_level = bt.stale_signature_level;
        } else {
            if (!child.explicitly_set.count("trust_policy.max_signature_age_days") && bt.max_signature_age_days > 0)
                ct.max_signature_age_days = bt.max_signature_age_days;
            if (!child.explicitly_set.count("trust_policy.require_fresh_signature") && bt.require_fresh_signature)
                ct.require_fresh_signature = true;
            if (!child.explicitly_set.count("trust_policy.stale_signature_level") &&
                bt.stale_signature_level != defaults.trust_policy.stale_signature_level)
                ct.stale_signature_level = bt.stale_signature_level;
        }
        if (!child.explicitly_set.count("trust_policy.check_key_expiry") && bt.check_key_expiry) ct.check_key_expiry = true;
        if (!child.explicitly_set.count("trust_policy.check_revocation") && bt.check_revocation) ct.check_revocation = true;
    }

    // --- Approval ---
    if (child.approval.approver_keys.empty() && !base.approval.approver_keys.empty())
        child.approval.approver_keys = base.approval.approver_keys;
    if (!child.explicitly_set.count("approval.default_expiry_hours") &&
        base.approval.default_expiry_hours != defaults.approval.default_expiry_hours)
        child.approval.default_expiry_hours = base.approval.default_expiry_hours;

    // --- Agent dispatch ---
    {
        const auto& bad = base.agent_dispatch;
        auto& cad = child.agent_dispatch;
        if (parent_wins) {
            if (bad.max_concurrent != defaults.agent_dispatch.max_concurrent) cad.max_concurrent = bad.max_concurrent;
            if (bad.pool_size != defaults.agent_dispatch.pool_size) cad.pool_size = bad.pool_size;
            if (bad.pool_queue_max != defaults.agent_dispatch.pool_queue_max) cad.pool_queue_max = bad.pool_queue_max;
            if (bad.max_retries_per_run > 0) cad.max_retries_per_run = bad.max_retries_per_run;
            if (bad.default_timeout_seconds > 0) cad.default_timeout_seconds = bad.default_timeout_seconds;
            if (bad.hard_stop.max_calls_per_run > 0) cad.hard_stop.max_calls_per_run = bad.hard_stop.max_calls_per_run;
            if (bad.hard_stop.max_tokens_per_run > 0) cad.hard_stop.max_tokens_per_run = bad.hard_stop.max_tokens_per_run;
            if (bad.hard_stop.max_agent_time_ms > 0) cad.hard_stop.max_agent_time_ms = bad.hard_stop.max_agent_time_ms;
            if (bad.hard_stop.consecutive_failure_limit > 0)
                cad.hard_stop.consecutive_failure_limit = bad.hard_stop.consecutive_failure_limit;
            if (bad.hard_stop.action != defaults.agent_dispatch.hard_stop.action) cad.hard_stop.action = bad.hard_stop.action;
        } else {
            if (!child.explicitly_set.count("agent_dispatch.max_concurrent") && bad.max_concurrent != defaults.agent_dispatch.max_concurrent)
                cad.max_concurrent = bad.max_concurrent;
            if (!child.explicitly_set.count("agent_dispatch.pool_size") && bad.pool_size != defaults.agent_dispatch.pool_size)
                cad.pool_size = bad.pool_size;
            if (!child.explicitly_set.count("agent_dispatch.pool_queue_max") && bad.pool_queue_max != defaults.agent_dispatch.pool_queue_max)
                cad.pool_queue_max = bad.pool_queue_max;
            if (!child.explicitly_set.count("agent_dispatch.max_retries_per_run") && bad.max_retries_per_run > 0)
                cad.max_retries_per_run = bad.max_retries_per_run;
            if (!child.explicitly_set.count("agent_dispatch.default_timeout_seconds") && bad.default_timeout_seconds > 0)
                cad.default_timeout_seconds = bad.default_timeout_seconds;
            if (!child.explicitly_set.count("agent_dispatch.hard_stop.max_calls_per_run") && bad.hard_stop.max_calls_per_run > 0)
                cad.hard_stop.max_calls_per_run = bad.hard_stop.max_calls_per_run;
            if (!child.explicitly_set.count("agent_dispatch.hard_stop.max_tokens_per_run") && bad.hard_stop.max_tokens_per_run > 0)
                cad.hard_stop.max_tokens_per_run = bad.hard_stop.max_tokens_per_run;
            if (!child.explicitly_set.count("agent_dispatch.hard_stop.max_agent_time_ms") && bad.hard_stop.max_agent_time_ms > 0)
                cad.hard_stop.max_agent_time_ms = bad.hard_stop.max_agent_time_ms;
            if (!child.explicitly_set.count("agent_dispatch.hard_stop.consecutive_failure_limit") && bad.hard_stop.consecutive_failure_limit > 0)
                cad.hard_stop.consecutive_failure_limit = bad.hard_stop.consecutive_failure_limit;
            if (!child.explicitly_set.count("agent_dispatch.hard_stop.action") && bad.hard_stop.action != defaults.agent_dispatch.hard_stop.action)
                cad.hard_stop.action = bad.hard_stop.action;
        }
    }

    // --- Audit ---
    if (parent_wins) {
        if (base.audit.level != defaults.audit.level) child.audit.level = base.audit.level;
    } else {
        if (!child.explicitly_set.count("audit.level") && base.audit.level != defaults.audit.level)
            child.audit.level = base.audit.level;
    }
    if (!child.explicitly_set.count("audit.output_file") &&
        base.audit.output_file != defaults.audit.output_file)
        child.audit.output_file = base.audit.output_file;
    if (!child.explicitly_set.count("audit.tamper_evidence") && base.audit.tamper_evidence.enabled)
        child.audit.tamper_evidence = base.audit.tamper_evidence;
    if (!child.explicitly_set.count("audit.provenance") && base.audit.provenance.enabled)
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
        if (!child.explicitly_set.count("languages.require_explicit") && base.languages.require_explicit)
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
    if (!child.explicitly_set.count("output.max_advisories") &&
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
    if (!child.explicitly_set.count("polyglot.context_isolation") && base.polyglot.context_isolation.enabled)
        child.polyglot.context_isolation = base.polyglot.context_isolation;
    if (!child.explicitly_set.count("polyglot.variable_binding.require_explicit") && base.polyglot.variable_binding.require_explicit)
        child.polyglot.variable_binding.require_explicit = true;
    if (!child.explicitly_set.count("polyglot.variable_binding.max_bound_variables") &&
        base.polyglot.variable_binding.max_bound_variables > 0)
        child.polyglot.variable_binding.max_bound_variables = base.polyglot.variable_binding.max_bound_variables;
    if (!child.explicitly_set.count("polyglot.output.require_json_pipe") && base.polyglot.output.require_json_pipe)
        child.polyglot.output.require_json_pipe = true;
    if (!child.explicitly_set.count("polyglot.output.require_naab_return") && base.polyglot.output.require_naab_return)
        child.polyglot.output.require_naab_return = true;
    if (!child.explicitly_set.count("polyglot.output.max_output_lines") && base.polyglot.output.max_output_lines > 0)
        child.polyglot.output.max_output_lines = base.polyglot.output.max_output_lines;
    if (!child.explicitly_set.count("polyglot.parallel.max_parallel_blocks") && base.polyglot.parallel.max_parallel_blocks > 0)
        child.polyglot.parallel.max_parallel_blocks = base.polyglot.parallel.max_parallel_blocks;
    if (!child.explicitly_set.count("polyglot.parallel.timeout_per_block") && base.polyglot.parallel.timeout_per_block > 0)
        child.polyglot.parallel.timeout_per_block = base.polyglot.parallel.timeout_per_block;
    if (!child.explicitly_set.count("polyglot.parallel.fail_strategy") &&
        base.polyglot.parallel.fail_strategy != defaults.polyglot.parallel.fail_strategy)
        child.polyglot.parallel.fail_strategy = base.polyglot.parallel.fail_strategy;
    if (!child.explicitly_set.count("polyglot.persistent_runtime.max_sessions") && base.polyglot.persistent_runtime.max_sessions > 0)
        child.polyglot.persistent_runtime.max_sessions = base.polyglot.persistent_runtime.max_sessions;
    if (!child.explicitly_set.count("polyglot.persistent_runtime.session_timeout") && base.polyglot.persistent_runtime.session_timeout > 0)
        child.polyglot.persistent_runtime.session_timeout = base.polyglot.persistent_runtime.session_timeout;
    if (!child.explicitly_set.count("polyglot.persistent_runtime.max_memory_per_session_mb") &&
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
        // chain_files collects every file that contributed, so the run
        // fingerprint below covers a parent policy change too — a child whose
        // own bytes never moved still ran a different effective config.
        std::set<std::string> chain_files;
        chain_files.insert(std::filesystem::weakly_canonical(path).string());
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
            chain_files = visited;   // every file that took part
        }

        // Run identity. std::set gives a deterministic traversal, and hashing
        // CONTENT rather than paths keeps the fingerprint identical across
        // checkouts of the same config.
        {
            std::string acc;
            for (const auto& f : chain_files) {
                acc += security::CryptoUtils::sha256File(f);
                acc += ";";
            }
            config_fingerprint_ = security::CryptoUtils::sha256(acc).substr(0, 16);

            // Mandate digest: agent name + system prompt, sorted by name so the
            // parse order of the agents vector cannot change the value. Hashes
            // only — prompts are operator content and telemetry is forwarded.
            std::vector<std::pair<std::string, std::string>> mandates;
            for (const auto& a : new_rules->agents) {
                mandates.emplace_back(a.name,
                    security::CryptoUtils::sha256(a.system_prompt).substr(0, 16));
            }
            std::sort(mandates.begin(), mandates.end());
            std::string macc;
            for (const auto& [n, h] : mandates) macc += n + "=" + h + ";";
            mandate_digest_ = mandates.empty()
                ? std::string("none")
                : security::CryptoUtils::sha256(macc).substr(0, 16);
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
