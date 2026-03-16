// NAAb Scanner — Redundancy Checks (15 checks)
// Port from naab-q/src/checks/redundancy.naab

#include "naab/scanner.h"
#include <regex>
#include <unordered_set>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void ScannerEngine::checkRedundancy(const std::string& filepath,
                                     const std::vector<std::string>& lines,
                                     const std::string& language,
                                     std::vector<Issue>& issues) const {
    const std::string CAT = "redundancy";
    auto test_lines = ScannerEngine::detectTestFuncLines(lines, language);

    // 1. obvious_comments
    if (isEnabled(CAT, "obvious_comments")) {
        static const std::regex pat_loop(R"(^[#/]{1,2}\s*(loop|iterate)\s*(through|over))", std::regex::icase);
        static const std::regex pat_return(R"(^[#/]{1,2}\s*return\b)", std::regex::icase);
        static const std::regex pat_assign(R"(^[#/]{1,2}\s*(set|assign|initialize)\s+\w+)", std::regex::icase);
        static const std::regex pat_check(R"(^[#/]{1,2}\s*check\s+if)", std::regex::icase);
        static const std::regex pat_import_c(R"(^[#/]{1,2}\s*import\b)", std::regex::icase);
        static const std::regex pat_import_next(R"(^(import|from|#include|require|use)\b)");
        static const std::regex pat_var_assign(R"(^\w+\s*=)");

        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            std::string stripped = trim(lines[i]);
            std::string nxt = trim(lines[i + 1]);

            if (std::regex_search(stripped, pat_loop) && startsWith(nxt, "for")) {
                addIssue(issues, filepath, i + 1, "obvious_comments", CAT,
                         "Comment restates the for loop below", stripped, "Remove obvious comment");
            } else if (std::regex_search(stripped, pat_return) && startsWith(nxt, "return")) {
                addIssue(issues, filepath, i + 1, "obvious_comments", CAT,
                         "Comment restates the return statement", stripped, "Remove obvious comment");
            } else if (std::regex_search(stripped, pat_assign) && std::regex_search(nxt, pat_var_assign)) {
                addIssue(issues, filepath, i + 1, "obvious_comments", CAT,
                         "Comment restates the assignment", stripped, "Remove obvious comment");
            } else if (std::regex_search(stripped, pat_check) && startsWith(nxt, "if")) {
                addIssue(issues, filepath, i + 1, "obvious_comments", CAT,
                         "Comment restates the if condition", stripped, "Remove obvious comment");
            } else if (std::regex_search(stripped, pat_import_c) && std::regex_search(nxt, pat_import_next)) {
                addIssue(issues, filepath, i + 1, "obvious_comments", CAT,
                         "Comment restates the import", stripped, "Remove obvious comment");
            }
        }
    }

    // 2. over_abstraction
    if (isEnabled(CAT, "over_abstraction")) {
        static const std::regex func_pat(R"(^\s*(?:def|function|fn|func|export\s+fn)\s+(\w+)\s*\()");
        static const std::regex return_call(R"(^return\s+\w+\()");

        for (size_t i = 0; i + 3 < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, func_pat)) {
                std::vector<std::string> body_lines;
                for (size_t j = i + 1; j < std::min(i + 5, lines.size()); ++j) {
                    std::string s = trim(lines[j]);
                    if (!s.empty() && s != "{" && s != "}") {
                        body_lines.push_back(s);
                    }
                }
                std::vector<std::string> non_brace;
                for (const auto& b : body_lines) {
                    if (b != "{" && b != "}") non_brace.push_back(b);
                }
                if (non_brace.size() == 1 && std::regex_search(non_brace[0], return_call)) {
                    addIssue(issues, filepath, i + 1, "over_abstraction", CAT,
                             "Function just wraps another function call", trim(lines[i]),
                             "Inline the wrapper or add meaningful logic");
                }
            }
        }
    }

    // 3. redundant_try_catch
    if (isEnabled(CAT, "redundant_try_catch")) {
        static const std::regex except_pat(R"(^except\s*.*:\s*$)");
        static const std::regex catch_pat(R"(^catch\s*\([^)]*\)\s*\{)");

        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            std::string stripped = trim(lines[i]);
            std::string nxt = trim(lines[i + 1]);

            if (std::regex_match(stripped, except_pat) && nxt == "raise") {
                addIssue(issues, filepath, i + 1, "redundant_try_catch", CAT,
                         "try/catch that just re-raises", stripped, "Remove redundant try/except");
            }
            if (std::regex_search(stripped, catch_pat) && (nxt == "throw;" || nxt == "throw")) {
                addIssue(issues, filepath, i + 1, "redundant_try_catch", CAT,
                         "catch block that just re-throws", stripped, "Remove redundant try/catch");
            }
        }
    }

    // 3b. suspicious_try_catch — try around non-throwing code (NAAb)
    if (isEnabled(CAT, "suspicious_try_catch") && language == "naab") {
        static const std::regex try_start(R"(^\s*try\s*\{)");
        static const std::regex catch_line_pat(R"(^\s*\}\s*catch\s*\()");
        // Patterns that CAN throw — if any appear in the try body, it's legit
        static const std::regex throwable_pat(
            R"(\b(?:int|float|string|parse|open|read|write|fetch|json)\s*\(|)"
            R"(\w+\s*\.\s*\w+\s*\(|)"
            R"(\w+\s*\[|)"
            R"(\b(?!if|else|for|while|return|let|const|print|len|typeof|type)\w{2,}\s*\()");

        for (size_t i = 0; i + 2 < lines.size(); ++i) {
            std::string s = trim(lines[i]);
            if (!std::regex_search(s, try_start)) continue;

            // Find the } catch line and check body for throwable ops
            bool found_catch = false;
            bool has_throwable = false;

            for (size_t j = i + 1; j < lines.size(); ++j) {
                std::string curr = trim(lines[j]);

                // Is this the } catch line?
                if (std::regex_search(curr, catch_line_pat)) {
                    found_catch = true;
                    break;
                }

                // Check if this body line has throwable operations
                // Skip braces-only lines and empty lines
                if (!curr.empty() && curr != "{" && curr != "}") {
                    if (std::regex_search(lines[j], throwable_pat)) {
                        has_throwable = true;
                        break;
                    }
                }
            }

            if (found_catch && !has_throwable) {
                addIssue(issues, filepath, i + 1, "suspicious_try_catch", CAT,
                         "try/catch around code that cannot throw",
                         trim(lines[i]),
                         "Remove unnecessary try/catch");
            }
        }
    }

    // 4. generic_variable_names
    if (isEnabled(CAT, "generic_variable_names")) {
        auto names_list = getListOption(CAT, "generic_variable_names");
        std::unordered_set<std::string> bad_names;
        if (names_list.empty()) {
            bad_names = {"data", "result", "temp", "tmp", "item", "obj", "val",
                         "value", "ret", "res", "buf", "info", "stuff", "things",
                         "foo", "bar", "baz", "qux"};
        } else {
            for (const auto& n : names_list) bad_names.insert(n);
        }

        static const std::regex var_pat(R"((?:let|var|const|int|str|auto|val)\s+(\w+)\s*=)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (test_lines.count(i)) continue;
            std::sregex_iterator it(lines[i].begin(), lines[i].end(), var_pat);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                std::string name = (*it)[1].str();
                std::string lower_name = name;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                if (bad_names.count(lower_name)) {
                    addIssue(issues, filepath, i + 1, "generic_variable_names", CAT,
                             fmt::format("Generic variable name '{}'", name), trim(lines[i]),
                             "Use a descriptive name");
                }
            }
        }
    }

    // 5. excessive_comments
    if (isEnabled(CAT, "excessive_comments")) {
        double threshold = getNumOption(CAT, "excessive_comments", "threshold", 0.6);
        int comment_lines = 0, code_lines = 0;
        for (const auto& line : lines) {
            std::string s = trim(line);
            if (s.empty()) continue;
            if (startsWith(s, "#") || startsWith(s, "//") || startsWith(s, "/*") || startsWith(s, "*")) {
                comment_lines++;
            } else {
                code_lines++;
            }
        }
        if (code_lines > 10) {
            double ratio = static_cast<double>(comment_lines) / std::max(code_lines, 1);
            if (ratio > threshold) {
                addIssue(issues, filepath, 1, "excessive_comments", CAT,
                         fmt::format("Comment-to-code ratio {:.2f} exceeds {:.1f}", ratio, threshold),
                         fmt::format("{} comments / {} code", comment_lines, code_lines),
                         "Remove unnecessary comments");
            }
        }
    }

    // 6. apologetic_comments
    if (isEnabled(CAT, "apologetic_comments")) {
        static const std::regex apology_pat(
            R"((?:i apologize|sorry|unfortunately|as an ai|i cannot|i can't|i'm not sure|might not work|this might|this should|i think this|i believe this|note:\s*this))",
            std::regex::icase);
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string stripped = trim(lines[i]);
            if (startsWith(stripped, "#") || startsWith(stripped, "//") ||
                startsWith(stripped, "/*") || startsWith(stripped, "*")) {
                if (std::regex_search(stripped, apology_pat)) {
                    addIssue(issues, filepath, i + 1, "apologetic_comments", CAT,
                             "Apologetic language in comment", stripped,
                             "Remove apologetic language");
                }
            }
        }
    }

    // 7. placeholder_code
    if (isEnabled(CAT, "placeholder_code")) {
        static const std::regex todo_pat(R"(^[#/]{1,2}\s*TODO:?\s*(implement|add|fix|complete))", std::regex::icase);

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string stripped = trim(lines[i]);
            if (stripped == "raise NotImplementedError" || stripped == "raise NotImplementedError()") {
                addIssue(issues, filepath, i + 1, "placeholder_code", CAT,
                         "Placeholder NotImplementedError", stripped, "Implement the function body");
            } else if (stripped == "pass" && i > 0) {
                static const std::regex def_pat(R"(\s*def\s+)");
                if (std::regex_search(lines[i - 1], def_pat)) {
                    addIssue(issues, filepath, i + 1, "placeholder_code", CAT,
                             "Function body is just 'pass'", stripped, "Implement the function body");
                }
            } else if (std::regex_search(stripped, todo_pat)) {
                addIssue(issues, filepath, i + 1, "placeholder_code", CAT,
                         "TODO placeholder left in code", stripped, "Implement the TODO item");
            } else if (stripped == "throw \"not implemented\"" || stripped == "throw 'not implemented'" ||
                       stripped == "panic(\"not implemented\")" || stripped == "panic!(\"not implemented\")" ||
                       stripped == "unimplemented!()" || stripped == "todo!()") {
                addIssue(issues, filepath, i + 1, "placeholder_code", CAT,
                         "Placeholder in code", stripped, "Implement the function body");
            }
        }
    }

    // 8. restating_docstrings
    if (isEnabled(CAT, "restating_docstrings")) {
        static const std::regex func_pat2(R"(^\s*def\s+(\w+)\s*\()");
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, func_pat2)) {
                std::string fname = m[1].str();
                // Split function name by underscores
                std::unordered_set<std::string> name_words;
                std::string word;
                for (char c : fname) {
                    if (c == '_') {
                        if (!word.empty()) {
                            std::transform(word.begin(), word.end(), word.begin(), ::tolower);
                            name_words.insert(word);
                            word.clear();
                        }
                    } else {
                        word += c;
                    }
                }
                if (!word.empty()) {
                    std::transform(word.begin(), word.end(), word.begin(), ::tolower);
                    name_words.insert(word);
                }

                if (name_words.size() <= 1) continue;

                std::string nxt = trim(lines[i + 1]);
                // Strip quotes
                if (nxt.size() >= 2 && (nxt.front() == '"' || nxt.front() == '\'')) {
                    nxt = nxt.substr(1);
                    if (!nxt.empty() && (nxt.back() == '"' || nxt.back() == '\'')) {
                        nxt.pop_back();
                    }
                }
                std::transform(nxt.begin(), nxt.end(), nxt.begin(), ::tolower);
                if (nxt.empty()) continue;

                // Split doc words
                std::unordered_set<std::string> doc_words;
                std::string dw;
                for (char c : nxt) {
                    if (c == ' ' || c == '.' || c == ',') {
                        if (!dw.empty()) { doc_words.insert(dw); dw.clear(); }
                    } else {
                        dw += c;
                    }
                }
                if (!dw.empty()) doc_words.insert(dw);

                // Check if all name words are subset of doc words
                bool all_in = true;
                for (const auto& nw : name_words) {
                    if (!doc_words.count(nw)) { all_in = false; break; }
                }
                if (all_in && doc_words.size() - name_words.size() < 3) {
                    addIssue(issues, filepath, i + 2, "restating_docstrings", CAT,
                             "Docstring just restates the function name",
                             trim(lines[i + 1]), "Add meaningful description");
                }
            }
        }
    }

    // 9. unnecessary_else_after_return
    if (isEnabled(CAT, "unnecessary_else_after_return")) {
        static const std::regex return_pat(R"(^return\b)");
        static const std::regex else_brace(R"(^\}\s*else\s*\{)");
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            std::string stripped = trim(lines[i]);
            if (std::regex_search(stripped, return_pat)) {
                std::string nxt = trim(lines[i + 1]);
                if (std::regex_search(nxt, else_brace) || nxt == "else:") {
                    addIssue(issues, filepath, i + 2, "unnecessary_else_after_return", CAT,
                             "Unnecessary else after return", nxt,
                             "Remove else, continue at same level");
                }
            }
        }
    }

    // 10. wrapper_classes
    if (isEnabled(CAT, "wrapper_classes")) {
        static const std::regex class_pat(R"(^\s*class\s+(\w+))");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, class_pat)) {
                std::string cls_name = m[1].str();
                int class_indent = static_cast<int>(lines[i].size() - trim(lines[i]).size());
                // Actually compute leading spaces
                size_t first = lines[i].find_first_not_of(" \t");
                class_indent = (first != std::string::npos) ? static_cast<int>(first) : 0;

                std::vector<std::string> methods;
                for (size_t j = i + 1; j < lines.size(); ++j) {
                    std::string jl = lines[j];
                    std::string jt = trim(jl);
                    if (jt.empty()) continue;
                    size_t jfirst = jl.find_first_not_of(" \t");
                    int cur_indent = (jfirst != std::string::npos) ? static_cast<int>(jfirst) : 0;
                    if (cur_indent <= class_indent && !jt.empty()) break;

                    static const std::regex meth_pat(R"(\s+def\s+(\w+)\s*\()");
                    std::smatch mm;
                    if (std::regex_search(jl, mm, meth_pat)) {
                        methods.push_back(mm[1].str());
                    }
                }

                int non_init = 0;
                for (const auto& mname : methods) {
                    if (mname != "__init__" && mname != "__new__") non_init++;
                }
                if (static_cast<int>(methods.size()) <= 2 && non_init == 1) {
                    addIssue(issues, filepath, i + 1, "wrapper_classes", CAT,
                             fmt::format("Class '{}' has only one method besides __init__", cls_name),
                             trim(lines[i]), "Consider using a function instead");
                }
            }
        }
    }

    // 11. unused_imports (Python only)
    if (isEnabled(CAT, "unused_imports") && language == "python") {
        static const std::regex import_pat(R"(^\s*(?:import\s+(\w+)|from\s+(\w+)\s+import\s+(\w+)))");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, import_pat)) {
                std::string name = m[3].matched ? m[3].str() : m[1].str();
                if (name.empty()) continue;

                bool found = false;
                std::regex name_pat("\\b" + name + "\\b");
                for (size_t j = 0; j < lines.size(); ++j) {
                    if (j == i) continue;
                    if (std::regex_search(lines[j], name_pat)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    addIssue(issues, filepath, i + 1, "unused_imports", CAT,
                             fmt::format("Imported '{}' is never used", name),
                             trim(lines[i]), "Remove unused import");
                }
            }
        }
    }

    // 11b. unused_imports (NAAb)
    if (isEnabled(CAT, "unused_imports") && language == "naab") {
        static const std::regex use_pat(R"(^\s*use\s+(\w+)\s*$)");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, use_pat)) {
                std::string module_name = m[1].str();
                if (module_name == "debug") continue;

                bool found = false;
                std::regex usage_pat("\\b" + module_name + "\\.");
                for (size_t j = 0; j < lines.size(); ++j) {
                    if (j == i) continue;
                    if (std::regex_search(lines[j], usage_pat)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    addIssue(issues, filepath, i + 1, "unused_imports", CAT,
                             fmt::format("Module '{}' imported but never used", module_name),
                             trim(lines[i]), "Remove unused import", "advisory");
                }
            }
        }
    }

    // 12. single_use_variable
    if (isEnabled(CAT, "single_use_variable")) {
        static const std::regex var_assign(R"(^\s*(?:let|const|var|)\s*(\w+)\s*=\s*(.+)$)");
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, var_assign)) {
                std::string vname = m[1].str();
                if (vname.size() < 2 || vname[0] == '_') continue;

                std::regex vname_pat("\\b" + vname + "\\b");
                if (!std::regex_search(lines[i + 1], vname_pat)) continue;

                int count = 0;
                for (size_t j = i + 1; j < lines.size(); ++j) {
                    if (std::regex_search(lines[j], vname_pat)) {
                        count++;
                        if (count > 1) break;
                    }
                }
                if (count == 1) {
                    addIssue(issues, filepath, i + 1, "single_use_variable", CAT,
                             fmt::format("Variable '{}' used only once on next line", vname),
                             trim(lines[i]), "Inline directly");
                }
            }
        }
    }

    // 13. copy_paste_signatures
    if (isEnabled(CAT, "copy_paste_signatures")) {
        static const std::regex sig_pat(R"((?:def|function|fn|func)\s+\w+\s*\(([^)]*)\))");
        std::unordered_map<std::string, std::vector<int>> sigs;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, sig_pat)) {
                std::string params = m[1].str();
                std::string trimmed_params = trim(params);
                if (trimmed_params.empty()) continue;
                // Normalize: remove all whitespace
                std::string key;
                for (char c : trimmed_params) {
                    if (c != ' ' && c != '\t') key += c;
                }
                sigs[key].push_back(i + 1);
            }
        }
        for (const auto& [key, locs] : sigs) {
            if (locs.size() >= 3) {
                // Check if all functions with this signature are test functions
                bool all_test_fns = true;
                static const std::regex test_sig_pat(R"((?:def|function|fn|func)\s+test_)");
                for (int ln : locs) {
                    if (ln > 0 && static_cast<size_t>(ln - 1) < lines.size()) {
                        if (!std::regex_search(lines[ln - 1], test_sig_pat)) {
                            all_test_fns = false;
                            break;
                        }
                    }
                }

                std::string locs_str;
                for (int ln : locs) {
                    if (!locs_str.empty()) locs_str += ", ";
                    locs_str += std::to_string(ln);
                }
                addIssue(issues, filepath, locs[0], "copy_paste_signatures", CAT,
                         "3+ functions share identical parameter list", locs_str,
                         "Extract shared logic",
                         all_test_fns ? "advisory" : "");
            }
        }
    }

    // 14. excessive_type_checks
    if (isEnabled(CAT, "excessive_type_checks")) {
        static const std::regex overkill_pat(R"(if\s+\w+\s+is\s+not\s+None\s+and\s+isinstance\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], overkill_pat)) {
                addIssue(issues, filepath, i + 1, "excessive_type_checks", CAT,
                         "Excessive defensive checking", trim(lines[i]), "Trust the type");
            }
        }
    }

    // 15. decorative_separators
    if (isEnabled(CAT, "decorative_separators")) {
        static const std::regex sep_pat(R"(^\s*[#/\*=\-]{10,}\s*$)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_match(lines[i], sep_pat)) {
                addIssue(issues, filepath, i + 1, "decorative_separators", CAT,
                         "Decorative comment separator", trim(lines[i]),
                         "Remove or add descriptive text");
            }
        }
    }

    // 16. trivial_constant_alias
    if (isEnabled(CAT, "trivial_constant_alias")) {
        static const std::unordered_map<std::string, std::string> word_to_num = {
            {"zero", "0"}, {"one", "1"}, {"two", "2"}, {"three", "3"},
            {"four", "4"}, {"five", "5"}, {"six", "6"}, {"seven", "7"},
            {"eight", "8"}, {"nine", "9"}, {"ten", "10"},
            {"hundred", "100"}, {"thousand", "1000"}
        };
        static const std::regex alias_pat(R"(^\s*(?:let|var|const)\s+(\w+)\s*=\s*(-?\d+)\s*$)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, alias_pat)) {
                std::string name = m[1].str();
                std::string value = m[2].str();
                std::string lower_name = name;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

                auto it = word_to_num.find(lower_name);
                if (it != word_to_num.end() && it->second == value) {
                    addIssue(issues, filepath, i + 1, "trivial_constant_alias", CAT,
                             fmt::format("'{}' is a trivial alias for {} — use the literal", name, value),
                             trim(lines[i]),
                             "Use the numeric literal directly");
                }
            }
        }
    }

    // 17. gaming_comments
    if (isEnabled(CAT, "gaming_comments")) {
        static const std::regex gaming_pat(
            R"((?:complexity|score|floor|threshold|metric)\s*(?:>=|<=|==|>|<|\+|-|\*)|)"
            R"((?:\+\d+\s*(?:complexity|score))|)"
            R"((?:adds?\s+complexity)|(?:increase[sd]?\s+complexity)|(?:provides?\s+.*complexity)|)"
            R"((?:for\s+(?:\w+\s+){0,3}complexity(?:\s+score)?)|)"
            R"((?:to\s+(?:pass|ensure|meet|hit|reach)\s+.*(?:complexity|score|check|threshold))|)"
            R"((?:\(\+\d+\s*(?:score)?\))|)"
            R"((?:\+\s*(?:score|complexity)))",
            std::regex::icase);

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string stripped = trim(lines[i]);
            if (startsWith(stripped, "//") || startsWith(stripped, "#") ||
                startsWith(stripped, "/*") || startsWith(stripped, "*")) {
                if (std::regex_search(stripped, gaming_pat)) {
                    addIssue(issues, filepath, i + 1, "gaming_comments", CAT,
                             "Comment reveals metric gaming intent", stripped,
                             "Remove gaming comment — write natural code instead");
                }
            }
        }
    }
}

} // namespace scanner
} // namespace naab
