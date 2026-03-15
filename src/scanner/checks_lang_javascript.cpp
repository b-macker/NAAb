// NAAb Scanner — JavaScript Language Checks (12 checks)
// Port from naab-q/src/checks/lang_javascript.naab

#include "naab/scanner.h"
#include <regex>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string js_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline bool js_startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void ScannerEngine::checkLangJavascript(const std::string& filepath,
                                         const std::vector<std::string>& lines,
                                         std::vector<Issue>& issues) const {
    const std::string CAT = "lang_javascript";

    // 1. loose_equality
    if (isEnabled(CAT, "loose_equality")) {
        static const std::regex pat(R"([^=!<>]==[^=])");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = js_trim(lines[i]);
            if (js_startsWith(s, "//")) continue;
            if (std::regex_search(s, pat)) {
                addIssue(issues, filepath, i + 1, "loose_equality", CAT,
                         "Loose equality (==) — use strict (===)", s,
                         "Replace == with ===");
            }
        }
    }

    // 2. var_usage
    if (isEnabled(CAT, "var_usage")) {
        static const std::regex pat(R"(\bvar\s+\w+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "var_usage", CAT,
                         "var used — prefer let/const", js_trim(lines[i]),
                         "Replace var with let or const");
            }
        }
    }

    // 3. eval_usage
    if (isEnabled(CAT, "eval_usage")) {
        static const std::regex pat(R"(\beval\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = js_trim(lines[i]);
            if (js_startsWith(s, "//")) continue;
            if (std::regex_search(s, pat)) {
                addIssue(issues, filepath, i + 1, "eval_usage", CAT,
                         "eval() is a security risk", s, "Use safer alternatives");
            }
        }
    }

    // 4. for_in_array
    if (isEnabled(CAT, "for_in_array")) {
        static const std::regex pat(R"(for\s*\(\s*(?:let|var|const)\s+\w+\s+in\s+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "for_in_array", CAT,
                         "for...in iterates keys — use for...of", js_trim(lines[i]),
                         "Replace 'in' with 'of' for arrays");
            }
        }
    }

    // 5. prototype_pollution
    if (isEnabled(CAT, "prototype_pollution")) {
        static const std::regex pat1(R"(__proto__\s*=)");
        static const std::regex pat2(R"(Object\.assign\s*\(.*prototype)");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = js_trim(lines[i]);
            if (std::regex_search(s, pat1) || std::regex_search(s, pat2)) {
                addIssue(issues, filepath, i + 1, "prototype_pollution", CAT,
                         "Direct prototype modification", s,
                         "Use Object.create() or class syntax");
            }
        }
    }

    // 6. document_write
    if (isEnabled(CAT, "document_write")) {
        static const std::regex pat(R"(document\.write\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "document_write", CAT,
                         "document.write() blocks rendering", js_trim(lines[i]),
                         "Use DOM methods");
            }
        }
    }

    // 7. innerhtml_assignment
    if (isEnabled(CAT, "innerhtml_assignment")) {
        static const std::regex pat(R"(\.innerHTML\s*[+]?=)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "innerhtml_assignment", CAT,
                         "innerHTML assignment — XSS risk", js_trim(lines[i]),
                         "Use textContent or DOM methods");
            }
        }
    }

    // 8. promise_no_catch
    if (isEnabled(CAT, "promise_no_catch")) {
        static const std::regex pat(R"(\.then\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                // Check next 5 lines for .catch(
                std::string remaining;
                for (size_t j = i; j < std::min(i + 5, lines.size()); ++j) {
                    remaining += lines[j] + "\n";
                }
                if (remaining.find(".catch(") == std::string::npos) {
                    addIssue(issues, filepath, i + 1, "promise_no_catch", CAT,
                             "Promise without .catch()", js_trim(lines[i]),
                             "Add .catch() for error handling");
                }
            }
        }
    }

    // 9. async_no_await
    if (isEnabled(CAT, "async_no_await")) {
        static const std::regex pat(R"(async\s+function\s+(\w+))");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, pat)) {
                std::string name = m[1].str();
                size_t end = std::min(i + 50, lines.size());
                std::string body;
                for (size_t j = i + 1; j < end; ++j) body += lines[j] + "\n";
                if (body.find("await ") == std::string::npos) {
                    addIssue(issues, filepath, i + 1, "async_no_await", CAT,
                             fmt::format("Async function '{}' never uses await", name),
                             js_trim(lines[i]), "Remove async or add await");
                }
            }
        }
    }

    // 10. callback_hell
    if (isEnabled(CAT, "callback_hell")) {
        int max_cb = static_cast<int>(getNumOption(CAT, "callback_hell", "max_depth", 3));
        static const std::regex cb_pat(R"(function\s*\(|=>\s*\{)");
        int cb_depth = 0;

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = js_trim(lines[i]);
            int cb_count = 0;
            std::sregex_iterator it(s.begin(), s.end(), cb_pat);
            std::sregex_iterator end;
            for (; it != end; ++it) cb_count++;

            cb_depth += cb_count;
            int close_count = 0;
            for (char c : s) if (c == '}') close_count++;
            cb_depth -= close_count;
            cb_depth = std::max(0, cb_depth);

            if (cb_count > 0 && cb_depth > max_cb) {
                addIssue(issues, filepath, i + 1, "callback_hell", CAT,
                         fmt::format("Callback nesting depth {} (max {})", cb_depth, max_cb),
                         s, "Use async/await or Promises");
            }
        }
    }

    // 11. no_strict_mode
    if (isEnabled(CAT, "no_strict_mode")) {
        bool has_strict = false;
        bool has_module = false;
        static const std::regex module_pat(R"(^\s*(import|export)\s+)");
        for (size_t i = 0; i < std::min(lines.size(), size_t(5)); ++i) {
            if (lines[i].find("'use strict'") != std::string::npos ||
                lines[i].find("\"use strict\"") != std::string::npos) {
                has_strict = true;
            }
        }
        for (const auto& l : lines) {
            if (std::regex_search(l, module_pat)) { has_module = true; break; }
        }
        if (!has_strict && !has_module && lines.size() > 5) {
            addIssue(issues, filepath, 1, "no_strict_mode", CAT,
                     "Missing 'use strict' directive",
                     lines.empty() ? "" : js_trim(lines[0]),
                     "Add 'use strict' at top");
        }
    }

    // 12. implicit_globals
    if (isEnabled(CAT, "implicit_globals")) {
        static const std::regex assign_pat(R"(^(\w+)\s*=\s*)");
        static const std::regex keyword_pat(R"(^(let|const|var|function|class|if|for|while|return|export|import|this|module|window|document|console)\b)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = js_trim(lines[i]);
            if (js_startsWith(s, "//") || js_startsWith(s, "/*") || js_startsWith(s, "*")) continue;
            if (std::regex_search(s, assign_pat) && !std::regex_search(s, keyword_pat)) {
                int indent = static_cast<int>(lines[i].find_first_not_of(" \t"));
                if (indent > 0) {
                    addIssue(issues, filepath, i + 1, "implicit_globals", CAT,
                             "Assignment without declaration", s,
                             "Add let, const, or var");
                }
            }
        }
    }
}

} // namespace scanner
} // namespace naab
