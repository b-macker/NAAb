// NAAb Scanner — C++ Language Checks (12 checks)
// Port from naab-q/src/checks/lang_cpp.naab

#include "naab/scanner.h"
#include <regex>
#include <algorithm>
#include <fmt/core.h>

namespace naab {
namespace scanner {

static inline std::string cpp_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
}

static inline bool cpp_startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void ScannerEngine::checkLangCpp(const std::string& filepath,
                                  const std::vector<std::string>& lines,
                                  std::vector<Issue>& issues) const {
    const std::string CAT = "lang_cpp";
    bool is_header = filepath.size() >= 2 &&
                     (filepath.substr(filepath.size() - 2) == ".h" ||
                      filepath.substr(filepath.size() - 4) == ".hpp" ||
                      filepath.substr(filepath.size() - 4) == ".hxx");

    // 1. raw_new_delete
    if (isEnabled(CAT, "raw_new_delete")) {
        static const std::regex new_pat(R"(\bnew\s+\w+)");
        static const std::regex smart_pat(R"(unique_ptr|shared_ptr|make_unique|make_shared)");
        static const std::regex del_pat(R"(\bdelete\s+)");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cpp_trim(lines[i]);
            if (cpp_startsWith(s, "//")) continue;
            if (std::regex_search(s, new_pat) && !std::regex_search(s, smart_pat)) {
                addIssue(issues, filepath, i + 1, "raw_new_delete", CAT,
                         "Raw new without smart pointer", s,
                         "Use std::make_unique or make_shared");
            }
            if (std::regex_search(s, del_pat)) {
                addIssue(issues, filepath, i + 1, "raw_new_delete", CAT,
                         "Raw delete", s, "Use smart pointers");
            }
        }
    }

    // 2. using_namespace_std_header
    if (isEnabled(CAT, "using_namespace_std_header") && is_header) {
        static const std::regex pat(R"(^\s*using\s+namespace\s+std\s*;)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "using_namespace_std_header", CAT,
                         "using namespace std in header", cpp_trim(lines[i]),
                         "Use std:: prefix");
            }
        }
    }

    // 3. c_style_casts
    if (isEnabled(CAT, "c_style_casts")) {
        static const std::regex pat(R"(\(\s*(int|float|double|char|long|short|unsigned|void\s*\*|size_t)\s*\)\s*\w)");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cpp_trim(lines[i]);
            if (cpp_startsWith(s, "//")) continue;
            if (std::regex_search(s, pat)) {
                addIssue(issues, filepath, i + 1, "c_style_casts", CAT,
                         "C-style cast", s, "Use static_cast<Type>()");
            }
        }
    }

    // 4. manual_memory
    if (isEnabled(CAT, "manual_memory")) {
        static const std::regex pat(R"(\b(malloc|calloc|realloc|free)\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cpp_trim(lines[i]);
            if (cpp_startsWith(s, "//")) continue;
            if (std::regex_search(s, pat)) {
                addIssue(issues, filepath, i + 1, "manual_memory", CAT,
                         "C-style memory management", s,
                         "Use std::vector or smart pointers");
            }
        }
    }

    // 5. missing_virtual_dtor
    if (isEnabled(CAT, "missing_virtual_dtor")) {
        static const std::regex class_pat(R"(^\s*class\s+(\w+))");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, class_pat)) {
                std::string name = m[1].str();
                size_t end = std::min(i + 100, lines.size());
                std::string body;
                for (size_t j = i; j < end; ++j) body += lines[j] + "\n";

                bool has_virtual = body.find("virtual") != std::string::npos;
                std::regex vdtor_pat("virtual\\s*~" + name);
                bool has_virtual_dtor = std::regex_search(body, vdtor_pat);
                if (has_virtual && !has_virtual_dtor) {
                    addIssue(issues, filepath, i + 1, "missing_virtual_dtor", CAT,
                             fmt::format("Class '{}' has virtual methods but no virtual destructor", name),
                             cpp_trim(lines[i]),
                             fmt::format("Add virtual ~{}() = default;", name));
                }
            }
        }
    }

    // 6. goto_usage
    if (isEnabled(CAT, "goto_usage")) {
        static const std::regex pat(R"(\bgoto\s+\w+)");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "goto_usage", CAT,
                         "goto statement", cpp_trim(lines[i]),
                         "Use structured control flow");
            }
        }
    }

    // 7. macro_functions
    if (isEnabled(CAT, "macro_functions")) {
        static const std::regex pat(R"(^\s*#define\s+\w+\s*\()");
        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], pat)) {
                addIssue(issues, filepath, i + 1, "macro_functions", CAT,
                         "Function-like macro", cpp_trim(lines[i]),
                         "Use inline/constexpr function");
            }
        }
    }

    // 8. exception_by_value
    if (isEnabled(CAT, "exception_by_value")) {
        static const std::regex pat(R"(catch\s*\(\s*(?!const\b)(\w+)\s+(\w+)\s*\))");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, pat) && lines[i].find("&") == std::string::npos) {
                addIssue(issues, filepath, i + 1, "exception_by_value", CAT,
                         "Catching exception by value (slicing)", cpp_trim(lines[i]),
                         "Catch by const reference");
            }
        }
    }

    // 9. include_guard_missing
    if (isEnabled(CAT, "include_guard_missing") && is_header) {
        static const std::regex pragma_pat(R"(^\s*#pragma\s+once)");
        static const std::regex ifndef_pat(R"(^\s*#ifndef\s+\w+)");
        bool has_guard = false;
        for (size_t i = 0; i < std::min(lines.size(), size_t(5)); ++i) {
            if (std::regex_search(lines[i], pragma_pat) || std::regex_search(lines[i], ifndef_pat)) {
                has_guard = true;
                break;
            }
        }
        if (!has_guard) {
            addIssue(issues, filepath, 1, "include_guard_missing", CAT,
                     "Header missing include guard", filepath, "Add #pragma once");
        }
    }

    // 10. array_decay
    if (isEnabled(CAT, "array_decay")) {
        static const std::regex pat(R"(\b\w+\s+\w+\[\d+\]\s*[;=])");
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s = cpp_trim(lines[i]);
            if (std::regex_search(s, pat) && s.find("template") == std::string::npos &&
                s.find("argv") == std::string::npos) {
                if (!cpp_startsWith(s, "//") && !cpp_startsWith(s, "char")) {
                    addIssue(issues, filepath, i + 1, "array_decay", CAT,
                             "C-style array", s,
                             "Use std::array or std::vector");
                }
            }
        }
    }

    // 11. string_c_str_misuse
    if (isEnabled(CAT, "string_c_str_misuse")) {
        static const std::regex cstr_pat(R"(\w+\s*=\s*\w+\.c_str\s*\(\))");
        static const std::regex charptr_pat(R"((const\s+)?char\s*\*)");
        static const std::regex auto_pat(R"(auto\s+)");

        for (size_t i = 0; i < lines.size(); ++i) {
            if (std::regex_search(lines[i], cstr_pat)) {
                if (std::regex_search(lines[i], charptr_pat) || std::regex_search(lines[i], auto_pat)) {
                    addIssue(issues, filepath, i + 1, "string_c_str_misuse", CAT,
                             "Storing .c_str() result (dangling)", cpp_trim(lines[i]),
                             "Use std::string or .c_str() directly in call");
                }
            }
        }
    }
}

} // namespace scanner
} // namespace naab
