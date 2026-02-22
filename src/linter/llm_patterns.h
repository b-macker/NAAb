#pragma once

#include <string>
#include <vector>
#include <memory>

// Forward declarations
namespace naab {
namespace ast {
    class Program;
    class Node;
    class FunctionDecl;
    class VarDeclStmt;
    class InlineCodeExpr;
    class ModuleUseStmt;
}
}

namespace naab {
namespace linter {

// Diagnostic severity levels
enum class DiagnosticSeverity {
    Error,
    Warning,
    Info,
    Hint
};

// Diagnostic message
struct Diagnostic {
    DiagnosticSeverity severity;
    std::string message;
    std::string suggestion;
    std::string file_path;
    size_t line;
    size_t column;

    Diagnostic(DiagnosticSeverity sev,
               const std::string& msg,
               const std::string& sug = "",
               const std::string& file = "",
               size_t l = 0,
               size_t c = 0)
        : severity(sev), message(msg), suggestion(sug),
          file_path(file), line(l), column(c) {}

    std::string toString() const;
    std::string formatWithSeverity() const;
};

// Detector for common LLM code generation mistakes
class LLMPatternDetector {
public:
    LLMPatternDetector();

    // Main entry point: Detect all patterns in a program
    std::vector<Diagnostic> detectPatterns(const ast::Program& program);

    // Enable/disable specific pattern checks
    void enablePattern(const std::string& pattern_name);
    void disablePattern(const std::string& pattern_name);

private:
    // Pattern detectors - each returns diagnostics
    std::vector<Diagnostic> detectUnnecessaryTypeAnnotations(const ast::Program& program);
    std::vector<Diagnostic> detectRedundantNullChecks(const ast::Program& program);
    std::vector<Diagnostic> detectOveruseOfAny(const ast::Program& program);
    std::vector<Diagnostic> detectIncorrectErrorHandling(const ast::Program& program);
    std::vector<Diagnostic> detectPolyglotBlockMisuse(const ast::Program& program);
    std::vector<Diagnostic> detectModuleImportIssues(const ast::Program& program);
    std::vector<Diagnostic> detectAsyncWithoutImplementation(const ast::Program& program);
    std::vector<Diagnostic> detectIncorrectMainFunction(const ast::Program& program);
    std::vector<Diagnostic> detectUnquotedDictKeys(const ast::Program& program);
    std::vector<Diagnostic> detectJavaScriptIdioms(const ast::Program& program);
    std::vector<Diagnostic> detectPythonIdioms(const ast::Program& program);
    std::vector<Diagnostic> detectUnnecessaryComplexity(const ast::Program& program);

    // Helper functions
    bool hasTypeAnnotation(const ast::VarDeclStmt& var_decl) const;
    bool isTypeInferable(const ast::VarDeclStmt& var_decl) const;
    bool isAsyncFunction(const ast::FunctionDecl& func) const;
    bool hasPolyglotBlock(const ast::Node& node) const;
    bool looksLikeJavaScriptImport(const ast::ModuleUseStmt& stmt) const;
    bool looksLikePythonIdiom(const ast::Node& node) const;

    // Enabled patterns (for configuration)
    std::vector<std::string> enabled_patterns_;
};

} // namespace linter
} // namespace naab

