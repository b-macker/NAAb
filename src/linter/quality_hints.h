#pragma once

#include "llm_patterns.h"
#include <string>
#include <vector>
#include <memory>

// Forward declarations
namespace naab {
namespace ast {
    class Program;
    class Node;
    class FunctionDecl;
    class ForStmt;
    class WhileStmt;
    class IfStmt;
    class BinaryExpr;
    class CallExpr;
}
}

namespace naab {
namespace linter {

// Code quality hint categories
enum class HintCategory {
    Performance,     // Performance improvements
    BestPractice,    // Best practice recommendations
    Security,        // Security concerns
    Maintainability, // Code maintainability
    Readability      // Code readability
};

// Code quality hint detector
class QualityHintDetector {
public:
    QualityHintDetector();

    // Main entry point: detect all quality issues
    std::vector<Diagnostic> detectHints(const ast::Program& program);

    // Enable/disable specific categories
    void enableCategory(HintCategory category);
    void disableCategory(HintCategory category);

private:
    // Performance hints
    std::vector<Diagnostic> detectPerformanceIssues(const ast::Program& program);
    std::vector<Diagnostic> detectInefficientLoops(const ast::Program& program);
    std::vector<Diagnostic> detectRedundantOperations(const ast::Program& program);
    std::vector<Diagnostic> detectStringConcatenationInLoop(const ast::Program& program);

    // Best practice hints
    std::vector<Diagnostic> detectBestPracticeIssues(const ast::Program& program);
    std::vector<Diagnostic> detectLongFunctions(const ast::Program& program);
    std::vector<Diagnostic> detectDeepNesting(const ast::Program& program);
    std::vector<Diagnostic> detectMagicNumbers(const ast::Program& program);
    std::vector<Diagnostic> detectUnusedVariables(const ast::Program& program);

    // Security hints
    std::vector<Diagnostic> detectSecurityIssues(const ast::Program& program);
    std::vector<Diagnostic> detectPotentialSQLInjection(const ast::Program& program);
    std::vector<Diagnostic> detectUnsafePolyglotUsage(const ast::Program& program);
    std::vector<Diagnostic> detectHardcodedSecrets(const ast::Program& program);

    // Maintainability hints
    std::vector<Diagnostic> detectMaintainabilityIssues(const ast::Program& program);
    std::vector<Diagnostic> detectComplexConditions(const ast::Program& program);
    std::vector<Diagnostic> detectDuplicateCode(const ast::Program& program);

    // Readability hints
    std::vector<Diagnostic> detectReadabilityIssues(const ast::Program& program);
    std::vector<Diagnostic> detectPoorNaming(const ast::Program& program);
    std::vector<Diagnostic> detectMissingComments(const ast::Program& program);

    // Helper functions
    size_t getFunctionLineCount(const ast::FunctionDecl& func) const;
    size_t getNestingDepth(const ast::Node& node) const;
    bool isMagicNumber(const std::string& value) const;
    bool looksLikeSQLQuery(const std::string& str) const;
    bool looksLikeSecret(const std::string& str) const;
    bool hasGoodVariableName(const std::string& name) const;

    // Enabled categories
    std::vector<HintCategory> enabled_categories_;
};

} // namespace linter
} // namespace naab

