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
    // Removed: redundant_null_checks, overuse_of_any, incorrect_error_handling,
    //          unquoted_dict_keys, unnecessary_complexity — superseded by scanner or dead
    enabled_patterns_ = {
        "incorrect_main_function",
        "async_without_implementation",
        "unnecessary_type_annotations",
        "polyglot_block_misuse",
        "module_import_issues",
        "javascript_idioms",
        "python_idioms",
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

    check_pattern("incorrect_main_function", [this](const auto& p) { return detectIncorrectMainFunction(p); });
    check_pattern("async_without_implementation", [this](const auto& p) { return detectAsyncWithoutImplementation(p); });
    // STUB detectors — NAAb-specific checks that need AST traversal (not superseded by scanner)
    check_pattern("unnecessary_type_annotations", [this](const auto& p) { return detectUnnecessaryTypeAnnotations(p); });
    check_pattern("polyglot_block_misuse", [this](const auto& p) { return detectPolyglotBlockMisuse(p); });
    check_pattern("module_import_issues", [this](const auto& p) { return detectModuleImportIssues(p); });
    check_pattern("javascript_idioms", [this](const auto& p) { return detectJavaScriptIdioms(p); });

    return diagnostics;
}

// ============================================================================
// Pattern Detectors
// ============================================================================

std::vector<Diagnostic> LLMPatternDetector::detectUnnecessaryTypeAnnotations(const ast::Program& program) {
    // V-LN-001: STUB — not yet implemented (requires AST traversal of VarDeclStmt type annotations)
    (void)program;
    return {};  // Returns empty: no false positives, but no detection either
}

// Removed: detectRedundantNullChecks, detectOveruseOfAny, detectIncorrectErrorHandling
// — superseded by scanner checks (empty_catch, catch_and_ignore) or never dispatched

std::vector<Diagnostic> LLMPatternDetector::detectPolyglotBlockMisuse(const ast::Program& program) {
    // V-LN-001: STUB — not yet implemented (requires AST traversal for variable list + async checks)
    (void)program;
    return {};
}

std::vector<Diagnostic> LLMPatternDetector::detectModuleImportIssues(const ast::Program& program) {
    // V-LN-001: STUB — not yet implemented (requires AST traversal for JS/Python import syntax)
    (void)program;
    return {};
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

// Removed: detectUnquotedDictKeys — NAAb supports bare dict keys by design, not a bug

std::vector<Diagnostic> LLMPatternDetector::detectJavaScriptIdioms(const ast::Program& program) {
    // V-LN-001: STUB — not yet implemented (requires AST traversal for const/var/===/ undefined)
    (void)program;
    return {};
}

std::vector<Diagnostic> LLMPatternDetector::detectPythonIdioms(const ast::Program& program) {
    // V-LN-001: STUB — not yet implemented (requires AST traversal for def/None/: annotations)
    (void)program;
    return {};
}

// Removed: detectUnnecessaryComplexity — superseded by scanner (god_functions + deep_nesting)

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
