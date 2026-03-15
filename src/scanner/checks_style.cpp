// NAAb Scanner — Style Checks (10 checks)
// Port from naab-q/src/checks/style.naab

#include "naab/scanner.h"
#include <regex>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string st_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline bool st_startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

static inline int st_indent(const std::string& s) {
    size_t first = s.find_first_not_of(" \t");
    return (first != std::string::npos) ? static_cast<int>(first) : 0;
}

void ScannerEngine::checkStyle(const std::string& filepath,
                                const std::vector<std::string>& lines,
                                const std::string& content,
                                const std::string& language,
                                std::vector<Issue>& issues) const {
    const std::string CAT = "style";

    // Detect NAAb main{} lines for debug_leftovers exclusion
    std::unordered_set<size_t> main_lines;
    if (language == "naab") {
        bool in_main = false;
        int main_depth = 0;
        static const std::regex main_pat(R"(^main\s*\{)");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string sl = st_trim(lines[i]);
            if (!in_main) {
                if (std::regex_search(sl, main_pat) ||
                    (sl == "main" && i + 1 < lines.size() && st_trim(lines[i + 1])[0] == '{')) {
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
    }

    auto test_lines = ScannerEngine::detectTestFuncLines(lines, language);

    // 1. inconsistent_naming
    if (isEnabled(CAT, "inconsistent_naming")) {
        static const std::regex func_name_pat(R"((?:def|function|fn|func|export\s+fn|pub\s+fn)\s+(\w+))");
        static const std::regex snake_pat(R"(^[a-z][a-z0-9_]*$)");
        static const std::regex camel_pat(R"(^[a-z][a-zA-Z0-9]*$)");

        std::vector<std::pair<std::string, int>> func_names;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, func_name_pat)) {
                std::string name = m[1].str();
                if (!st_startsWith(name, "__")) {
                    func_names.push_back({name, static_cast<int>(i + 1)});
                }
            }
        }

        if (func_names.size() >= 3) {
            int snake = 0, camel = 0;
            for (const auto& [n, _] : func_names) {
                bool has_upper = false;
                for (char c : n) if (std::isupper(c)) { has_upper = true; break; }
                if (std::regex_match(n, snake_pat) && n.find('_') != std::string::npos) snake++;
                if (std::regex_match(n, camel_pat) && n.find('_') == std::string::npos && has_upper) camel++;
            }
            if (snake > 0 && camel > 0) {
                addIssue(issues, filepath, 1, "inconsistent_naming", CAT,
                         fmt::format("Mixed naming: {} snake_case + {} camelCase", snake, camel),
                         fmt::format("{} functions total", func_names.size()), "Pick one convention");
            }
        }
    }

    // 2. debug_leftovers
    if (isEnabled(CAT, "debug_leftovers")) {
        static const std::unordered_map<std::string, std::vector<std::string>> debug_pats = {
            {"python", {R"(\bprint\s*\()", R"(\bbreakpoint\s*\()"}},
            {"javascript", {R"(console\.(log|debug|warn|error|info)\s*\()", R"(\bdebugger\b)"}},
            {"go", {R"(fmt\.Print(ln|f)?\s*\()"}},
            {"cpp", {R"(std::(cout|cerr)\s*<<)", R"(\bprintf\s*\()"}},
            {"rust", {R"(println!\s*\()", R"(dbg!\s*\()", R"(eprintln!\s*\()"}},
            {"naab", {R"(\bprint\s*\()"}},
        };

        auto it = debug_pats.find(language);
        if (it != debug_pats.end()) {
            // For Python: detect __name__ == "__main__" main func lines
            std::unordered_set<size_t> name_main_lines;
            if (language == "python") {
                std::string name_main_func;
                for (size_t mi = 0; mi < lines.size(); ++mi) {
                    if (lines[mi].find("__name__") != std::string::npos &&
                        lines[mi].find("__main__") != std::string::npos) {
                        for (size_t mk = mi; mk < std::min(mi + 5, lines.size()); ++mk) {
                            static const std::regex mc_pat(R"(\s+(\w+)\s*\(\s*\))");
                            std::smatch mc;
                            if (std::regex_search(lines[mk], mc, mc_pat)) {
                                name_main_func = mc[1].str();
                                break;
                            }
                        }
                        break;
                    }
                }
                if (!name_main_func.empty()) {
                    bool in_mf = false;
                    int mf_indent = 0;
                    std::regex mf_start("^def\\s+" + name_main_func + "\\s*\\(");
                    for (size_t mi = 0; mi < lines.size(); ++mi) {
                        if (!in_mf && std::regex_search(lines[mi], mf_start)) {
                            in_mf = true;
                            mf_indent = st_indent(lines[mi]);
                        } else if (in_mf) {
                            std::string trimmed = st_trim(lines[mi]);
                            if (!trimmed.empty() && st_indent(lines[mi]) <= mf_indent) {
                                in_mf = false;
                            } else {
                                name_main_lines.insert(mi);
                            }
                        }
                    }
                }
            }

            for (size_t i = 0; i < lines.size(); ++i) {
                std::string s = st_trim(lines[i]);
                if (st_startsWith(s, "#") || st_startsWith(s, "//") || st_startsWith(s, "/*")) continue;
                if (main_lines.count(i)) continue;
                if (test_lines.count(i)) continue;
                if (name_main_lines.count(i)) continue;

                for (const auto& pat_str : it->second) {
                    std::regex pat(pat_str);
                    if (std::regex_search(s, pat)) {
                        // Python: skip if within __name__ == "__main__" context
                        if (language == "python" && i >= 5) {
                            std::string context;
                            for (size_t k = (i >= 5 ? i - 5 : 0); k < i; ++k) {
                                context += lines[k] + "\n";
                            }
                            if (context.find("__name__") != std::string::npos) continue;
                        }
                        addIssue(issues, filepath, i + 1, "debug_leftovers", CAT,
                                 "Debug output in production code", s, "Remove debug statement");
                        break;
                    }
                }
            }
        }
    }

    // 3. commented_out_code
    if (isEnabled(CAT, "commented_out_code")) {
        int min_consec = static_cast<int>(getNumOption(CAT, "commented_out_code", "min_lines", 3));
        static const std::regex code_chars(R"([(){};=\[\]<>]|\w+\.\w+|\w+\()");
        static const std::regex cpp_preprocessor(R"(^#\s*(include|define|ifdef|ifndef|endif|pragma|if|else|elif|undef|error|warning)\b)");

        int run = 0;
        size_t run_start = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = st_trim(lines[i]);
            bool is_comment = st_startsWith(s, "//");

            if (st_startsWith(s, "#") && !st_startsWith(s, "#!") && s.find("# type:") == std::string::npos) {
                if (language == "cpp" || language == "c") {
                    if (!std::regex_search(s, cpp_preprocessor)) {
                        is_comment = true;
                    }
                } else {
                    is_comment = true;
                }
            }

            if (is_comment) {
                // Strip comment prefix
                std::string comment_body = std::regex_replace(s, std::regex(R"(^[#/]+\s*)"), "");
                if (std::regex_search(comment_body, code_chars)) {
                    if (run == 0) run_start = i;
                    run++;
                } else {
                    if (run >= min_consec) {
                        addIssue(issues, filepath, run_start + 1, "commented_out_code", CAT,
                                 fmt::format("{} lines of commented-out code", run),
                                 st_trim(lines[run_start]), "Remove or use version control");
                    }
                    run = 0;
                }
            } else {
                if (run >= min_consec) {
                    addIssue(issues, filepath, run_start + 1, "commented_out_code", CAT,
                             fmt::format("{} lines of commented-out code", run),
                             st_trim(lines[run_start]), "Remove or use version control");
                }
                run = 0;
            }
        }
        if (run >= min_consec) {
            addIssue(issues, filepath, run_start + 1, "commented_out_code", CAT,
                     fmt::format("{} lines of commented-out code", run),
                     st_trim(lines[run_start]), "Remove or use version control");
        }
    }

    // 4. inconsistent_spacing
    if (isEnabled(CAT, "inconsistent_spacing")) {
        bool has_tabs = false, has_spaces = false;
        for (const auto& l : lines) {
            if (l.find('\t') != std::string::npos && !st_trim(l).empty()) has_tabs = true;
            if (st_startsWith(l, "  ") && !st_trim(l).empty()) has_spaces = true;
        }
        if (has_tabs && has_spaces) {
            addIssue(issues, filepath, 1, "inconsistent_spacing", CAT,
                     "File mixes tabs and spaces", "Both detected", "Choose one style");
        }
    }

    // 5. long_lines
    if (isEnabled(CAT, "long_lines")) {
        int max_len = static_cast<int>(getNumOption(CAT, "long_lines", "max_length", 120));
        for (size_t i = 0; i < lines.size(); ++i) {
            if (static_cast<int>(lines[i].size()) > max_len) {
                std::string s = st_trim(lines[i]);
                if (!st_startsWith(s, "import") && !st_startsWith(s, "#include") &&
                    !st_startsWith(s, "from") && !st_startsWith(s, "//") &&
                    !st_startsWith(s, "#") && s.find("http") == std::string::npos) {
                    std::string preview = s.substr(0, 80) + "...";
                    addIssue(issues, filepath, i + 1, "long_lines", CAT,
                             fmt::format("Line is {} chars (max {})", lines[i].size(), max_len),
                             preview, "Break into multiple lines");
                }
            }
        }
    }

    // 6. missing_final_newline
    if (isEnabled(CAT, "missing_final_newline")) {
        if (!content.empty() && content.back() != '\n') {
            addIssue(issues, filepath, static_cast<int>(lines.size()), "missing_final_newline", CAT,
                     "File does not end with newline", "EOF", "Add newline at end");
        }
    }

    // 7. multiple_blank_lines
    if (isEnabled(CAT, "multiple_blank_lines")) {
        int max_blank = static_cast<int>(getNumOption(CAT, "multiple_blank_lines", "max_consecutive", 2));
        int blank_run = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (st_trim(lines[i]).empty()) {
                blank_run++;
            } else {
                if (blank_run > max_blank) {
                    addIssue(issues, filepath, static_cast<int>(i + 1 - blank_run),
                             "multiple_blank_lines", CAT,
                             fmt::format("{} consecutive blank lines (max {})", blank_run, max_blank),
                             fmt::format("Lines {}-{}", i + 1 - blank_run, i),
                             "Remove extra blank lines");
                }
                blank_run = 0;
            }
        }
    }

    // 8. inconsistent_quotes
    if (isEnabled(CAT, "inconsistent_quotes")) {
        static const std::regex single_pat(R"('[^']*')");
        static const std::regex double_pat(R"("[^"]*")");

        int single_count = 0, double_count = 0;
        std::sregex_iterator it1(content.begin(), content.end(), single_pat);
        std::sregex_iterator end;
        for (; it1 != end; ++it1) single_count++;

        std::sregex_iterator it2(content.begin(), content.end(), double_pat);
        for (; it2 != end; ++it2) double_count++;

        if (single_count > 5 && double_count > 5) {
            double ratio = static_cast<double>(std::min(single_count, double_count)) /
                          std::max(single_count, double_count);
            if (ratio > 0.3) {
                addIssue(issues, filepath, 1, "inconsistent_quotes", CAT,
                         fmt::format("Mixed quote styles: {} single, {} double", single_count, double_count),
                         "Both used frequently", "Pick one style");
            }
        }
    }

    // 9. import_ordering (Python only)
    if (isEnabled(CAT, "import_ordering") && language == "python") {
        static const std::regex import_line(R"(^(import|from)\s+)");
        static const std::regex import_name(R"(^(?:import|from)\s+(\S+))");

        std::vector<std::pair<int, std::string>> imports;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = st_trim(lines[i]);
            if (std::regex_search(s, import_line)) {
                std::smatch m;
                if (std::regex_search(s, m, import_name)) {
                    imports.push_back({static_cast<int>(i + 1), m[1].str()});
                }
            }
        }

        if (imports.size() >= 3) {
            std::vector<std::string> names;
            for (const auto& [_, n] : imports) names.push_back(n);
            std::vector<std::string> sorted_names = names;
            std::sort(sorted_names.begin(), sorted_names.end());
            if (names != sorted_names) {
                addIssue(issues, filepath, imports[0].first, "import_ordering", CAT,
                         "Imports not alphabetically sorted",
                         st_trim(lines[imports[0].first - 1]),
                         "Sort imports (consider isort)");
            }
        }
    }
}

} // namespace scanner
} // namespace naab
