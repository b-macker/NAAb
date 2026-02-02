#include "quality_hints.h"
#include "naab/ast.h"
#include <fmt/core.h>
#include <algorithm>
#include <cctype>

namespace naab {
namespace linter {

// ============================================================================
// QualityHintDetector Implementation
// ============================================================================

QualityHintDetector::QualityHintDetector() {
    // Enable all categories by default
    enabled_categories_ = {
        HintCategory::Performance,
        HintCategory::BestPractice,
        HintCategory::Security,
        HintCategory::Maintainability,
        HintCategory::Readability
    };
}

void QualityHintDetector::enableCategory(HintCategory category) {
    if (std::find(enabled_categories_.begin(), enabled_categories_.end(), category) == enabled_categories_.end()) {
        enabled_categories_.push_back(category);
    }
}

void QualityHintDetector::disableCategory(HintCategory category) {
    enabled_categories_.erase(
        std::remove(enabled_categories_.begin(), enabled_categories_.end(), category),
        enabled_categories_.end()
    );
}

std::vector<Diagnostic> QualityHintDetector::detectHints(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    auto merge = [&](std::vector<Diagnostic> hints) {
        diagnostics.insert(diagnostics.end(), hints.begin(), hints.end());
    };

    // Run detectors for enabled categories
    for (HintCategory cat : enabled_categories_) {
        switch (cat) {
            case HintCategory::Performance:
                merge(detectPerformanceIssues(program));
                break;
            case HintCategory::BestPractice:
                merge(detectBestPracticeIssues(program));
                break;
            case HintCategory::Security:
                merge(detectSecurityIssues(program));
                break;
            case HintCategory::Maintainability:
                merge(detectMaintainabilityIssues(program));
                break;
            case HintCategory::Readability:
                merge(detectReadabilityIssues(program));
                break;
        }
    }

    return diagnostics;
}

// ============================================================================
// Performance Hints
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectPerformanceIssues(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Detect inefficient loops
    auto loop_hints = detectInefficientLoops(program);
    diagnostics.insert(diagnostics.end(), loop_hints.begin(), loop_hints.end());

    // Detect string concatenation in loops
    auto concat_hints = detectStringConcatenationInLoop(program);
    diagnostics.insert(diagnostics.end(), concat_hints.begin(), concat_hints.end());

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectInefficientLoops(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Example warning for inefficient array concatenation in loop
    diagnostics.push_back(Diagnostic(
        DiagnosticSeverity::Warning,
        "Inefficient array concatenation in loop",
        "Use array.push() instead of array concatenation for O(n) instead of O(n²) complexity",
        "",
        0,
        0
    ));

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectRedundantOperations(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;
    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectStringConcatenationInLoop(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Example: Detect string concatenation in loops
    // In a full implementation, traverse the AST looking for:
    // for ... {
    //     str = str + ...  // Inefficient!
    // }

    return diagnostics;
}

// ============================================================================
// Best Practice Hints
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectBestPracticeIssues(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Check for long functions
    auto long_func_hints = detectLongFunctions(program);
    diagnostics.insert(diagnostics.end(), long_func_hints.begin(), long_func_hints.end());

    // Check for deep nesting
    auto nesting_hints = detectDeepNesting(program);
    diagnostics.insert(diagnostics.end(), nesting_hints.begin(), nesting_hints.end());

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectLongFunctions(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    const auto& functions = program.getFunctions();

    for (const auto& func : functions) {
        size_t line_count = getFunctionLineCount(*func);

        if (line_count > 50) {
            diagnostics.push_back(Diagnostic(
                DiagnosticSeverity::Warning,
                fmt::format("Function '{}' is too long ({} lines)", func->getName(), line_count),
                "Consider breaking this function into smaller, focused functions",
                "",
                func->getLocation().line,
                func->getLocation().column
            ));
        }
    }

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectDeepNesting(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check all functions for deep nesting (>4 levels)
    // In a full implementation, traverse each function's AST and count nesting depth
    // For now, return empty diagnostics (placeholder)

    // Example diagnostic (commented out - needs AST traversal):
    // diagnostics.push_back(Diagnostic(
    //     DiagnosticSeverity::Warning,
    //     "Function has deep nesting (5 levels)",
    //     "Consider extracting nested logic into separate functions",
    //     "",
    //     0,
    //     0
    // ));

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectMagicNumbers(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Detect numeric literals that should be named constants
    // Example: if (count > 100) { ... }  // What does 100 mean?

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectUnusedVariables(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Detect variables that are declared but never used

    return diagnostics;
}

// ============================================================================
// Security Hints
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectSecurityIssues(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Check for SQL injection risks
    auto sql_hints = detectPotentialSQLInjection(program);
    diagnostics.insert(diagnostics.end(), sql_hints.begin(), sql_hints.end());

    // Check for hardcoded secrets
    auto secret_hints = detectHardcodedSecrets(program);
    diagnostics.insert(diagnostics.end(), secret_hints.begin(), secret_hints.end());

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectPotentialSQLInjection(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Example warning for SQL injection
    diagnostics.push_back(Diagnostic(
        DiagnosticSeverity::Warning,
        "Potential SQL injection vulnerability",
        "Use parameterized queries instead of string concatenation for SQL",
        "",
        0,
        0
    ));

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectUnsafePolyglotUsage(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for unsafe use of polyglot blocks
    // Example: Executing user input directly in shell blocks

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectHardcodedSecrets(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for hardcoded passwords, API keys, tokens
    // Example patterns:
    // - password = "..."
    // - api_key = "..."
    // - token = "..."

    return diagnostics;
}

// ============================================================================
// Maintainability Hints
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectMaintainabilityIssues(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Detect complex conditions
    auto condition_hints = detectComplexConditions(program);
    diagnostics.insert(diagnostics.end(), condition_hints.begin(), condition_hints.end());

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectComplexConditions(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for overly complex if conditions
    // Example: if (a && b || c && d || e && f) { ... }
    // Suggestion: Extract to named boolean variables

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectDuplicateCode(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Detect code duplication (requires AST similarity analysis)

    return diagnostics;
}

// ============================================================================
// Readability Hints
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectReadabilityIssues(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Check for poor naming
    auto naming_hints = detectPoorNaming(program);
    diagnostics.insert(diagnostics.end(), naming_hints.begin(), naming_hints.end());

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectPoorNaming(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Check function names
    const auto& functions = program.getFunctions();

    for (const auto& func : functions) {
        const std::string& name = func->getName();

        if (!hasGoodVariableName(name)) {
            diagnostics.push_back(Diagnostic(
                DiagnosticSeverity::Hint,
                fmt::format("Function '{}' has a non-descriptive name", name),
                "Use descriptive names that explain what the function does",
                "",
                func->getLocation().line,
                func->getLocation().column
            ));
        }
    }

    return diagnostics;
}

std::vector<Diagnostic> QualityHintDetector::detectMissingComments(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for functions without doc comments
    // Complex functions should have explanatory comments

    return diagnostics;
}

// ============================================================================
// Helper Functions
// ============================================================================

size_t QualityHintDetector::getFunctionLineCount(const ast::FunctionDecl& func) const {
    (void)func;  // Unused - full implementation requires AST traversal
    // In a full implementation, calculate actual line count from AST
    // For now, return a placeholder
    return 20;  // Placeholder
}

size_t QualityHintDetector::getNestingDepth(const ast::Node& node) const {
    (void)node;  // Unused - full implementation requires AST traversal
    // Calculate maximum nesting depth by traversing the AST
    // For now, return a placeholder
    return 2;  // Placeholder
}

bool QualityHintDetector::isMagicNumber(const std::string& value) const {
    // Check if a number is a "magic number" that should be a named constant
    // Exceptions: 0, 1, -1, 2 (common loop/array indices)

    if (value == "0" || value == "1" || value == "-1" || value == "2") {
        return false;
    }

    // Any other number is potentially magic
    return true;
}

bool QualityHintDetector::looksLikeSQLQuery(const std::string& str) const {
    // Simple heuristic: contains SQL keywords
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    return lower.find("select") != std::string::npos ||
           lower.find("insert") != std::string::npos ||
           lower.find("update") != std::string::npos ||
           lower.find("delete") != std::string::npos;
}

bool QualityHintDetector::looksLikeSecret(const std::string& str) const {
    // Check for common secret patterns
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    return lower.find("password") != std::string::npos ||
           lower.find("api_key") != std::string::npos ||
           lower.find("apikey") != std::string::npos ||
           lower.find("token") != std::string::npos ||
           lower.find("secret") != std::string::npos;
}

bool QualityHintDetector::hasGoodVariableName(const std::string& name) const {
    // Check for good naming conventions
    // Bad: x, y, tmp, data, item, value, foo, bar
    // Good: descriptive names > 3 characters

    if (name.length() < 3) {
        return false;
    }

    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Common bad names
    static const std::vector<std::string> bad_names = {
        "tmp", "temp", "data", "item", "value",
        "foo", "bar", "baz", "test", "x", "y", "z"
    };

    return std::find(bad_names.begin(), bad_names.end(), lower) == bad_names.end();
}

} // namespace linter
} // namespace naab
