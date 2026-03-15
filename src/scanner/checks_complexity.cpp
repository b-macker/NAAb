// NAAb Scanner — Complexity Checks (8 checks)
// Port from naab-q/src/checks/complexity.naab

#include "naab/scanner.h"
#include <regex>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string cx_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

void ScannerEngine::checkComplexity(const std::string& filepath,
                                     const std::vector<std::string>& lines,
                                     const std::string& language,
                                     std::vector<Issue>& issues) const {
    const std::string CAT = "complexity";

    // Collect function starts (shared by multiple checks)
    static const std::regex func_pat(R"(^(\s*)(?:def|function|fn|func|export\s+fn|pub\s+fn)\s+(\w+))");
    struct FuncInfo { size_t line; int indent; std::string name; };
    std::vector<FuncInfo> funcs;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::smatch m;
        if (std::regex_search(lines[i], m, func_pat)) {
            funcs.push_back({i, static_cast<int>(m[1].length()), m[2].str()});
        }
    }

    // 1. cyclomatic_complexity
    if (isEnabled(CAT, "cyclomatic_complexity")) {
        int max_cc = static_cast<int>(getNumOption(CAT, "cyclomatic_complexity", "max", 15));
        static const std::regex decision_pat(R"(\b(if|elif|else\s+if|for|while|case|catch|except)\b|\b(and|or)\b|&&|\|\|)");

        for (const auto& func : funcs) {
            size_t end = findFuncEnd(lines, func.line, func.indent, language);
            int cc = 1;
            for (size_t j = func.line + 1; j < end; ++j) {
                std::sregex_iterator it(lines[j].begin(), lines[j].end(), decision_pat);
                std::sregex_iterator e;
                for (; it != e; ++it) cc++;
            }
            if (cc > max_cc) {
                addIssue(issues, filepath, func.line + 1, "cyclomatic_complexity", CAT,
                         fmt::format("Function '{}' has cyclomatic complexity {} (max {})", func.name, cc, max_cc),
                         cx_trim(lines[func.line]), "Break into smaller functions");
            }
        }
    }

    // 2. cognitive_complexity
    if (isEnabled(CAT, "cognitive_complexity")) {
        int max_cog = static_cast<int>(getNumOption(CAT, "cognitive_complexity", "max", 20));
        static const std::regex ctrl_pat(R"(^(if|for|while|switch|match)\b)");
        static const std::regex else_pat(R"(^(else|elif|else\s+if|catch|except)\b)");
        static const std::regex bool_pat(R"(\b(and|or)\b|&&|\|\|)");

        for (const auto& func : funcs) {
            size_t end = findFuncEnd(lines, func.line, func.indent, language);
            int cog = 0;
            int nesting = 0;
            for (size_t j = func.line + 1; j < end; ++j) {
                std::string s = cx_trim(lines[j]);
                if (std::regex_search(s, ctrl_pat)) {
                    cog += 1 + nesting;
                    nesting++;
                } else if (s == "}" || s == "end") {
                    nesting = std::max(0, nesting - 1);
                } else if (std::regex_search(s, else_pat)) {
                    cog += 1;
                }
                // Count boolean operators
                std::sregex_iterator it(s.begin(), s.end(), bool_pat);
                std::sregex_iterator e;
                for (; it != e; ++it) cog++;
            }
            if (cog > max_cog) {
                addIssue(issues, filepath, func.line + 1, "cognitive_complexity", CAT,
                         fmt::format("Function '{}' has cognitive complexity {} (max {})", func.name, cog, max_cog),
                         cx_trim(lines[func.line]), "Simplify control flow");
            }
        }
    }

    // 3. file_length
    if (isEnabled(CAT, "file_length")) {
        int max_file = static_cast<int>(getNumOption(CAT, "file_length", "max_lines", 500));
        if (static_cast<int>(lines.size()) > max_file) {
            addIssue(issues, filepath, 1, "file_length", CAT,
                     fmt::format("File has {} lines (max {})", lines.size(), max_file),
                     filepath, "Split into multiple files");
        }
    }

    // 4. function_count
    if (isEnabled(CAT, "function_count")) {
        int max_funcs = static_cast<int>(getNumOption(CAT, "function_count", "max", 25));
        if (static_cast<int>(funcs.size()) > max_funcs) {
            addIssue(issues, filepath, 1, "function_count", CAT,
                     fmt::format("File has {} functions (max {})", funcs.size(), max_funcs),
                     filepath, "Split into modules");
        }
    }

    // 5. class_count
    if (isEnabled(CAT, "class_count")) {
        int max_cls = static_cast<int>(getNumOption(CAT, "class_count", "max", 5));
        static const std::regex cls_pat(R"(^\s*class\s+\w+)");
        int cls_count = 0;
        for (const auto& line : lines) {
            if (std::regex_search(line, cls_pat)) cls_count++;
        }
        if (cls_count > max_cls) {
            addIssue(issues, filepath, 1, "class_count", CAT,
                     fmt::format("File has {} classes (max {})", cls_count, max_cls),
                     filepath, "Split into separate files");
        }
    }

    // 6. import_count
    if (isEnabled(CAT, "import_count")) {
        int max_imp = static_cast<int>(getNumOption(CAT, "import_count", "max", 30));
        static const std::regex imp_pat(R"(^\s*(import|from\s+\w+\s+import|#include|require|use)\b)");
        int imp_count = 0;
        for (const auto& line : lines) {
            if (std::regex_search(line, imp_pat)) imp_count++;
        }
        if (imp_count > max_imp) {
            addIssue(issues, filepath, 1, "import_count", CAT,
                     fmt::format("File has {} imports (max {})", imp_count, max_imp),
                     filepath, "Remove unused imports");
        }
    }

    // 7. return_count
    if (isEnabled(CAT, "return_count")) {
        int max_ret = static_cast<int>(getNumOption(CAT, "return_count", "max", 5));
        static const std::regex ret_pat(R"(^\s*return\b)");

        for (const auto& func : funcs) {
            size_t end = findFuncEnd(lines, func.line, func.indent, language);
            int ret_count = 0;
            for (size_t j = func.line + 1; j < end; ++j) {
                if (std::regex_search(lines[j], ret_pat)) ret_count++;
            }
            if (ret_count > max_ret) {
                addIssue(issues, filepath, func.line + 1, "return_count", CAT,
                         fmt::format("Function '{}' has {} returns (max {})", func.name, ret_count, max_ret),
                         cx_trim(lines[func.line]), "Simplify control flow");
            }
        }
    }

    // 8. nested_ternary
    if (isEnabled(CAT, "nested_ternary")) {
        static const std::regex ternary_pat(R"(\?[^:?]*\?)");
        static const std::regex py_ternary_pat(R"(if\s+.+\s+else\s+.+\s+if\s+.+\s+else)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cx_trim(lines[i]);
            if (std::regex_search(s, ternary_pat) && s.find("??") == std::string::npos) {
                addIssue(issues, filepath, i + 1, "nested_ternary", CAT,
                         "Nested ternary expression", s, "Use if/else statements");
            }
            if (language == "python" && std::regex_search(s, py_ternary_pat)) {
                addIssue(issues, filepath, i + 1, "nested_ternary", CAT,
                         "Nested inline conditional", s, "Use if/elif/else statements");
            }
        }
    }
}

} // namespace scanner
} // namespace naab
