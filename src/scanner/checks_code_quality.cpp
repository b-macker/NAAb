// NAAb Scanner — Code Quality Checks (15 checks)
// Port from naab-q/src/checks/code_quality.naab

#include "naab/scanner.h"
#include <regex>
#include <sstream>
#include <functional>
#include <unordered_set>
#include <algorithm>
#include <fmt/core.h>
#include <cmath>

namespace naab {
namespace scanner {

static inline std::string cq_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline int cq_indent(const std::string& s) {
    size_t first = s.find_first_not_of(" \t");
    return (first != std::string::npos) ? static_cast<int>(first) : 0;
}

static inline bool cq_startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Detect NAAb main{} lines
static std::unordered_set<size_t> detectMainLines(const std::vector<std::string>& lines,
                                                    const std::string& language) {
    std::unordered_set<size_t> main_lines;
    if (language != "naab") return main_lines;

    bool in_main = false;
    int main_depth = 0;
    static const std::regex main_pat(R"(^main\s*\{)");

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string sl = cq_trim(lines[i]);
        if (!in_main) {
            if (std::regex_search(sl, main_pat) ||
                (sl == "main" && i + 1 < lines.size() && cq_trim(lines[i + 1])[0] == '{')) {
                in_main = true;
                int opens = 0, closes = 0;
                for (char c : sl) { if (c == '{') opens++; if (c == '}') closes++; }
                main_depth = opens - closes;
            }
        } else {
            int opens = 0, closes = 0;
            for (char c : sl) { if (c == '{') opens++; if (c == '}') closes++; }
            main_depth += opens - closes;
            main_lines.insert(i);
            if (main_depth <= 0) in_main = false;
        }
    }
    return main_lines;
}

void ScannerEngine::checkCodeQuality(const std::string& filepath,
                                      const std::vector<std::string>& lines,
                                      const std::string& language,
                                      std::vector<Issue>& issues) const {
    const std::string CAT = "code_quality";
    auto main_lines = detectMainLines(lines, language);
    auto test_lines = ScannerEngine::detectTestFuncLines(lines, language);

    // 1. empty_catch
    if (isEnabled(CAT, "empty_catch")) {
        static const std::regex except_pass(R"(^except\s*.*:\s*pass\s*$)");
        static const std::regex except_bare(R"(^except\s*.*:\s*$)");
        static const std::regex catch_empty(R"(^(?:\}\s*)?catch\s*\([^)]*\)\s*\{\s*\}\s*$)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cq_trim(lines[i]);
            if (std::regex_match(s, except_pass)) {
                addIssue(issues, filepath, i + 1, "empty_catch", CAT,
                         "Empty except block swallows exception", s, "Handle the error or log it");
            } else if (std::regex_match(s, except_bare) && i + 1 < lines.size()) {
                std::string nxt = cq_trim(lines[i + 1]);
                if (nxt == "pass") {
                    bool has_more = false;
                    if (i + 2 < lines.size()) {
                        std::string nxt2 = lines[i + 2];
                        int indent_except = cq_indent(lines[i]);
                        int indent_nxt2 = cq_indent(nxt2);
                        if (cq_trim(nxt2).size() > 0 && indent_nxt2 > indent_except) {
                            has_more = true;
                        }
                    }
                    if (!has_more) {
                        addIssue(issues, filepath, i + 1, "empty_catch", CAT,
                                 "Empty except block swallows exception", s, "Handle the error or log it");
                    }
                }
            }
            if (std::regex_match(s, catch_empty)) {
                addIssue(issues, filepath, i + 1, "empty_catch", CAT,
                         "Empty catch block swallows exception", s, "Handle the error or log it");
            }
        }

        // Fix B: Multi-line catch with trivial/no-op body
        {
            static const std::regex catch_open(R"((?:^|\}\s*)catch\s*\([^)]*\)\s*\{)");
            static const std::regex trivial_assign(R"(^let\s+\w+\s*=\s*\w+\s*;?\s*$)");

            for (size_t i = 0; i + 1 < lines.size(); ++i) {
                std::string s = cq_trim(lines[i]);
                if (!std::regex_search(s, catch_open)) continue;
                if (s.empty() || s.back() != '{') continue;  // Must end with { (multi-line body)

                // Scan the catch body
                bool only_trivial = true;
                bool has_any_code = false;
                for (size_t j = i + 1; j < std::min(i + 10, lines.size()); ++j) {
                    std::string body = cq_trim(lines[j]);
                    if (body == "}") break;
                    if (body.empty()) continue;
                    if (cq_startsWith(body, "//") || cq_startsWith(body, "#") || cq_startsWith(body, "/*")) continue;
                    has_any_code = true;
                    if (std::regex_match(body, trivial_assign) || body == "pass" || body == "pass;" || body == ";") {
                        continue;
                    }
                    only_trivial = false;
                    break;
                }
                if (only_trivial) {
                    addIssue(issues, filepath, i + 1, "empty_catch", CAT,
                             "Catch block swallows exception" + std::string(has_any_code ? " with trivial body" : ""),
                             s, "Handle the error: log it, re-throw, or recover");
                }
            }
        }
    }

    // 2. catch_and_ignore
    if (isEnabled(CAT, "catch_and_ignore")) {
        static const std::regex except_typed(R"(^except\s+\w+.*:)");
        static const std::regex log_pat(R"(^(print|log|logger\.\w+|logging\.\w+|console\.(log|warn|error))\()");

        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            std::string s = cq_trim(lines[i]);
            if (std::regex_search(s, except_typed)) {
                std::string nxt = cq_trim(lines[i + 1]);
                if (std::regex_search(nxt, log_pat)) {
                    if (i + 2 < lines.size()) {
                        std::string nxt2 = cq_trim(lines[i + 2]);
                        if (!cq_startsWith(nxt2, "raise") && !cq_startsWith(nxt2, "return") &&
                            !cq_startsWith(nxt2, "throw")) {
                            addIssue(issues, filepath, i + 1, "catch_and_ignore", CAT,
                                     "Exception caught but only logged", s,
                                     "Handle or re-raise after logging");
                        }
                    }
                }
            }
        }
    }

    // 3. magic_numbers
    if (isEnabled(CAT, "magic_numbers")) {
        std::unordered_set<double> allowed = {0, 1, -1, 2, 10, 100, 1000};

        // No lookbehind in ECMAScript — use simpler pattern + manual boundary check
        static const std::regex num_pat(R"((-?\d+\.?\d*))");
        static const std::regex const_line(R"(^\s*(const|#define|final|CONST|let\s+[A-Z_]+))");

        for (size_t i = 0; i < lines.size(); ++i) {
            bool skip_main = getNumOption(CAT, "magic_numbers", "skip_main", 1) > 0;
            if (skip_main && main_lines.count(i)) continue;
            if (test_lines.count(i)) continue;
            std::string s = cq_trim(lines[i]);
            if (cq_startsWith(s, "#") || cq_startsWith(s, "//") || cq_startsWith(s, "/*") ||
                cq_startsWith(s, "*") || s.find("import") != std::string::npos ||
                s.find("include") != std::string::npos) continue;
            if (std::regex_search(lines[i], const_line)) continue;

            std::sregex_iterator it(lines[i].begin(), lines[i].end(), num_pat);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                // Manual boundary check: char before match must not be word/dot
                auto pos = (*it).position();
                if (pos > 0) {
                    char before = lines[i][pos - 1];
                    if (std::isalnum(before) || before == '_' || before == '.') continue;
                }
                // Char after match must not be word/dot
                auto end_pos = pos + (*it).length();
                if (end_pos < static_cast<long>(lines[i].size())) {
                    char after = lines[i][end_pos];
                    if (std::isalnum(after) || after == '_' || after == '.') continue;
                }
                try {
                    double val = std::stod((*it)[1].str());
                    if (allowed.count(val) == 0 && std::abs(val) >= 3) {
                        addIssue(issues, filepath, i + 1, "magic_numbers", CAT,
                                 fmt::format("Magic number {}", (*it)[1].str()), s,
                                 "Extract to named constant");
                        break;
                    }
                } catch (...) {}
            }
        }
    }

    // 4. magic_strings
    if (isEnabled(CAT, "magic_strings")) {
        int min_len = static_cast<int>(getNumOption(CAT, "magic_strings", "min_length", 5));
        int min_occ = static_cast<int>(getNumOption(CAT, "magic_strings", "min_occurrences", 3));
        // Separate patterns for single and double quotes (no backreference in lookahead)
        static const std::regex str_pat_sq(R"('([^']+?)')");
        static const std::regex str_pat_dq(R"RE("([^"]+?)")RE");
        static const std::regex junk_pat(R"(^[\s\d:,{}\[\]()=<>+\-*/&|!.;]+$)");

        std::unordered_map<std::string, std::vector<int>> str_counts;
        for (size_t i = 0; i < lines.size(); ++i) {
            // Check both quote types
            for (const auto& pat : {std::cref(str_pat_sq), std::cref(str_pat_dq)}) {
                std::sregex_iterator it(lines[i].begin(), lines[i].end(), pat.get());
                std::sregex_iterator end_it;
                for (; it != end_it; ++it) {
                    std::string sv = (*it)[1].str();
                    if (static_cast<int>(sv.size()) >= min_len) {
                        str_counts[sv].push_back(i + 1);
                    }
                }
            }
        }
        for (const auto& [sv, locs] : str_counts) {
            if (static_cast<int>(locs.size()) >= min_occ) {
                std::string trimmed = cq_trim(sv);
                if (static_cast<int>(trimmed.size()) < min_len) continue;
                if (std::regex_match(sv, junk_pat)) continue;
                std::string locs_str;
                for (size_t j = 0; j < std::min(locs.size(), size_t(5)); ++j) {
                    if (!locs_str.empty()) locs_str += ", ";
                    locs_str += std::to_string(locs[j]);
                }
                addIssue(issues, filepath, locs[0], "magic_strings", CAT,
                         fmt::format("String '{}' repeated {} times", sv, locs.size()),
                         locs_str, "Extract to constant");
            }
        }
    }

    // 5. dead_code_after_return
    if (isEnabled(CAT, "dead_code_after_return")) {
        static const std::regex term_pat(R"(^\s*(return\b|throw\b|break\b|continue\b|exit\(|sys\.exit|os\.exit|process\.exit))");
        static const std::unordered_set<std::string> allowed_next = {
            "}", ")", "]", "else:", "else {", "elif", "except", "catch", "finally", "case"
        };

        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            if (std::regex_search(lines[i], term_pat)) {
                std::string s_end = lines[i];
                // Trim trailing whitespace
                while (!s_end.empty() && (s_end.back() == ' ' || s_end.back() == '\t')) s_end.pop_back();
                if (!s_end.empty() && (s_end.back() == '{' || s_end.back() == '[' ||
                    s_end.back() == '(' || s_end.back() == ',')) continue;

                std::string nxt = cq_trim(lines[i + 1]);
                if (nxt.empty()) continue;
                if (allowed_next.count(nxt)) continue;

                int indent_cur = cq_indent(lines[i]);
                int indent_nxt = cq_indent(lines[i + 1]);
                if (indent_nxt >= indent_cur && !cq_startsWith(nxt, "#") &&
                    !cq_startsWith(nxt, "//") && !cq_startsWith(nxt, "/*")) {
                    addIssue(issues, filepath, i + 2, "dead_code_after_return", CAT,
                             "Unreachable code after return/throw/break", nxt, "Remove dead code");
                }
            }
        }
    }

    // 6. dead_conditional
    if (isEnabled(CAT, "dead_conditional")) {
        struct DeadPat { std::regex pat; std::string desc; };
        static const std::vector<std::pair<std::string, std::string>> dead_pats = {
            // Python-style
            {R"(^if\s+True\s*:)", "if True"}, {R"(^if\s+False\s*:)", "if False"},
            {R"(^if\s+1\s*:)", "if 1"}, {R"(^if\s+0\s*:)", "if 0"},
            // C-style with parens
            {R"(^if\s*\(\s*true\s*\))", "if (true)"}, {R"(^if\s*\(\s*false\s*\))", "if (false)"},
            // NAAb-style (no parens, { brace)
            {R"(^if\s+true\s*\{)", "if true"}, {R"(^if\s+false\s*\{)", "if false"},
            // Identity comparisons (always true/false)
            {R"(^if\s+1\s*==\s*1)", "if 1 == 1"},
            {R"(^if\s+0\s*==\s*0)", "if 0 == 0"},
            {R"(^if\s+null\s*==\s*null)", "if null == null"},
            {R"(^if\s+null\s*!=\s*null)", "if null != null"},
            // C-style identity comparisons
            {R"(^if\s*\(\s*1\s*==\s*1\s*\))", "if (1 == 1)"},
            {R"(^if\s*\(\s*0\s*==\s*0\s*\))", "if (0 == 0)"},
            {R"(^if\s*\(\s*null\s*!=\s*null\s*\))", "if (null != null)"},
        };
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cq_trim(lines[i]);
            bool found = false;
            for (const auto& [pat_str, desc] : dead_pats) {
                std::regex pat(pat_str);
                if (std::regex_search(s, pat)) {
                    addIssue(issues, filepath, i + 1, "dead_conditional", CAT,
                             fmt::format("Condition always {}", desc), s,
                             "Remove or fix the condition");
                    found = true;
                    break;
                }
            }
            // Manual string equality detection: if "a" == "a" {
            if (!found) {
                static const std::regex string_cmp(R"RE(^if\s+"([^"]+)"\s*==\s*"([^"]+)"\s*[\{:])RE");
                std::smatch sm;
                if (std::regex_search(s, sm, string_cmp) && sm[1].str() == sm[2].str()) {
                    addIssue(issues, filepath, i + 1, "dead_conditional", CAT,
                             "Condition always true: identical string comparison", s,
                             "Remove or fix the condition");
                }
            }
        }
    }

    // 7. god_functions
    if (isEnabled(CAT, "god_functions")) {
        int max_lines_f = static_cast<int>(getNumOption(CAT, "god_functions", "max_lines", 80));
        static const std::regex func_start(R"(^(\s*)(?:def|function|fn|func|export\s+fn|pub\s+fn)\s+(\w+))");

        struct FuncInfo { size_t line; int indent; std::string name; };
        std::vector<FuncInfo> funcs;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, func_start)) {
                funcs.push_back({i, static_cast<int>(m[1].length()), m[2].str()});
            }
        }

        for (size_t idx = 0; idx < funcs.size(); ++idx) {
            size_t end = findFuncEnd(lines, funcs[idx].line, funcs[idx].indent, language);
            int length = static_cast<int>(end - funcs[idx].line);
            if (length > max_lines_f) {
                addIssue(issues, filepath, funcs[idx].line + 1, "god_functions", CAT,
                         fmt::format("Function '{}' is {} lines (max {})", funcs[idx].name, length, max_lines_f),
                         cq_trim(lines[funcs[idx].line]), "Break into smaller functions");
            }
        }
    }

    // 8. god_classes
    if (isEnabled(CAT, "god_classes")) {
        int max_cls_lines = static_cast<int>(getNumOption(CAT, "god_classes", "max_lines", 300));
        int max_cls_methods = static_cast<int>(getNumOption(CAT, "god_classes", "max_methods", 20));
        static const std::regex cls_pat(R"(^(\s*)class\s+(\w+))");
        static const std::regex meth_pat(R"(\s+def\s+\w+)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch mc;
            if (std::regex_search(lines[i], mc, cls_pat)) {
                std::string cls_name = mc[2].str();
                int cls_indent = static_cast<int>(mc[1].length());
                int cls_methods = 0;
                size_t cls_end = i + 1;
                while (cls_end < lines.size()) {
                    std::string el = lines[cls_end];
                    if (cq_trim(el).empty()) { cls_end++; continue; }
                    if (cq_indent(el) <= cls_indent && !cq_trim(el).empty()) break;
                    if (std::regex_search(el, meth_pat)) cls_methods++;
                    cls_end++;
                }
                int cls_length = static_cast<int>(cls_end - i);
                if (cls_length > max_cls_lines) {
                    addIssue(issues, filepath, i + 1, "god_classes", CAT,
                             fmt::format("Class '{}' is {} lines (max {})", cls_name, cls_length, max_cls_lines),
                             cq_trim(lines[i]), "Break into smaller classes");
                } else if (cls_methods > max_cls_methods) {
                    addIssue(issues, filepath, i + 1, "god_classes", CAT,
                             fmt::format("Class '{}' has {} methods (max {})", cls_name, cls_methods, max_cls_methods),
                             cq_trim(lines[i]), "Extract into mixins or helper classes");
                }
            }
        }
    }

    // 9. deep_nesting
    if (isEnabled(CAT, "deep_nesting")) {
        int max_depth = static_cast<int>(getNumOption(CAT, "deep_nesting", "max_depth", 4));
        static const std::regex nest_pat(R"(^\s*(if|for|while|try|match|switch|with)\b)");

        // Auto-detect indent unit
        int indent_unit = 4;
        for (const auto& il : lines) {
            if (!il.empty() && il[0] == ' ' && !cq_trim(il).empty()) {
                int sp = cq_indent(il);
                if (sp > 0 && sp <= 8) { indent_unit = sp; break; }
            }
        }

        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], nest_pat)) {
                int indent = cq_indent(lines[i]);
                int depth = indent / indent_unit + 1;
                if (depth > max_depth) {
                    addIssue(issues, filepath, i + 1, "deep_nesting", CAT,
                             fmt::format("Nesting depth {} exceeds max {}", depth, max_depth),
                             cq_trim(lines[i]), "Extract nested logic");
                }
            }
        }
    }

    // 10. long_parameter_list
    if (isEnabled(CAT, "long_parameter_list")) {
        int max_params = static_cast<int>(getNumOption(CAT, "long_parameter_list", "max_params", 5));
        static const std::regex sig_pat(R"((?:def|function|fn|func)\s+\w+\s*\(([^)]*)\))");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, sig_pat)) {
                std::string params_str = m[1].str();
                // Split by comma and count non-self/cls/this params
                std::vector<std::string> params;
                std::istringstream ss(params_str);
                std::string param;
                while (std::getline(ss, param, ',')) {
                    std::string p = cq_trim(param);
                    if (!p.empty() && p != "self" && p != "cls" && p != "this") {
                        params.push_back(p);
                    }
                }
                if (static_cast<int>(params.size()) > max_params) {
                    addIssue(issues, filepath, i + 1, "long_parameter_list", CAT,
                             fmt::format("Function has {} params (max {})", params.size(), max_params),
                             cq_trim(lines[i]), "Use parameter object or split function");
                }
            }
        }
    }

    // 11. complex_boolean_expr
    if (isEnabled(CAT, "complex_boolean_expr")) {
        int max_cond = static_cast<int>(getNumOption(CAT, "complex_boolean_expr", "max_conditions", 3));
        static const std::regex bool_op(R"(\b(and|or)\b|&&|\|\|)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cq_trim(lines[i]);
            if (cq_startsWith(s, "if") || cq_startsWith(s, "while") || cq_startsWith(s, "elif")) {
                int count = 0;
                std::sregex_iterator it(s.begin(), s.end(), bool_op);
                std::sregex_iterator end;
                for (; it != end; ++it) count++;
                if (count > max_cond) {
                    addIssue(issues, filepath, i + 1, "complex_boolean_expr", CAT,
                             fmt::format("Boolean expression has {} operators (max {})", count, max_cond),
                             s, "Extract into named booleans");
                }
            }
        }
    }

    // 12. mutable_global_state (Python only)
    if (isEnabled(CAT, "mutable_global_state") && language == "python") {
        static const std::regex lowercase_assign(R"(^[a-z_]\w*\s*=\s*)");
        static const std::regex uppercase_assign(R"(^[A-Z_]+\s*=)");
        static const std::regex import_or_keyword(R"(^(import|from|class|def|if|for|while|try|with))");
        static const std::regex logger_assign(R"(^[a-z_]\w*\s*=\s*logging\.getLogger)");

        for (size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];
            if (line.empty()) continue;
            if (line[0] == ' ' || line[0] == '\t' || line[0] == '#') continue;
            if (std::regex_search(line, lowercase_assign) && !std::regex_search(line, uppercase_assign)) {
                std::string s = cq_trim(line);
                if (!std::regex_search(s, import_or_keyword)) {
                    if (std::regex_search(line, logger_assign)) continue;
                    addIssue(issues, filepath, i + 1, "mutable_global_state", CAT,
                             "Mutable global state", s,
                             "Use UPPER_CASE for constants or move into function");
                }
            }
        }
    }

    // 13. boolean_function_returns
    if (isEnabled(CAT, "boolean_function_returns")) {
        static const std::regex if_cond(R"(^if\s+.+:)");
        for (size_t i = 0; i + 3 < lines.size(); ++i) {
            std::string s = cq_trim(lines[i]);
            if (std::regex_match(s, if_cond)) {
                if (cq_trim(lines[i + 1]) == "return True" &&
                    cq_trim(lines[i + 2]) == "else:" &&
                    cq_trim(lines[i + 3]) == "return False") {
                    addIssue(issues, filepath, i + 1, "boolean_function_returns", CAT,
                             "if/else returning True/False", s,
                             "Replace with: return <condition>");
                }
            }
        }
    }

    // 14. string_concat_in_loop
    if (isEnabled(CAT, "string_concat_in_loop")) {
        static const std::regex loop_pat(R"(^(for|while)\b)");
        static const std::regex concat_pat(R"(\+=\s*['"])");
        int loop_indent = -1;

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cq_trim(lines[i]);
            int cur_indent = s.empty() ? -1 : cq_indent(lines[i]);

            if (std::regex_search(s, loop_pat)) {
                loop_indent = cur_indent;
            } else if (loop_indent >= 0 && cur_indent >= 0 && cur_indent <= loop_indent && !s.empty()) {
                loop_indent = -1;
            }
            if (loop_indent >= 0 && std::regex_search(s, concat_pat)) {
                addIssue(issues, filepath, i + 1, "string_concat_in_loop", CAT,
                         "String concatenation in loop", s, "Use list+join()");
            }
        }
    }

    // 15. recursive_no_base_case
    if (isEnabled(CAT, "recursive_no_base_case")) {
        static const std::regex func_pat(R"(^(\s*)(?:def|function|fn|func|export\s+fn|pub\s+fn)\s+(\w+))");
        static const std::regex guard_pat(R"(\b(visited|seen|depth|max_depth|limit|memo|cache)\b)", std::regex::icase);

        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, func_pat)) {
                std::string fname = m[2].str();
                int base_ind = static_cast<int>(m[1].length());
                size_t end = findFuncEnd(lines, i, base_ind, language);

                // Build body
                std::string body;
                for (size_t j = i + 1; j < end; ++j) {
                    body += lines[j] + "\n";
                }

                // Check if self-calling
                std::regex self_call("\\b" + fname + "\\s*\\(");
                if (std::regex_search(body, self_call)) {
                    // Fix J: Validate guard variables are actually USED as guards
                    bool has_effective_guard = false;
                    std::smatch gm;
                    std::string body_copy = body;
                    while (std::regex_search(body_copy, gm, guard_pat)) {
                        std::string guard_word = gm[1].str();
                        // Verify the guard word is used in a conditional or modified
                        std::regex guard_in_cond("(?:if|while)\\s+[^{]*\\b" + guard_word + "\\b");
                        std::regex guard_compared("\\b" + guard_word + "\\s*(?:==|!=|>=|<=|>|<)");
                        std::regex guard_modified("\\b" + guard_word + "\\s*(?:[-+]=)");
                        std::regex guard_len("(?:len|size|length)\\s*\\(\\s*" + guard_word + "\\s*\\)");

                        if (std::regex_search(body, guard_in_cond) ||
                            std::regex_search(body, guard_compared) ||
                            std::regex_search(body, guard_modified) ||
                            std::regex_search(body, guard_len)) {
                            has_effective_guard = true;
                            break;
                        }
                        body_copy = gm.suffix().str();
                    }
                    if (!has_effective_guard) {
                        addIssue(issues, filepath, i + 1, "recursive_no_base_case", CAT,
                                 fmt::format("Recursive function '{}' without effective depth/visited guard", fname),
                                 cq_trim(lines[i]),
                                 "Add depth limit or visited set and CHECK it in a conditional", "advisory");
                    }
                }
            }
        }
    }
}

} // namespace scanner
} // namespace naab
