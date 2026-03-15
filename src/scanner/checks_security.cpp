// NAAb Scanner — Security Checks (10 checks)
// Port from naab-q/src/checks/security.naab

#include "naab/scanner.h"
#include <regex>
#include <unordered_set>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string sec_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline bool sec_startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Security uses different severity mapping: hard->critical, soft->medium, advisory->low
static std::string sec_levelToSeverity(const std::string& level) {
    if (level == "hard") return "critical";
    if (level == "soft") return "medium";
    if (level == "advisory") return "low";
    return "medium";
}

void ScannerEngine::checkSecurity(const std::string& filepath,
                                   const std::vector<std::string>& lines,
                                   const std::string& language,
                                   std::vector<Issue>& issues) const {
    const std::string CAT = "security";

    // Helper to add security issue with proper severity mapping
    auto addSecIssue = [&](int line, const std::string& rule,
                           const std::string& msg, const std::string& preview,
                           const std::string& fix, const std::string& level_override = "") {
        std::string lvl = level_override.empty() ? getLevel(CAT, rule) : level_override;
        Issue issue;
        issue.file = filepath;
        issue.line = line;
        issue.rule = rule;
        issue.category = CAT;
        issue.level = lvl;
        issue.severity = sec_levelToSeverity(lvl);
        issue.message = msg;
        issue.preview = preview.substr(0, 120);
        issue.fix = fix;
        issues.push_back(std::move(issue));
    };

    // 1. hardcoded_credentials
    if (isEnabled(CAT, "hardcoded_credentials")) {
        static const std::regex cred_pat(
            R"((?:password|passwd|pwd|secret|api_key|apikey|token|auth_token|access_key)\s*[=:]\s*['"][^'"]{8,}['"])",
            std::regex::icase);
        static const std::regex redact_pat(R"([=:]\s*['"][^'"]+['"])");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            if (sec_startsWith(s, "#") || sec_startsWith(s, "//") ||
                sec_startsWith(s, "/*") || sec_startsWith(s, "*")) continue;
            if (std::regex_search(s, cred_pat)) {
                std::string redacted = std::regex_replace(s, redact_pat, "= \"***REDACTED***\"");
                addSecIssue(i + 1, "hardcoded_credentials",
                           "Hardcoded credential detected", redacted,
                           "Use environment variables or secrets manager");
            }
        }
    }

    // 2. sql_string_concat
    if (isEnabled(CAT, "sql_string_concat")) {
        static const std::regex sql_pat(
            R"((?:SELECT|INSERT|UPDATE|DELETE|DROP)\s+.*(?:\+\s*\w+|\.format\(|f['"]))",
            std::regex::icase);

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            if (sec_startsWith(s, "#") || sec_startsWith(s, "//") || sec_startsWith(s, "/*")) continue;
            if (std::regex_search(s, sql_pat)) {
                addSecIssue(i + 1, "sql_string_concat",
                           "SQL injection risk via string concatenation", s,
                           "Use parameterized queries");
            }
        }
    }

    // 3. shell_injection
    if (isEnabled(CAT, "shell_injection")) {
        static const std::regex shell_pats[] = {
            std::regex(R"(os\.system\s*\([^)]*\+)"),
            std::regex(R"(subprocess\.\w+\(.*shell\s*=\s*True)"),
            std::regex(R"(child_process\.exec\s*\([^)]*\+)")
        };

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            if (sec_startsWith(s, "#") || sec_startsWith(s, "//") || sec_startsWith(s, "/*")) continue;
            for (const auto& pat : shell_pats) {
                if (std::regex_search(s, pat)) {
                    addSecIssue(i + 1, "shell_injection",
                               "Command injection risk", s,
                               "Use subprocess with list args, no shell=True");
                    break;
                }
            }
        }
    }

    // 4. insecure_random
    if (isEnabled(CAT, "insecure_random")) {
        static const std::regex random_pat(
            R"((?:random\.(?:random|randint|choice|randrange)\(|Math\.random\(\)))",
            std::regex::icase);
        static const std::regex sec_context(
            R"((?:password|token|key|secret|salt|nonce|otp|csrf|session))",
            std::regex::icase);

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            bool has_random = std::regex_search(s, random_pat);
            bool has_random_word = s.find("random") != std::string::npos;

            if (has_random || (has_random_word && std::regex_search(s, sec_context))) {
                bool in_sec = std::regex_search(s, sec_context);
                if (!in_sec && i > 0) {
                    in_sec = std::regex_search(lines[i - 1], sec_context);
                }
                if (in_sec) {
                    addSecIssue(i + 1, "insecure_random",
                               "Insecure random in security context", s,
                               "Use secrets module or crypto.randomBytes");
                }
            }
        }
    }

    // 5. weak_crypto
    if (isEnabled(CAT, "weak_crypto")) {
        static const std::regex weak_pat(R"(\b(?:md5|sha1)\s*[(.])");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            if (sec_startsWith(s, "#") || sec_startsWith(s, "//") || sec_startsWith(s, "/*")) continue;
            std::string lower_s = s;
            std::transform(lower_s.begin(), lower_s.end(), lower_s.begin(), ::tolower);
            if (std::regex_search(lower_s, weak_pat) && lower_s.find("checksum") == std::string::npos) {
                addSecIssue(i + 1, "weak_crypto",
                           "Weak hash algorithm (MD5/SHA1)", s,
                           "Use SHA-256 or stronger");
            }
        }
    }

    // 6. hardcoded_ip
    if (isEnabled(CAT, "hardcoded_ip")) {
        static const std::regex ip_pat(R"(\b(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})\b)");
        static const std::unordered_set<std::string> safe_ips = {
            "127.0.0.1", "0.0.0.0", "255.255.255.255", "255.255.255.0"
        };

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            if (sec_startsWith(s, "#") || sec_startsWith(s, "//") || sec_startsWith(s, "/*")) continue;
            std::sregex_iterator it(s.begin(), s.end(), ip_pat);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                std::string ip = (*it)[1].str();
                if (!safe_ips.count(ip)) {
                    addSecIssue(i + 1, "hardcoded_ip",
                               fmt::format("Hardcoded IP {}", ip), s,
                               "Move to config");
                }
            }
        }
    }

    // 7. hardcoded_url
    if (isEnabled(CAT, "hardcoded_url")) {
        static const std::regex url_pat(R"(https?://(?!example\.com|localhost|127\.0\.0\.1)[^\s'"]+)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            if (sec_startsWith(s, "#") || sec_startsWith(s, "//") || sec_startsWith(s, "/*")) continue;
            if (std::regex_search(s, url_pat)) {
                addSecIssue(i + 1, "hardcoded_url",
                           "Hardcoded URL", s, "Move to config");
            }
        }
    }

    // 8. path_traversal
    if (isEnabled(CAT, "path_traversal")) {
        static const std::regex path_pats[] = {
            std::regex(R"(open\s*\([^)]*\+)"),
            std::regex(R"(os\.path\.join\s*\([^)]*request)"),
            std::regex(R"(fs\.readFile\s*\([^)]*req\.)")
        };

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            for (const auto& pat : path_pats) {
                if (std::regex_search(s, pat)) {
                    addSecIssue(i + 1, "path_traversal",
                               "Potential path traversal", s,
                               "Validate and sanitize file paths");
                    break;
                }
            }
        }
    }

    // 9. insecure_deserialization
    if (isEnabled(CAT, "insecure_deserialization")) {
        struct DeserPat { std::regex pat; std::string desc; };
        static const std::vector<std::pair<std::string, std::string>> deser_pats = {
            {R"(pickle\.loads?\s*\()", "Unsafe pickle"},
            {R"(yaml\.load\s*\((?!.*SafeLoader))", "yaml.load without SafeLoader"},
            {R"(eval\s*\(.*input)", "eval with user input"}
        };

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            if (sec_startsWith(s, "#") || sec_startsWith(s, "//") || sec_startsWith(s, "/*")) continue;
            for (const auto& [pat_str, desc] : deser_pats) {
                std::regex pat(pat_str);
                if (std::regex_search(s, pat)) {
                    addSecIssue(i + 1, "insecure_deserialization", desc, s,
                               "Use safe deserialization");
                    break;
                }
            }
        }
    }

    // 10. exposed_stack_traces
    if (isEnabled(CAT, "exposed_stack_traces")) {
        static const std::regex trace_pat(R"(traceback\.format_exc\s*\(\))");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = sec_trim(lines[i]);
            if (std::regex_search(s, trace_pat)) {
                addSecIssue(i + 1, "exposed_stack_traces",
                           "Stack trace may be exposed", s,
                           "Log server-side, send generic error to client");
            }
        }
    }
}

} // namespace scanner
} // namespace naab
