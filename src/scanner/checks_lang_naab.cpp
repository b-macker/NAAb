// NAAb Scanner — NAAb Language Checks (10 checks)
// Port from naab-q/src/checks/lang_naab.naab

#include "naab/scanner.h"
#include <regex>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string nb_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

void ScannerEngine::checkLangNaab(const std::string& filepath,
                                   const std::vector<std::string>& lines,
                                   const std::vector<std::string>& orig_lines,
                                   const std::string& content,
                                   std::vector<Issue>& issues) const {
    const std::string CAT = "lang_naab";

    bool is_module = content.find("main {") == std::string::npos &&
                     content.find("main{") == std::string::npos;

    // 1. value_semantics_bug
    if (isEnabled(CAT, "value_semantics_bug")) {
        static const std::regex get_pat(R"(^let\s+(\w+)\s*=\s*(\w+)\.get\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = nb_trim(lines[i]);
            std::smatch m;
            if (std::regex_search(s, m, get_pat)) {
                std::string var_name = m[1].str();
                std::string dict_name = m[2].str();

                // Check remaining lines for mutation without set-back
                std::string remaining;
                for (size_t j = i + 1; j < std::min(i + 10, lines.size()); ++j) {
                    remaining += lines[j] + "\n";
                }

                std::regex mutate_pat("\\b" + var_name + "\\.(push|set|pop|remove|insert)\\s*\\(");
                std::regex setback_pat(dict_name + "\\.set\\s*\\(");
                std::regex bracket_pat(dict_name + "\\[");

                if (std::regex_search(remaining, mutate_pat) &&
                    !std::regex_search(remaining, setback_pat) &&
                    !std::regex_search(remaining, bracket_pat)) {
                    addIssue(issues, filepath, i + 1, "value_semantics_bug", CAT,
                             fmt::format("Value semantics: modifying '{}' without .set() back", var_name),
                             s, fmt::format("Call {}.set(key, {}) after modifying", dict_name, var_name));
                }
            }
        }
    }

    // 2. missing_null_check
    if (isEnabled(CAT, "missing_null_check")) {
        static const std::regex get_pat(R"(^let\s+\w+\s*=\s*\w+\.get\s*\([^)]+\)\s*$)");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = nb_trim(lines[i]);
            if (std::regex_match(s, get_pat) && s.find("??") == std::string::npos) {
                // Check context for null handling
                std::string ctx;
                size_t start = (i >= 2) ? i - 2 : 0;
                for (size_t j = start; j < std::min(i + 3, lines.size()); ++j) {
                    ctx += lines[j] + "\n";
                }
                if (ctx.find("null") == std::string::npos) {
                    addIssue(issues, filepath, i + 1, "missing_null_check", CAT,
                             ".get() without ?? fallback", s, "Add ?? default_value");
                }
            }
        }
    }

    // 3. polyglot_no_binding
    if (isEnabled(CAT, "polyglot_no_binding")) {
        static const std::regex pat(R"(^<<\w+\s*$)");
        for (size_t i = 0; i < orig_lines.size(); ++i) {
            std::string s = nb_trim(orig_lines[i]);
            if (std::regex_match(s, pat) && s.find('[') == std::string::npos) {
                addIssue(issues, filepath, i + 1, "polyglot_no_binding", CAT,
                         "Polyglot block without variable binding", s,
                         "Add: <<python[var1, var2]");
            }
        }
    }

    // 4. throw_raw_string
    if (isEnabled(CAT, "throw_raw_string")) {
        static const std::regex pat(R"(^throw\s+['"])");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = nb_trim(lines[i]);
            if (std::regex_search(s, pat)) {
                addIssue(issues, filepath, i + 1, "throw_raw_string", CAT,
                         "Throwing raw string", s,
                         "Use structured error: throw { \"code\": ..., \"message\": ... }");
            }
        }
    }

    // 5. oversized_polyglot
    if (isEnabled(CAT, "oversized_polyglot")) {
        int max_poly = static_cast<int>(getNumOption(CAT, "oversized_polyglot", "max_lines", 100));
        static const std::regex poly_open(R"(<<\w+)");
        static const std::regex poly_close(R"(^>>)");
        bool in_block = false;
        size_t block_start = 0;

        for (size_t i = 0; i < orig_lines.size(); ++i) {
            std::string s = nb_trim(orig_lines[i]);
            if (!in_block && s.find("<<") != std::string::npos && std::regex_search(s, poly_open)) {
                in_block = true;
                block_start = i;
            } else if (in_block && std::regex_match(orig_lines[i], poly_close)) {
                int block_len = static_cast<int>(i - block_start);
                if (block_len > max_poly) {
                    addIssue(issues, filepath, block_start + 1, "oversized_polyglot", CAT,
                             fmt::format("Polyglot block is {} lines (max {})", block_len, max_poly),
                             nb_trim(orig_lines[block_start]),
                             "Split into smaller blocks");
                }
                in_block = false;
            }
        }
    }

    // 6. top_level_let
    if (isEnabled(CAT, "top_level_let")) {
        static const std::regex pat(R"(^let\s+\w+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "top_level_let", CAT,
                         "top-level let", nb_trim(lines[i]),
                         "Move inside main {} or function");
            }
        }
    }

    // 7. arrow_lambda
    if (isEnabled(CAT, "arrow_lambda")) {
        static const std::regex pat1(R"(\(\w+\)\s*=>)");
        static const std::regex pat2(R"(=>\s*\{)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat1) || std::regex_search(lines[i], pat2)) {
                addIssue(issues, filepath, i + 1, "arrow_lambda", CAT,
                         "Arrow function syntax not supported", nb_trim(lines[i]),
                         "Use: fn(x) { return x * 2 }");
            }
        }
    }

    // 8. missing_export
    if (isEnabled(CAT, "missing_export") && is_module) {
        static const std::regex fn_pat(R"(^fn\s+\w+)");
        static const std::regex export_pat(R"(^export\s+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], fn_pat) && !std::regex_search(lines[i], export_pat)) {
                addIssue(issues, filepath, i + 1, "missing_export", CAT,
                         "Function without export", nb_trim(lines[i]),
                         "Add export: export fn name()");
            }
        }
    }

    // 9. dict_bracket_access
    if (isEnabled(CAT, "dict_bracket_access")) {
        static const std::regex pat1(R"(\w+\["[^"]+"\])");
        static const std::regex pat2(R"(\w+\['[^']+'\])");
        static const std::regex assign_pat(R"(\w+\[["'][^"']+["']\]\s*=\s)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = nb_trim(lines[i]);
            if (std::regex_search(s, pat1) || std::regex_search(s, pat2)) {
                if (std::regex_search(s, assign_pat)) continue;
                addIssue(issues, filepath, i + 1, "dict_bracket_access", CAT,
                         "Dict bracket access throws on missing key", s,
                         "Use .get(key) with ?? fallback");
            }
        }
    }

    // 10. python_return_in_block
    if (isEnabled(CAT, "python_return_in_block")) {
        static const std::regex py_open(R"(<<python)");
        static const std::regex py_close(R"(^>>)");
        static const std::regex return_pat(R"(^return\s+)");
        static const std::regex def_pat(R"(^(def|class)\s+\w+)");
        bool in_python = false;
        int py_func_depth = 0;
        int py_func_indent = -1;

        for (size_t i = 0; i < orig_lines.size(); ++i) {
            std::string s = nb_trim(orig_lines[i]);
            if (!in_python && std::regex_search(s, py_open)) {
                in_python = true;
                py_func_depth = 0;
                py_func_indent = -1;
            } else if (in_python && std::regex_match(orig_lines[i], py_close)) {
                in_python = false;
                py_func_depth = 0;
            } else if (in_python) {
                // Calculate leading whitespace (Python indentation)
                int indent = 0;
                for (size_t ci = 0; ci < orig_lines[i].size(); ++ci) {
                    if (orig_lines[i][ci] == ' ') indent++;
                    else if (orig_lines[i][ci] == '\t') indent += 4;
                    else break;
                }

                // Check if we've dedented back out of a def/class
                if (py_func_depth > 0 && !s.empty() && indent <= py_func_indent) {
                    py_func_depth = 0;
                    py_func_indent = -1;
                }

                // Entering a new def/class?
                if (std::regex_search(s, def_pat)) {
                    py_func_depth++;
                    py_func_indent = indent;
                }

                // Only flag return at TOP LEVEL of the polyglot block
                if (py_func_depth == 0 && std::regex_search(s, return_pat)) {
                    addIssue(issues, filepath, i + 1, "python_return_in_block", CAT,
                             "return in Python polyglot (SyntaxError)", s,
                             "Use print() or assign to bound variable");
                }
            }
        }
    }
}

} // namespace scanner
} // namespace naab
