#include "naab/analyzer/language_mismatch_detector.h"
#include <algorithm>

namespace naab {
namespace analyzer {

LanguageMismatchDetector::LanguageMismatchDetector() {
    // Python patterns
    python_patterns_.list_comprehension = std::regex(R"(\[.+\s+for\s+.+\s+in\s+.+\])");
    python_patterns_.dict_literal = std::regex(R"(\{[^{}]+:\s*[^{}]+\})");
    python_patterns_.snake_case_func = std::regex(R"(\bdef\s+\w+_\w+)");
    python_patterns_.triple_quote = std::regex(R"('''|""")");
    python_patterns_.python_range = std::regex(R"(\brange\s*\()");
    python_patterns_.python_enumerate = std::regex(R"(\benumerate\s*\()");
    python_patterns_.python_zip = std::regex(R"(\bzip\s*\()");
    python_patterns_.python_with = std::regex(R"(\bwith\s+\w+.*\s+as\s+)");

    // JavaScript patterns
    js_patterns_.arrow_function = std::regex(R"(=>\s*\{|=>\s*[^{])");
    js_patterns_.const_let = std::regex(R"(\b(const|let)\s+\w+\s*=)");
    js_patterns_.promise_chain = std::regex(R"(\.then\s*\(.*\.catch\s*\()");
    js_patterns_.require_stmt = std::regex(R"(\brequire\s*\(['"]\w+['"])");
    js_patterns_.template_literal = std::regex(R"(`[^`]*\$\{[^}]+\}[^`]*`)");
    js_patterns_.spread_operator = std::regex(R"(\.\.\.\w+)");
    js_patterns_.async_await = std::regex(R"(\basync\s+\w+\s*\(|\bawait\s+)");

    // Go patterns
    go_patterns_.defer_stmt = std::regex(R"(\bdefer\s+\w+)");
    go_patterns_.channel_op = std::regex(R"(<-\s*\w+|\w+\s*<-)");
    go_patterns_.type_after_name = std::regex(R"(\w+\s+\w+\s*:=)");
    go_patterns_.goroutine = std::regex(R"(\bgo\s+\w+\s*\()");
    go_patterns_.select_stmt = std::regex(R"(\bselect\s*\{)");

    // Shell patterns
    shell_patterns_.pipe_operator = std::regex(R"(\|\s*\w+)");
    shell_patterns_.redirect = std::regex(R"(>+\s*\w+|<\s*\w+)");
    shell_patterns_.command_sub = std::regex(R"(\$\([^)]+\))");
    shell_patterns_.variable_expansion = std::regex(R"(\$\{\w+\})");

    // Rust patterns
    rust_patterns_.match_expr = std::regex(R"(\bmatch\s+\w+\s*\{)");
    rust_patterns_.lifetime = std::regex(R"('<\w+>|\b'\w+\b)");
    rust_patterns_.borrow_operator = std::regex(R"(&mut\s+\w+|&\w+)");
    rust_patterns_.result_unwrap = std::regex(R"(\.unwrap\s*\(|\.expect\s*\()");
    rust_patterns_.macro_bang = std::regex(R"(\w+!\s*\()");

    // Ruby patterns
    ruby_patterns_.symbol = std::regex(R"(:\w+)");
    ruby_patterns_.block_do = std::regex(R"(\bdo\s+\|[^|]+\|)");
    ruby_patterns_.string_interpolation = std::regex(R"(#\{\w+\})");
    ruby_patterns_.safe_navigation = std::regex(R"(\w+\?\.)" );

    // Zig patterns
    zig_patterns_.comptime = std::regex(R"(\bcomptime\s+)");
    zig_patterns_.error_union = std::regex(R"(!\w+)");
    zig_patterns_.anytype = std::regex(R"(\banytype\b)");

    // Julia patterns
    julia_patterns_.multiple_dispatch = std::regex(R"(\bfunction\s+\w+\([^)]*::\w+[^)]*\))");
    julia_patterns_.broadcast_operator = std::regex(R"(\.\+|\.-|\.\*|\./|\.\^)");
    julia_patterns_.type_annotation = std::regex(R"(\w+::\w+)");
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detect(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    // Detect patterns from each language that shouldn't be in target_lang
    if (target_lang != "python") {
        auto python_mismatches = detectPythonInOther(code, target_lang);
        mismatches.insert(mismatches.end(), python_mismatches.begin(), python_mismatches.end());
    }

    if (target_lang != "javascript") {
        auto js_mismatches = detectJavaScriptInOther(code, target_lang);
        mismatches.insert(mismatches.end(), js_mismatches.begin(), js_mismatches.end());
    }

    if (target_lang != "go") {
        auto go_mismatches = detectGoInOther(code, target_lang);
        mismatches.insert(mismatches.end(), go_mismatches.begin(), go_mismatches.end());
    }

    if (target_lang != "shell" && target_lang != "bash") {
        auto shell_mismatches = detectShellInOther(code, target_lang);
        mismatches.insert(mismatches.end(), shell_mismatches.begin(), shell_mismatches.end());
    }

    if (target_lang != "rust") {
        auto rust_mismatches = detectRustInOther(code, target_lang);
        mismatches.insert(mismatches.end(), rust_mismatches.begin(), rust_mismatches.end());
    }

    if (target_lang != "ruby") {
        auto ruby_mismatches = detectRubyInOther(code, target_lang);
        mismatches.insert(mismatches.end(), ruby_mismatches.begin(), ruby_mismatches.end());
    }

    if (target_lang != "zig") {
        auto zig_mismatches = detectZigInOther(code, target_lang);
        mismatches.insert(mismatches.end(), zig_mismatches.begin(), zig_mismatches.end());
    }

    if (target_lang != "julia") {
        auto julia_mismatches = detectJuliaInOther(code, target_lang);
        mismatches.insert(mismatches.end(), julia_mismatches.begin(), julia_mismatches.end());
    }

    return mismatches;
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detectPythonInOther(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    if (std::regex_search(code, python_patterns_.list_comprehension)) {
        mismatches.push_back(createMismatch(
            target_lang, "python",
            "List comprehension [x for x in y]",
            "Use " + target_lang + " equivalent loop or map/filter",
            85
        ));
    }

    if (std::regex_search(code, python_patterns_.triple_quote)) {
        mismatches.push_back(createMismatch(
            target_lang, "python",
            "Triple-quoted string ''' or \"\"\"",
            "Use " + target_lang + " multi-line string syntax",
            90
        ));
    }

    if (std::regex_search(code, python_patterns_.python_with)) {
        mismatches.push_back(createMismatch(
            target_lang, "python",
            "Python 'with ... as' context manager",
            "Use " + target_lang + " equivalent resource management",
            80
        ));
    }

    return mismatches;
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detectJavaScriptInOther(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    if (std::regex_search(code, js_patterns_.arrow_function)) {
        mismatches.push_back(createMismatch(
            target_lang, "javascript",
            "Arrow function =>",
            "Use " + target_lang + " function syntax",
            95
        ));
    }

    if (std::regex_search(code, js_patterns_.const_let)) {
        mismatches.push_back(createMismatch(
            target_lang, "javascript",
            "JavaScript const/let declaration",
            "Use " + target_lang + " variable declaration syntax",
            85
        ));
    }

    if (std::regex_search(code, js_patterns_.promise_chain)) {
        mismatches.push_back(createMismatch(
            target_lang, "javascript",
            "Promise chain .then().catch()",
            "Use " + target_lang + " async/promise equivalent",
            90
        ));
    }

    if (std::regex_search(code, js_patterns_.template_literal)) {
        mismatches.push_back(createMismatch(
            target_lang, "javascript",
            "Template literal `${var}`",
            "Use " + target_lang + " string interpolation",
            85
        ));
    }

    return mismatches;
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detectGoInOther(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    if (std::regex_search(code, go_patterns_.defer_stmt)) {
        mismatches.push_back(createMismatch(
            target_lang, "go",
            "Go defer statement",
            "Use " + target_lang + " cleanup/finally equivalent",
            90
        ));
    }

    if (std::regex_search(code, go_patterns_.channel_op)) {
        mismatches.push_back(createMismatch(
            target_lang, "go",
            "Go channel operator <-",
            "Use " + target_lang + " message passing equivalent",
            95
        ));
    }

    if (std::regex_search(code, go_patterns_.goroutine)) {
        mismatches.push_back(createMismatch(
            target_lang, "go",
            "Go goroutine 'go func()'",
            "Use " + target_lang + " threading/async equivalent",
            85
        ));
    }

    return mismatches;
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detectShellInOther(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    if (std::regex_search(code, shell_patterns_.command_sub)) {
        mismatches.push_back(createMismatch(
            target_lang, "shell",
            "Shell command substitution $()",
            "Use " + target_lang + " subprocess/exec equivalent",
            85
        ));
    }

    if (std::regex_search(code, shell_patterns_.variable_expansion)) {
        mismatches.push_back(createMismatch(
            target_lang, "shell",
            "Shell variable expansion ${}",
            "Use " + target_lang + " variable syntax",
            80
        ));
    }

    return mismatches;
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detectRustInOther(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    if (std::regex_search(code, rust_patterns_.match_expr)) {
        mismatches.push_back(createMismatch(
            target_lang, "rust",
            "Rust match expression",
            "Use " + target_lang + " switch/case equivalent",
            85
        ));
    }

    if (std::regex_search(code, rust_patterns_.lifetime)) {
        mismatches.push_back(createMismatch(
            target_lang, "rust",
            "Rust lifetime annotation '<a>",
            "Lifetimes are Rust-specific, remove annotation",
            95
        ));
    }

    if (std::regex_search(code, rust_patterns_.borrow_operator)) {
        mismatches.push_back(createMismatch(
            target_lang, "rust",
            "Rust borrow operator & or &mut",
            "Use " + target_lang + " reference/pointer syntax",
            80
        ));
    }

    if (std::regex_search(code, rust_patterns_.result_unwrap)) {
        mismatches.push_back(createMismatch(
            target_lang, "rust",
            "Rust .unwrap() or .expect()",
            "Use " + target_lang + " error handling",
            85
        ));
    }

    return mismatches;
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detectRubyInOther(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    if (std::regex_search(code, ruby_patterns_.symbol)) {
        mismatches.push_back(createMismatch(
            target_lang, "ruby",
            "Ruby symbol :symbol",
            "Use " + target_lang + " string/enum equivalent",
            75
        ));
    }

    if (std::regex_search(code, ruby_patterns_.block_do)) {
        mismatches.push_back(createMismatch(
            target_lang, "ruby",
            "Ruby block 'do |x| ... end'",
            "Use " + target_lang + " lambda/function syntax",
            85
        ));
    }

    if (std::regex_search(code, ruby_patterns_.string_interpolation)) {
        mismatches.push_back(createMismatch(
            target_lang, "ruby",
            "Ruby string interpolation #{}",
            "Use " + target_lang + " string formatting",
            80
        ));
    }

    if (std::regex_search(code, ruby_patterns_.safe_navigation)) {
        mismatches.push_back(createMismatch(
            target_lang, "ruby",
            "Ruby safe navigation operator ?.",
            "Use " + target_lang + " optional chaining",
            75
        ));
    }

    return mismatches;
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detectZigInOther(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    if (std::regex_search(code, zig_patterns_.comptime)) {
        mismatches.push_back(createMismatch(
            target_lang, "zig",
            "Zig comptime keyword",
            "Use " + target_lang + " compile-time equivalent",
            90
        ));
    }

    if (std::regex_search(code, zig_patterns_.error_union)) {
        mismatches.push_back(createMismatch(
            target_lang, "zig",
            "Zig error union !T",
            "Use " + target_lang + " error handling",
            85
        ));
    }

    if (std::regex_search(code, zig_patterns_.anytype)) {
        mismatches.push_back(createMismatch(
            target_lang, "zig",
            "Zig anytype generic",
            "Use " + target_lang + " generic syntax",
            80
        ));
    }

    return mismatches;
}

std::vector<LanguageMismatch> LanguageMismatchDetector::detectJuliaInOther(
    const std::string& code,
    const std::string& target_lang
) const {
    std::vector<LanguageMismatch> mismatches;

    if (std::regex_search(code, julia_patterns_.broadcast_operator)) {
        mismatches.push_back(createMismatch(
            target_lang, "julia",
            "Julia broadcast operator .+, .-, etc.",
            "Use " + target_lang + " element-wise operations",
            85
        ));
    }

    if (std::regex_search(code, julia_patterns_.type_annotation)) {
        mismatches.push_back(createMismatch(
            target_lang, "julia",
            "Julia type annotation x::Type",
            "Use " + target_lang + " type annotation syntax",
            80
        ));
    }

    return mismatches;
}

LanguageMismatch LanguageMismatchDetector::createMismatch(
    const std::string& target_lang,
    const std::string& source_lang,
    const std::string& pattern,
    const std::string& suggestion,
    int confidence
) const {
    LanguageMismatch mismatch;
    mismatch.target_lang = target_lang;
    mismatch.source_lang = source_lang;
    mismatch.pattern_detected = pattern;
    mismatch.suggestion = suggestion;
    mismatch.confidence = confidence;
    return mismatch;
}

} // namespace analyzer
} // namespace naab
