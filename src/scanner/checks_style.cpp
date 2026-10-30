// NAAb Scanner — Style Checks (10 checks)
// Port from naab-q/src/checks/style.naab

#include "naab/scanner.h"
#include <sstream>
#include <regex>
#include "naab/rx.h"
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
        static const naab::rx::regex main_pat(R"(^main\s*\{)");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string sl = st_trim(lines[i]);
            if (!in_main) {
                if (naab::rx::regex_search(sl, main_pat) ||
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
        static const naab::rx::regex func_name_pat(R"((?:def|function|fn|func|export\s+fn|pub\s+fn)\s+(\w+))");
        static const naab::rx::regex snake_pat(R"(^[a-z][a-z0-9_]*$)");
        static const naab::rx::regex camel_pat(R"(^[a-z][a-zA-Z0-9]*$)");

        std::vector<std::pair<std::string, int>> func_names;
        for (size_t i = 0; i < lines.size(); ++i) {
            naab::rx::smatch m;
            if (naab::rx::regex_search(lines[i], m, func_name_pat)) {
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
                if (naab::rx::regex_match(n, snake_pat) && n.find('_') != std::string::npos) snake++;
                if (naab::rx::regex_match(n, camel_pat) && n.find('_') == std::string::npos && has_upper) camel++;
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
                            static const naab::rx::regex mc_pat(R"(\s+(\w+)\s*\(\s*\))");
                            naab::rx::smatch mc;
                            if (naab::rx::regex_search(lines[mk], mc, mc_pat)) {
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
                    naab::rx::regex mf_start("^def\\s+" + name_main_func + "\\s*\\(");
                    for (size_t mi = 0; mi < lines.size(); ++mi) {
                        if (!in_mf && naab::rx::regex_search(lines[mi], mf_start)) {
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
                    naab::rx::regex pat(pat_str);
                    if (naab::rx::regex_search(s, pat)) {
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

        // Fix K: Additional NAAb-specific debug checks for custom log functions
        if (language == "naab") {
            static const naab::rx::regex naab_custom_debug(R"((?:^|[^.\w])(?:log|debug|trace)\s*\()");
            for (size_t i = 0; i < lines.size(); ++i) {
                if (main_lines.count(i)) continue;
                if (test_lines.count(i)) continue;
                std::string s = st_trim(lines[i]);
                if (st_startsWith(s, "#") || st_startsWith(s, "//") || st_startsWith(s, "/*")) continue;
                if (naab::rx::regex_search(s, naab_custom_debug)) {
                    addIssue(issues, filepath, i + 1, "debug_leftovers", CAT,
                             "Debug output in production code", s, "Remove debug statement");
                }
            }
        }
    }

    // 3. commented_out_code
    if (isEnabled(CAT, "commented_out_code")) {
        int min_consec = static_cast<int>(getNumOption(CAT, "commented_out_code", "min_lines", 3));
        static const naab::rx::regex code_chars(R"([(){};=\[\]<>]|\w+\.\w+|\w+\()");
        static const naab::rx::regex cpp_preprocessor(R"(^#\s*(include|define|ifdef|ifndef|endif|pragma|if|else|elif|undef|error|warning)\b)");

        int run = 0;
        size_t run_start = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = st_trim(lines[i]);
            bool is_comment = st_startsWith(s, "//");

            if (st_startsWith(s, "#") && !st_startsWith(s, "#!") && s.find("# type:") == std::string::npos) {
                if (language == "cpp" || language == "c") {
                    if (!naab::rx::regex_search(s, cpp_preprocessor)) {
                        is_comment = true;
                    }
                } else {
                    is_comment = true;
                }
            }

            if (is_comment) {
                // Strip comment prefix
                std::string comment_body = naab::rx::regex_replace(s, naab::rx::regex(R"(^[#/]+\s*)"), "");
                if (naab::rx::regex_search(comment_body, code_chars)) {
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
        // In NAAb, `lines` has every polyglot block line emptied, so a block
        // read as a run of blank lines: a 10-line file with none drew "8
        // consecutive blank lines" at its block (A76(a)). A block line is not
        // a blank NAAb line, and the block's own blank lines are its
        // language's, so every block line ends a run.
        std::vector<bool> block_line;
        if (language == "naab") {
            std::vector<std::string> original;
            std::istringstream in(content);
            for (std::string l; std::getline(in, l);) original.push_back(l);
            stripPolyglotBlocks(original, &block_line);
        }
        int blank_run = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            const bool in_block = i < block_line.size() && block_line[i];
            if (!in_block && st_trim(lines[i]).empty()) {
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
        static const naab::rx::regex single_pat(R"('[^']*')");
        static const naab::rx::regex double_pat(R"("[^"]*")");

        int single_count = 0, double_count = 0;
        naab::rx::sregex_iterator it1(content.begin(), content.end(), single_pat);
        naab::rx::sregex_iterator end;
        for (; it1 != end; ++it1) single_count++;

        naab::rx::sregex_iterator it2(content.begin(), content.end(), double_pat);
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
        static const naab::rx::regex import_line(R"(^(import|from)\s+)");
        static const naab::rx::regex import_name(R"(^(?:import|from)\s+(\S+))");

        std::vector<std::pair<int, std::string>> imports;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = st_trim(lines[i]);
            if (naab::rx::regex_search(s, import_line)) {
                naab::rx::smatch m;
                if (naab::rx::regex_search(s, m, import_name)) {
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
