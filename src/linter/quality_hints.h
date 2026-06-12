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
    // Category dispatchers (Performance/BestPractice/Security/Maintainability
    // return empty — superseded by scanner checks)
    std::vector<Diagnostic> detectPerformanceIssues(const ast::Program& program);
    std::vector<Diagnostic> detectBestPracticeIssues(const ast::Program& program);
    std::vector<Diagnostic> detectSecurityIssues(const ast::Program& program);
    std::vector<Diagnostic> detectMaintainabilityIssues(const ast::Program& program);

    // Readability — working
    std::vector<Diagnostic> detectReadabilityIssues(const ast::Program& program);
    std::vector<Diagnostic> detectPoorNaming(const ast::Program& program);

    // Helper functions
    bool hasGoodVariableName(const std::string& name) const;

    // Enabled categories
    std::vector<HintCategory> enabled_categories_;
};

} // namespace linter
} // namespace naab

