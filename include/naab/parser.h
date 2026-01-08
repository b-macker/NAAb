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

namespace naab {
namespace parser {

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

class Parser {
public:
    explicit Parser(const std::vector<lexer::Token>& tokens);

    // Parse complete program
    std::unique_ptr<ast::Program> parseProgram();

    // Set source code for error reporting (Phase 1.3)
    void setSource(const std::string& source_code, const std::string& filename = "");

    // Get error reporter for diagnostics
    const error::ErrorReporter& getErrorReporter() const { return error_reporter_; }

private:
    const std::vector<lexer::Token>& tokens_;
    size_t pos_;
    error::ErrorReporter error_reporter_;  // Phase 1.3: Enhanced error reporting

    // Token navigation
    const lexer::Token& current() const;
    const lexer::Token& peek(int offset = 1) const;
    bool isAtEnd() const;
    void advance();
    bool match(lexer::TokenType type);
    bool check(lexer::TokenType type) const;
    const lexer::Token& expect(lexer::TokenType type, const std::string& msg);

    // Parsing methods
    std::unique_ptr<ast::UseStatement> parseUseStatement();
    std::unique_ptr<ast::ImportStmt> parseImportStmt();    // Phase 3.1
    std::unique_ptr<ast::ExportStmt> parseExportStmt();    // Phase 3.1
    std::unique_ptr<ast::FunctionDecl> parseFunctionDecl();
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
    std::unique_ptr<ast::Expr> parseExpression();
    std::unique_ptr<ast::Expr> parseAssignment();
    std::unique_ptr<ast::Expr> parsePipeline();  // Phase 3.4
    std::unique_ptr<ast::Expr> parseLogicalOr();
    std::unique_ptr<ast::Expr> parseLogicalAnd();
    std::unique_ptr<ast::Expr> parseEquality();
    std::unique_ptr<ast::Expr> parseComparison();
    std::unique_ptr<ast::Expr> parseTerm();
    std::unique_ptr<ast::Expr> parseFactor();
    std::unique_ptr<ast::Expr> parseUnary();
    std::unique_ptr<ast::Expr> parsePostfix();
    std::unique_ptr<ast::Expr> parsePrimary();

    // Type parsing
    ast::Type parseType();

    // Helpers
    void skipNewlines();
    void optionalSemicolon();  // Allow optional semicolon after statements
    std::string formatError(const std::string& msg, const lexer::Token& token);
};

} // namespace parser
} // namespace naab

#endif // NAAB_PARSER_H
