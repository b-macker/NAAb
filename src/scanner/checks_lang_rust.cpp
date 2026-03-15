// NAAb Scanner — Rust Language Checks (10 checks)
// Port from naab-q/src/checks/lang_rust.naab

#include "naab/scanner.h"
#include <regex>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string rs_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline int rs_indent(const std::string& s) {
    size_t first = s.find_first_not_of(" \t");
    return (first != std::string::npos) ? static_cast<int>(first) : 0;
}

void ScannerEngine::checkLangRust(const std::string& filepath,
                                   const std::vector<std::string>& lines,
                                   std::vector<Issue>& issues) const {
    const std::string CAT = "lang_rust";

    std::string lower_path = filepath;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
    bool is_test_file = lower_path.find("test") != std::string::npos;

    // 1. unwrap_in_prod
    if (isEnabled(CAT, "unwrap_in_prod")) {
        static const std::regex cfg_test(R"(#\[cfg\(test\)\])");
        bool in_test_mod = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], cfg_test)) in_test_mod = true;
            if (lines[i].find(".unwrap()") != std::string::npos && !in_test_mod && !is_test_file) {
                addIssue(issues, filepath, i + 1, "unwrap_in_prod", CAT,
                         ".unwrap() in production code", rs_trim(lines[i]),
                         "Use ? operator or .expect()");
            }
        }
    }

    // 2. clone_abuse
    if (isEnabled(CAT, "clone_abuse")) {
        int max_clones = static_cast<int>(getNumOption(CAT, "clone_abuse", "max_per_function", 5));
        static const std::regex func_pat(R"(^\s*(?:pub\s+)?fn\s+(\w+))");

        struct FuncInfo { size_t line; std::string name; };
        std::vector<FuncInfo> funcs;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, func_pat)) {
                funcs.push_back({i, m[1].str()});
            }
        }

        for (size_t idx = 0; idx < funcs.size(); ++idx) {
            size_t end = (idx + 1 < funcs.size()) ? funcs[idx + 1].line : lines.size();
            int clone_count = 0;
            for (size_t j = funcs[idx].line; j < end; ++j) {
                if (lines[j].find(".clone()") != std::string::npos) clone_count++;
            }
            if (clone_count > max_clones) {
                addIssue(issues, filepath, funcs[idx].line + 1, "clone_abuse", CAT,
                         fmt::format("Function '{}' has {} .clone() (max {})",
                                    funcs[idx].name, clone_count, max_clones),
                         rs_trim(lines[funcs[idx].line]),
                         "Review ownership, use references");
            }
        }
    }

    // 3. unsafe_blocks
    if (isEnabled(CAT, "unsafe_blocks")) {
        static const std::regex pat(R"(\bunsafe\s*\{)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "unsafe_blocks", CAT,
                         "unsafe block", rs_trim(lines[i]),
                         "Document safety invariants");
            }
        }
    }

    // 4. todo_unimplemented
    if (isEnabled(CAT, "todo_unimplemented")) {
        static const std::regex pat(R"(\b(todo|unimplemented)!\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "todo_unimplemented", CAT,
                         "Placeholder macro", rs_trim(lines[i]),
                         "Implement the logic");
            }
        }
    }

    // 5. expect_without_msg
    if (isEnabled(CAT, "expect_without_msg")) {
        static const std::regex pat(R"(\.expect\(\s*""\s*\))");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "expect_without_msg", CAT,
                         ".expect() without message", rs_trim(lines[i]),
                         "Add descriptive message");
            }
        }
    }

    // 6. string_push
    if (isEnabled(CAT, "string_push")) {
        static const std::regex loop_pat(R"(^(for|while|loop)\b)");
        static const std::regex push_pat(R"(\.push_str\()");
        bool in_loop = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = rs_trim(lines[i]);
            if (std::regex_search(s, loop_pat)) in_loop = true;
            if (in_loop && std::regex_search(s, push_pat)) {
                addIssue(issues, filepath, i + 1, "string_push", CAT,
                         "push_str in loop", s, "Use write! or collect+join");
            }
            if (s == "}" && in_loop) {
                int indent = rs_indent(lines[i]);
                if (indent <= 4) in_loop = false;
            }
        }
    }

    // 7. box_vec
    if (isEnabled(CAT, "box_vec")) {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("Box<Vec<") != std::string::npos) {
                addIssue(issues, filepath, i + 1, "box_vec", CAT,
                         "Box<Vec<T>> double indirection", rs_trim(lines[i]),
                         "Vec is already heap-allocated");
            }
        }
    }

    // 8. return_impl_trait
    if (isEnabled(CAT, "return_impl_trait")) {
        static const std::regex pat(R"(pub\s+fn.*->\s*impl\s+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "return_impl_trait", CAT,
                         "impl Trait in public API return", rs_trim(lines[i]),
                         "Consider named type for stability");
            }
        }
    }

    // 9. needless_lifetimes
    if (isEnabled(CAT, "needless_lifetimes")) {
        static const std::regex pat(R"(fn\s+\w+<'\w+>\s*\(\s*\w+:\s*&'\w+\s+\w+\s*\)\s*->\s*&'\w+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "needless_lifetimes", CAT,
                         "Lifetime could be elided", rs_trim(lines[i]),
                         "Remove explicit lifetime");
            }
        }
    }

    // 10. manual_map
    if (isEnabled(CAT, "manual_map")) {
        static const std::regex match_pat(R"(match\s+\w+\s*\{)");
        static const std::regex some_pat(R"(^Some\(\w+\)\s*=>\s*Some\()");
        for (size_t i = 0; i + 2 < lines.size(); ++i) {
            if (std::regex_search(lines[i], match_pat)) {
                std::string nxt = rs_trim(lines[i + 1]);
                std::string nxt2 = rs_trim(lines[i + 2]);
                if (std::regex_search(nxt, some_pat) && nxt2.find("None") == 0) {
                    addIssue(issues, filepath, i + 1, "manual_map", CAT,
                             "Manual match on Option — use .map()", rs_trim(lines[i]),
                             "Use .map(|x| transform(x))");
                }
            }
        }
    }
}

} // namespace scanner
} // namespace naab
