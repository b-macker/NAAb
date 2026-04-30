// governance_checks.cpp — GovernanceEngine check implementations
// Extracted from governance.cpp lines 2718-5602

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

        // V-GOV-001: consume Python/JS string prefixes (f, b, r, u and two-letter
        // combinations rb, br, rf, fr) before quote detection. Without this, the
        // prefix character is left in the output, allowing f"malicious_code()" to
        // leak the 'f' into the stripped result and fool pattern matchers.
        if (!in_single && !in_double && !in_backtick) {
            char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lc == 'f' || lc == 'b' || lc == 'r' || lc == 'u') {
                // Check for two-letter prefix (rb, br, rf, fr) followed by quote
                if (i + 2 < code.size()) {
                    char lc2 = static_cast<char>(std::tolower(static_cast<unsigned char>(code[i+1])));
                    char q2 = code[i+2];
                    std::string two = {lc, lc2};
                    if ((two == "rb" || two == "br" || two == "rf" || two == "fr") &&
                        (q2 == '"' || q2 == '\'')) {
                        result += "  ";  // replace two-char prefix with spaces
                        i += 2;
                        c = q2; // fall through to quote handling below
                        // Re-run the quote handling with c = q2
                        if (c == '"' && !in_single && !in_backtick && !in_double
                            && i+2 < code.size() && code[i+1] == '"' && code[i+2] == '"') {
                            i += 3;
                            while (i+2 < code.size()) {
                                if (code[i] == '"' && code[i+1] == '"' && code[i+2] == '"') { i += 2; break; }
                                i++;
                            }
                            continue;
                        }
                        if (c == '\'' && !in_double && !in_backtick && !in_single
                            && i+2 < code.size() && code[i+1] == '\'' && code[i+2] == '\'') {
                            i += 3;
                            while (i+2 < code.size()) {
                                if (code[i] == '\'' && code[i+1] == '\'' && code[i+2] == '\'') { i += 2; break; }
                                i++;
                            }
                            continue;
                        }
                        if (c == '"')  { in_double  = true; continue; }
                        if (c == '\'') { in_single  = true; continue; }
                        continue;
                    }
                }
                // Check for single-letter prefix followed by quote
                if (i + 1 < code.size() && (code[i+1] == '"' || code[i+1] == '\'')) {
                    result += ' ';  // replace prefix with space
                    i++;
                    c = code[i];
                    // fall through to standard quote handling
                }
            }
        }

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
std::string normalizeLanguage(const std::string& language) {
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
        } catch (const std::regex_error&) {
            // Invalid pattern — skip silently
        } catch (const std::bad_alloc&) {
            // Memory exhausted during governance check — fail-safe: deny execution
            throw std::runtime_error(
                "Governance: memory exhausted during security check — execution halted.\n"
                "  This is a fail-safe: governance cannot verify safety under memory pressure.\n"
                "  Increase available memory or reduce RLIMIT_AS if set.\n"
            );
        }
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
        } catch (const std::regex_error&) {
            // Invalid pattern — skip
        } catch (const std::bad_alloc&) {
            throw std::runtime_error(
                "Governance: memory exhausted during PII check — execution halted.\n"
                "  This is a fail-safe: governance cannot verify safety under memory pressure.\n"
            );
        }
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
        // NAAb commented-out code patterns (Gate 18: commented-out logic detection)
        "//\\s*\\w+\\s*=\\s*(?:true|false|null|\\d|\")",   // // var = true/false/value (commented assignment)
        "//\\s*\\w+\\.push\\(",                              // // arr.push(x) (commented mutation)
        "//\\s*return\\s+\\w",                               // // return value (commented return)
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
    // Only flag suspiciously precise values (0.85, 0.92, etc.), NOT common initializers (0.0, 0.5, 1.0)
    "(?:score|accuracy|precision|recall|f1|confidence|probability|similarity|distance|weight)\\s*=\\s*0\\.[1-9]\\d{1,}\\s*(?:#|//|$)",

    // Degenerate implementations
    "for\\s+\\w+\\s+in\\s+\\w+\\s*\\{\\s*\\}",
    "while\\s+\\w+\\s*\\{\\s*break\\s*\\}",

    // EVA-9: NAAb-specific incomplete logic patterns
    // NAAb catch-and-swallow (empty catch blocks)
    "catch\\s*\\([^)]*\\)\\s*\\{\\s*\\}",
    // NAAb identity/passthrough validation functions
    "fn\\s+(?:validate|check|verify|is_)\\w*\\([^)]*\\)\\s*\\{\\s*return\\s+true\\s*\\}",
    // NAAb identity sanitizer: validate_*(param) { return param } — returns input unchanged
    "fn\\s+(?:validate|sanitize|check|verify)_\\w*\\(\\s*(\\w+)\\s*\\)\\s*\\{\\s*return\\s+\\1\\s*;?\\s*\\}",
    // NAAb identity pipeline: x |> fn(param) { return param } — adds complexity points for nothing
    "\\|>\\s*fn\\s*\\(\\s*\\w+\\s*\\)\\s*\\{\\s*return\\s+\\w+\\s*;?\\s*\\}",
    // NAAb error sentinel: validate_*/check_* returning hardcoded error string instead of throwing
    "fn\\s+(?:validate|check|verify)_\\w+\\([^)]*\\)\\s*\\{[^}]*return\\s+\"(?:invalid|error|failed|unknown|missing|bad|none)[^\"]*\"",
    // NAAb copy-and-return identity sanitizer: validate_*(p) { let x = p; return x }
    "fn\\s+(?:validate|sanitize|check|verify)_\\w*\\(\\s*(\\w+)\\s*\\)\\s*\\{\\s*let\\s+(\\w+)\\s*=\\s*\\1\\s*;?\\s*return\\s+\\2\\s*;?\\s*\\}",
    // NAAb multi-let copy-and-return: validate_*(p) { let a = p; let b = a; return b }
    "fn\\s+(?:validate|sanitize|check|verify)_\\w*\\(\\s*(\\w+)\\s*\\)\\s*\\{\\s*(?:let\\s+\\w+\\s*=\\s*\\w+\\s*;?\\s*){1,3}return\\s+\\w+\\s*;?\\s*\\}",

    // EVA-EXTRA-3: Numeric result fabrication patterns
    // Suspiciously precise hardcoded scores (LLMs love 0.85, 0.92, etc.)
    "(?:score|accuracy|precision|recall|f1|confidence|similarity)\\s*=\\s*0\\.[89]\\d\\s*(?:#|//|$)",
    // Array of hardcoded floats (fabricated results)
    "\\[\\s*(?:0\\.\\d+,\\s*){4,}\\]",
};

std::string GovernanceEngine::checkIncompleteLogic(const std::string& code, int line, const std::string& source_file) {
    auto& cfg = rules_.code_quality.no_incomplete_logic;
    if (!cfg.enabled) return "";
    auto& pats = cfg.patterns.empty() ? DEFAULT_INCOMPLETE_LOGIC_PATTERNS : cfg.patterns;

    std::vector<std::string> active_pats;
    for (const auto& p : pats) active_pats.push_back(p);
    for (const auto& p : cfg.custom_patterns) active_pats.push_back(p);

    std::string found = searchPatterns(code, active_pats, !cfg.case_sensitive);
    if (!found.empty()) {
        // Check suppressions: format is "file_glob" or "category:file_glob"
        if (!cfg.suppressions.empty() && !source_file.empty()) {
            std::string basename = source_file;
            auto slash = basename.rfind('/');
            if (slash != std::string::npos) basename = basename.substr(slash + 1);
            for (const auto& s : cfg.suppressions) {
                // Simple glob: *.naab, test_*.naab, validators.naab
                std::string glob = s;
                auto colon = s.find(':');
                if (colon != std::string::npos) glob = s.substr(colon + 1);
                // Match: exact match, or wildcard prefix match
                if (glob == basename) { recordPass("code_quality.no_incomplete_logic", cfg.level); return ""; }
                if (glob.size() > 1 && glob[0] == '*') {
                    std::string suffix = glob.substr(1);
                    if (basename.size() >= suffix.size() &&
                        basename.compare(basename.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        recordPass("code_quality.no_incomplete_logic", cfg.level);
                        return "";
                    }
                }
            }
        }
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
    int line,
    const std::string& source_file) {

    setCheckContext(source_file, line);

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
        } else if (detail.find("match pattern") != std::string::npos) {
            bad_ex = "return \"invalid-trace-id\"  // error sentinel, not real data";
            good_ex = "if !valid { throw \"Invalid trace ID\" }\nreturn trace_id  // only valid values";
        } else if (detail.find("return_keys_non_null") != std::string::npos ||
                   detail.find("return_keys_non_empty") != std::string::npos) {
            bad_ex = "return {\"services\": [], \"stats\": {}}  // keys present but empty";
            good_ex = "return {\"services\": [real_data], \"stats\": {\"count\": n}}  // substantive values";
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

    // return_matches (regex validation for string returns)
    if (!contract.return_matches.empty() && result_type == "string") {
        try {
            std::regex re(contract.return_matches);
            if (!std::regex_match(result_str, re)) {
                return make_err(fmt::format(
                    "return value \"{}\" does not match pattern /{}/",
                    result_str, contract.return_matches));
            }
        } catch (const std::regex_error&) {
            // Invalid regex in contract config — skip, don't crash
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

    // return_keys_non_null / return_keys_non_empty (value validation for dict keys)
    if ((contract.return_keys_non_null || contract.return_keys_non_empty) &&
        result_type == "dict" && !contract.return_keys.empty()) {
        for (const auto& key : contract.return_keys) {
            std::string quoted_key = "\"" + key + "\":";
            auto pos = result_str.find(quoted_key);
            if (pos == std::string::npos) continue; // already caught by return_keys
            auto val_start = pos + quoted_key.size();
            while (val_start < result_str.size() && result_str[val_start] == ' ') val_start++;
            if (contract.return_keys_non_null) {
                if (result_str.compare(val_start, 4, "null") == 0) {
                    return make_err(fmt::format(
                        "return dict key '{}' is null (return_keys_non_null requires non-null values)", key));
                }
            }
            if (contract.return_keys_non_empty) {
                if (result_str.compare(val_start, 4, "null") == 0 ||
                    result_str.compare(val_start, 2, "[]") == 0 ||
                    result_str.compare(val_start, 2, "{}") == 0 ||
                    result_str.compare(val_start, 2, "\"\"") == 0) {
                    return make_err(fmt::format(
                        "return dict key '{}' is empty (return_keys_non_empty requires substantive values)", key));
                }
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

// --- Input Contract Validation (v4) ---

std::string GovernanceEngine::checkFunctionInputContract(
    const std::string& func_name,
    const std::vector<std::string>& arg_types,
    int line) {

    if (!rules_.contracts.validate_inputs) return "";

    auto it = rules_.contracts.functions.find(func_name);
    if (it == rules_.contracts.functions.end()) return "";

    const auto& contract = it->second;
    if (contract.params.empty()) return "";

    // Check argument count
    if (arg_types.size() != contract.params.size()) {
        std::string msg = "Contract violation [" + func_name + "]: expected " +
                          std::to_string(contract.params.size()) + " arguments, got " +
                          std::to_string(arg_types.size());
        if (line > 0) msg += " (line " + std::to_string(line) + ")";

        EnforcementLevel lvl = (contract.level != EnforcementLevel::NONE)
            ? contract.level : rules_.contracts.level;
        if (lvl == EnforcementLevel::HARD || (lvl == EnforcementLevel::SOFT && !override_enabled_)) {
            return msg;
        }
        fmt::print(stderr, "[GOVERNANCE] WARNING: {}\n", msg);
        return "";
    }

    // Check each argument type
    for (size_t i = 0; i < contract.params.size(); ++i) {
        const auto& param_spec = contract.params[i];
        // Parse "name:type" format
        auto colon_pos = param_spec.find(':');
        if (colon_pos == std::string::npos) continue;  // No type constraint

        std::string param_name = param_spec.substr(0, colon_pos);
        std::string expected_type = param_spec.substr(colon_pos + 1);

        if (expected_type == "any") continue;  // Accept anything

        if (i < arg_types.size() && arg_types[i] != expected_type) {
            std::string msg = "Contract violation [" + func_name + "]: parameter '" +
                              param_name + "' expected type '" + expected_type +
                              "', got '" + arg_types[i] + "'";
            if (line > 0) msg += " (line " + std::to_string(line) + ")";

            EnforcementLevel lvl = (contract.level != EnforcementLevel::NONE)
                ? contract.level : rules_.contracts.level;
            if (lvl == EnforcementLevel::HARD || (lvl == EnforcementLevel::SOFT && !override_enabled_)) {
                return msg;
            }
            fmt::print(stderr, "[GOVERNANCE] WARNING: {}\n", msg);
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
    int line,
    const std::string& source_file) {

    setCheckContext(source_file, line);

    auto& os_cfg = rules_.code_quality.no_oversimplification;
    auto& il_cfg = rules_.code_quality.no_incomplete_logic;
    auto& ph_cfg = rules_.code_quality.no_placeholders;

    // Skip if all checks disabled (but still allow complexity_floor, secrets, PII, and plugins if enabled)
    auto& cf_cfg_early = rules_.code_quality.complexity_floor;
    bool has_plugins = !rules_.governance_plugins.empty();
    bool has_secrets = rules_.code_quality.no_secrets.enabled;
    bool has_pii = rules_.code_quality.no_pii.enabled;
    if (!os_cfg.enabled && !il_cfg.enabled && !ph_cfg.enabled && !cf_cfg_early.enabled && !has_plugins && !has_secrets && !has_pii) return "";

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
        err = checkIncompleteLogic(stripped, line, source_file);
        if (!err.empty()) return err;
    }

    // Secrets/PII checks — use RAW source (secrets CAN be in string literals)
    if (rules_.code_quality.no_secrets.enabled) {
        err = checkSecrets(source_code, line);
        if (!err.empty()) return err;
    }
    if (rules_.code_quality.no_pii.enabled) {
        err = checkPii(source_code, line);
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

    // FIX-DX-6: Detect duplicate function calls — deferred to grouped output
    {
        auto& dc_cfg = rules_.code_quality.duplicate_calls;
        if (dc_cfg.enabled) {
            std::unordered_map<std::string, int> call_counts;
            size_t ci = 0;
            while (ci < stripped.size()) {
                if (std::isalpha(static_cast<unsigned char>(stripped[ci])) || stripped[ci] == '_') {
                    std::string word1;
                    while (ci < stripped.size() &&
                           (std::isalnum(static_cast<unsigned char>(stripped[ci])) || stripped[ci] == '_'))
                        word1 += stripped[ci++];
                    if (ci < stripped.size() && stripped[ci] == '.') {
                        ci++;
                        std::string word2;
                        while (ci < stripped.size() &&
                               (std::isalnum(static_cast<unsigned char>(stripped[ci])) || stripped[ci] == '_'))
                            word2 += stripped[ci++];
                        if (ci < stripped.size() && stripped[ci] == '(') {
                            call_counts[word1 + "." + word2 + "("]++;
                        }
                    }
                } else {
                    ci++;
                }
            }
            for (const auto& [call, count] : call_counts) {
                if (count >= dc_cfg.threshold) {
                    dup_call_summary_[call].push_back({function_name, count, line});
                }
            }
        }
    }

    // FIX-DX-7: Polyglot blocks without try/catch — deferred to grouped output
    // Keep il_cfg.enabled check for backward compatibility
    {
        auto& ptc_cfg = rules_.code_quality.polyglot_try_catch;
        if (il_cfg.enabled && ptc_cfg.enabled) {
            bool has_polyglot_block = source_code.find("<<") != std::string::npos;
            bool has_try_block = source_code.find("try") != std::string::npos;
            if (has_polyglot_block && !has_try_block) {
                ptc_functions_.push_back({function_name, line});
            }
        }
    }

    // Plugin rules (NAAb-based governance checks)
    err = checkPluginRules("naab_function", {
        {"function_name", interpreter::NaabVal::makeString(function_name)},
        {"source_code", interpreter::NaabVal::makeString(source_code)},
        {"source_file", interpreter::NaabVal::makeString(source_file)},
        {"line", interpreter::NaabVal::makeInt(line)},
    }, line);
    if (!err.empty()) return err;

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
    // FIX-DX-11: Additional cross-language patterns
    {"\\.forEach\\(", ".forEach() is JavaScript — in Go, use for _, v := range slice"},
    {"\\.map\\(", ".map() is JavaScript — in Go, use a for loop with append"},
    {"\\.filter\\(", ".filter() is JavaScript — in Go, use a for loop with condition"},
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

// Nim patterns: Python/JS idioms in Nim (FIX-DX-11: expanded)
static const std::vector<std::pair<std::string, std::string>> NIM_HALLUCINATION_PATTERNS = {
    {"\\bconsole\\.log\\(", "console.log() is JavaScript — in Nim, use echo"},
    {"\\bnull\\b", "null is JavaScript — in Nim, use nil"},
    {"\\bNone\\b", "None is Python — in Nim, use nil"},
    {"\\bTrue\\b", "True is Python — in Nim, use true (lowercase)"},
    {"\\bFalse\\b", "False is Python — in Nim, use false (lowercase)"},
    {"\\bdef\\s+\\w+", "def is Python — in Nim, use proc or func"},
    {"\\blen\\(", "len() is also valid in Nim — make sure to use it without import"},
    {"\\.upper\\(", ".upper() is Python — in Nim, use .toUpperAscii()"},
    {"\\.lower\\(", ".lower() is Python — in Nim, use .toLowerAscii()"},
    {"\\.append\\(", ".append() is Python — in Nim, use .add()"},
    {"\\.strip\\(\\)", ".strip() is Python — in Nim, use strip() from strutils (import strutils)"},
    {"\\bprint\\(", "print() is Python — in Nim, use echo"},
};

// Cross-language confusion patterns
static const std::vector<std::pair<std::string, std::string>> CROSS_LANG_PATTERNS = {
    {"#\\s+\\w", "# comments are Python/Ruby — in JavaScript, use //"},
    {"//\\s+\\w", "// comments are JS/C++ — in Python, use #"},
};

// Strip comments from code based on language syntax.
// Replaces comment content with spaces (preserving line structure for regex).
// Must be called AFTER string stripping to avoid matching # or // inside strings.
static std::string stripComments(const std::string& code, const std::string& language) {
    bool uses_hash = (language == "python" || language == "ruby" || language == "rb" ||
                      language == "shell" || language == "bash" || language == "sh" ||
                      language == "nim");
    bool uses_slashslash = (language == "javascript" || language == "js" || language == "node" ||
                            language == "go" || language == "golang" ||
                            language == "cpp" || language == "c++" ||
                            language == "rust" || language == "csharp" || language == "cs");
    bool uses_block = (language == "javascript" || language == "js" || language == "node" ||
                       language == "go" || language == "golang" ||
                       language == "cpp" || language == "c++" ||
                       language == "rust" || language == "csharp" || language == "cs");
    // V-GOV-002: add -- line comment style for SQL, Lua, and similar languages.
    // Without this, `-- DROP TABLE users` in a <<sql block is not stripped and the
    // governance scanner sees "DROP TABLE users" as active code (false positive or bypass).
    bool uses_dash_dash = (language == "sql" || language == "lua" ||
                           language == "haskell" || language == "ada");

    std::string result;
    result.reserve(code.size());

    for (size_t i = 0; i < code.size(); ++i) {
        // Check for block comments /* ... */
        if (uses_block && i + 1 < code.size() && code[i] == '/' && code[i + 1] == '*') {
            // Replace with spaces until closing */
            result += ' ';
            result += ' ';
            i += 2;
            while (i < code.size()) {
                if (i + 1 < code.size() && code[i] == '*' && code[i + 1] == '/') {
                    result += ' ';
                    result += ' ';
                    i += 1; // outer loop does +1
                    break;
                }
                result += (code[i] == '\n') ? '\n' : ' ';
                ++i;
            }
            continue;
        }

        // Check for // line comments
        if (uses_slashslash && i + 1 < code.size() && code[i] == '/' && code[i + 1] == '/') {
            // Replace rest of line with spaces
            while (i < code.size() && code[i] != '\n') {
                result += ' ';
                ++i;
            }
            if (i < code.size()) result += '\n'; // preserve the newline
            continue;
        }

        // Check for # line comments
        if (uses_hash && code[i] == '#') {
            // Shell special case: #! (shebang) at very start is still a comment — strip it
            // Python: # at line start or after whitespace/code is always a comment
            // (strings already stripped, so no risk of matching # inside strings)
            while (i < code.size() && code[i] != '\n') {
                result += ' ';
                ++i;
            }
            if (i < code.size()) result += '\n';
            continue;
        }

        // V-GOV-002: -- line comment (SQL, Lua, Haskell, Ada)
        if (uses_dash_dash && i + 1 < code.size() && code[i] == '-' && code[i+1] == '-') {
            while (i < code.size() && code[i] != '\n') {
                result += ' ';
                ++i;
            }
            if (i < code.size()) result += '\n';
            continue;
        }

        result += code[i];
    }
    return result;
}

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

    // Strip comments for the target language (after string stripping).
    // This prevents false positives like "# TODO: use append" in Shell
    // triggering Python hallucination patterns, or "// use len()" in Python
    // triggering JS hallucination patterns.
    std::string code_clean = stripComments(code_no_strings, language);

    // Check language-specific patterns (on code with strings AND comments stripped)
    if (lang_patterns) {
        for (const auto& [pattern, suggestion] : *lang_patterns) {
            try {
                auto flags = cfg.case_sensitive ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase);
                std::regex re(pattern, flags);
                std::smatch match;
                if (std::regex_search(code_clean, match, re)) {
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

    // Check cross-language confusion patterns (on string-stripped code,
    // but NOT comment-stripped — we WANT to find wrong-language comments)
    if (cfg.check_cross_language) {
        // Only check relevant cross-language patterns
        if (language == "python") {
            // Check for JS comment style in Python (// not stripped by Python comment rules)
            try {
                std::regex re("(^|\\n)\\s*//\\s+");
                std::smatch match;
                if (std::regex_search(code_clean, match, re)) {
                    return enforce("code_quality.no_hallucinated_apis", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Cross-language confusion in {} block: \"{}\"", language, match[0].str()),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.no_hallucinated_apis",
                            "// comments are JavaScript — in Python, use #", "", ""));
                }
            } catch (const std::regex_error&) {}
        } else if (language == "javascript" || language == "js") {
            // Check for Python comment style in JS (# not stripped by JS comment rules)
            try {
                std::regex re("(^|\\n)\\s*#\\s+");
                std::smatch match;
                if (std::regex_search(code_clean, match, re)) {
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

    // Check custom patterns (on cleaned code)
    if (!cfg.custom_patterns.empty()) {
        std::string found = searchPatterns(code_clean, cfg.custom_patterns);
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

// ==========================================================================
// Semantic Checks — targeted validation for Python, JavaScript, Shell
// ==========================================================================

// Known Python stdlib modules + common third-party
static const std::unordered_set<std::string> PYTHON_KNOWN_MODULES = {
    // Python 3.x stdlib
    "abc", "aifc", "argparse", "array", "ast", "asynchat", "asyncio", "asyncore",
    "atexit", "audioop", "base64", "bdb", "binascii", "binhex", "bisect",
    "builtins", "bz2", "calendar", "cgi", "cgitb", "chunk", "cmath", "cmd",
    "code", "codecs", "codeop", "collections", "colorsys", "compileall",
    "concurrent", "configparser", "contextlib", "contextvars", "copy", "copyreg",
    "cProfile", "crypt", "csv", "ctypes", "curses", "dataclasses", "datetime",
    "dbm", "decimal", "difflib", "dis", "distutils", "doctest", "email",
    "encodings", "enum", "errno", "faulthandler", "fcntl", "filecmp", "fileinput",
    "fnmatch", "formatter", "fractions", "ftplib", "functools", "gc", "getopt",
    "getpass", "gettext", "glob", "grp", "gzip", "hashlib", "heapq", "hmac",
    "html", "http", "idlelib", "imaplib", "imghdr", "imp", "importlib",
    "inspect", "io", "ipaddress", "itertools", "json", "keyword", "lib2to3",
    "linecache", "locale", "logging", "lzma", "mailbox", "mailcap", "marshal",
    "math", "mimetypes", "mmap", "modulefinder", "multiprocessing", "netrc",
    "nis", "nntplib", "numbers", "operator", "optparse", "os", "ossaudiodev",
    "parser", "pathlib", "pdb", "pickle", "pickletools", "pipes", "pkgutil",
    "platform", "plistlib", "poplib", "posix", "posixpath", "pprint",
    "profile", "pstats", "pty", "pwd", "py_compile", "pyclbr", "pydoc",
    "queue", "quopri", "random", "re", "readline", "reprlib", "resource",
    "rlcompleter", "runpy", "sched", "secrets", "select", "selectors",
    "shelve", "shlex", "shutil", "signal", "site", "smtpd", "smtplib",
    "sndhdr", "socket", "socketserver", "sqlite3", "ssl", "stat", "statistics",
    "string", "stringprep", "struct", "subprocess", "sunau", "symtable",
    "sys", "sysconfig", "syslog", "tabnanny", "tarfile", "telnetlib", "tempfile",
    "termios", "test", "textwrap", "threading", "time", "timeit", "tkinter",
    "token", "tokenize", "tomllib", "trace", "traceback", "tracemalloc",
    "tty", "turtle", "turtledemo", "types", "typing", "unicodedata",
    "unittest", "urllib", "uu", "uuid", "venv", "warnings", "wave",
    "weakref", "webbrowser", "winreg", "winsound", "wsgiref", "xdrlib",
    "xml", "xmlrpc", "zipapp", "zipfile", "zipimport", "zlib",
    // Internal/private modules that are valid
    "_thread", "_io", "_collections", "_functools", "_operator",
    // Common third-party
    "numpy", "np", "pandas", "pd", "requests", "flask", "django",
    "scipy", "matplotlib", "plt", "sklearn", "tensorflow", "tf",
    "torch", "cv2", "PIL", "pillow", "sqlalchemy", "celery",
    "pytest", "setuptools", "pip", "yaml", "pyyaml", "toml",
    "aiohttp", "httpx", "fastapi", "uvicorn", "pydantic",
    "beautifulsoup4", "bs4", "lxml", "scrapy", "selenium",
    "boto3", "botocore", "paramiko", "fabric", "click",
    "rich", "tqdm", "colorama", "pygments", "jinja2",
    "cryptography", "bcrypt", "jwt", "dotenv", "six", "attrs",
    "dateutil", "pytz", "tzdata", "chardet", "certifi",
    "urllib3", "idna", "packaging", "appdirs", "distlib",
    "virtualenv", "wheel", "twine", "black", "flake8",
    "mypy", "pylint", "isort", "autopep8", "bandit",
    "gunicorn", "gevent", "twisted", "tornado",
    "redis", "pymongo", "psycopg2", "mysql",
    "networkx", "sympy", "nltk", "spacy",
};

// Known Node.js builtins + common third-party
static const std::unordered_set<std::string> JS_KNOWN_MODULES = {
    // Node.js builtins
    "assert", "buffer", "child_process", "cluster", "console", "constants",
    "crypto", "dgram", "dns", "domain", "events", "fs", "http", "http2",
    "https", "inspector", "module", "net", "os", "path", "perf_hooks",
    "process", "punycode", "querystring", "readline", "repl", "stream",
    "string_decoder", "sys", "timers", "tls", "trace_events", "tty",
    "url", "util", "v8", "vm", "wasi", "worker_threads", "zlib",
    // Common third-party
    "express", "lodash", "underscore", "axios", "moment", "dayjs",
    "react", "vue", "angular", "svelte", "next", "nuxt",
    "webpack", "babel", "typescript", "esbuild", "vite", "rollup",
    "jest", "mocha", "chai", "sinon", "supertest", "vitest",
    "mongoose", "sequelize", "prisma", "knex", "typeorm",
    "dotenv", "cors", "helmet", "morgan", "winston", "pino",
    "ws", "redis", "ioredis", "pg", "mysql2",
    "uuid", "chalk", "commander", "yargs", "inquirer",
    "cheerio", "puppeteer", "sharp", "multer",
    "jsonwebtoken", "bcrypt", "bcryptjs", "passport",
};

// Known shell commands
static const std::unordered_set<std::string> SHELL_KNOWN_COMMANDS = {
    // Core POSIX/GNU utilities
    "echo", "printf", "cat", "head", "tail", "wc", "sort", "uniq",
    "grep", "egrep", "fgrep", "sed", "awk", "cut", "tr", "paste",
    "ls", "cp", "mv", "rm", "mkdir", "rmdir", "touch", "stat",
    "chmod", "chown", "chgrp", "ln", "find", "xargs", "tee",
    "cd", "pwd", "pushd", "popd", "export", "source", "eval",
    "read", "test", "expr", "true", "false", "yes", "sleep",
    "date", "cal", "who", "whoami", "id", "groups", "hostname",
    "uname", "uptime", "df", "du", "free", "top", "ps", "kill",
    "bg", "fg", "jobs", "wait", "nohup", "nice", "time", "timeout",
    "tar", "gzip", "gunzip", "bzip2", "zip", "unzip", "xz",
    "curl", "wget", "ssh", "scp", "rsync", "nc", "ping", "dig", "nslookup",
    "git", "docker", "make", "cmake", "python", "python3", "node", "npm", "npx",
    "pip", "pip3", "ruby", "perl", "java", "javac", "go", "rustc", "cargo",
    "jq", "yq", "xmllint", "base64", "md5sum", "sha256sum", "shasum",
    "diff", "patch", "comm", "join", "bc", "dc",
    "env", "set", "unset", "trap", "shift", "getopts", "local", "return",
    "if", "then", "else", "elif", "fi", "for", "do", "done", "while",
    "until", "case", "esac", "select", "in", "function",
    "break", "continue", "exit",
    // Common tools
    "apt", "apt-get", "brew", "yum", "dnf", "pacman", "pkg",
    "systemctl", "service", "journalctl",
    "crontab", "at", "screen", "tmux",
    "vim", "vi", "nano", "less", "more", "man",
    "which", "whereis", "type", "file", "realpath", "dirname", "basename",
};

// Shell command typos → suggestions
static const std::unordered_map<std::string, std::string> SHELL_COMMAND_TYPOS = {
    {"grpe", "grep"}, {"grrep", "grep"}, {"greep", "grep"},
    {"awke", "awk"}, {"sde", "sed"}, {"mkdri", "mkdir"},
    {"chmd", "chmod"}, {"cta", "cat"}, {"ehco", "echo"},
    {"pritnf", "printf"}, {"pyhton", "python"}, {"pytohn", "python"},
    {"touhc", "touch"}, {"rn", "rm"}, {"mdkir", "mkdir"},
    {"whcih", "which"}, {"wegt", "wget"}, {"curlr", "curl"},
    {"ecoh", "echo"}, {"caht", "cat"}, {"grp", "grep"},
};

// API signature check structure
struct SemanticCheck {
    std::string pattern;
    std::string message;
    std::string suggestion;
};

// Python API misuse patterns
static const std::vector<SemanticCheck> PYTHON_API_CHECKS = {
    {"range\\([^)]*\\d+\\.\\d+", "range() only accepts integers, not floats", "Use int(): range(int(n)) or use numpy.arange() for float ranges"},
    {"re\\.sub\\(\\s*[^,)]+\\s*\\)", "re.sub() needs 3 args: pattern, replacement, string", "re.sub(r'pattern', 'replacement', text)"},
    {"sorted\\([^)]*,\\s*reverse\\s*[^=\\s)]", "sorted() reverse parameter needs =True or =False", "sorted(lst, reverse=True)"},
    {"=\\s*\\w+\\.sort\\(\\s*\\)", ".sort() returns None — it sorts in-place, don't assign result", "lst.sort()  # modifies lst in-place; or use sorted(lst)"},
    {"=\\s*\\w+\\.update\\(", ".update() returns None — it modifies dict in-place, don't assign result", "d.update(other)  # modifies d; or use {**d, **other}"},
    {"\\.split\\(\\s*\\d+\\s*\\)", "str.split() takes a string delimiter, not an integer", "s.split(',') or s.split() for whitespace"},
    {"int\\(\\s*\\d+\\s*,\\s*\\d+", "int() with base requires string first arg, not int literal", "int('ff', 16) or int('101', 2)"},
};

// Python API checks that need RAW code (patterns involve string literals)
static const std::vector<SemanticCheck> PYTHON_API_RAW_CHECKS = {
    {"json\\.dumps\\(\\s*\\)", "json.dumps() requires at least 1 argument", "json.dumps(data)"},
    {"json\\.loads\\(\\s*\\)", "json.loads() requires at least 1 argument", "json.loads(string)"},
    {"open\\([^)]*['\"](?:rw|wr)['\"]", "Invalid file mode — 'rw'/'wr' don't exist", "Use 'r+' for read/write or 'w+' for write/read: open(f, 'r+')"},
};

// Python dangerous eval/exec patterns — RAW code (f-string/quote patterns)
static const std::vector<SemanticCheck> PYTHON_EVAL_RAW_CHECKS = {
    {"eval\\(\\s*f['\"]", "eval() with f-string is dangerous — user input can execute arbitrary code", "Use ast.literal_eval() for safe parsing of literals"},
    {"exec\\(\\s*f['\"]", "exec() with f-string is dangerous — arbitrary code execution", "Avoid exec() with dynamic strings; use safer alternatives"},
};

// Python dangerous eval/exec patterns — CLEANED code (string content stripped)
static const std::vector<SemanticCheck> PYTHON_EVAL_CHECKS = {
    {"eval\\([^)]*\\+\\s*\\w", "eval() with string concatenation is dangerous — injection risk", "Use ast.literal_eval() or json.loads() for safe parsing"},
    {"__import__\\(", "__import__() dynamic import is hard to audit and potentially dangerous", "Use regular import statements for clarity and safety"},
};

// JavaScript API misuse patterns
static const std::vector<SemanticCheck> JS_API_CHECKS = {
    {"Array\\.isArray\\(\\s*\\)", "Array.isArray() requires 1 argument", "Array.isArray(value)"},
    {"new\\s+Promise\\(\\s*\\)", "new Promise() requires an executor function", "new Promise((resolve, reject) => { ... })"},
};

// JS API checks needing RAW code (patterns involve string/backtick delimiters)
static const std::vector<SemanticCheck> JS_API_RAW_CHECKS = {
    {"JSON\\.parse\\(\\s*\\)", "JSON.parse() requires at least 1 argument", "JSON.parse(jsonString)"},
    {"JSON\\.stringify\\(\\s*\\)", "JSON.stringify() requires at least 1 argument", "JSON.stringify(data)"},
    {"set(?:Timeout|Interval)\\(\\s*['\"]", "setTimeout/setInterval with string arg is like eval() — security risk", "Use a function: setTimeout(() => { ... }, ms)"},
};

// JavaScript dangerous patterns (need RAW code — patterns include backtick/quote chars)
static const std::vector<SemanticCheck> JS_EVAL_CHECKS = {
    {"eval\\(\\s*`", "eval() with template literal is dangerous — arbitrary code execution", "Use JSON.parse() for data or a safe parser"},
    {"\\.innerHTML\\s*=\\s*[^'\"`\\s;]", "Setting innerHTML with a variable risks XSS attacks", "Use .textContent for plain text, or sanitize with DOMPurify"},
    {"document\\.write\\(", "document.write() is dangerous and can overwrite the entire page", "Use DOM methods: .appendChild(), .textContent, .innerHTML with sanitized input"},
};

// Shell syntax validation patterns
static const std::vector<SemanticCheck> SHELL_SYNTAX_CHECKS = {
    {"\\[(?!\\[)[^ \\t\\n]", "Missing space after '[' in test command", "Use spaces: [ -f file ] not [-f file]"},
    {"[^ \\t\\n]\\](?!\\])", "Missing space before ']' in test command", "Use spaces: [ -f file ] not [ -f file]"},
    {"\\[\\s+[^\\]]*\\s+==\\s+", "'==' in [ ] is not POSIX — may fail on some shells", "Use '=' in [ ] or use [[ ]] for '==': [ \"$x\" = \"$y\" ]"},
    {"\\]\\s+then\\b", "Missing ';' or newline between ']' and 'then'", "if [ condition ]; then  # add semicolon"},
};

// Shell safety patterns
static const std::vector<SemanticCheck> SHELL_SAFETY_CHECKS = {
    {"rm\\s+-[rR]f\\s+/\\s", "rm -rf / is extremely dangerous — will destroy the filesystem", "Never use rm -rf / — specify exact paths"},
    {"rm\\s+-[rR]f\\s+/\\n", "rm -rf / is extremely dangerous — will destroy the filesystem", "Never use rm -rf / — specify exact paths"},
    {"rm\\s+-[rR]f\\s+\"?\\$\\{?\\w*\\}?\"?/", "rm -rf with variable — if variable is empty, becomes rm -rf /", "Guard: [ -n \"$VAR\" ] && rm -rf \"$VAR/path\""},
};

// Unquoted variable check for shell (separate because more complex)
static const std::vector<SemanticCheck> SHELL_UNQUOTED_VAR_CHECKS = {
    {"(?:rm|mv|cp)\\s+(?:-[a-zA-Z]+\\s+)*[^\"']*\\$[a-zA-Z_]", "Unquoted variable in rm/mv/cp — empty variable causes unexpected behavior", "Always quote variables: rm \"$file\" not rm $file"},
};

std::string GovernanceEngine::checkSemanticIssues(
    const std::string& language, const std::string& code, int line) {

    auto& cfg = rules_.code_quality.semantic_checks;
    if (!cfg.enabled) return "";

    // Strip strings and comments for pattern matching
    std::string code_no_strings = stripStringLiterals(code);
    std::string clean = stripComments(code_no_strings, language);

    // Helper: run a vector of SemanticCheck patterns against a target string
    auto runChecksOn = [&](const std::vector<SemanticCheck>& checks,
                           const std::string& target) -> std::string {
        for (const auto& check : checks) {
            try {
                std::regex re(check.pattern, std::regex::ECMAScript);
                std::smatch match;
                if (std::regex_search(target, match, re)) {
                    return enforce("code_quality.semantic_checks", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Semantic issue in {} block: {}", language, check.message),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.semantic_checks",
                            check.suggestion, "", ""));
                }
            } catch (const std::regex_error&) {}
        }
        return "";
    };
    // Convenience: run against cleaned (string+comment stripped) code
    auto runChecks = [&](const std::vector<SemanticCheck>& checks) -> std::string {
        return runChecksOn(checks, clean);
    };
    // Run against raw code (for patterns involving string literals like open("rw"), eval(f"..."))
    auto runChecksRaw = [&](const std::vector<SemanticCheck>& checks) -> std::string {
        return runChecksOn(checks, code);
    };

    // === PYTHON ===
    if (language == "python") {
        // 1. Import validation
        if (cfg.check_imports) {
            try {
                std::regex import_re("(?:^|\\n)\\s*(?:import\\s+(\\w+)|from\\s+(\\w+)\\s+import)",
                                     std::regex::ECMAScript);
                auto begin = std::sregex_iterator(clean.begin(), clean.end(), import_re);
                auto end_it = std::sregex_iterator();
                for (auto it = begin; it != end_it; ++it) {
                    std::string mod = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
                    if (PYTHON_KNOWN_MODULES.find(mod) == PYTHON_KNOWN_MODULES.end()) {
                        return enforce("code_quality.semantic_checks", cfg.level,
                            formatError(cfg.level,
                                fmt::format("Unknown Python module '{}' — verify it's installed", mod),
                                line > 0 ? fmt::format("line {}", line) : "",
                                "code_quality.semantic_checks",
                                "This module is not in Python's stdlib or common packages.\n"
                                "  If it's a project dependency, ensure it's in requirements.txt.", "", ""));
                    }
                }
            } catch (const std::regex_error&) {}
        }

        // 2. API signature checks (cleaned code for most, raw for string-literal patterns)
        if (cfg.check_api_signatures) {
            std::string err = runChecks(PYTHON_API_CHECKS);
            if (!err.empty()) return err;
            // open() mode check needs raw code (mode is a string literal)
            err = runChecksRaw(PYTHON_API_RAW_CHECKS);
            if (!err.empty()) return err;
        }

        // 3. Dangerous eval/exec
        if (cfg.check_dangerous_eval) {
            // f-string patterns need raw code (quote chars stripped otherwise)
            std::string err = runChecksRaw(PYTHON_EVAL_RAW_CHECKS);
            if (!err.empty()) return err;
            // Concatenation/__import__ patterns use cleaned code
            err = runChecks(PYTHON_EVAL_CHECKS);
            if (!err.empty()) return err;
        }
    }

    // === JAVASCRIPT ===
    else if (language == "javascript" || language == "js" || language == "node") {
        // 1. Import/require validation (RAW code — string literals contain module names)
        if (cfg.check_imports) {
            try {
                // Check require() calls on RAW code (module name is in string)
                std::regex require_re("require\\s*\\(\\s*['\"]([^'\"]+)['\"]\\s*\\)",
                                      std::regex::ECMAScript);
                auto begin = std::sregex_iterator(code.begin(), code.end(), require_re);
                auto end_it = std::sregex_iterator();
                for (auto it = begin; it != end_it; ++it) {
                    std::string mod = (*it)[1].str();
                    // Strip node: prefix
                    if (mod.size() > 5 && mod.substr(0, 5) == "node:") mod = mod.substr(5);
                    // Skip relative imports
                    if (!mod.empty() && (mod[0] == '.' || mod[0] == '/')) continue;
                    // Strip subpath: fs/promises → fs
                    auto slash = mod.find('/');
                    if (slash != std::string::npos && (mod.empty() || mod[0] != '@'))
                        mod = mod.substr(0, slash);
                    if (JS_KNOWN_MODULES.find(mod) == JS_KNOWN_MODULES.end()) {
                        return enforce("code_quality.semantic_checks", cfg.level,
                            formatError(cfg.level,
                                fmt::format("Unknown Node.js module '{}' — verify it's installed", mod),
                                line > 0 ? fmt::format("line {}", line) : "",
                                "code_quality.semantic_checks",
                                "This module is not a Node.js builtin or common package.\n"
                                "  If it's a project dependency, ensure it's in package.json.", "", ""));
                    }
                }

                // Check ES import ... from '...' statements on RAW code
                std::regex import_re("from\\s+['\"]([^'\"]+)['\"]",
                                     std::regex::ECMAScript);
                begin = std::sregex_iterator(code.begin(), code.end(), import_re);
                for (auto it = begin; it != end_it; ++it) {
                    std::string mod = (*it)[1].str();
                    if (mod.size() > 5 && mod.substr(0, 5) == "node:") mod = mod.substr(5);
                    if (!mod.empty() && (mod[0] == '.' || mod[0] == '/')) continue;
                    auto slash = mod.find('/');
                    if (slash != std::string::npos && (mod.empty() || mod[0] != '@'))
                        mod = mod.substr(0, slash);
                    if (JS_KNOWN_MODULES.find(mod) == JS_KNOWN_MODULES.end()) {
                        return enforce("code_quality.semantic_checks", cfg.level,
                            formatError(cfg.level,
                                fmt::format("Unknown Node.js module '{}' — verify it's installed", mod),
                                line > 0 ? fmt::format("line {}", line) : "",
                                "code_quality.semantic_checks",
                                "This module is not a Node.js builtin or common package.\n"
                                "  If it's a project dependency, ensure it's in package.json.", "", ""));
                    }
                }
            } catch (const std::regex_error&) {}
        }

        // 2. API signature checks (cleaned + raw for string-literal patterns)
        if (cfg.check_api_signatures) {
            std::string err = runChecks(JS_API_CHECKS);
            if (!err.empty()) return err;
            err = runChecksRaw(JS_API_RAW_CHECKS);
            if (!err.empty()) return err;
        }

        // 3. Dangerous patterns (raw code — patterns include backtick/quote chars)
        if (cfg.check_dangerous_eval) {
            std::string err = runChecksRaw(JS_EVAL_CHECKS);
            if (!err.empty()) return err;
        }
    }

    // === SHELL ===
    else if (language == "shell" || language == "bash" || language == "sh") {
        // 1. Syntax validation
        if (cfg.check_shell_syntax) {
            std::string err = runChecks(SHELL_SYNTAX_CHECKS);
            if (!err.empty()) return err;
        }

        // 2. Command typo detection
        if (cfg.check_imports) {
            // Extract first word of each line in the cleaned code
            std::istringstream stream(clean);
            std::string cmd_line;
            while (std::getline(stream, cmd_line)) {
                // ltrim
                size_t start = cmd_line.find_first_not_of(" \t");
                if (start == std::string::npos) continue;
                std::string trimmed = cmd_line.substr(start);
                if (trimmed.empty() || trimmed[0] == '#') continue;
                // Skip variable assignments (VAR=value), pipes, subshells
                size_t eq_pos = trimmed.find('=');
                size_t sp_pos = trimmed.find_first_of(" \t");
                if (eq_pos != std::string::npos && (sp_pos == std::string::npos || eq_pos < sp_pos))
                    continue;
                // Skip lines starting with control chars
                if (trimmed[0] == '|' || trimmed[0] == '&' || trimmed[0] == ';' ||
                    trimmed[0] == '(' || trimmed[0] == ')' || trimmed[0] == '{' ||
                    trimmed[0] == '}' || trimmed[0] == '!' || trimmed[0] == '$')
                    continue;
                // Extract first word
                size_t word_end = trimmed.find_first_of(" \t;|&<>()");
                std::string cmd = (word_end != std::string::npos)
                    ? trimmed.substr(0, word_end) : trimmed;
                if (cmd.empty()) continue;
                // Skip paths (/usr/bin/foo, ./script)
                if (cmd.find('/') != std::string::npos || cmd.find('.') == 0) continue;

                // Check known typos first
                auto typo_it = SHELL_COMMAND_TYPOS.find(cmd);
                if (typo_it != SHELL_COMMAND_TYPOS.end()) {
                    return enforce("code_quality.semantic_checks", cfg.level,
                        formatError(cfg.level,
                            fmt::format("Unknown command '{}' — did you mean '{}'?",
                                cmd, typo_it->second),
                            line > 0 ? fmt::format("line {}", line) : "",
                            "code_quality.semantic_checks",
                            fmt::format("Replace '{}' with '{}'", cmd, typo_it->second), "", ""));
                }
            }
        }

        // 3. Safety checks
        if (cfg.check_dangerous_eval) {
            std::string err = runChecks(SHELL_SAFETY_CHECKS);
            if (!err.empty()) return err;
            err = runChecks(SHELL_UNQUOTED_VAR_CHECKS);
            if (!err.empty()) return err;
        }
    }

    recordPass("code_quality.semantic_checks", cfg.level);
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

    // Check per-language config even if restrictions.imports is not enabled
    auto it = rules_.languages.per_language.find(language);
    bool has_per_lang = (it != rules_.languages.per_language.end() &&
                         (!it->second.imports.blocked.empty() || !it->second.banned_imports.empty()));
    if (!cfg.enabled && !has_per_lang) return "";

    // Build import patterns for this language
    std::vector<std::string> blocked;
    if (cfg.enabled) {
        if (cfg.blocked.count(language)) blocked = cfg.blocked.at(language);
        if (cfg.blocked.count("any")) {
            auto& any = cfg.blocked.at("any");
            blocked.insert(blocked.end(), any.begin(), any.end());
        }
    }

    // Also check per-language config
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
                auto level = cfg.enabled ? cfg.level : EnforcementLevel::HARD;
                // Suggest NAAb stdlib alternatives for commonly-blocked imports
                static const std::unordered_map<std::string, std::string> import_alternatives = {
                    {"os",         "NAAb alternatives: file.list_dir(), file.exists(), path.join(),\n"
                                   "  path.basename(), env.get(), process.run()"},
                    {"subprocess", "NAAb alternative: process.run(cmd, [args]) returns\n"
                                   "  {exit_code, stdout, stderr}"},
                    {"shutil",     "NAAb alternatives: file.copy(), file.move(), file.delete()"},
                    {"socket",     "NAAb has no raw socket API. Use the http module if available,\n"
                                   "  or process.run() to call curl/wget"},
                    {"ctypes",     "NAAb has no FFI. Use a polyglot block in the target language instead"},
                    {"pickle",     "NAAb alternative: json.stringify() / json.parse()"},
                    {"sys",        "NAAb alternatives: env.get_args() for CLI arguments,\n"
                                   "  process.exit(code) for sys.exit, env.get() for sys.platform"},
                };
                std::string help_text = fmt::format("The import \"{}\" is blocked by governance", imp);
                auto alt = import_alternatives.find(imp);
                if (alt != import_alternatives.end()) {
                    help_text += ".\n\n  " + alt->second;
                }
                return enforce("restrictions.imports", level,
                    formatError(level, fmt::format("Blocked import in {} block: \"{}\"", language, imp),
                        line > 0 ? fmt::format("line {}", line) : "",
                        cfg.enabled ? "restrictions.imports" : fmt::format("languages.per_language.{}.imports.blocked", language),
                        help_text, "", ""));
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
            // Escape regex metacharacters so entries like "curl |" are treated as literals
            std::string escaped;
            for (char c : func) {
                if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos)
                    escaped += '\\';
                escaped += c;
            }
            // For multi-word patterns, require word boundary or end-of-string after match
            // to prevent "rm -rf /" from matching "rm -rf /tmp/safe"
            if (func.find(' ') != std::string::npos) {
                escaped += "(?:\\s|$)";
            }
            std::regex re(escaped, std::regex::icase);
            if (std::regex_search(code, re)) {
                // Per-function alternatives so LLMs know what to use instead
                static const std::unordered_map<std::string, std::string> alternatives = {
                    {"eval(",     "Use a lookup table or if/else chain instead of dynamic evaluation"},
                    {"Function(", "Use a lookup table, switch/match, or define functions statically.\n"
                                  "  Example: instead of new Function('return ' + expr),\n"
                                  "  use a dict mapping: let ops = {\"add\": fn(a,b) { return a + b }}"},
                    {"exec(",     "Use file.read() + json.parse() for data, or import for code"},
                    {"compile(",  "Pre-define functions instead of compiling at runtime"},
                };
                auto alt_it = alternatives.find(func);
                std::string help = (alt_it != alternatives.end())
                    ? alt_it->second
                    : "This function is banned by governance policy";
                return enforce("languages.per_language.banned_functions", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        fmt::format("Banned function in {} block: \"{}\"", language, func),
                        line > 0 ? fmt::format("line {}", line) : "",
                        fmt::format("languages.per_language.{}.banned_functions", language),
                        help, "", ""));
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
    const std::string& source_file, int line) {

    // Set check context for report tracking (file + line propagate to enforce/recordPass)
    setCheckContext(source_file, line);

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
    err = checkSemanticIssues(lang, code, line);  // Has its own stripping
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
    err = checkFilesystemImports(lang, stripped, line);
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

    // Plugin rules (NAAb-based governance checks)
    err = checkPluginRules("polyglot_block", {
        {"code", interpreter::NaabVal::makeString(code)},
        {"language", interpreter::NaabVal::makeString(lang)},
        {"source_file", interpreter::NaabVal::makeString(current_check_file_)},
        {"line", interpreter::NaabVal::makeInt(line)},
    }, line);
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
        "contracts", "baselines", "project_context", "scanner",
        "governance_plugins", "governance",
        "taint_tracking", "quality_gate", "governance_baseline",
        "environments", "runtime_versions", "agent_roles", "agents", "telemetry",
        "runtime", "security", "api", "integrity", "project_name"
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

// --- Taint Tracking ---
void GovernanceEngine::markTainted(const std::string& var_name) {
    if (!rules_.taint_tracking.enabled) return;
    std::lock_guard<std::mutex> lock(taint_mutex_);
    taint_set_.insert(var_name);
}

void GovernanceEngine::clearTaint(const std::string& var_name) {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    taint_set_.erase(var_name);
}

bool GovernanceEngine::isTainted(const std::string& var_name) const {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    return taint_set_.count(var_name) > 0;
}

// BUG-O: Save/restore taint state for module loading isolation
std::unordered_set<std::string> GovernanceEngine::saveTaintState() const {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    return taint_set_;
}

void GovernanceEngine::restoreTaintState(const std::unordered_set<std::string>& state) {
    std::lock_guard<std::mutex> lock(taint_mutex_);
    taint_set_ = state;
}

// V-GOV-004: counter state save/restore for async task inheritance.
// Each async task creates a fresh Interpreter with zeroed counters; restoring
// the parent's counters ensures limits like polyglot_blocks are enforced
// across the full execution (parent + all async tasks combined).
GovernanceEngine::CounterState GovernanceEngine::saveCounterState() const {
    CounterState s;
    s.polyglot_block_count = polyglot_block_count_;
    s.total_polyglot_lines = total_polyglot_lines_;
    s.advisory_count       = advisory_count_;
    s.advisory_suppressed  = advisory_suppressed_;
    return s;
}

void GovernanceEngine::restoreCounterState(const CounterState& s) {
    polyglot_block_count_ = s.polyglot_block_count;
    total_polyglot_lines_ = s.total_polyglot_lines;
    advisory_count_       = s.advisory_count;
    advisory_suppressed_  = s.advisory_suppressed;
}

// BUG-D: Track function return taint
bool GovernanceEngine::lastReturnWasTainted() const {
    return last_return_tainted_;
}

void GovernanceEngine::setLastReturnTainted(bool v) {
    last_return_tainted_ = v;
}

bool GovernanceEngine::isTaintSource(const std::string& func_name) const {
    if (!rules_.taint_tracking.enabled) return false;
    for (const auto& src : rules_.taint_tracking.sources) {
        // FIX-DX-1: PREFIX match (not substring) to avoid false positives
        // e.g., "env.get" matches "env.get_var" but NOT "prevent.get"
        if (func_name.size() >= src.size() &&
            func_name.compare(0, src.size(), src) == 0) return true;
        // Also check after module dot: "json.env.get" → check "env.get" prefix
        auto dot = func_name.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            // Check each dot-separated suffix for prefix match
            size_t pos = 0;
            while ((pos = func_name.find('.', pos)) != std::string::npos) {
                std::string suffix = func_name.substr(pos + 1);
                if (suffix.size() >= src.size() &&
                    suffix.compare(0, src.size(), src) == 0) return true;
                pos++;
            }
        }
    }
    return false;
}

bool GovernanceEngine::isSanitizer(const std::string& func_name) const {
    if (!rules_.taint_tracking.enabled) return false;
    for (const auto& san : rules_.taint_tracking.sanitizers) {
        // FIX-DX-1: PREFIX match (not substring) to avoid false positives
        // e.g., "validate_" matches "validate_input" but NOT "revalidate_input"
        // e.g., "int(" matches "int(x)" but NOT "print(x)" or "hint(x)"
        if (func_name.size() >= san.size() &&
            func_name.compare(0, san.size(), san) == 0) return true;
        // Also check after module dot: "utils.validate_input" → check "validate_" prefix
        size_t pos = 0;
        while ((pos = func_name.find('.', pos)) != std::string::npos) {
            std::string suffix = func_name.substr(pos + 1);
            if (suffix.size() >= san.size() &&
                suffix.compare(0, san.size(), san) == 0) return true;
            pos++;
        }
    }
    return false;
}

std::string GovernanceEngine::checkTaintedSink(const std::string& var_name,
                                                const std::string& sink_type,
                                                const std::string& file, int line) {
    if (!rules_.taint_tracking.enabled) return "";
    {
        std::lock_guard<std::mutex> lock(taint_mutex_);
        if (taint_set_.count(var_name) == 0) return "";
    }

    // FIX-DX-1: PREFIX match for sink types (not substring)
    // Auto-expand: file.append is equivalent to file.write for taint purposes
    std::string effective_sink = sink_type;
    if (sink_type == "file.append") effective_sink = "file.write";
    bool is_sink = false;
    for (const auto& s : rules_.taint_tracking.sinks) {
        if (effective_sink.size() >= s.size() &&
            effective_sink.compare(0, s.size(), s) == 0) { is_sink = true; break; }
        // Also check the original sink_type for exact/prefix match
        if (sink_type.size() >= s.size() &&
            sink_type.compare(0, s.size(), s) == 0) { is_sink = true; break; }
        if (sink_type == s) { is_sink = true; break; }
    }
    if (!is_sink) return "";

    // Log the decision
    logTaintDecision(var_name, "BLOCKED", sink_type, file, line);

    // Pass 2: Record taint flow for post-execution audit
    taint_flows_.push_back({var_name, "", sink_type, "BLOCKED", file, line});

    // Use "variable 'x'" for real names, omit "variable" for synthetic labels like
    // "argument 0 of 'file.write()'" to produce cleaner error messages
    bool is_synthetic = var_name.find(' ') != std::string::npos;
    std::string msg = "Taint tracking violation: " +
                      std::string(is_synthetic ? "" : "variable ") + "'" + var_name +
                      "' contains untrusted data and reached sink '" + sink_type +
                      "' without sanitization";
    if (!file.empty()) msg += " at " + file + ":" + std::to_string(line);

    // Add sanitizer guidance (don't list specific sanitizers — adversarial LLMs
    // would use the list to create identity-function bypasses)
    msg += "\n\n  Help: Pass the value through a real sanitization function before\n"
           "  using it in a '" + sink_type + "' sink. Sanitizers must actually\n"
           "  validate or transform the data — identity functions are detected.\n";

    // Enforce based on level
    if (rules_.taint_tracking.level == "hard") {
        return msg;  // Caller will throw
    } else if (rules_.taint_tracking.level == "soft") {
        if (!override_enabled_) return msg;
        // With override, fall through to warning
    }
    // Advisory: just warn
    fmt::print(stderr, "[GOVERNANCE] WARNING: {}\n", msg);
    return "";
}

// FIX-DX-8: Validate scope patterns against actual function names
void GovernanceEngine::validateScopePatterns(const std::vector<std::string>& function_names) {
    for (const auto& scope : rules_.scopes) {
        if (scope.glob_pattern.empty()) continue;
        bool any_match = false;
        for (const auto& fn : function_names) {
            // Simple glob match: only supports * wildcard
            const std::string& pat = scope.glob_pattern;
            if (pat == "*") { any_match = true; break; }
            auto star = pat.find('*');
            if (star == std::string::npos) {
                // Exact match
                if (fn == pat) { any_match = true; break; }
            } else {
                // Prefix*suffix match
                std::string prefix = pat.substr(0, star);
                std::string suffix = pat.substr(star + 1);
                if (fn.size() >= prefix.size() + suffix.size() &&
                    fn.compare(0, prefix.size(), prefix) == 0 &&
                    (suffix.empty() || fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                    any_match = true;
                    break;
                }
            }
        }
        if (!any_match) {
            fmt::print(stderr, "[WARN] Governance scope (pattern '{}') "
                       "matched zero functions in this file.\n",
                       scope.glob_pattern);
        }
    }
}

// ============================================================================
// Pass 2: Determinism, Output Entropy, Error Dump Checks
// ============================================================================

std::string GovernanceEngine::checkDeterminism(const std::string& language,
                                                const std::string& code, int line) {
    static const std::vector<std::pair<std::string, std::string>> patterns = {
        // Random
        {R"(\brandom\b)", "random function"},
        {R"(\brand\s*\()", "rand() call"},
        {R"(Math\.random\(\))", "Math.random()"},
        {R"(\$RANDOM\b)", "shell $RANDOM"},
        {R"(\bshuffle\b)", "shuffle function"},
        // Time
        {R"(\btime\s*\()", "time() call"},
        {R"(datetime\.now)", "datetime.now()"},
        {R"(Date\.now\(\))", "Date.now()"},
        {R"(\bdate\b)", "date command"},
        {R"(clock\s*\()", "clock() call"},
        {R"(Time\.now)", "Time.now"},
        // UUID
        {R"(\buuid)", "UUID generation"},
        {R"(randomUUID)", "crypto.randomUUID()"},
        // Network
        {R"(requests\.get)", "HTTP request"},
        {R"(\bfetch\s*\()", "fetch() call"},
        {R"(\bcurl\b)", "curl command"},
        {R"(\bwget\b)", "wget command"},
        // Process
        {R"(\bgetpid\b)", "getpid()"},
        {R"(\$\$)", "shell PID"},
        // Temp files
        {R"(\bmktemp\b)", "mktemp"},
        {R"(\btmpfile\b)", "tmpfile()"},
        // Entropy sources
        {R"(/dev/u?random)", "/dev/urandom"},
    };

    for (const auto& [pat, desc] : patterns) {
        try {
            std::regex re(pat, std::regex::ECMAScript);
            if (std::regex_search(code, re)) {
                return "uses " + desc + " (non-deterministic)";
            }
        } catch (...) {
            // Skip broken regex
        }
    }
    return "";
}

std::string GovernanceEngine::checkOutputEntropy(const std::string& output, int line) {
    // Check each line for high-entropy strings (possible leaked credentials)
    std::istringstream stream(output);
    std::string line_str;
    while (std::getline(stream, line_str)) {
        if (line_str.size() < 16) continue;  // Too short to be meaningful
        double entropy = calculateEntropy(line_str);
        if (entropy > 4.5) {
            return "high-entropy output (entropy=" +
                   fmt::format("{:.1f}", entropy) + ", possible credential leak)";
        }
    }
    return "";
}

std::string GovernanceEngine::checkErrorDumps(const std::string& output, int line) {
    static const std::vector<std::pair<std::string, std::string>> patterns = {
        {R"(Traceback \(most recent call last\))", "Python traceback"},
        {R"(at [\w\.]+\([\w\.]+:\d+\))", "Java/JS stack trace"},
        {R"(\bpanic:)", "Go/Rust panic"},
        {R"(Segmentation fault)", "segfault"},
        {R"(core dumped)", "core dump"},
        {R"(FATAL ERROR)", "fatal error"},
        {R"(undefined reference)", "linker error"},
        {R"(error\[E\d+\])", "Rust compiler error"},
    };

    for (const auto& [pat, desc] : patterns) {
        try {
            std::regex re(pat, std::regex::ECMAScript);
            if (std::regex_search(output, re)) {
                return "error dump detected: " + desc;
            }
        } catch (...) {
            // Skip broken regex
        }
    }
    return "";
}

} // namespace governance
} // namespace naab
