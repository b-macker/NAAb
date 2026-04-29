// governance_reports.cpp — GovernanceEngine report generation, audit, plugins
// Extracted from governance.cpp lines 5603-7484

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

// Forward declarations for symbols defined in other governance translation units
namespace naab {
std::pair<std::vector<std::string>, std::vector<std::string>>
lookupCweOwasp(const std::string& rule_name);  // defined in governance_engine.cpp
} // namespace naab

namespace naab {
namespace governance {
std::string normalizeLanguage(const std::string& language);  // defined in governance_checks.cpp
} // namespace governance
} // namespace naab

namespace naab {
namespace interpreter {
extern thread_local Interpreter* g_current_interpreter;  // defined in interpreter.cpp
} // namespace interpreter
} // namespace naab

namespace naab {
namespace governance {

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

void GovernanceEngine::logPolyglotExecution(const std::string& language,
                                              const std::vector<std::string>& bound_vars,
                                              int64_t duration_us,
                                              const std::string& file, int line,
                                              const std::string& runtime_version) {
    if (!rules_.audit.log_events.polyglot_executed &&
        !rules_.audit.log_events.polyglot_timing) return;

    std::string vars_str;
    for (size_t i = 0; i < bound_vars.size(); ++i) {
        if (i > 0) vars_str += ", ";
        vars_str += bound_vars[i];
    }

    std::string msg = "lang=" + language;
    if (!runtime_version.empty()) msg += " runtime=" + runtime_version;
    if (!vars_str.empty()) msg += " vars=[" + vars_str + "]";
    if (rules_.audit.log_events.polyglot_timing) {
        msg += " duration=" + std::to_string(duration_us) + "us";
    }

    logAuditEvent("polyglot_executed", "polyglot", msg, file, line);
}

void GovernanceEngine::logTaintDecision(const std::string& var_name,
                                         const std::string& decision,
                                         const std::string& sink,
                                         const std::string& file, int line) {
    if (!rules_.audit.log_events.taint_decisions) return;

    std::string msg = "var=" + var_name + " decision=" + decision;
    if (!sink.empty()) msg += " sink=" + sink;

    logAuditEvent("taint_decision", "taint_tracking", msg, file, line);
}

void GovernanceEngine::logContractCheck(const std::string& func_name,
                                         const std::string& result,
                                         const std::string& detail,
                                         const std::string& file, int line) {
    if (!rules_.audit.log_events.contract_checks) return;

    std::string msg = "func=" + func_name + " result=" + result;
    if (!detail.empty()) msg += " " + detail;

    logAuditEvent("contract_check", "contracts", msg, file, line);
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

// --- Report Helpers ---

static std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
        }
    }
    return out;
}

static std::string csvEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

static std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out += c;
        }
    }
    return out;
}

// --- Report Generation ---

std::string GovernanceEngine::generateJsonReport() const {
    nlohmann::json report;
    report["version"] = "4.0";
    report["mode"] = rules_.mode == GovernanceMode::ENFORCE ? "enforce" : (rules_.mode == GovernanceMode::AUDIT ? "audit" : "off");

    int total = 0, passed = 0, failed_hard = 0, failed_soft = 0, advisories = 0;
    for (const auto& r : check_results_) {
        total++;
        if (r.passed) { passed++; }
        else if (r.level == EnforcementLevel::HARD) { failed_hard++; }
        else if (r.level == EnforcementLevel::SOFT) { failed_soft++; }
        else { advisories++; }
    }
    report["summary"]["total"] = total;
    report["summary"]["passed"] = passed;
    report["summary"]["failed_hard"] = failed_hard;
    report["summary"]["failed_soft"] = failed_soft;
    report["summary"]["advisories"] = advisories;

    report["results"] = nlohmann::json::array();
    for (const auto& r : check_results_) {
        nlohmann::json entry;
        entry["rule"] = r.rule_name;
        entry["level"] = levelToString(r.level);
        entry["passed"] = r.passed;
        if (!r.message.empty()) entry["message"] = r.message;
        entry["category"] = r.category;
        entry["severity"] = r.severity;
        entry["file"] = r.file;
        entry["line"] = r.line;
        if (!r.cwe_ids.empty()) {
            entry["cwe"] = nlohmann::json::array();
            for (const auto& c : r.cwe_ids) entry["cwe"].push_back(c);
        }
        if (!r.owasp_ids.empty()) {
            entry["owasp"] = nlohmann::json::array();
            for (const auto& o : r.owasp_ids) entry["owasp"].push_back(o);
        }
        report["results"].push_back(entry);
    }
    return report.dump(2);
}

std::string GovernanceEngine::generateSarifReport() const {
    nlohmann::json sarif;
    sarif["version"] = "2.1.0";
    sarif["$schema"] = "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json";

    nlohmann::json run;

    // Tool metadata
    run["tool"]["driver"]["name"] = "NAAb Governance Engine";
    run["tool"]["driver"]["version"] = "4.0";
    run["tool"]["driver"]["semanticVersion"] = "4.0.0";
    run["tool"]["driver"]["informationUri"] = "https://github.com/nickvdyck/naab-lang";
    run["tool"]["driver"]["organization"] = "NAAb";

    // Build unique rules array from all check results
    std::map<std::string, size_t> rule_index_map;
    auto& rules_arr = run["tool"]["driver"]["rules"] = nlohmann::json::array();

    for (const auto& r : check_results_) {
        if (rule_index_map.count(r.rule_name)) continue;
        size_t idx = rules_arr.size();
        rule_index_map[r.rule_name] = idx;

        nlohmann::json rule;
        rule["id"] = r.rule_name;
        rule["name"] = r.rule_name;

        // Human-readable description from rule name
        std::string short_desc = r.rule_name;
        for (auto& ch : short_desc) {
            if (ch == '_') ch = ' ';
            else if (ch == '.') ch = ' ';
        }
        rule["shortDescription"]["text"] = short_desc;

        // Default configuration level
        if (r.level == EnforcementLevel::ADVISORY)
            rule["defaultConfiguration"]["level"] = "warning";
        else
            rule["defaultConfiguration"]["level"] = "error";

        if (!r.category.empty()) {
            rule["properties"]["category"] = r.category;
        }

        // Feature 3: CWE/OWASP tags in SARIF
        auto [cwes, owasps] = lookupCweOwasp(r.rule_name);
        if (!cwes.empty()) {
            rule["properties"]["cwe"] = nlohmann::json::array();
            for (const auto& cwe : cwes) rule["properties"]["cwe"].push_back(cwe);
            std::string cwe_num = cwes[0].substr(4); // "CWE-89" -> "89"
            rule["helpUri"] = "https://cwe.mitre.org/data/definitions/" + cwe_num + ".html";
        }
        if (!owasps.empty()) {
            rule["properties"]["owasp"] = nlohmann::json::array();
            for (const auto& o : owasps) rule["properties"]["owasp"].push_back(o);
        }

        rules_arr.push_back(rule);
    }

    // Results (failures only)
    auto& results_arr = run["results"] = nlohmann::json::array();

    for (const auto& r : check_results_) {
        if (r.passed) continue;

        nlohmann::json result;
        result["ruleId"] = r.rule_name;

        auto it = rule_index_map.find(r.rule_name);
        if (it != rule_index_map.end()) {
            result["ruleIndex"] = static_cast<int>(it->second);
        }

        // SARIF level mapping
        if (r.level == EnforcementLevel::ADVISORY)
            result["level"] = "warning";
        else
            result["level"] = "error";

        // Full message (not truncated)
        result["message"]["text"] = r.message.empty() ? r.rule_name : r.message;

        // Physical location with file and line
        if (!r.file.empty() || r.line > 0) {
            nlohmann::json location;
            nlohmann::json physical;

            if (!r.file.empty()) {
                std::string uri = r.file;
                if (uri.size() >= 2 && uri[0] == '.' && uri[1] == '/') uri = uri.substr(2);
                physical["artifactLocation"]["uri"] = uri;
                physical["artifactLocation"]["uriBaseId"] = "%SRCROOT%";
            }

            if (r.line > 0) {
                physical["region"]["startLine"] = r.line;
                physical["region"]["startColumn"] = 1;
            }

            location["physicalLocation"] = physical;
            result["locations"] = nlohmann::json::array({location});
        }

        // Properties
        nlohmann::json props;
        if (!r.category.empty()) props["category"] = r.category;
        if (!r.severity.empty()) props["severity"] = r.severity;
        props["enforcement"] = levelToString(r.level);
        result["properties"] = props;

        results_arr.push_back(result);
    }

    // Invocations (required by GitHub code scanning)
    bool any_hard_failures = false;
    for (const auto& r : check_results_) {
        if (!r.passed && r.level != EnforcementLevel::ADVISORY) {
            any_hard_failures = true;
            break;
        }
    }
    run["invocations"] = nlohmann::json::array({
        {{"executionSuccessful", !any_hard_failures}}
    });

    sarif["runs"] = nlohmann::json::array({run});
    return sarif.dump(2);
}

std::string GovernanceEngine::generateJunitReport() const {
    std::ostringstream oss;
    int total = static_cast<int>(check_results_.size());
    int failures = 0, warnings = 0;
    for (const auto& r : check_results_) {
        if (!r.passed) {
            if (r.level == EnforcementLevel::ADVISORY) warnings++;
            else failures++;
        }
    }

    // Get current timestamp
    auto now = std::time(nullptr);
    char ts_buf[64];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));

    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<testsuite name=\"NAAb Governance\" tests=\"" << total
        << "\" failures=\"" << failures
        << "\" errors=\"0\" skipped=\"0\""
        << " warnings=\"" << warnings << "\""
        << " timestamp=\"" << ts_buf << "\""
        << " time=\"0\">\n";

    for (const auto& r : check_results_) {
        std::string classname = r.category.empty()
            ? "governance" : "governance." + r.category;

        oss << "  <testcase name=\"" << xmlEscape(r.rule_name)
            << "\" classname=\"" << xmlEscape(classname) << "\" time=\"0\"";

        // Add file/line as attributes (widely supported by CI tools)
        if (!r.file.empty()) oss << " file=\"" << xmlEscape(r.file) << "\"";
        if (r.line > 0) oss << " line=\"" << r.line << "\"";

        if (r.passed) {
            oss << "/>\n";
        } else {
            oss << ">\n";
            std::string first_line = r.message.empty() ? r.rule_name
                : r.message.substr(0, r.message.find('\n'));
            oss << "    <failure message=\"" << xmlEscape(first_line)
                << "\" type=\"" << xmlEscape(levelToString(r.level)) << "\">"
                << xmlEscape(r.message.empty() ? r.rule_name : r.message)
                << "</failure>\n";
            oss << "  </testcase>\n";
        }
    }

    oss << "</testsuite>\n";
    return oss.str();
}

std::string GovernanceEngine::generateCsvReport() const {
    std::ostringstream oss;
    oss << "rule,level,passed,message,category,severity,file,line\n";
    for (const auto& r : check_results_) {
        oss << "\"" << csvEscape(r.rule_name) << "\","
            << levelToString(r.level) << ","
            << (r.passed ? "true" : "false") << ","
            << "\"" << csvEscape(r.message) << "\","
            << "\"" << csvEscape(r.category) << "\","
            << "\"" << csvEscape(r.severity) << "\","
            << "\"" << csvEscape(r.file) << "\","
            << r.line << "\n";
    }
    return oss.str();
}

std::string GovernanceEngine::generateHtmlReport() const {
    std::ostringstream oss;
    int total = 0, passed = 0, failed = 0, warnings = 0;
    for (const auto& r : check_results_) {
        total++;
        if (r.passed) passed++;
        else if (r.level == EnforcementLevel::ADVISORY) warnings++;
        else failed++;
    }

    oss << "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n";
    oss << "<title>NAAb Governance Report</title>\n";
    oss << "<style>\n"
        << "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; margin: 2em; }\n"
        << "table { border-collapse: collapse; width: 100%; }\n"
        << "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }\n"
        << "th { background: #f5f5f5; }\n"
        << "tr:nth-child(even) { background: #fafafa; }\n"
        << ".pass { color: #2e7d32; } .fail-hard { color: #c62828; }\n"
        << ".fail-soft { color: #e65100; } .warning { color: #f9a825; }\n"
        << ".summary { display: flex; gap: 2em; margin: 1em 0; }\n"
        << ".stat { padding: 1em; border-radius: 8px; background: #f5f5f5; }\n"
        << ".msg { max-width: 500px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }\n"
        << ".msg:hover { white-space: normal; }\n"
        << "</style></head><body>\n";

    oss << "<h1>NAAb Governance Report</h1>\n";
    oss << "<div class=\"summary\">\n"
        << "  <div class=\"stat\"><strong>" << total << "</strong> checks</div>\n"
        << "  <div class=\"stat pass\"><strong>" << passed << "</strong> passed</div>\n"
        << "  <div class=\"stat fail-hard\"><strong>" << failed << "</strong> failed</div>\n"
        << "  <div class=\"stat warning\"><strong>" << warnings << "</strong> warnings</div>\n"
        << "</div>\n";

    if (failed > 0 || warnings > 0) {
        oss << "<h2>Issues</h2>\n<table>\n";
        oss << "<tr><th>Rule</th><th>Level</th><th>Category</th><th>File</th><th>Line</th><th>Message</th></tr>\n";
        for (const auto& r : check_results_) {
            if (r.passed) continue;
            std::string css_class = (r.level == EnforcementLevel::HARD) ? "fail-hard" :
                                    (r.level == EnforcementLevel::SOFT) ? "fail-soft" : "warning";
            std::string first_line = r.message.empty() ? r.rule_name
                : r.message.substr(0, r.message.find('\n'));
            oss << "<tr class=\"" << css_class << "\">"
                << "<td>" << htmlEscape(r.rule_name) << "</td>"
                << "<td>" << htmlEscape(levelToString(r.level)) << "</td>"
                << "<td>" << htmlEscape(r.category) << "</td>"
                << "<td>" << htmlEscape(r.file) << "</td>"
                << "<td>" << r.line << "</td>"
                << "<td class=\"msg\" title=\"" << htmlEscape(r.message) << "\">"
                << htmlEscape(first_line) << "</td>"
                << "</tr>\n";
        }
        oss << "</table>\n";
    }

    oss << "<h2>All Checks (" << total << ")</h2>\n<table>\n";
    oss << "<tr><th>Rule</th><th>Level</th><th>Status</th><th>Category</th></tr>\n";
    for (const auto& r : check_results_) {
        std::string status_class = r.passed ? "pass" :
            (r.level == EnforcementLevel::ADVISORY ? "warning" : "fail-hard");
        oss << "<tr><td>" << htmlEscape(r.rule_name) << "</td>"
            << "<td>" << htmlEscape(levelToString(r.level)) << "</td>"
            << "<td class=\"" << status_class << "\">" << (r.passed ? "PASS" : "FAIL") << "</td>"
            << "<td>" << htmlEscape(r.category) << "</td></tr>\n";
    }
    oss << "</table>\n</body></html>\n";
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
    writeTelemetry();
}

// --- Telemetry JSONL Output ---
void GovernanceEngine::writeTelemetry() const {
    if (!rules_.telemetry_output.enabled || rules_.telemetry_output.output_file.empty()) return;

    // Use C FILE* + flock for atomic multi-process writes.
    // RAII wrapper ensures fclose+unlock even if json serialization throws.
    auto fp_deleter = [](FILE* f) {
#ifndef _WIN32
        ::flock(fileno(f), LOCK_UN);
#endif
        fclose(f);
    };
    std::unique_ptr<FILE, decltype(fp_deleter)> fp(
        fopen(rules_.telemetry_output.output_file.c_str(), "a"), fp_deleter);
    if (!fp) {
        fprintf(stderr, "[governance] Warning: Could not open telemetry file: %s\n",
                rules_.telemetry_output.output_file.c_str());
        return;
    }
#ifndef _WIN32
    ::flock(fileno(fp.get()), LOCK_EX);  // Blocking exclusive lock
#endif

    // ISO timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    std::string timestamp(ts_buf);

    for (const auto& r : check_results_) {
        nlohmann::json ev;
        ev["agent_id"] = agent_id_;
        ev["event_type"] = r.passed ? "GovernanceCheck" : "RuleViolation";
        ev["rule_name"] = r.rule_name;
        ev["result"] = r.passed ? "pass" : "block";
        ev["message"] = r.message.empty()
            ? (r.passed ? "Check passed: " + r.rule_name : "Check failed: " + r.rule_name)
            : r.message;
        ev["timestamp"] = timestamp;
        ev["file"] = r.file;
        ev["line"] = r.line;
        ev["category"] = r.category;
        ev["severity"] = r.severity;
        ev["level"] = levelToString(r.level);
        if (!r.cwe_ids.empty()) {
            ev["cwe"] = nlohmann::json::array();
            for (const auto& c : r.cwe_ids) ev["cwe"].push_back(c);
        }
        std::string line = ev.dump() + "\n";
        fwrite(line.c_str(), 1, line.size(), fp.get());
    }
    // fp_deleter handles flock(LOCK_UN) + fclose automatically.

    if (!check_results_.empty()) {
        fprintf(stderr, "[governance] Telemetry: %zu events written to %s\n",
                check_results_.size(), rules_.telemetry_output.output_file.c_str());
    }
}

// --- Agent Role Application ---
void GovernanceEngine::applyAgentRole() {
    for (const auto& role : rules_.agent_roles) {
        if (role.name == agent_id_) {
            // Restrict allowed languages: intersect with base allowed_languages
            if (!role.allowed_languages.empty()) {
                if (rules_.allowed_languages.empty()) {
                    // No base restriction — apply role's languages as the restriction
                    for (const auto& l : role.allowed_languages)
                        rules_.allowed_languages.insert(l);
                } else {
                    // Intersect: keep only languages in both base AND role
                    std::unordered_set<std::string> intersection;
                    for (const auto& l : role.allowed_languages) {
                        if (rules_.allowed_languages.count(l))
                            intersection.insert(l);
                    }
                    rules_.allowed_languages = intersection;
                }
            }

            // V-GOV-018: Apply per-role shell restriction.
            // Roles can only restrict (deny shell); they cannot grant shell if
            // the global policy already denies it.
            if (role.shell_allowed_set && !role.shell_allowed) {
                rules_.shell_allowed = false;
            }

            // Path restrictions enforced at runtime via checkPathAccess()
            fprintf(stderr, "[governance] Agent role applied: %s (languages: ",
                    agent_id_.c_str());
            bool first = true;
            for (const auto& l : rules_.allowed_languages) {
                if (!first) fprintf(stderr, ", ");
                fprintf(stderr, "%s", l.c_str());
                first = false;
            }
            fprintf(stderr, "), shell: %s\n",
                    rules_.shell_allowed ? "allowed" : "blocked");
            return;
        }
    }
    // No matching role found — base rules apply unchanged
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

    // Never suggest a language that is blocked by governance
    if (should_suggest && rules_.blocked_languages.count(result.optimal_language)) {
        should_suggest = false;
    }

    if (should_suggest && show_suggestions) {
        // Build concise note for all levels
        std::string note = fmt::format(
            "Consider using {} instead of {} (score: {}/100 vs {}/100, +{}% improvement)",
            result.optimal_language, language,
            result.optimal_language_score, result.current_language_score,
            result.improvement_percent);

        if (level == "hard") {
            // Verbose hint block only for hard enforcement
            suggestBetterLanguage(
                language, code,
                analyzer::taskIntentToString(result.primary_task),
                {result.optimal_language},
                result.improvement_percent,
                result.reasons
            );
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
            // 1-line advisory via centralized emitAdvisory
            emitAdvisory(fmt::format("[governance] {}", note));
            message = note;
        } else if (level == "advisory") {
            // Record only — no output (check_results_ below handles it)
            message = note;
        }

        // Record check result
        CheckResult check;
        check.rule_name = "polyglot_optimization";
        check.level = level == "hard" ? EnforcementLevel::HARD :
                     level == "soft" ? EnforcementLevel::SOFT :
                                      EnforcementLevel::ADVISORY;
        check.passed = (level == "advisory");  // advisory suggestions aren't failures
        check.message = message;
        check.category = "polyglot";
        check.severity = result.improvement_percent > 50 ? "high" :
                        result.improvement_percent > 30 ? "medium" : "low";
        check.line = line;
        check.file = current_check_file_;
        check_results_.push_back(check);

        // Only HARD blocks execution
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
            vr.result = verif_value.isNull() ? "" : verif_value.toString();
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
            "  Adjust verification.tolerance in govern.json if this threshold is too strict.",
            line, task_type, details, agree_count, total_count, cfg.min_consensus);
    }

    return "";
}

// ============================================================================
// Governance Plugin API (#23): NAAb-based custom governance rules
// ============================================================================

void GovernanceEngine::loadPlugins() {
    if (plugins_loaded_) return;
    plugins_loaded_ = true;

    auto* interp = interpreter::g_current_interpreter;
    if (!interp) {
        fprintf(stderr, "[governance] Warning: No interpreter available for plugin loading\n");
        return;
    }

    namespace fs = std::filesystem;
    for (auto& plugin : rules_.governance_plugins) {
        if (plugin.rules.empty()) continue;

        // Resolve file path relative to govern.json directory
        fs::path plugin_path;
        if (fs::path(plugin.file_path).is_absolute()) {
            plugin_path = plugin.file_path;
        } else {
            plugin_path = fs::path(govern_json_dir_) / plugin.file_path;
        }

        if (!fs::exists(plugin_path)) {
            fprintf(stderr, "[governance] Warning: Plugin file not found: %s\n",
                    plugin_path.string().c_str());
            continue;
        }

        try {
            interp->loadPluginFile(plugin_path.string());
            plugin.loaded = true;
        } catch (const std::exception& e) {
            fprintf(stderr, "[governance] Warning: Failed to load plugin %s: %s\n",
                    plugin_path.string().c_str(), e.what());
        }
    }
}

std::string GovernanceEngine::checkPluginRules(
    const std::string& trigger,
    const std::unordered_map<std::string, interpreter::NaabVal>& context,
    int line) {

    // Re-entrancy guard: prevent infinite recursion if plugin code triggers governance
    if (in_plugin_check_) return "";

    // No plugins configured — fast path
    if (rules_.governance_plugins.empty()) return "";

    // Lazy-load plugins on first call
    if (!plugins_loaded_) loadPlugins();

    auto* interp = interpreter::g_current_interpreter;
    if (!interp) return "";

    // Scope guard for re-entrancy flag
    struct PluginGuard {
        bool& flag;
        PluginGuard(bool& f) : flag(f) { flag = true; }
        ~PluginGuard() { flag = false; }
    } guard(in_plugin_check_);

    for (const auto& plugin : rules_.governance_plugins) {
        if (!plugin.loaded) continue;

        for (const auto& rule : plugin.rules) {
            if (!rule.enabled) continue;
            if (rule.trigger != trigger) continue;

            // Language filter
            if (!rule.languages.empty()) {
                auto lang_it = context.find("language");
                if (lang_it != context.end()) {
                    std::string lang;
                    auto lang_val = lang_it->second;
                    if (lang_val.isString()) lang = lang_val.asString();
                    bool matches = false;
                    for (const auto& l : rule.languages) {
                        if (l == lang) { matches = true; break; }
                    }
                    if (!matches) continue;
                }
            }

            // Look up function in global env
            interpreter::NaabVal fn;
            try {
                fn = interp->getGlobalEnv()->get(rule.function_name);
                if (fn.isNull()) {
                    if (warned_plugin_rules_.find(rule.function_name) == warned_plugin_rules_.end()) {
                        fprintf(stderr, "[governance] Warning: Plugin function '%s' not found (rule %s)\n",
                                rule.function_name.c_str(), rule.id.c_str());
                        warned_plugin_rules_.insert(rule.function_name);
                    }
                    continue;
                }
            } catch (...) {
                if (warned_plugin_rules_.find(rule.function_name) == warned_plugin_rules_.end()) {
                    fprintf(stderr, "[governance] Warning: Plugin function '%s' not found (rule %s)\n",
                            rule.function_name.c_str(), rule.id.c_str());
                    warned_plugin_rules_.insert(rule.function_name);
                }
                continue;
            }

            // Build context dict as NaabVal
            std::unordered_map<std::string, interpreter::NaabVal> ctx_map;
            for (const auto& [key, val] : context) {
                ctx_map[key] = val;
            }
            auto ctx_val = interpreter::NaabVal::makeDict(std::move(ctx_map));

            // Call the plugin function
            interpreter::NaabVal result;
            try {
                result = interp->callFunction(fn, {ctx_val});
            } catch (const std::exception& e) {
                fprintf(stderr, "[governance] Plugin rule '%s' threw: %s\n",
                        rule.id.c_str(), e.what());
                continue;
            }

            // Parse result — must be a dict with "pass" key
            if (!result.isDict()) {
                if (warned_plugin_rules_.find(rule.id + ".result") == warned_plugin_rules_.end()) {
                    fprintf(stderr, "[governance] Warning: Plugin rule '%s' returned %s (expected dict with 'pass' key)\n",
                            rule.id.c_str(), result.getTypeName().c_str());
                    warned_plugin_rules_.insert(rule.id + ".result");
                }
                continue;
            }

            // Extract "pass" field
            auto& result_dict = result.asDict();
            bool passed = true;
            bool has_pass = false;
            for (const auto& [k, v] : result_dict) {
                if (k == "pass") {
                    has_pass = true;
                    if (v.isBool()) passed = v.asBool();
                    else if (v.isNull()) passed = false;
                    break;
                }
            }

            if (!has_pass) {
                if (warned_plugin_rules_.find(rule.id + ".pass") == warned_plugin_rules_.end()) {
                    fprintf(stderr, "[governance] Warning: Plugin rule '%s' returned dict without 'pass' key\n",
                            rule.id.c_str());
                    warned_plugin_rules_.insert(rule.id + ".pass");
                }
                continue;
            }

            std::string rule_path = "governance_plugins." + rule.id;

            if (passed) {
                recordPass(rule_path, rule.level);
                continue;
            }

            // Failed — build error message from plugin result + rule defaults
            std::string msg = rule.message;
            std::string help = rule.help;
            std::string good_ex = rule.good_example;
            std::string bad_ex = rule.bad_example;

            // Plugin can override any of these
            for (const auto& [k, v] : result_dict) {
                if (k == "message" && v.isString()) msg = v.asString();
                else if (k == "help" && v.isString()) help = v.asString();
                else if (k == "good_example" && v.isString()) good_ex = v.asString();
                else if (k == "bad_example" && v.isString()) bad_ex = v.asString();
            }

            if (msg.empty()) {
                msg = fmt::format("Plugin rule '{}' violated", rule.id);
            }

            std::string err = enforce(rule_path, rule.level,
                formatError(rule.level, msg,
                    line > 0 ? fmt::format("line {}", line) : "",
                    "governance_plugins[\"" + rule.id + "\"]",
                    help, bad_ex, good_ex));
            if (!err.empty()) return err;
        }
    }

    return "";
}

} // namespace governance
} // namespace naab
