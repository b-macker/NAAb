// NAAb Scanner — Python Language Checks (14 checks)
// Port from naab-q/src/checks/lang_python.naab

#include "naab/scanner.h"
#include <regex>
#include <functional>
#include <algorithm>
#include <unordered_map>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string py_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline bool py_startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static inline int py_indent(const std::string& s) {
    size_t first = s.find_first_not_of(" \t");
    return (first != std::string::npos) ? static_cast<int>(first) : 0;
}

void ScannerEngine::checkLangPython(const std::string& filepath,
                                     const std::vector<std::string>& lines,
                                     const std::string& content,
                                     std::vector<Issue>& issues) const {
    const std::string CAT = "lang_python";

    // 1. bare_except
    if (isEnabled(CAT, "bare_except")) {
        static const std::regex pat(R"(^\s*except\s*:)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "bare_except", CAT,
                         "Bare except catches everything", py_trim(lines[i]),
                         "Specify exception type");
            }
        }
    }

    // 2. mutable_default_args
    if (isEnabled(CAT, "mutable_default_args")) {
        static const std::regex pat(R"(def\s+\w+\([^)]*=\s*[\[\{])");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "mutable_default_args", CAT,
                         "Mutable default argument", py_trim(lines[i]),
                         "Use None, create inside body");
            }
        }
    }

    // 3. star_imports
    if (isEnabled(CAT, "star_imports")) {
        static const std::regex pat(R"(^\s*from\s+\w+\s+import\s+\*)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "star_imports", CAT,
                         "Wildcard import pollutes namespace", py_trim(lines[i]),
                         "Import specific names");
            }
        }
    }

    // 4. assert_for_validation
    if (isEnabled(CAT, "assert_for_validation")) {
        std::string lower_path = filepath;
        std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
        bool is_test = lower_path.find("test") != std::string::npos;
        if (!is_test) {
            static const std::regex pat(R"(^\s*assert\s+)");
            for (size_t i = 0; i < lines.size(); ++i) {
                std::string lower_line = lines[i];
                std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);
                if (std::regex_search(lines[i], pat) && lower_line.find("test") == std::string::npos) {
                    addIssue(issues, filepath, i + 1, "assert_for_validation", CAT,
                             "assert for validation (disabled with -O)", py_trim(lines[i]),
                             "Use if/raise");
                }
            }
        }
    }

    // 5. string_format_mix
    if (isEnabled(CAT, "string_format_mix")) {
        bool has_percent = content.find("%s") != std::string::npos ||
                          content.find("%d") != std::string::npos ||
                          content.find("%r") != std::string::npos ||
                          content.find("%f") != std::string::npos;
        bool has_format = content.find(".format(") != std::string::npos;
        static const std::regex fstring_pat(R"(f['"])");
        bool has_fstring = std::regex_search(content, fstring_pat);
        int styles = (has_percent ? 1 : 0) + (has_format ? 1 : 0) + (has_fstring ? 1 : 0);
        if (styles >= 2) {
            addIssue(issues, filepath, 1, "string_format_mix", CAT,
                     fmt::format("Mixed string formatting styles ({})", styles),
                     "%, .format(), f-strings", "Pick one (f-strings preferred)");
        }
    }

    // 6. global_keyword
    if (isEnabled(CAT, "global_keyword")) {
        static const std::regex pat(R"(^\s*global\s+\w+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "global_keyword", CAT,
                         "global keyword usage", py_trim(lines[i]),
                         "Pass as parameter instead");
            }
        }
    }

    // 7. nested_comprehension
    if (isEnabled(CAT, "nested_comprehension")) {
        static const std::regex pat(R"([\[\{].*for.*for.*for)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "nested_comprehension", CAT,
                         "Deeply nested comprehension", py_trim(lines[i]),
                         "Use explicit loops");
            }
        }
    }

    // 8. broad_exception_type
    if (isEnabled(CAT, "broad_exception_type")) {
        static const std::regex pat(R"(^\s*except\s+(Exception|BaseException)\b)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "broad_exception_type", CAT,
                         "Catching overly broad exception", py_trim(lines[i]),
                         "Catch specific types");
            }
        }
    }

    // 9. open_without_with
    if (isEnabled(CAT, "open_without_with")) {
        static const std::regex pat(R"(\w+\s*=\s*open\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                // Check previous line for "with"
                std::string prev;
                if (i > 0) prev = lines[i - 1];
                std::string combined = prev + "\n" + lines[i];
                if (combined.find("with") == std::string::npos) {
                    addIssue(issues, filepath, i + 1, "open_without_with", CAT,
                             "open() without context manager", py_trim(lines[i]),
                             "Use: with open(...) as f:");
                }
            }
        }
    }

    // 10. type_check_isinstance
    if (isEnabled(CAT, "type_check_isinstance")) {
        static const std::regex pat(R"(type\s*\(\s*\w+\s*\)\s*==)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "type_check_isinstance", CAT,
                         "type() comparison — use isinstance()", py_trim(lines[i]),
                         "Use isinstance(x, Type)");
            }
        }
    }

    // 11. import_inside_function
    if (isEnabled(CAT, "import_inside_function")) {
        static const std::regex func_pat(R"(def\s+\w+)");
        static const std::regex import_pat(R"(^\s+(import\s+\w+|from\s+\w+\s+import))");
        bool in_func = false;
        int func_indent = 0;

        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], func_pat)) {
                in_func = true;
                func_indent = py_indent(lines[i]);
            } else if (in_func && !py_trim(lines[i]).empty()) {
                int cur_ind = py_indent(lines[i]);
                std::string trimmed = py_trim(lines[i]);
                if (cur_ind <= func_indent && !py_startsWith(trimmed, "@") && !py_startsWith(trimmed, "#")) {
                    in_func = false;
                }
            }
            if (in_func && std::regex_search(lines[i], import_pat)) {
                addIssue(issues, filepath, i + 1, "import_inside_function", CAT,
                         "Import inside function body", py_trim(lines[i]),
                         "Move to top of file");
            }
        }
    }

    // 12. mutable_class_vars
    if (isEnabled(CAT, "mutable_class_vars")) {
        static const std::regex cls_pat(R"(^(\s*)class\s+\w+)");
        static const std::regex meth_pat(R"(\s+def\s+)");
        static const std::regex mut_var_pat(R"(^\s+\w+\s*=\s*[\[\{])");
        bool in_cls = false;
        int cls_ind = 0;
        bool in_method = false;

        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, cls_pat)) {
                in_cls = true;
                cls_ind = static_cast<int>(m[1].length());
                in_method = false;
            } else if (in_cls && !py_trim(lines[i]).empty()) {
                int cur = py_indent(lines[i]);
                if (cur <= cls_ind) {
                    in_cls = false;
                    in_method = false;
                } else if (std::regex_search(lines[i], meth_pat)) {
                    in_method = true;
                } else if (in_method && cur <= cls_ind + 4) {
                    std::string trimmed = py_trim(lines[i]);
                    if (!py_startsWith(trimmed, "@") && !py_startsWith(trimmed, "#") &&
                        !py_startsWith(trimmed, ")")) {
                        in_method = false;
                    }
                }
            }
            if (in_cls && !in_method && std::regex_search(lines[i], mut_var_pat)) {
                std::string s = py_trim(lines[i]);
                if (!py_startsWith(s, "#") && !py_startsWith(s, "@") && !py_startsWith(s, "def ")) {
                    addIssue(issues, filepath, i + 1, "mutable_class_vars", CAT,
                             "Mutable class variable", s,
                             "Move to __init__ or use field(default_factory=...)");
                }
            }
        }
    }

    // 13. duplicate_dict_key
    if (isEnabled(CAT, "duplicate_dict_key")) {
        static const std::regex dict_key_pat(R"((['"])(\w+)\1\s*:)");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = py_trim(lines[i]);
            if (s.find('{') != std::string::npos && s.find(':') != std::string::npos) {
                // Collect block
                std::string block = lines[i];
                int brace_depth = 0;
                for (char c : s) { if (c == '{') brace_depth++; if (c == '}') brace_depth--; }
                size_t j = i + 1;
                while (brace_depth > 0 && j < lines.size()) {
                    block += "\n" + lines[j];
                    for (char c : lines[j]) { if (c == '{') brace_depth++; if (c == '}') brace_depth--; }
                    j++;
                }

                std::unordered_map<std::string, bool> keys_found;
                std::sregex_iterator it(block.begin(), block.end(), dict_key_pat);
                std::sregex_iterator end;
                for (; it != end; ++it) {
                    std::string key = (*it)[2].str();
                    if (keys_found.count(key)) {
                        addIssue(issues, filepath, i + 1, "duplicate_dict_key", CAT,
                                 fmt::format("Duplicate dict key '{}'", key), s,
                                 "Remove duplicate key");
                        break;
                    }
                    keys_found[key] = true;
                }
            }
        }
    }

    // 14. f_string_without_expression
    if (isEnabled(CAT, "f_string_without_expression")) {
        // Separate patterns for single and double quoted f-strings (no backreference in lookahead)
        static const std::regex fstr_pat_sq(R"(f'([^']*)')");
        static const std::regex fstr_pat_dq(R"RE(f"([^"]*)")RE");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = py_trim(lines[i]);
            if (py_startsWith(s, "#") || py_startsWith(s, "//")) continue;
            bool found = false;
            for (const auto& fstr_pat : {std::cref(fstr_pat_sq), std::cref(fstr_pat_dq)}) {
                if (found) break;
                std::sregex_iterator it(lines[i].begin(), lines[i].end(), fstr_pat.get());
                std::sregex_iterator end;
                for (; it != end; ++it) {
                    std::string body = (*it)[1].str();
                    if (body.find('{') == std::string::npos) {
                        addIssue(issues, filepath, i + 1, "f_string_without_expression", CAT,
                                 "f-string without interpolation", s,
                                 "Remove f prefix or add {expression}");
                        found = true;
                        break;
                    }
                }
            }
        }
    }
}

} // namespace scanner
} // namespace naab
