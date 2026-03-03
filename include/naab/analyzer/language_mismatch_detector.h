#pragma once

#include <string>
#include <vector>
#include <regex>

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
        std::regex list_comprehension;
        std::regex dict_literal;
        std::regex snake_case_func;
        std::regex triple_quote;
        std::regex python_range;
        std::regex python_enumerate;
        std::regex python_zip;
        std::regex python_with;
    };

    // JavaScript patterns
    struct JavaScriptPatterns {
        std::regex arrow_function;
        std::regex const_let;
        std::regex promise_chain;
        std::regex require_stmt;
        std::regex template_literal;
        std::regex spread_operator;
        std::regex async_await;
    };

    // Go patterns
    struct GoPatterns {
        std::regex defer_stmt;
        std::regex channel_op;
        std::regex type_after_name;
        std::regex goroutine;
        std::regex select_stmt;
    };

    // Shell patterns
    struct ShellPatterns {
        std::regex pipe_operator;
        std::regex redirect;
        std::regex command_sub;
        std::regex variable_expansion;
    };

    // Rust patterns
    struct RustPatterns {
        std::regex match_expr;
        std::regex lifetime;
        std::regex borrow_operator;
        std::regex result_unwrap;
        std::regex macro_bang;
    };

    // Ruby patterns
    struct RubyPatterns {
        std::regex symbol;
        std::regex block_do;
        std::regex string_interpolation;
        std::regex safe_navigation;
    };

    // Zig patterns
    struct ZigPatterns {
        std::regex comptime;
        std::regex error_union;
        std::regex anytype;
    };

    // Julia patterns
    struct JuliaPatterns {
        std::regex multiple_dispatch;
        std::regex broadcast_operator;
        std::regex type_annotation;
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
