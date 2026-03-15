// NAAb Scanner — Go Language Checks (9 checks)
// Port from naab-q/src/checks/lang_go.naab

#include "naab/scanner.h"
#include <regex>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string go_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline int go_indent(const std::string& s) {
    size_t first = s.find_first_not_of(" \t");
    return (first != std::string::npos) ? static_cast<int>(first) : 0;
}

void ScannerEngine::checkLangGo(const std::string& filepath,
                                 const std::vector<std::string>& lines,
                                 std::vector<Issue>& issues) const {
    const std::string CAT = "lang_go";

    // Check if package main
    static const std::regex pkg_main(R"(^package\s+main)");
    bool is_main = false;
    for (size_t i = 0; i < std::min(lines.size(), size_t(5)); ++i) {
        if (std::regex_search(lines[i], pkg_main)) { is_main = true; break; }
    }

    // 1. ignored_error
    if (isEnabled(CAT, "ignored_error")) {
        static const std::regex pat(R"(\w+\s*,\s*_\s*:?=\s*\w+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = go_trim(lines[i]);
            if (std::regex_search(s, pat)) {
                addIssue(issues, filepath, i + 1, "ignored_error", CAT,
                         "Error return value discarded with _", s,
                         "Handle the error");
            }
        }
    }

    // 2. panic_in_library
    if (isEnabled(CAT, "panic_in_library") && !is_main) {
        static const std::regex pat(R"(\bpanic\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "panic_in_library", CAT,
                         "panic() in library code", go_trim(lines[i]),
                         "Return error instead");
            }
        }
    }

    // 3. empty_interface_abuse
    if (isEnabled(CAT, "empty_interface_abuse")) {
        static const std::regex pat(R"(interface\s*\{\s*\}|\bany\b)");
        int count = 0;
        for (const auto& l : lines) {
            if (std::regex_search(l, pat)) count++;
        }
        if (count > 3) {
            addIssue(issues, filepath, 1, "empty_interface_abuse", CAT,
                     fmt::format("Excessive interface{{}}/any ({})", count),
                     fmt::format("{} uses", count),
                     "Use specific types or generics");
        }
    }

    // 4. shadow_err
    if (isEnabled(CAT, "shadow_err")) {
        static const std::regex err_check(R"(^if\s+err\s*!=\s*nil)");
        static const std::regex err_shadow(R"(err\s*:=)");
        bool in_err = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = go_trim(lines[i]);
            if (std::regex_search(s, err_check)) {
                in_err = true;
            } else if (in_err) {
                if (std::regex_search(s, err_shadow)) {
                    addIssue(issues, filepath, i + 1, "shadow_err", CAT,
                             "err shadowed with :=", s, "Use = instead of :=");
                }
                if (s == "}") in_err = false;
            }
        }
    }

    // 5. init_function
    if (isEnabled(CAT, "init_function")) {
        static const std::regex pat(R"(^\s*func\s+init\s*\(\s*\))");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "init_function", CAT,
                         "init() function — hard to test", go_trim(lines[i]),
                         "Use explicit initialization");
            }
        }
    }

    // 6. error_string_format
    if (isEnabled(CAT, "error_string_format")) {
        static const std::regex pat1(R"(fmt\.Errorf\s*\(\s*"[A-Z])");
        static const std::regex pat2(R"(errors\.New\s*\(\s*"[A-Z])");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat1) || std::regex_search(lines[i], pat2)) {
                addIssue(issues, filepath, i + 1, "error_string_format", CAT,
                         "Error string starts with capital", go_trim(lines[i]),
                         "Use lowercase per Go conventions");
            }
        }
    }

    // 7. defer_in_loop
    if (isEnabled(CAT, "defer_in_loop")) {
        static const std::regex loop_pat(R"(^for\s+)");
        static const std::regex defer_pat(R"(\bdefer\s+)");
        bool in_loop = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = go_trim(lines[i]);
            if (std::regex_search(s, loop_pat) || s == "for {") {
                in_loop = true;
            }
            if (in_loop && std::regex_search(s, defer_pat)) {
                addIssue(issues, filepath, i + 1, "defer_in_loop", CAT,
                         "defer inside loop", s, "Move cleanup into helper function");
            }
            if (s == "}" && in_loop) {
                int indent = go_indent(lines[i]);
                if (indent <= 1) in_loop = false;
            }
        }
    }

    // 8. goroutine_leak
    if (isEnabled(CAT, "goroutine_leak")) {
        static const std::regex pat(R"(\bgo\s+(func\(|\w+\())");
        static const std::regex sync_pat(R"(WaitGroup|context\.|chan\b|select\s*\{|sync\.)");

        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                size_t start = (i >= 10) ? i - 10 : 0;
                size_t end = std::min(i + 10, lines.size());
                std::string context;
                for (size_t j = start; j < end; ++j) context += lines[j] + "\n";
                if (!std::regex_search(context, sync_pat)) {
                    addIssue(issues, filepath, i + 1, "goroutine_leak", CAT,
                             "Goroutine without sync", go_trim(lines[i]),
                             "Use WaitGroup, context, or channel");
                }
            }
        }
    }

    // 9. naked_return
    if (isEnabled(CAT, "naked_return")) {
        static const std::regex func_named_ret(R"(^\s*func\s+.*\)\s*\(.*\)\s*\{)");
        static const std::regex func_start(R"(^func\s+)");
        static const std::regex naked_ret(R"(^\s*return\s*$)");
        bool in_named_ret = false;

        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], func_named_ret)) {
                in_named_ret = true;
            } else if (std::regex_search(lines[i], func_start) || go_trim(lines[i]) == "}") {
                in_named_ret = false;
            }
            if (in_named_ret && std::regex_match(lines[i], naked_ret)) {
                addIssue(issues, filepath, i + 1, "naked_return", CAT,
                         "Naked return in named-return function", go_trim(lines[i]),
                         "Return values explicitly for clarity");
            }
        }
    }
}

} // namespace scanner
} // namespace naab
