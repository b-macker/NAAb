#pragma once

#include <string>
#include <vector>
#include <regex>
#include "naab/rx.h"

namespace naab {
namespace analyzer {

/**
 * Language Mismatch Result
 *
 * Describes when code uses idioms from the wrong language
 */
struct LanguageMismatch {
    std::string target_lang;        // Language being used
    std::string source_lang;        // Language pattern comes from
    std::string pattern_detected;   // What pattern matched
    std::string suggestion;         // How to fix
    int confidence;                 // 0-100
};

/**
 * Language Mismatch Detector
 *
 * Detects when code uses patterns/idioms from wrong language
 */
class LanguageMismatchDetector {
public:
    LanguageMismatchDetector();

    /**
     * Detect mismatches in code
     *
     * @param code Source code to analyze
     * @param target_lang Language the code is written in
     * @return List of detected mismatches
     */
    std::vector<LanguageMismatch> detect(
        const std::string& code,
        const std::string& target_lang
    ) const;

private:
    // Python patterns
    struct PythonPatterns {
        naab::rx::regex list_comprehension;
        naab::rx::regex dict_literal;
        naab::rx::regex snake_case_func;
        naab::rx::regex triple_quote;
        naab::rx::regex python_range;
        naab::rx::regex python_enumerate;
        naab::rx::regex python_zip;
        naab::rx::regex python_with;
    };

    // JavaScript patterns
    struct JavaScriptPatterns {
        naab::rx::regex arrow_function;
        naab::rx::regex const_let;
        naab::rx::regex promise_chain;
        naab::rx::regex require_stmt;
        naab::rx::regex template_literal;
        naab::rx::regex spread_operator;
        naab::rx::regex async_await;
    };

    // Go patterns
    struct GoPatterns {
        naab::rx::regex defer_stmt;
        naab::rx::regex channel_op;
        naab::rx::regex type_after_name;
        naab::rx::regex goroutine;
        naab::rx::regex select_stmt;
    };

    // Shell patterns
    struct ShellPatterns {
        naab::rx::regex pipe_operator;
        naab::rx::regex redirect;
        naab::rx::regex command_sub;
        naab::rx::regex variable_expansion;
    };

    // Rust patterns
    struct RustPatterns {
        naab::rx::regex match_expr;
        naab::rx::regex lifetime;
        naab::rx::regex borrow_operator;
        naab::rx::regex result_unwrap;
        naab::rx::regex macro_bang;
    };

    // Ruby patterns
    struct RubyPatterns {
        naab::rx::regex symbol;
        naab::rx::regex block_do;
        naab::rx::regex string_interpolation;
        naab::rx::regex safe_navigation;
    };

    // Zig patterns
    struct ZigPatterns {
        naab::rx::regex comptime;
        naab::rx::regex error_union;
        naab::rx::regex anytype;
    };

    // Julia patterns
    struct JuliaPatterns {
        naab::rx::regex multiple_dispatch;
        naab::rx::regex broadcast_operator;
        naab::rx::regex type_annotation;
    };

    // Pattern databases
    PythonPatterns python_patterns_;
    JavaScriptPatterns js_patterns_;
    GoPatterns go_patterns_;
    ShellPatterns shell_patterns_;
    RustPatterns rust_patterns_;
    RubyPatterns ruby_patterns_;
    ZigPatterns zig_patterns_;
    JuliaPatterns julia_patterns_;

    // Detection methods
    std::vector<LanguageMismatch> detectPythonInOther(
        const std::string& code,
        const std::string& target_lang
    ) const;

    std::vector<LanguageMismatch> detectJavaScriptInOther(
        const std::string& code,
        const std::string& target_lang
    ) const;

    std::vector<LanguageMismatch> detectGoInOther(
        const std::string& code,
        const std::string& target_lang
    ) const;

    std::vector<LanguageMismatch> detectShellInOther(
        const std::string& code,
        const std::string& target_lang
    ) const;

    std::vector<LanguageMismatch> detectRustInOther(
        const std::string& code,
        const std::string& target_lang
    ) const;

    std::vector<LanguageMismatch> detectRubyInOther(
        const std::string& code,
        const std::string& target_lang
    ) const;

    std::vector<LanguageMismatch> detectZigInOther(
        const std::string& code,
        const std::string& target_lang
    ) const;

    std::vector<LanguageMismatch> detectJuliaInOther(
        const std::string& code,
        const std::string& target_lang
    ) const;

    // Helper to create mismatch result
    LanguageMismatch createMismatch(
        const std::string& target_lang,
        const std::string& source_lang,
        const std::string& pattern,
        const std::string& suggestion,
        int confidence
    ) const;
};

} // namespace analyzer
} // namespace naab
