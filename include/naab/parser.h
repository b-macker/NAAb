#ifndef NAAB_PARSER_H
#define NAAB_PARSER_H

// NAAb Block Assembly Language - Parser
// Recursive descent parser assembled from LLVM patterns

#include "naab/ast.h"
#include "naab/lexer.h"
#include "naab/error_reporter.h"
#include <memory>
#include <vector>
#include <stdexcept>
#include <unordered_set>
#include <optional>

namespace naab {
namespace parser {

// Forward declaration for Phase 2.1: Enhanced error hints
struct ParserContext;

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

class Parser {
public:
    explicit Parser(const std::vector<lexer::Token>& tokens);
    ~Parser();  // Explicitly declared destructor (required for unique_ptr<ParserContext>)

    // Parse complete program
    std::unique_ptr<ast::Program> parseProgram();

    // Parse a single expression (useful for REPL, debugger conditions, etc.)
    std::unique_ptr<ast::Expr> parseExpression();

    // Set source code for error reporting (Phase 1.3)
    void setSource(const std::string& source_code, const std::string& filename = "");

    // Get error reporter for diagnostics
    const error::ErrorReporter& getErrorReporter() const { return error_reporter_; }

private:
    const std::vector<lexer::Token>& tokens_;
    size_t pos_;
    error::ErrorReporter error_reporter_;  // Phase 1.3: Enhanced error reporting
    std::string filename_;  // Phase 3.1: Track filename for source locations
    std::unordered_set<std::string> enum_names_;  // Phase 4.1: Track defined enum names
    mutable std::optional<lexer::Token> pending_token_;  // For splitting >> into > > in nested generics
    mutable lexer::Token stored_gt_token_;  // Storage for first > when splitting >>

    // Brace tracking for better "Expected '}'" error messages
    std::vector<size_t> brace_stack_;  // Stack of line numbers where '{' was opened

    // Phase 2.1: Parser context for enhanced error hints
    std::unique_ptr<ParserContext> parser_context_;

    // Week 1, Task 1.3: Track recursion depth to prevent stack overflow
    size_t parse_depth_ = 0;

    // Helper class for automatic depth tracking
    class DepthGuard {
    public:
        explicit DepthGuard(size_t& depth);
        ~DepthGuard();
    private:
        size_t& depth_;
    };

    // Token navigation
    const lexer::Token& current() const;
    const lexer::Token& peek(int offset = 1) const;
    bool isAtEnd() const;
    void advance();
    bool match(lexer::TokenType type);
    bool check(lexer::TokenType type) const;
    const lexer::Token& expect(lexer::TokenType type, const std::string& msg);
    const lexer::Token& expectGTOrSplitGTGT(const std::string& msg);  // Helper for nested generics

    // Parsing methods
    std::unique_ptr<ast::UseStatement> parseUseStatement();
    std::unique_ptr<ast::ModuleUseStmt> parseModuleUseStmt();  // Phase 4.0
    std::unique_ptr<ast::ImportStmt> parseImportStmt();    // Phase 3.1
    std::unique_ptr<ast::ExportStmt> parseExportStmt();    // Phase 3.1
    std::unique_ptr<ast::FunctionDecl> parseFunctionDecl();
    std::unique_ptr<ast::StructDecl> parseStructDecl();
    std::unique_ptr<ast::EnumDecl> parseEnumDecl();  // Phase 2.4.3
    std::unique_ptr<ast::StructLiteralExpr> parseStructLiteral(const std::string& struct_name);
    std::unique_ptr<ast::MainBlock> parseMainBlock();

    // Statements
    std::unique_ptr<ast::Stmt> parseStatement();
    std::unique_ptr<ast::CompoundStmt> parseCompoundStmt();
    std::unique_ptr<ast::ReturnStmt> parseReturnStmt();
    std::unique_ptr<ast::BreakStmt> parseBreakStmt();
    std::unique_ptr<ast::ContinueStmt> parseContinueStmt();
    std::unique_ptr<ast::IfStmt> parseIfStmt();
    std::unique_ptr<ast::ForStmt> parseForStmt();
    std::unique_ptr<ast::WhileStmt> parseWhileStmt();
    std::unique_ptr<ast::TryStmt> parseTryStmt();          // Phase 4.1
    std::unique_ptr<ast::ThrowStmt> parseThrowStmt();      // Phase 4.1
    std::unique_ptr<ast::VarDeclStmt> parseVarDeclStmt();
    std::unique_ptr<ast::ExprStmt> parseExprStmt();

    // Expressions
    std::unique_ptr<ast::Expr> parseAssignment();
    std::unique_ptr<ast::Expr> parsePipeline();  // Phase 3.4
    std::unique_ptr<ast::Expr> parseLogicalOr();
    std::unique_ptr<ast::Expr> parseLogicalAnd();
    std::unique_ptr<ast::Expr> parseEquality();
    std::unique_ptr<ast::Expr> parseRange();
    std::unique_ptr<ast::Expr> parseComparison();
    std::unique_ptr<ast::Expr> parseTerm();
    std::unique_ptr<ast::Expr> parseFactor();
    std::unique_ptr<ast::Expr> parseUnary();
    std::unique_ptr<ast::Expr> parsePostfix();
    std::unique_ptr<ast::Expr> parsePrimary();
    std::unique_ptr<ast::Expr> parseIfExpr();
    std::unique_ptr<ast::Expr> parseLambdaExpr();

    // Type parsing
    ast::Type parseType();
    ast::Type parseBaseType();  // Phase 2.4.2: Parse non-union type

    // Helpers
    void skipNewlines();
    void optionalSemicolon();  // Allow optional semicolon after statements
    std::string formatLocation(int line, int column);
    std::string formatError(const std::string& msg, const lexer::Token& token);

    // Phase 2.1: Parser context helpers for enhanced error hints
    void updateParserContext();
    void setContextFlag(bool& flag, bool value);
    std::vector<std::string> getErrorHints(const lexer::Token& unexpected, const std::string& expected);
};

} // namespace parser
} // namespace naab

#endif // NAAB_PARSER_H
