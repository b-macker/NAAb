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
// Performance Hints — superseded by scanner (string_concat_in_loop)
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectPerformanceIssues(const ast::Program& program) {
    (void)program;
    return {};
}

// ============================================================================
// Best Practice — superseded by scanner (god_functions, deep_nesting, magic_numbers, assigned_never_read)
// detectLongFunctions was broken: getFunctionLineCount() returns hardcoded 20, never triggers
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectBestPracticeIssues(const ast::Program& program) {
    (void)program;
    return {};
}

// ============================================================================
// Security — stubs never implemented, no scanner equivalent but low value
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectSecurityIssues(const ast::Program& program) {
    (void)program;
    return {};
}

// ============================================================================
// Maintainability — superseded by scanner (complex_boolean_expr)
// ============================================================================

std::vector<Diagnostic> QualityHintDetector::detectMaintainabilityIssues(const ast::Program& program) {
    (void)program;
    return {};
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

// Removed: detectMissingComments (stub), detectLongFunctions (broken helper),
// detectDeepNesting, detectMagicNumbers, detectUnusedVariables (superseded by scanner),
// detectPotentialSQLInjection, detectHardcodedSecrets, detectUnsafePolyglotUsage (stubs),
// detectComplexConditions, detectDuplicateCode (superseded by scanner)
// Removed helpers: getFunctionLineCount (hardcoded 20), getNestingDepth (hardcoded 2),
// isMagicNumber, looksLikeSQLQuery, looksLikeSecret — all unused after stub removal

// ============================================================================
// Helper Functions (still used by detectPoorNaming)
// ============================================================================

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
