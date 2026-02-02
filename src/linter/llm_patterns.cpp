#include "llm_patterns.h"
#include "naab/ast.h"
#include <fmt/core.h>
#include <algorithm>

namespace naab {
namespace linter {

// ============================================================================
// Diagnostic Implementation
// ============================================================================

std::string Diagnostic::toString() const {
    std::string result = fmt::format("{}:{}:{}: {}",
        file_path.empty() ? "<unknown>" : file_path,
        line,
        column,
        message
    );

    if (!suggestion.empty()) {
        result += "\n    Suggestion: " + suggestion;
    }

    return result;
}

std::string Diagnostic::formatWithSeverity() const {
    const char* sev_str = "";
    switch (severity) {
        case DiagnosticSeverity::Error:   sev_str = "Error"; break;
        case DiagnosticSeverity::Warning: sev_str = "Warning"; break;
        case DiagnosticSeverity::Info:    sev_str = "Info"; break;
        case DiagnosticSeverity::Hint:    sev_str = "Hint"; break;
    }

    std::string result = fmt::format("{}: {}", sev_str, message);

    if (!suggestion.empty()) {
        result += "\n    Suggestion: " + suggestion;
    }

    return result;
}

// ============================================================================
// LLMPatternDetector Implementation
// ============================================================================

LLMPatternDetector::LLMPatternDetector() {
    // Enable all patterns by default
    enabled_patterns_ = {
        "unnecessary_type_annotations",
        "redundant_null_checks",
        "overuse_of_any",
        "incorrect_error_handling",
        "polyglot_block_misuse",
        "module_import_issues",
        "async_without_implementation",
        "incorrect_main_function",
        "unquoted_dict_keys",
        "javascript_idioms",
        "python_idioms",
        "unnecessary_complexity",
    };
}

void LLMPatternDetector::enablePattern(const std::string& pattern_name) {
    if (std::find(enabled_patterns_.begin(), enabled_patterns_.end(), pattern_name) == enabled_patterns_.end()) {
        enabled_patterns_.push_back(pattern_name);
    }
}

void LLMPatternDetector::disablePattern(const std::string& pattern_name) {
    enabled_patterns_.erase(
        std::remove(enabled_patterns_.begin(), enabled_patterns_.end(), pattern_name),
        enabled_patterns_.end()
    );
}

std::vector<Diagnostic> LLMPatternDetector::detectPatterns(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Run each enabled pattern detector
    auto check_pattern = [&](const std::string& name, auto detector_fn) {
        if (std::find(enabled_patterns_.begin(), enabled_patterns_.end(), name) != enabled_patterns_.end()) {
            auto results = detector_fn(program);
            diagnostics.insert(diagnostics.end(), results.begin(), results.end());
        }
    };

    check_pattern("unnecessary_type_annotations", [this](const auto& p) { return detectUnnecessaryTypeAnnotations(p); });
    check_pattern("polyglot_block_misuse", [this](const auto& p) { return detectPolyglotBlockMisuse(p); });
    check_pattern("module_import_issues", [this](const auto& p) { return detectModuleImportIssues(p); });
    check_pattern("async_without_implementation", [this](const auto& p) { return detectAsyncWithoutImplementation(p); });
    check_pattern("javascript_idioms", [this](const auto& p) { return detectJavaScriptIdioms(p); });

    return diagnostics;
}

// ============================================================================
// Pattern Detectors
// ============================================================================

std::vector<Diagnostic> LLMPatternDetector::detectUnnecessaryTypeAnnotations(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check all variable declarations
    // In a full implementation, we would traverse the AST and check each VarDeclStmt
    // For now, return empty diagnostics (placeholder)

    // Example diagnostic (commented out - needs AST traversal):
    // diagnostics.push_back(Diagnostic(
    //     DiagnosticSeverity::Hint,
    //     "Unnecessary type annotation - type can be inferred",
    //     "Remove ': int' - the type is clear from the initializer",
    //     "file.naab",
    //     10,
    //     5
    // ));

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectRedundantNullChecks(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for patterns like:
    // if x != null {
    //     if x != null {  // Redundant!
    //         ...
    //     }
    // }

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectOveruseOfAny(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for excessive use of 'any' type
    // This often indicates LLMs being too generic

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectIncorrectErrorHandling(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for:
    // - Empty catch blocks
    // - Catch-all error handling without logging
    // - Swallowing exceptions

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectPolyglotBlockMisuse(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Common mistakes:
    // 1. Missing variable list: <<python data.mean() >>
    //    Should be: <<python[data] data.mean() >>
    //
    // 2. Wrong variable list syntax
    //
    // 3. Trying to use async in polyglot blocks

    // Example diagnostic:
    diagnostics.push_back(Diagnostic(
        DiagnosticSeverity::Warning,
        "Polyglot block missing variable list",
        "Add variables in brackets: <<python[data] ...>>",
        "",
        0,
        0
    ));

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectModuleImportIssues(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for:
    // - Using 'import' instead of 'use'
    // - Incorrect import syntax from JavaScript/Python

    // Example:
    // import io from "std"  // ❌ Wrong
    // use io                // ✅ Correct

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectAsyncWithoutImplementation(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Check all functions
    const auto& functions = program.getFunctions();

    for (const auto& func : functions) {
        if (isAsyncFunction(*func)) {
            // Warn that async/await is not yet implemented
            diagnostics.push_back(Diagnostic(
                DiagnosticSeverity::Warning,
                fmt::format("Function '{}' uses async keyword", func->getName()),
                "async/await is not yet fully implemented. Consider using polyglot blocks for async operations.",
                "",
                func->getLocation().line,
                func->getLocation().column
            ));
        }
    }

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectIncorrectMainFunction(const ast::Program& program) {
    std::vector<Diagnostic> diagnostics;

    // Check for 'fn main()' pattern
    // This is a common mistake from C/Rust/Go programmers

    const auto& functions = program.getFunctions();
    for (const auto& func : functions) {
        if (func->getName() == "main") {
            diagnostics.push_back(Diagnostic(
                DiagnosticSeverity::Error,
                "Incorrect entry point: NAAb uses 'main {}' block, not 'fn main()'",
                "Change 'fn main() { ... }' to 'main { ... }'",
                "",
                func->getLocation().line,
                func->getLocation().column
            ));
        }
    }

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectUnquotedDictKeys(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for unquoted dictionary keys
    // This is common in JavaScript/Python code generation

    // Example:
    // {name: "Alice"}     // ❌ Wrong
    // {"name": "Alice"}   // ✅ Correct

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectJavaScriptIdioms(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for common JavaScript patterns that don't work in NAAb:
    // - const instead of let
    // - var instead of let
    // - import/export syntax
    // - === instead of ==
    // - undefined instead of null

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectPythonIdioms(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for common Python patterns:
    // - Using 'def' instead of 'fn'
    // - Using 'None' instead of 'null'
    // - Using ':' for type annotations in wrong places

    return diagnostics;
}

std::vector<Diagnostic> LLMPatternDetector::detectUnnecessaryComplexity(const ast::Program& program) {
    (void)program;  // Full implementation requires AST traversal
    std::vector<Diagnostic> diagnostics;

    // Check for:
    // - Overly long functions (>50 lines)
    // - Deeply nested conditionals (>4 levels)
    // - Unused variables
    // - Redundant conditionals

    return diagnostics;
}

// ============================================================================
// Helper Functions
// ============================================================================

bool LLMPatternDetector::hasTypeAnnotation(const ast::VarDeclStmt& var_decl) const {
    return var_decl.getType().has_value();
}

bool LLMPatternDetector::isTypeInferable(const ast::VarDeclStmt& var_decl) const {
    // Check if the initializer has an obvious type
    auto* init = var_decl.getInit();
    if (!init) return false;

    // Literal expressions are always inferable
    if (init->getKind() == ast::NodeKind::LiteralExpr) {
        return true;
    }

    // Struct literals are inferable
    if (init->getKind() == ast::NodeKind::StructLiteralExpr) {
        return true;
    }

    return false;
}

bool LLMPatternDetector::isAsyncFunction(const ast::FunctionDecl& func) const {
    (void)func;  // Unused - async detection not yet implemented
    return false;  // Async not yet in AST
}

bool LLMPatternDetector::hasPolyglotBlock(const ast::Node& node) const {
    (void)node;  // Unused - polyglot detection not yet implemented
    // Check if node or its children contain InlineCodeExpr
    // Would need AST traversal
    return false;
}

bool LLMPatternDetector::looksLikeJavaScriptImport(const ast::ModuleUseStmt& stmt) const {
    (void)stmt;  // Unused - detection not yet implemented
    // Check for patterns like "import ... from ..."
    // In a full implementation, we'd check the statement structure
    return false;
}

bool LLMPatternDetector::looksLikePythonIdiom(const ast::Node& node) const {
    (void)node;  // Unused - detection not yet implemented
    // Check for Python-specific patterns
    return false;
}

} // namespace linter
} // namespace naab
