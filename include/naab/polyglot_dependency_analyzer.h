#pragma once
// NAAb Polyglot Dependency Analyzer
// Analyzes dependencies between polyglot blocks to enable parallel execution
// Part of parallel polyglot execution implementation


#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace naab {
namespace ast {
    class Stmt;
    class InlineCodeExpr;
}

namespace interpreter {

// Represents a single polyglot block with its dependencies
struct PolyglotBlock {
    ast::Stmt* statement;                    // The full statement (VarDeclStmt or ExprStmt)
    ast::InlineCodeExpr* node;               // The actual InlineCodeExpr AST node
    std::string assigned_var;                // Variable being assigned to (or empty)
    std::vector<std::string> read_vars;      // Variables used as inputs (from bound_variables)
    std::vector<std::string> write_vars;     // Variables modified (the assigned var)
    size_t statement_index;                  // Position in source code for ordering

    PolyglotBlock()
        : statement(nullptr), node(nullptr), assigned_var(""), statement_index(0) {}
};

// Represents a group of polyglot blocks that can execute in parallel
struct DependencyGroup {
    std::vector<PolyglotBlock> parallel_blocks;  // Blocks that can execute together
    std::vector<size_t> depends_on_groups;       // Group indices that must complete first

    DependencyGroup() = default;
};

// Analyzes dependencies between polyglot blocks to determine parallel execution groups
class PolyglotDependencyAnalyzer {
public:
    PolyglotDependencyAnalyzer() = default;
    ~PolyglotDependencyAnalyzer() = default;

    // Analyze a sequence of statements and group polyglot blocks by dependencies
    // Returns groups that should execute sequentially (parallel within each group)
    std::vector<DependencyGroup> analyze(const std::vector<ast::Stmt*>& statements);

    // Check if two blocks have any dependency (for testing/debugging)
    bool hasDependency(const PolyglotBlock& a, const PolyglotBlock& b) const;

private:
    // Check for Read-After-Write dependency: b reads what a writes
    bool hasDataDependency(const PolyglotBlock& a, const PolyglotBlock& b) const;

    // Check for Write-After-Write dependency: both write to same variable
    bool hasOutputDependency(const PolyglotBlock& a, const PolyglotBlock& b) const;

    // Check for Write-After-Read dependency: b writes what a reads
    bool hasAntiDependency(const PolyglotBlock& a, const PolyglotBlock& b) const;

    // Extract polyglot blocks from statements
    std::vector<PolyglotBlock> extractPolyglotBlocks(const std::vector<ast::Stmt*>& statements);

    // Build dependency groups from independent blocks using greedy algorithm
    std::vector<DependencyGroup> buildDependencyGroups(const std::vector<PolyglotBlock>& blocks);
};

} // namespace interpreter
} // namespace naab

