// NAAb Parser - Recursive descent parser
// Assembled from LLVM/Clang parser patterns

#include "naab/parser.h"
#include <fmt/core.h>

namespace naab {
namespace parser {

Parser::Parser(const std::vector<lexer::Token>& tokens)
    : tokens_(tokens), pos_(0) {
    skipNewlines();  // Skip leading newlines
}

void Parser::setSource(const std::string& source_code, const std::string& filename) {
    error_reporter_.setSource(source_code, filename);
}

// ============================================================================
// Token Navigation
// ============================================================================

const lexer::Token& Parser::current() const {
    if (pos_ < tokens_.size()) {
        return tokens_[pos_];
    }
    return tokens_.back();  // EOF token
}

const lexer::Token& Parser::peek(int offset) const {
    size_t peek_pos = pos_ + offset;
    if (peek_pos < tokens_.size()) {
        return tokens_[peek_pos];
    }
    return tokens_.back();  // EOF token
}

bool Parser::isAtEnd() const {
    return current().type == lexer::TokenType::END_OF_FILE;
}

void Parser::advance() {
    if (!isAtEnd()) {
        pos_++;
    }
}

bool Parser::match(lexer::TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::check(lexer::TokenType type) const {
    return current().type == type;
}

const lexer::Token& Parser::expect(lexer::TokenType type, const std::string& msg) {
    if (check(type)) {
        const auto& token = current();
        advance();
        return token;
    }

    // Phase 1.3: Use error reporter for better error messages
    const auto& token = current();
    error_reporter_.error(msg, token.line, token.column);
    error_reporter_.addSuggestion(fmt::format("Expected token type, but got: '{}'", token.value));

    throw ParseError(formatError(msg, token));
}

void Parser::skipNewlines() {
    while (match(lexer::TokenType::NEWLINE)) {
        // Skip
    }
}

void Parser::optionalSemicolon() {
    match(lexer::TokenType::SEMICOLON);  // Optional semicolon for compatibility
}

std::string Parser::formatError(const std::string& msg, const lexer::Token& token) {
    return fmt::format("Parse error at line {}, column {}: {}\n  Got: '{}'",
                      token.line, token.column, msg, token.value);
}

// ============================================================================
// Program Structure
// ============================================================================

std::unique_ptr<ast::Program> Parser::parseProgram() {
    std::vector<std::unique_ptr<ast::UseStatement>> imports;
    std::vector<std::unique_ptr<ast::FunctionDecl>> functions;
    std::unique_ptr<ast::MainBlock> main_block;

    skipNewlines();

    // Parse use statements (blocks from registry)
    // Note: import statements (file-based modules) are parsed later in the main loop
    while (check(lexer::TokenType::USE)) {
        imports.push_back(parseUseStatement());
        skipNewlines();
    }

    // Collect module imports, exports, and structs in vectors
    std::vector<std::unique_ptr<ast::ImportStmt>> module_imports;
    std::vector<std::unique_ptr<ast::ExportStmt>> exports;
    std::vector<std::unique_ptr<ast::StructDecl>> structs;

    // Parse module imports, exports, structs, and functions (Phase 3.1)
    while (!isAtEnd()) {
        skipNewlines();

        if (check(lexer::TokenType::IMPORT)) {
            auto import_stmt = parseImportStmt();
            module_imports.push_back(std::move(import_stmt));
        }
        else if (check(lexer::TokenType::EXPORT)) {
            auto export_stmt = parseExportStmt();
            exports.push_back(std::move(export_stmt));
        }
        else if (check(lexer::TokenType::STRUCT)) {
            structs.push_back(parseStructDecl());
        }
        else if (check(lexer::TokenType::FUNCTION) || check(lexer::TokenType::ASYNC)) {
            functions.push_back(parseFunctionDecl());
        }
        else if (check(lexer::TokenType::MAIN)) {
            main_block = parseMainBlock();
            break;  // Main block ends the program
        }
        else {
            // Unknown token, stop parsing
            if (!isAtEnd()) {
                auto& tok = current();
                fmt::print("[PARSER] Stopping parse at unknown token: {} (line {}, col {})\n",
                           tok.value, tok.line, tok.column);
            }
            break;
        }

        skipNewlines();
    }

    // Create final program with all components
    auto program = std::make_unique<ast::Program>(
        std::move(imports),
        std::move(functions),
        std::move(main_block)
    );

    // Add module imports, exports, and structs to the program
    for (auto& import : module_imports) {
        program->addModuleImport(std::move(import));
    }
    for (auto& export_stmt : exports) {
        program->addExport(std::move(export_stmt));
    }
    for (auto& struct_decl : structs) {
        program->addStruct(std::move(struct_decl));
    }

    return program;
}

std::unique_ptr<ast::UseStatement> Parser::parseUseStatement() {
    auto start = current();

    // Parse "use" keyword (for block registry imports)
    expect(lexer::TokenType::USE, "Expected 'use'");

    // Accept either BLOCK_ID (BLOCK-CPP-12345) or STRING ("string") or IDENTIFIER (string)
    std::string block_id;
    auto& token = current();

    if (token.type == lexer::TokenType::BLOCK_ID) {
        block_id = token.value;
        advance();
    } else if (token.type == lexer::TokenType::STRING) {
        block_id = token.value;
        advance();
    } else if (token.type == lexer::TokenType::IDENTIFIER) {
        block_id = token.value;
        advance();
    } else {
        throw std::runtime_error(
            "Expected block ID, string literal, or identifier at line " +
            std::to_string(token.line) + ", column " + std::to_string(token.column)
        );
    }

    expect(lexer::TokenType::AS, "Expected 'as'");

    auto& alias_token = expect(lexer::TokenType::IDENTIFIER, "Expected identifier");
    std::string alias = alias_token.value;

    return std::make_unique<ast::UseStatement>(
        block_id, alias,
        ast::SourceLocation(start.line, start.column)
    );
}

// Phase 3.1: Parse import statement
// import {func1, func2 as alias} from "./module.naab"
// import * as mod from "./module.naab"
std::unique_ptr<ast::ImportStmt> Parser::parseImportStmt() {
    auto start = current();
    expect(lexer::TokenType::IMPORT, "Expected 'import'");

    std::vector<ast::ImportStmt::ImportItem> items;
    bool is_wildcard = false;
    std::string wildcard_alias;
    std::string module_path;

    // Simple syntax: import "./path" as alias
    if (check(lexer::TokenType::STRING)) {
        auto& path_token = current();
        module_path = path_token.value;
        advance();
        expect(lexer::TokenType::AS, "Expected 'as' after module path");
        auto& alias_token = expect(lexer::TokenType::IDENTIFIER, "Expected alias name");
        wildcard_alias = alias_token.value;
        is_wildcard = true;  // Import whole module as single name
    }
    // Check for wildcard import: import * as mod from "./path"
    else if (check(lexer::TokenType::STAR)) {
        advance();  // consume *
        expect(lexer::TokenType::AS, "Expected 'as' after '*'");
        auto& alias_token = expect(lexer::TokenType::IDENTIFIER, "Expected alias name");
        wildcard_alias = alias_token.value;
        is_wildcard = true;

        // from "./module.naab"
        expect(lexer::TokenType::IDENTIFIER, "Expected 'from'");  // TODO: Add FROM token
        auto& path_token = expect(lexer::TokenType::STRING, "Expected module path string");
        module_path = path_token.value;
    }
    // Named imports: import {name1, name2 as alias} from "./path"
    else if (match(lexer::TokenType::LBRACE)) {
        do {
            skipNewlines();
            auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected import name");
            std::string name = name_token.value;
            std::string alias;

            // Optional alias: as newName
            if (match(lexer::TokenType::AS)) {
                auto& alias_token = expect(lexer::TokenType::IDENTIFIER, "Expected alias name");
                alias = alias_token.value;
            }

            items.emplace_back(name, alias);
            skipNewlines();
        } while (match(lexer::TokenType::COMMA));

        expect(lexer::TokenType::RBRACE, "Expected '}'");

        // from "./module.naab"
        expect(lexer::TokenType::IDENTIFIER, "Expected 'from'");  // TODO: Add FROM token
        auto& path_token = expect(lexer::TokenType::STRING, "Expected module path string");
        module_path = path_token.value;
    }
    else {
        throw ParseError("Expected string path, '{', or '*' after 'import'");
    }

    return std::make_unique<ast::ImportStmt>(
        std::move(items), module_path, is_wildcard, wildcard_alias,
        ast::SourceLocation(start.line, start.column)
    );
}

// Phase 3.1: Parse export statement
// export function foo() { ... }
// export var x = 10;
std::unique_ptr<ast::ExportStmt> Parser::parseExportStmt() {
    auto start = current();
    expect(lexer::TokenType::EXPORT, "Expected 'export'");

    // export function
    if (check(lexer::TokenType::FUNCTION)) {
        auto func = parseFunctionDecl();
        return std::make_unique<ast::ExportStmt>(
            std::move(func),
            ast::SourceLocation(start.line, start.column)
        );
    }
    // export var/let/const
    else if (check(lexer::TokenType::LET) || check(lexer::TokenType::CONST)) {
        auto var = parseVarDeclStmt();
        return std::make_unique<ast::ExportStmt>(
            std::move(var),
            ast::SourceLocation(start.line, start.column)
        );
    }
    // export struct (Week 7)
    else if (check(lexer::TokenType::STRUCT)) {
        auto struct_decl = parseStructDecl();
        return std::make_unique<ast::ExportStmt>(
            std::move(struct_decl),
            ast::SourceLocation(start.line, start.column)
        );
    }
    // export default expr
    else {
        // TODO: Implement default export parsing
        throw ParseError("Export default not yet implemented");
    }
}

std::unique_ptr<ast::FunctionDecl> Parser::parseFunctionDecl() {
    auto start = current();
    bool is_async = match(lexer::TokenType::ASYNC);

    expect(lexer::TokenType::FUNCTION, "Expected 'function'");

    auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected function name");
    std::string name = name_token.value;

    expect(lexer::TokenType::LPAREN, "Expected '('");

    // Parse parameters
    std::vector<ast::Parameter> params;
    if (!check(lexer::TokenType::RPAREN)) {
        do {
            skipNewlines();
            auto& param_name = expect(lexer::TokenType::IDENTIFIER, "Expected parameter name");
            expect(lexer::TokenType::COLON, "Expected ':' after parameter name");
            ast::Type param_type = parseType();

            // Optional default value
            std::optional<std::unique_ptr<ast::Expr>> default_value;
            if (match(lexer::TokenType::EQ)) {
                default_value = parseExpression();
            }

            params.push_back({param_name.value, param_type, std::move(default_value)});

            skipNewlines();
        } while (match(lexer::TokenType::COMMA));
    }

    expect(lexer::TokenType::RPAREN, "Expected ')'");

    // Optional return type
    ast::Type return_type = ast::Type::makeVoid();
    if (match(lexer::TokenType::ARROW)) {
        return_type = parseType();
    }

    skipNewlines();

    // Function body
    auto body = parseCompoundStmt();

    // Note: is_async currently not supported in AST
    return std::make_unique<ast::FunctionDecl>(
        name, std::move(params), return_type, std::move(body),
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::StructDecl> Parser::parseStructDecl() {
    auto start = current();
    expect(lexer::TokenType::STRUCT, "Expected 'struct' keyword");

    auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected struct name");
    std::string struct_name = name_token.value;

    expect(lexer::TokenType::LBRACE, "Expected '{' after struct name");

    std::vector<ast::StructField> fields;
    skipNewlines();
    while (!match(lexer::TokenType::RBRACE)) {
        auto& field_name_token = expect(lexer::TokenType::IDENTIFIER, "Expected field name");
        expect(lexer::TokenType::COLON, "Expected ':' after field name");

        auto field_type = parseType();

        fields.emplace_back(ast::StructField{field_name_token.value, field_type, std::nullopt});

        if (!check(lexer::TokenType::RBRACE)) {
            expect(lexer::TokenType::SEMICOLON, "Expected ';' after field");
        }
        skipNewlines();
    }

    return std::make_unique<ast::StructDecl>(struct_name, std::move(fields),
                                             ast::SourceLocation(start.line, start.column));
}

std::unique_ptr<ast::StructLiteralExpr> Parser::parseStructLiteral(
    const std::string& struct_name) {
    auto start = current();

    expect(lexer::TokenType::LBRACE, "Expected '{' for struct literal");

    std::vector<std::pair<std::string, std::unique_ptr<ast::Expr>>> field_inits;
    while (!match(lexer::TokenType::RBRACE)) {
        auto& field_name = expect(lexer::TokenType::IDENTIFIER, "Expected field name");
        expect(lexer::TokenType::COLON, "Expected ':' after field name");

        auto field_expr = parseExpression();
        field_inits.emplace_back(field_name.value, std::move(field_expr));

        if (!check(lexer::TokenType::RBRACE)) {
            expect(lexer::TokenType::COMMA, "Expected ',' between fields");
        }
    }

    return std::make_unique<ast::StructLiteralExpr>(
        struct_name, std::move(field_inits), ast::SourceLocation(start.line, start.column));
}

std::unique_ptr<ast::MainBlock> Parser::parseMainBlock() {
    auto start = current();
    expect(lexer::TokenType::MAIN, "Expected 'main'");
    skipNewlines();

    auto body = parseCompoundStmt();

    return std::make_unique<ast::MainBlock>(
        std::move(body),
        ast::SourceLocation(start.line, start.column)
    );
}

// ============================================================================
// Statements
// ============================================================================

std::unique_ptr<ast::Stmt> Parser::parseStatement() {
    skipNewlines();

    if (check(lexer::TokenType::LBRACE)) {
        return parseCompoundStmt();
    }
    if (check(lexer::TokenType::RETURN)) {
        return parseReturnStmt();
    }
    if (check(lexer::TokenType::BREAK)) {
        return parseBreakStmt();
    }
    if (check(lexer::TokenType::CONTINUE)) {
        return parseContinueStmt();
    }
    if (check(lexer::TokenType::IF)) {
        return parseIfStmt();
    }
    if (check(lexer::TokenType::FOR)) {
        return parseForStmt();
    }
    if (check(lexer::TokenType::WHILE)) {
        return parseWhileStmt();
    }
    if (check(lexer::TokenType::TRY)) {
        return parseTryStmt();
    }
    if (check(lexer::TokenType::THROW)) {
        return parseThrowStmt();
    }
    if (check(lexer::TokenType::LET) || check(lexer::TokenType::CONST)) {
        return parseVarDeclStmt();
    }

    // Default: expression statement
    return parseExprStmt();
}

std::unique_ptr<ast::CompoundStmt> Parser::parseCompoundStmt() {
    auto start = current();
    expect(lexer::TokenType::LBRACE, "Expected '{'");
    skipNewlines();

    std::vector<std::unique_ptr<ast::Stmt>> stmts;

    while (!check(lexer::TokenType::RBRACE) && !isAtEnd()) {
        stmts.push_back(parseStatement());
        skipNewlines();
    }

    expect(lexer::TokenType::RBRACE, "Expected '}'");

    return std::make_unique<ast::CompoundStmt>(
        std::move(stmts),
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::ReturnStmt> Parser::parseReturnStmt() {
    auto start = current();
    expect(lexer::TokenType::RETURN, "Expected 'return'");

    std::unique_ptr<ast::Expr> value;
    if (!check(lexer::TokenType::NEWLINE) && !check(lexer::TokenType::RBRACE)) {
        value = parseExpression();
    }

    return std::make_unique<ast::ReturnStmt>(
        std::move(value),
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::BreakStmt> Parser::parseBreakStmt() {
    auto start = current();
    expect(lexer::TokenType::BREAK, "Expected 'break'");

    return std::make_unique<ast::BreakStmt>(
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::ContinueStmt> Parser::parseContinueStmt() {
    auto start = current();
    expect(lexer::TokenType::CONTINUE, "Expected 'continue'");

    return std::make_unique<ast::ContinueStmt>(
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::IfStmt> Parser::parseIfStmt() {
    auto start = current();
    expect(lexer::TokenType::IF, "Expected 'if'");

    auto condition = parseExpression();
    skipNewlines();
    auto then_stmt = parseStatement();

    std::unique_ptr<ast::Stmt> else_stmt;
    skipNewlines();
    if (match(lexer::TokenType::ELSE)) {
        skipNewlines();
        else_stmt = parseStatement();
    }

    return std::make_unique<ast::IfStmt>(
        std::move(condition),
        std::move(then_stmt),
        std::move(else_stmt),
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::ForStmt> Parser::parseForStmt() {
    auto start = current();
    expect(lexer::TokenType::FOR, "Expected 'for'");

    auto& var_name = expect(lexer::TokenType::IDENTIFIER, "Expected variable name");
    std::string var = var_name.value;

    expect(lexer::TokenType::IN, "Expected 'in'");

    auto iterable = parseExpression();
    skipNewlines();
    auto body = parseStatement();

    return std::make_unique<ast::ForStmt>(
        var,
        std::move(iterable),
        std::move(body),
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::WhileStmt> Parser::parseWhileStmt() {
    auto start = current();
    expect(lexer::TokenType::WHILE, "Expected 'while'");

    auto condition = parseExpression();
    skipNewlines();
    auto body = parseStatement();

    return std::make_unique<ast::WhileStmt>(
        std::move(condition),
        std::move(body),
        ast::SourceLocation(start.line, start.column)
    );
}

// Phase 4.1: Exception handling
std::unique_ptr<ast::TryStmt> Parser::parseTryStmt() {
    auto start = current();
    expect(lexer::TokenType::TRY, "Expected 'try'");

    skipNewlines();
    auto try_body = parseCompoundStmt();

    skipNewlines();
    expect(lexer::TokenType::CATCH, "Expected 'catch' after try block");

    // Parse catch (error_name) { ... }
    expect(lexer::TokenType::LPAREN, "Expected '(' after 'catch'");
    auto& error_name_token = expect(lexer::TokenType::IDENTIFIER, "Expected error variable name");
    std::string error_name = error_name_token.value;
    expect(lexer::TokenType::RPAREN, "Expected ')' after error name");

    skipNewlines();
    auto catch_body = parseCompoundStmt();

    auto catch_clause = std::make_unique<ast::TryStmt::CatchClause>(
        error_name,
        std::move(catch_body)
    );

    // Optional finally block
    std::unique_ptr<ast::CompoundStmt> finally_body;
    skipNewlines();
    if (match(lexer::TokenType::FINALLY)) {
        skipNewlines();
        finally_body = parseCompoundStmt();
    }

    return std::make_unique<ast::TryStmt>(
        std::move(try_body),
        std::move(catch_clause),
        std::move(finally_body),
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::ThrowStmt> Parser::parseThrowStmt() {
    auto start = current();
    expect(lexer::TokenType::THROW, "Expected 'throw'");

    auto expr = parseExpression();

    return std::make_unique<ast::ThrowStmt>(
        std::move(expr),
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::VarDeclStmt> Parser::parseVarDeclStmt() {
    auto start = current();
    bool is_const = match(lexer::TokenType::CONST);
    if (!is_const) {
        expect(lexer::TokenType::LET, "Expected 'let' or 'const'");
    }

    auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected variable name");
    std::string name = name_token.value;

    // Optional type annotation
    ast::Type var_type = ast::Type::makeAny();
    if (match(lexer::TokenType::COLON)) {
        var_type = parseType();
    }

    // Optional initializer
    std::unique_ptr<ast::Expr> init;
    if (match(lexer::TokenType::EQ)) {
        init = parseExpression();
    }

    // Note: is_const currently not supported in AST, type is optional
    std::optional<ast::Type> opt_type = (var_type.kind != ast::TypeKind::Any)
        ? std::optional<ast::Type>(var_type)
        : std::nullopt;

    optionalSemicolon();  // Allow optional semicolon after var decl

    return std::make_unique<ast::VarDeclStmt>(
        name,
        std::move(init),
        opt_type,
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::ExprStmt> Parser::parseExprStmt() {
    auto start = current();
    auto expr = parseExpression();

    optionalSemicolon();  // Allow optional semicolon after expression

    return std::make_unique<ast::ExprStmt>(
        std::move(expr),
        ast::SourceLocation(start.line, start.column)
    );
}

// ============================================================================
// Expressions (Precedence Climbing)
// ============================================================================

std::unique_ptr<ast::Expr> Parser::parseExpression() {
    return parseAssignment();
}

std::unique_ptr<ast::Expr> Parser::parseAssignment() {
    auto expr = parsePipeline();

    if (match(lexer::TokenType::EQ)) {
        auto value = parseAssignment();  // Right-associative
        expr = std::make_unique<ast::BinaryExpr>(
            ast::BinaryOp::Assign,
            std::move(expr),
            std::move(value),
            ast::SourceLocation()
        );
    }

    return expr;
}

// Phase 3.4: Pipeline operator (|>)
// Left-associative, lower precedence than logical operators
std::unique_ptr<ast::Expr> Parser::parsePipeline() {
    auto left = parseLogicalOr();

    // Pipeline is left-associative: a |> b |> c means (a |> b) |> c
    while (match(lexer::TokenType::PIPELINE)) {
        auto right = parseLogicalOr();
        left = std::make_unique<ast::BinaryExpr>(
            ast::BinaryOp::Pipeline,
            std::move(left),
            std::move(right),
            ast::SourceLocation()
        );
    }

    return left;
}

std::unique_ptr<ast::Expr> Parser::parseLogicalOr() {
    auto left = parseLogicalAnd();

    while (match(lexer::TokenType::OR)) {
        auto right = parseLogicalAnd();
        left = std::make_unique<ast::BinaryExpr>(
            ast::BinaryOp::Or,
            std::move(left),
            std::move(right),
            ast::SourceLocation()
        );
    }

    return left;
}

std::unique_ptr<ast::Expr> Parser::parseLogicalAnd() {
    auto left = parseEquality();

    while (match(lexer::TokenType::AND)) {
        auto right = parseEquality();
        left = std::make_unique<ast::BinaryExpr>(
            ast::BinaryOp::And,
            std::move(left),
            std::move(right),
            ast::SourceLocation()
        );
    }

    return left;
}

std::unique_ptr<ast::Expr> Parser::parseEquality() {
    auto left = parseComparison();

    while (true) {
        ast::BinaryOp op;
        if (match(lexer::TokenType::EQEQ)) {
            op = ast::BinaryOp::Eq;
        } else if (match(lexer::TokenType::NE)) {
            op = ast::BinaryOp::Ne;
        } else {
            break;
        }

        auto right = parseComparison();
        left = std::make_unique<ast::BinaryExpr>(
            op,
            std::move(left),
            std::move(right),
            ast::SourceLocation()
        );
    }

    return left;
}

std::unique_ptr<ast::Expr> Parser::parseComparison() {
    auto left = parseTerm();

    while (true) {
        ast::BinaryOp op;
        if (match(lexer::TokenType::LT)) {
            op = ast::BinaryOp::Lt;
        } else if (match(lexer::TokenType::LE)) {
            op = ast::BinaryOp::Le;
        } else if (match(lexer::TokenType::GT)) {
            op = ast::BinaryOp::Gt;
        } else if (match(lexer::TokenType::GE)) {
            op = ast::BinaryOp::Ge;
        } else {
            break;
        }

        auto right = parseTerm();
        left = std::make_unique<ast::BinaryExpr>(
            op,
            std::move(left),
            std::move(right),
            ast::SourceLocation()
        );
    }

    return left;
}

std::unique_ptr<ast::Expr> Parser::parseTerm() {
    auto left = parseFactor();

    while (true) {
        ast::BinaryOp op;
        if (match(lexer::TokenType::PLUS)) {
            op = ast::BinaryOp::Add;
        } else if (match(lexer::TokenType::MINUS)) {
            op = ast::BinaryOp::Sub;
        } else {
            break;
        }

        auto right = parseFactor();
        left = std::make_unique<ast::BinaryExpr>(
            op,
            std::move(left),
            std::move(right),
            ast::SourceLocation()
        );
    }

    return left;
}

std::unique_ptr<ast::Expr> Parser::parseFactor() {
    auto left = parseUnary();

    while (true) {
        ast::BinaryOp op;
        if (match(lexer::TokenType::STAR)) {
            op = ast::BinaryOp::Mul;
        } else if (match(lexer::TokenType::SLASH)) {
            op = ast::BinaryOp::Div;
        } else if (match(lexer::TokenType::PERCENT)) {
            op = ast::BinaryOp::Mod;
        } else {
            break;
        }

        auto right = parseUnary();
        left = std::make_unique<ast::BinaryExpr>(
            op,
            std::move(left),
            std::move(right),
            ast::SourceLocation()
        );
    }

    return left;
}

std::unique_ptr<ast::Expr> Parser::parseUnary() {
    if (match(lexer::TokenType::MINUS)) {
        auto operand = parseUnary();
        return std::make_unique<ast::UnaryExpr>(
            ast::UnaryOp::Neg,
            std::move(operand),
            ast::SourceLocation()
        );
    }

    if (match(lexer::TokenType::NOT)) {
        auto operand = parseUnary();
        return std::make_unique<ast::UnaryExpr>(
            ast::UnaryOp::Not,
            std::move(operand),
            ast::SourceLocation()
        );
    }

    return parsePostfix();
}

std::unique_ptr<ast::Expr> Parser::parsePostfix() {
    auto expr = parsePrimary();

    while (true) {
        // Function call
        if (match(lexer::TokenType::LPAREN)) {
            std::vector<std::unique_ptr<ast::Expr>> args;

            if (!check(lexer::TokenType::RPAREN)) {
                do {
                    skipNewlines();
                    args.push_back(parseExpression());
                    skipNewlines();
                } while (match(lexer::TokenType::COMMA));
            }

            expect(lexer::TokenType::RPAREN, "Expected ')'");

            expr = std::make_unique<ast::CallExpr>(
                std::move(expr),
                std::move(args),
                ast::SourceLocation()
            );
        }
        // Member access
        else if (match(lexer::TokenType::DOT)) {
            auto& member = expect(lexer::TokenType::IDENTIFIER, "Expected member name");
            expr = std::make_unique<ast::MemberExpr>(
                std::move(expr),
                member.value,
                ast::SourceLocation()
            );
        }
        // Array/Dict subscript
        else if (match(lexer::TokenType::LBRACKET)) {
            auto index = parseExpression();
            expect(lexer::TokenType::RBRACKET, "Expected ']'");

            expr = std::make_unique<ast::BinaryExpr>(
                ast::BinaryOp::Subscript,
                std::move(expr),
                std::move(index),
                ast::SourceLocation()
            );
        }
        else {
            break;
        }
    }

    return expr;
}

std::unique_ptr<ast::Expr> Parser::parsePrimary() {
    auto start = current();

    // Literals
    if (match(lexer::TokenType::NUMBER)) {
        std::string value = tokens_[pos_ - 1].value;
        ast::LiteralKind kind = (value.find('.') != std::string::npos)
            ? ast::LiteralKind::Float
            : ast::LiteralKind::Int;
        return std::make_unique<ast::LiteralExpr>(kind, value, ast::SourceLocation());
    }

    if (match(lexer::TokenType::STRING)) {
        std::string value = tokens_[pos_ - 1].value;
        return std::make_unique<ast::LiteralExpr>(ast::LiteralKind::String, value, ast::SourceLocation());
    }

    if (match(lexer::TokenType::BOOLEAN)) {
        std::string value = tokens_[pos_ - 1].value;
        return std::make_unique<ast::LiteralExpr>(ast::LiteralKind::Bool, value, ast::SourceLocation());
    }

    // Struct literal: new StructName { field: value, ... }
    if (match(lexer::TokenType::NEW)) {
        auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected struct name after 'new'");
        return parseStructLiteral(name_token.value);
    }

    // Inline polyglot code: <<language ... >>
    if (match(lexer::TokenType::INLINE_CODE)) {
        std::string value = tokens_[pos_ - 1].value;

        // Parse "language:code" format
        size_t colon_pos = value.find(':');
        if (colon_pos == std::string::npos) {
            throw ParseError("Invalid inline code format at line " + std::to_string(tokens_[pos_ - 1].line));
        }

        std::string language = value.substr(0, colon_pos);
        std::string code = value.substr(colon_pos + 1);

        return std::make_unique<ast::InlineCodeExpr>(language, code, ast::SourceLocation());
    }

    // Identifier
    if (match(lexer::TokenType::IDENTIFIER)) {
        std::string name = tokens_[pos_ - 1].value;
        return std::make_unique<ast::IdentifierExpr>(name, ast::SourceLocation());
    }

    // Parenthesized expression
    if (match(lexer::TokenType::LPAREN)) {
        auto expr = parseExpression();
        expect(lexer::TokenType::RPAREN, "Expected ')' after expression");
        return expr;
    }

    // List literal
    if (match(lexer::TokenType::LBRACKET)) {
        std::vector<std::unique_ptr<ast::Expr>> elements;

        if (!check(lexer::TokenType::RBRACKET)) {
            do {
                skipNewlines();
                elements.push_back(parseExpression());
                skipNewlines();
            } while (match(lexer::TokenType::COMMA));
        }

        expect(lexer::TokenType::RBRACKET, "Expected ']'");

        return std::make_unique<ast::ListExpr>(std::move(elements), ast::SourceLocation());
    }

    // Dict literal
    if (match(lexer::TokenType::LBRACE)) {
        std::vector<std::pair<std::unique_ptr<ast::Expr>, std::unique_ptr<ast::Expr>>> pairs;

        if (!check(lexer::TokenType::RBRACE)) {
            do {
                skipNewlines();
                auto key = parseExpression();
                expect(lexer::TokenType::COLON, "Expected ':' after dict key");
                auto value = parseExpression();
                pairs.emplace_back(std::move(key), std::move(value));
                skipNewlines();
            } while (match(lexer::TokenType::COMMA));
        }

        expect(lexer::TokenType::RBRACE, "Expected '}'");

        return std::make_unique<ast::DictExpr>(std::move(pairs), ast::SourceLocation());
    }

    throw ParseError(formatError("Unexpected token in expression", current()));
}

// ============================================================================
// Type Parsing
// ============================================================================

ast::Type Parser::parseType() {
    auto& token = current();

    if (match(lexer::TokenType::IDENTIFIER)) {
        std::string type_name = token.value;

        // Built-in types
        if (type_name == "int") return ast::Type::makeInt();
        if (type_name == "float") return ast::Type::makeFloat();
        if (type_name == "string") return ast::Type::makeString();
        if (type_name == "bool") return ast::Type::makeBool();
        if (type_name == "void") return ast::Type::makeVoid();
        if (type_name == "any") return ast::Type::makeAny();

        // List or Dict with type parameters
        if (type_name == "list") {
            if (match(lexer::TokenType::LBRACKET)) {
                auto elem_type = parseType();
                expect(lexer::TokenType::RBRACKET, "Expected ']'");
                ast::Type list_type(ast::TypeKind::List);
                list_type.element_type = std::make_shared<ast::Type>(elem_type);
                return list_type;
            }
            return ast::Type(ast::TypeKind::List);
        }

        if (type_name == "dict") {
            if (match(lexer::TokenType::LBRACKET)) {
                auto key_type = parseType();
                expect(lexer::TokenType::COMMA, "Expected ',' in dict type");
                auto val_type = parseType();
                expect(lexer::TokenType::RBRACKET, "Expected ']'");
                ast::Type dict_type(ast::TypeKind::Dict);
                dict_type.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(
                    key_type, val_type
                );
                return dict_type;
            }
            return ast::Type(ast::TypeKind::Dict);
        }

        // Block type or custom type
        return ast::Type::makeBlock();
    }

    throw ParseError(formatError("Expected type name", current()));
}

} // namespace parser
} // namespace naab
