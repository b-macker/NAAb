// NAAb Parser - Recursive descent parser
// Assembled from LLVM/Clang parser patterns

#include "naab/parser.h"
#include "naab/limits.h"  // Week 1, Task 1.3: Recursion depth limits
#include "error_hints.h"  // Phase 2.1: Enhanced error hints
#include <fmt/core.h>
#include <algorithm>  // Phase 1.3: for std::transform
#include <cctype>     // Phase 1.3: for ::tolower
#include <unordered_set>  // For reserved keyword checking
#include <unordered_map>  // For keyword suggestions

namespace naab {
namespace parser {

// Week 1, Task 1.3: DepthGuard implementation
Parser::DepthGuard::DepthGuard(size_t& depth) : depth_(depth) {
    if (++depth_ > limits::MAX_PARSE_DEPTH) {
        throw limits::RecursionLimitException(
            "Parser recursion depth exceeded: " +
            std::to_string(depth_) + " > " + std::to_string(limits::MAX_PARSE_DEPTH)
        );
    }
}

Parser::DepthGuard::~DepthGuard() {
    --depth_;
}

Parser::Parser(const std::vector<lexer::Token>& tokens)
    : tokens_(tokens), pos_(0),
      stored_gt_token_(lexer::TokenType::GT, ">", 0, 0),  // Initialize with dummy values
      parser_context_(std::make_unique<ParserContext>()) {  // Phase 2.1: Initialize context
    skipNewlines();  // Skip leading newlines
}

// Destructor definition (must be in .cpp where ParserContext is complete)
Parser::~Parser() = default;

void Parser::setSource(const std::string& source_code, const std::string& filename) {
    filename_ = filename;  // Phase 3.1: Store filename for AST source locations
    error_reporter_.setSource(source_code, filename);
}

// ============================================================================
// Helper Functions for Enhanced Error Messages
// ============================================================================

namespace {
    // Check if a name is a reserved keyword
    bool isReservedKeyword(const std::string& name) {
        static const std::unordered_set<std::string> keywords = {
            "use", "as", "function", "fn", "async", "method", "return",
            "if", "else", "for", "in", "while", "break", "continue",
            "match", "try", "catch", "throw", "finally",
            "struct", "class", "init", "module", "export", "import",
            "new", "config", "main", "let", "const", "await", "null",
            "ref", "enum", "true", "false"
        };
        return keywords.count(name) > 0;
    }

    // Suggest alternative names for reserved keywords
    std::string suggestAlternatives(const std::string& keyword) {
        static const std::unordered_map<std::string, std::string> suggestions = {
            {"config", "cfg, configuration, settings, options"},
            {"class", "cls, klass, type_name"},
            {"function", "func, fn, method_name"},
            {"module", "mod, module_name"},
            {"new", "create, make, build"},
            {"import", "include, require"},
            {"export", "expose, publish"},
            {"const", "constant, value"},
            {"let", "var, variable"},
            {"return", "ret, result"},
            {"async", "asynchronous, async_fn"},
            {"await", "wait, wait_for"}
        };

        auto it = suggestions.find(keyword);
        if (it != suggestions.end()) {
            return it->second;
        }
        return keyword + "_val, my_" + keyword + ", " + keyword + "_var";
    }
}

// ============================================================================
// Token Navigation
// ============================================================================

const lexer::Token& Parser::current() const {
    // If we have a pending token (from >> splitting), return it
    if (pending_token_.has_value()) {
        return *pending_token_;
    }
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
    // If we have a pending token (from >> splitting), clear it instead of advancing
    if (pending_token_.has_value()) {
        pending_token_.reset();
        updateParserContext();  // Phase 2.1: Update context
        return;
    }
    if (!isAtEnd()) {
        pos_++;
        updateParserContext();  // Phase 2.1: Update context after advancing
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

    // Phase 2.1: Use enhanced error hints for better error messages
    const auto& token = current();
    error_reporter_.error(msg, token.line, token.column);

    // Get context-aware hints
    auto hints = getErrorHints(token, msg);
    for (const auto& hint : hints) {
        error_reporter_.addSuggestion(hint);
    }

    throw ParseError(formatError(msg, token));
}

// Helper function to handle '>' in nested generics (splits '>>' into two '>')
const lexer::Token& Parser::expectGTOrSplitGTGT(const std::string& msg) {
    if (check(lexer::TokenType::GT)) {
        const auto& token = current();
        advance();
        return token;
    }

    // Handle >> as two separate > tokens for nested generics
    if (check(lexer::TokenType::GT_GT)) {
        const auto& token = current();
        // Advance position past the GT_GT first
        pos_++;

        // Create first > token and store it
        stored_gt_token_ = lexer::Token(lexer::TokenType::GT, ">", token.line, token.column);
        // Create second > token for pending (will be consumed next)
        lexer::Token second_gt(lexer::TokenType::GT, ">", token.line, token.column + 1);
        pending_token_ = second_gt;

        // Return a reference to the first > token (no advance needed, already moved pos)
        return stored_gt_token_;
    }

    // Neither GT nor GT_GT found - error
    const auto& token = current();
    error_reporter_.error(msg, token.line, token.column);
    error_reporter_.addSuggestion(fmt::format("Expected '>' but got: '{}'", token.value));
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
        // Look ahead to distinguish between:
        // - use BLOCK-CPP-12345 as Cord (block import - has BLOCK_ID token)
        // - use math_utils (module import - has IDENTIFIER token)
        // - use math_utils as math (module import with alias - has IDENTIFIER + AS)
        size_t saved_pos = pos_;
        advance();  // skip 'use'
        skipNewlines();

        // Check if this is a block import (starts with BLOCK_ID or STRING)
        // Module imports use regular IDENTIFIERs
        bool is_block_import = false;
        if (check(lexer::TokenType::BLOCK_ID) || check(lexer::TokenType::STRING)) {
            is_block_import = true;
        }

        // Restore position and parse accordingly
        pos_ = saved_pos;

        if (is_block_import) {
            imports.push_back(parseUseStatement());
        } else {
            break;  // Not a block import, will be handled in main loop as module use
        }

        skipNewlines();
    }

    // Collect module imports, exports, structs, and enums in vectors
    std::vector<std::unique_ptr<ast::ImportStmt>> module_imports;
    std::vector<std::unique_ptr<ast::ModuleUseStmt>> module_uses;  // Phase 4.0
    std::vector<std::unique_ptr<ast::ExportStmt>> exports;
    std::vector<std::unique_ptr<ast::StructDecl>> structs;
    std::vector<std::unique_ptr<ast::EnumDecl>> enums;  // Phase 2.4.3

    // Parse module imports, exports, structs, and functions (Phase 3.1)
    while (!isAtEnd()) {
        skipNewlines();

        if (check(lexer::TokenType::USE)) {
            // Phase 4.0: Module use statement (use math_utils)
            auto module_use = parseModuleUseStmt();
            module_uses.push_back(std::move(module_use));
        }
        else if (check(lexer::TokenType::IMPORT)) {
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
        else if (check(lexer::TokenType::ENUM)) {  // Phase 2.4.3
            enums.push_back(parseEnumDecl());
        }
        else if (check(lexer::TokenType::FUNCTION) || check(lexer::TokenType::ASYNC)) {
            functions.push_back(parseFunctionDecl());
        }
        else if (check(lexer::TokenType::MAIN)) {
            main_block = parseMainBlock();
            break;  // Main block ends the program
        }
        else {
            // Unknown token at top level - provide helpful error message
            if (!isAtEnd()) {
                auto& tok = current();

                // Special case: helpful error for common mistakes
                if (tok.type == lexer::TokenType::LET ||
                    tok.type == lexer::TokenType::CONST) {
                    throw std::runtime_error(
                        fmt::format(
                            "Parse error at line {}, column {}: '{}' statements must be inside a 'main {{}}' block or function.\n"
                            "  Hint: Top level can only contain: use, import, export, struct, enum, function, main",
                            tok.line, tok.column, tok.value
                        )
                    );
                } else {
                    fmt::print("[PARSER] Stopping parse at unknown token: {} (line {}, col {})\n",
                               tok.value, tok.line, tok.column);
                    fmt::print("[PARSER] Hint: Top level can only contain: use, import, export, struct, enum, function, main\n");
                }
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

    // Add module imports, exports, structs, and enums to the program
    for (auto& import : module_imports) {
        program->addModuleImport(std::move(import));
    }
    for (auto& module_use : module_uses) {  // Phase 4.0
        program->addModuleUse(std::move(module_use));
    }
    for (auto& export_stmt : exports) {
        program->addExport(std::move(export_stmt));
    }
    for (auto& struct_decl : structs) {
        program->addStruct(std::move(struct_decl));
    }
    for (auto& enum_decl : enums) {  // Phase 2.4.3
        program->addEnum(std::move(enum_decl));
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
            "Use statement error at line " + std::to_string(token.line) +
            ", column " + std::to_string(token.column) + "\n\n"
            "  Expected: block ID, string literal, or identifier\n"
            "  Got: " + token.value + "\n\n"
            "  Help:\n"
            "  - Block ID format: use BLOCK-abc123 from \"path/file.naab\"\n"
            "  - String format: use \"module_name\" from \"path/file.naab\"\n"
            "  - Identifier format: use some_module from \"path/file.naab\"\n"
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
        expect(lexer::TokenType::FROM, "Expected 'from'");
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
        expect(lexer::TokenType::FROM, "Expected 'from'");
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

// Phase 4.0: Parse module use statement (Rust-style)
// use math_utils
// use data.processor
// use data.processor as dp  (with optional alias)
std::unique_ptr<ast::ModuleUseStmt> Parser::parseModuleUseStmt() {
    auto start = current();

    // Parse "use" keyword
    expect(lexer::TokenType::USE, "Expected 'use'");

    // Parse module path: math_utils or data.processor
    auto& first_token = expect(lexer::TokenType::IDENTIFIER, "Expected module name");
    std::string module_path = first_token.value;

    // Handle dotted paths: data.processor
    while (match(lexer::TokenType::DOT)) {
        auto& next_token = expect(lexer::TokenType::IDENTIFIER, "Expected identifier after '.'");
        module_path += "." + next_token.value;
    }

    // Optional: as alias
    std::string alias;
    if (match(lexer::TokenType::AS)) {
        auto& alias_token = expect(lexer::TokenType::IDENTIFIER, "Expected identifier after 'as'");
        alias = alias_token.value;
    }

    return std::make_unique<ast::ModuleUseStmt>(
        module_path,
        alias,
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
    // export enum (Phase 4.1: Module System enum exports)
    else if (check(lexer::TokenType::ENUM)) {
        auto enum_decl = parseEnumDecl();
        return std::make_unique<ast::ExportStmt>(
            std::move(enum_decl),
            ast::SourceLocation(start.line, start.column)
        );
    }
    // export default expr
    else if (match(lexer::TokenType::DEFAULT)) {
        // Parse the expression or declaration to export as default
        auto expr = parseExpression();
        return ast::ExportStmt::createDefault(
            std::move(expr),
            ast::SourceLocation(start.line, start.column)
        );
    }
    else {
        throw ParseError("Expected function, variable, struct, enum, or default after 'export'");
    }
}

std::unique_ptr<ast::FunctionDecl> Parser::parseFunctionDecl() {
    auto start = current();
    bool is_async = match(lexer::TokenType::ASYNC);

    expect(lexer::TokenType::FUNCTION, "Expected 'function'");

    auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected function name");
    std::string name = name_token.value;

    // Phase 2.4.1: Parse optional generic type parameters
    std::vector<std::string> type_params;
    if (match(lexer::TokenType::LT)) {
        // Parse generic type parameter list: <T>, <T, U>, etc.
        do {
            auto& type_param = expect(lexer::TokenType::IDENTIFIER, "Expected type parameter name");
            type_params.push_back(type_param.value);
        } while (match(lexer::TokenType::COMMA));
        expect(lexer::TokenType::GT, "Expected '>' after type parameters");
    }

    expect(lexer::TokenType::LPAREN, "Expected '('");

    // Parse parameters
    std::vector<ast::Parameter> params;
    if (!check(lexer::TokenType::RPAREN)) {
        do {
            skipNewlines();
            auto& param_name = expect(lexer::TokenType::IDENTIFIER, "Expected parameter name");

            // Check if parameter name is a reserved keyword
            if (isReservedKeyword(param_name.value)) {
                throw ParseError(fmt::format(
                    "Cannot use reserved keyword '{}' as parameter name at line {}\n\n"
                    "Help: '{}' is a reserved keyword. Try using a different name:\n"
                    "  Suggestions: {}",
                    param_name.value,
                    param_name.line,
                    param_name.value,
                    suggestAlternatives(param_name.value)
                ));
            }

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
    // Phase 2.4.4 Phase 2: Use Any to mark "needs inference"
    ast::Type return_type = ast::Type::makeAny();  // Default to Any (will be inferred)
    if (match(lexer::TokenType::ARROW)) {
        return_type = parseType();  // Explicit return type provided
    }

    skipNewlines();

    // Function body
    auto body = parseCompoundStmt();

    // Phase 2.4.1: Pass type_params to FunctionDecl constructor
    return std::make_unique<ast::FunctionDecl>(
        name, std::move(params), return_type, std::move(body),
        std::move(type_params),
        is_async,  // Phase 6 (deferred): async functions
        ast::SourceLocation(start.line, start.column, filename_)  // Phase 3.1: Include filename
    );
}

std::unique_ptr<ast::StructDecl> Parser::parseStructDecl() {
    auto start = current();
    expect(lexer::TokenType::STRUCT, "Expected 'struct' keyword");

    auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected struct name");
    std::string struct_name = name_token.value;

    // Phase 2.4.1: Parse optional generic type parameters
    std::vector<std::string> type_params;
    if (match(lexer::TokenType::LT)) {
        // Parse generic type parameter list: <T>, <T, U>, etc.
        do {
            auto& type_param = expect(lexer::TokenType::IDENTIFIER, "Expected type parameter name");
            type_params.push_back(type_param.value);
        } while (match(lexer::TokenType::COMMA));
        expect(lexer::TokenType::GT, "Expected '>' after type parameters");
    }

    expect(lexer::TokenType::LBRACE, "Expected '{' after struct name");

    std::vector<ast::StructField> fields;
    skipNewlines();
    while (!match(lexer::TokenType::RBRACE)) {
        auto& field_name_token = expect(lexer::TokenType::IDENTIFIER, "Expected field name");
        expect(lexer::TokenType::COLON, "Expected ':' after field name");

        auto field_type = parseType();

        fields.emplace_back(ast::StructField{field_name_token.value, field_type, std::nullopt});

        // Phase 1.1: Make field separators optional and flexible
        // Support semicolons, commas, or just newlines
        if (!check(lexer::TokenType::RBRACE)) {
            // Optional separator: semicolon, comma, or newline
            if (match(lexer::TokenType::SEMICOLON) || match(lexer::TokenType::COMMA)) {
                // Explicit separator consumed
            }
            // If no explicit separator, newline acts as separator (ASI)
        }
        skipNewlines();
    }

    // Phase 2.4.1: Pass type_params to StructDecl constructor
    return std::make_unique<ast::StructDecl>(struct_name, std::move(fields),
                                             std::move(type_params),
                                             ast::SourceLocation(start.line, start.column));
}

// Phase 2.4.3: Parse enum declaration
std::unique_ptr<ast::EnumDecl> Parser::parseEnumDecl() {
    auto start = current();
    expect(lexer::TokenType::ENUM, "Expected 'enum' keyword");

    auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected enum name");
    std::string enum_name = name_token.value;

    // Phase 4.1: Register enum name for type checking
    enum_names_.insert(enum_name);

    expect(lexer::TokenType::LBRACE, "Expected '{' after enum name");

    std::vector<ast::EnumDecl::EnumVariant> variants;
    skipNewlines();

    while (!match(lexer::TokenType::RBRACE)) {
        auto& variant_name_token = expect(lexer::TokenType::IDENTIFIER, "Expected variant name");
        std::string variant_name = variant_name_token.value;

        // Optional explicit value: Variant = 10
        std::optional<int> explicit_value = std::nullopt;
        if (match(lexer::TokenType::EQ)) {
            auto& value_token = expect(lexer::TokenType::NUMBER, "Expected integer value after '='");
            explicit_value = std::stoi(value_token.value);
        }

        variants.emplace_back(ast::EnumDecl::EnumVariant(variant_name, explicit_value));

        // Flexible separators: comma, semicolon, or newline
        if (!check(lexer::TokenType::RBRACE)) {
            if (match(lexer::TokenType::COMMA) || match(lexer::TokenType::SEMICOLON)) {
                // Explicit separator consumed
            }
        }
        skipNewlines();
    }

    return std::make_unique<ast::EnumDecl>(enum_name, std::move(variants),
                                          ast::SourceLocation(start.line, start.column));
}

std::unique_ptr<ast::StructLiteralExpr> Parser::parseStructLiteral(
    const std::string& struct_name) {
    auto start = current();

    expect(lexer::TokenType::LBRACE, "Expected '{' for struct literal");

    // Phase 1.2: Support multi-line struct literals
    skipNewlines();  // Allow newlines after opening brace

    std::vector<std::pair<std::string, std::unique_ptr<ast::Expr>>> field_inits;
    while (!match(lexer::TokenType::RBRACE)) {
        // Phase 4.1: Support both identifiers and string literals as dict keys
        std::string field_name_value;
        if (check(lexer::TokenType::IDENTIFIER)) {
            field_name_value = current().value;
            advance();
        } else if (check(lexer::TokenType::STRING)) {
            field_name_value = current().value;
            advance();
        } else {
            // Enhanced error for common mistake: postfix ? operator
            if (check(lexer::TokenType::QUESTION)) {
                throw ParseError(
                    "Expected field name (identifier or string) at line " +
                    std::to_string(current().line) + "\n" +
                    "  Got: '?'\n\n" +
                    "Help: The '?' operator is only valid in type annotations (e.g., string?),\n" +
                    "      not as a postfix operator on expressions.\n\n" +
                    "  If you want optional/nullable values:\n" +
                    "    ✗ item[\"name\"]?          // Invalid syntax\n" +
                    "    ✓ item[\"name\"]           // Direct access\n" +
                    "    ✓ let name: string? = ... // Nullable type annotation\n\n" +
                    "  Note: NAAb does not support safe navigation operators like TypeScript's '?.'"
                );
            }
            throw ParseError("Expected field name (identifier or string) at line " +
                           std::to_string(current().line));
        }
        expect(lexer::TokenType::COLON, "Expected ':' after field name");

        auto field_expr = parseExpression();
        field_inits.emplace_back(field_name_value, std::move(field_expr));

        // Phase 1.2: Support flexible separators (commas optional, newlines work)
        if (!check(lexer::TokenType::RBRACE)) {
            // Optional separator: comma, semicolon, or newline
            if (match(lexer::TokenType::COMMA) || match(lexer::TokenType::SEMICOLON)) {
                // Explicit separator consumed
            }
            // If no explicit separator, newline acts as separator
        }
        skipNewlines();  // Allow newlines after each field
    }

    return std::make_unique<ast::StructLiteralExpr>(
        struct_name, std::move(field_inits), ast::SourceLocation(start.line, start.column));
}

std::unique_ptr<ast::MainBlock> Parser::parseMainBlock() {
    auto start = current();
    expect(lexer::TokenType::MAIN, "Expected 'main'");
    skipNewlines();

    // Phase 2.1: Set context flag for better error hints
    parser_context_->in_main_block = true;
    auto body = parseCompoundStmt();
    parser_context_->in_main_block = false;

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
    // Handle nested function declarations
    if (check(lexer::TokenType::FUNCTION)) {
        auto func_decl = parseFunctionDecl();
        auto loc = func_decl->getLocation();
        return std::make_unique<ast::FunctionDeclStmt>(std::move(func_decl), loc);
    }
    // Handle nested struct declarations
    if (check(lexer::TokenType::STRUCT)) {
        auto struct_decl = parseStructDecl();
        auto loc = struct_decl->getLocation();
        return std::make_unique<ast::StructDeclStmt>(std::move(struct_decl), loc);
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
    // Check for common mistake: catch e instead of catch (e)
    if (check(lexer::TokenType::IDENTIFIER)) {
        throw std::runtime_error(
            "Syntax error: Missing parentheses in catch clause\n\n"
            "  NAAb requires parentheses around the error variable:\n\n"
            "  ✗ Wrong: catch e { ... }\n"
            "  ✓ Right: catch (e) { ... }\n\n"
            "  Example:\n"
            "    try {\n"
            "      let x = 1 / 0\n"
            "    } catch (error) {\n"
            "      print(\"Error:\", error)\n"
            "    }"
        );
    }

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

    // Check for reserved keywords (production feedback 2026-01-31)
    if (isReservedKeyword(name)) {
        std::string msg = fmt::format(
            "Cannot use reserved keyword '{}' as variable name\n\n"
            "Help: '{}' is a reserved keyword in NAAb. Try using a different name:\n"
            "  Suggestions: {}",
            name, name, suggestAlternatives(name)
        );
        error_reporter_.error(msg, name_token.line, name_token.column);
        throw ParseError(formatError(msg, name_token));
    }

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
    // Week 1, Task 1.3: Track parse depth to prevent stack overflow
    DepthGuard guard(parse_depth_);
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
    // Allow newlines before pipeline operator
    skipNewlines();
    while (match(lexer::TokenType::PIPELINE)) {
        skipNewlines();  // Also allow newlines after the operator
        auto right = parseLogicalOr();
        skipNewlines();  // Check for more pipeline operators after newlines
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
    auto left = parseRange();

    while (true) {
        ast::BinaryOp op;
        if (match(lexer::TokenType::EQEQ)) {
            op = ast::BinaryOp::Eq;
        } else if (match(lexer::TokenType::NE)) {
            op = ast::BinaryOp::Ne;
        } else {
            break;
        }

        auto right = parseRange();
        left = std::make_unique<ast::BinaryExpr>(
            op,
            std::move(left),
            std::move(right),
            ast::SourceLocation()
        );
    }

    return left;
}

// Range operator: start..end (exclusive) or start..=end (inclusive)
std::unique_ptr<ast::Expr> Parser::parseRange() {
    auto left = parseComparison();

    // Check for inclusive range ..=
    if (match(lexer::TokenType::DOTDOT_EQ)) {
        auto right = parseComparison();
        return std::make_unique<ast::RangeExpr>(
            std::move(left),
            std::move(right),
            true,  // inclusive
            ast::SourceLocation()
        );
    }

    // Check for exclusive range ..
    if (match(lexer::TokenType::DOTDOT)) {
        auto right = parseComparison();
        return std::make_unique<ast::RangeExpr>(
            std::move(left),
            std::move(right),
            false,  // exclusive
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
        // Phase 2.4.4: Check for explicit type arguments before function call
        // Pattern: func<Type1, Type2>(args)
        std::vector<ast::Type> type_arguments;
        if (check(lexer::TokenType::LT)) {
            // Look ahead to distinguish func<Type>(...) from func < value
            // Save position for potential backtrack
            size_t saved_pos = pos_;
            match(lexer::TokenType::LT);  // consume <

            // Try to parse as type arguments
            bool is_type_args = false;
            try {
                do {
                    type_arguments.push_back(parseType());
                } while (match(lexer::TokenType::COMMA));

                // If we successfully parsed types and see > followed by (,
                // then these are type arguments
                if (match(lexer::TokenType::GT) && check(lexer::TokenType::LPAREN)) {
                    is_type_args = true;
                }
            } catch (...) {
                // Parse failed, backtrack
            }

            if (!is_type_args) {
                // Not type arguments, backtrack
                pos_ = saved_pos;
                type_arguments.clear();
            }
        }

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
                std::move(type_arguments),  // Pass type arguments
                ast::SourceLocation()
            );
        }
        // Member access
        else if (match(lexer::TokenType::DOT)) {
            // Check for reserved keyword used as member name
            if (check(lexer::TokenType::NEW)) {
                // Special hint for array.new() pattern
                auto* id_expr = dynamic_cast<ast::IdentifierExpr*>(expr.get());
                if (id_expr && id_expr->getName() == "array") {
                    throw ParseError(fmt::format(
                        "Expected member name at line {}\n"
                        "  Got: 'new' after 'array.'\n\n"
                        "Help: 'new' is reserved for struct instantiation, not module methods.\n\n"
                        "  To create an empty list:\n"
                        "    ✗ array.new()\n"
                        "    ✓ let items: list<string> = []\n"
                        "    ✓ let items = []  // Type inferred\n\n"
                        "  To create a list with values:\n"
                        "    ✓ let items = [\"a\", \"b\", \"c\"]",
                        current().line
                    ));
                }
                throw ParseError(fmt::format(
                    "Expected member name at line {}\n"
                    "  Got: 'new'\n\n"
                    "Help: 'new' is a reserved keyword and cannot be used as a method name.",
                    current().line
                ));
            }

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
    // Week 1, Task 1.3: Track parse depth to prevent stack overflow
    DepthGuard guard(parse_depth_);

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

    if (match(lexer::TokenType::NULL_LITERAL)) {
        return std::make_unique<ast::LiteralExpr>(ast::LiteralKind::Null, "null", ast::SourceLocation());
    }

    // Struct literal: new StructName<T> { field: value, ... } or new module.StructName { ... }
    if (match(lexer::TokenType::NEW)) {
        auto& name_token = expect(lexer::TokenType::IDENTIFIER, "Expected struct name after 'new'");
        std::string struct_name = name_token.value;

        // ISS-024 Fix: Check for module-qualified struct name (module.StructName)
        if (match(lexer::TokenType::DOT)) {
            // First identifier was module name, parse actual struct name
            auto& type_token = expect(lexer::TokenType::IDENTIFIER, "Expected struct name after '.'");
            struct_name = name_token.value + "." + type_token.value;  // Store as "module.StructName"
        }

        // ISS-001 Fix: Parse generic type arguments if present
        if (match(lexer::TokenType::LT)) {
            // Parse type arguments: <int>, <string>, <T>, etc.
            // For now, we parse and consume them but don't store them
            // This allows the syntax to work even if full generics aren't implemented
            do {
                parseType();  // Parse and consume the type argument
            } while (match(lexer::TokenType::COMMA));
            expect(lexer::TokenType::GT, "Expected '>' after generic type arguments");
        }

        return parseStructLiteral(struct_name);
    }

    // Inline polyglot code: <<language ... >>
    if (match(lexer::TokenType::INLINE_CODE)) {
        std::string value = tokens_[pos_ - 1].value;

        // Phase 2.2: Parse "language:code" or "language[var1,var2]:code" format
        size_t colon_pos = value.find(':');
        if (colon_pos == std::string::npos) {
            throw ParseError("Invalid inline code format at line " + std::to_string(tokens_[pos_ - 1].line));
        }

        std::string language_part = value.substr(0, colon_pos);
        std::string code = value.substr(colon_pos + 1);

        // Extract language and optional variable list
        std::string language;
        std::vector<std::string> bound_vars;

        size_t bracket_pos = language_part.find('[');
        if (bracket_pos != std::string::npos) {
            // Has variable binding: "language[var1,var2]"
            language = language_part.substr(0, bracket_pos);

            // Extract variable list from brackets
            size_t close_bracket = language_part.find(']', bracket_pos);
            if (close_bracket != std::string::npos) {
                std::string var_list = language_part.substr(bracket_pos + 1, close_bracket - bracket_pos - 1);

                // Split by comma
                size_t start = 0;
                while (start < var_list.size()) {
                    // Skip whitespace
                    while (start < var_list.size() && (var_list[start] == ' ' || var_list[start] == '\t')) {
                        start++;
                    }

                    // Find next comma or end
                    size_t comma = var_list.find(',', start);
                    size_t end = (comma != std::string::npos) ? comma : var_list.size();

                    // Extract variable name
                    std::string var_name = var_list.substr(start, end - start);

                    // Trim trailing whitespace
                    while (!var_name.empty() && (var_name.back() == ' ' || var_name.back() == '\t')) {
                        var_name.pop_back();
                    }

                    if (!var_name.empty()) {
                        bound_vars.push_back(var_name);
                    }

                    start = (comma != std::string::npos) ? comma + 1 : var_list.size();
                }
            }
        } else {
            // No variable binding
            language = language_part;
        }

        return std::make_unique<ast::InlineCodeExpr>(language, code, bound_vars, ast::SourceLocation());
    }

    // Identifier
    if (match(lexer::TokenType::IDENTIFIER)) {
        auto& token = tokens_[pos_ - 1];
        std::string name = token.value;
        ast::SourceLocation loc(token.line, token.column, filename_);
        return std::make_unique<ast::IdentifierExpr>(name, loc);
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
        // Phase 2.1: Set context flag for better error hints about dict keys
        parser_context_->in_dict_literal = true;

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

        parser_context_->in_dict_literal = false;

        return std::make_unique<ast::DictExpr>(std::move(pairs), ast::SourceLocation());
    }

    throw ParseError(formatError("Unexpected token in expression", current()));
}

// ============================================================================
// Type Parsing
// ============================================================================

// Phase 2.4.2: Parse a base type (non-union)
ast::Type Parser::parseBaseType() {
    auto& token = current();

    // Phase 2.1: Check for reference type (ref Type)
    bool is_reference = false;
    if (match(lexer::TokenType::REF)) {
        is_reference = true;
    }

    // Nullable will be checked AFTER parsing base type (int? not ?int)
    bool is_nullable = false;

    // ISS-002: Handle 'function' keyword as a type
    if (match(lexer::TokenType::FUNCTION)) {
        return ast::Type(ast::TypeKind::Function, "", is_nullable, is_reference);
    }

    // ISS-024 Fix: Check for IDENTIFIER (possibly module-qualified)
    if (check(lexer::TokenType::IDENTIFIER)) {
        std::string type_name = current().value;  // Capture BEFORE advancing
        advance();  // Consume the IDENTIFIER

        // ISS-024 Fix: Check for module-qualified type (module.Type)
        std::string module_prefix = "";
        if (check(lexer::TokenType::DOT)) {
            // First identifier was the module name
            module_prefix = type_name;
            advance();  // consume DOT

            // Parse actual type name
            if (!check(lexer::TokenType::IDENTIFIER)) {
                throw ParseError(formatError("Expected type name after '.'", current()));
            }
            type_name = current().value;
            advance();  // Consume the type name
        }

        // Phase 1.3: Detect uppercase type names and provide helpful error
        // Check for common uppercase type mistakes
        if (type_name == "INT" || type_name == "FLOAT" || type_name == "STRING" ||
            type_name == "BOOL" || type_name == "VOID" || type_name == "ANY") {
            std::string lowercase_name = type_name;
            std::transform(lowercase_name.begin(), lowercase_name.end(), lowercase_name.begin(), ::tolower);

            error_reporter_.error(
                fmt::format("Type names must be lowercase. Use '{}' instead of '{}'",
                           lowercase_name, type_name),
                token.line, token.column
            );
            error_reporter_.addSuggestion(
                fmt::format("Change '{}' to '{}'", type_name, lowercase_name)
            );

            throw ParseError(formatError(
                fmt::format("Invalid type name '{}'. Type names are case-sensitive and must be lowercase.", type_name),
                token
            ));
        }

        // Built-in types (only if no module prefix)
        if (module_prefix.empty()) {
            if (type_name == "int") return ast::Type(ast::TypeKind::Int, "", is_nullable, is_reference);
            if (type_name == "float") return ast::Type(ast::TypeKind::Float, "", is_nullable, is_reference);
            if (type_name == "string") return ast::Type(ast::TypeKind::String, "", is_nullable, is_reference);
            if (type_name == "bool") return ast::Type(ast::TypeKind::Bool, "", is_nullable, is_reference);
            if (type_name == "void") return ast::Type(ast::TypeKind::Void, "", is_nullable, is_reference);
            if (type_name == "any") return ast::Type(ast::TypeKind::Any, "", is_nullable, is_reference);
            // Note: 'function' is handled above as TokenType::FUNCTION before IDENTIFIER check
        }

        // List or Dict with type parameters (only if no module prefix)
        if (module_prefix.empty() && type_name == "list") {
            // Phase 2.4.1: Support both List<T> (angle brackets) and list[T] (square brackets)
            if (match(lexer::TokenType::LT)) {
                // List<T> syntax
                auto elem_type = parseType();
                expectGTOrSplitGTGT("Expected '>' after list element type");
                ast::Type list_type(ast::TypeKind::List);
                list_type.element_type = std::make_shared<ast::Type>(elem_type);
                return list_type;
            } else if (match(lexer::TokenType::LBRACKET)) {
                // Legacy list[T] syntax
                auto elem_type = parseType();
                expect(lexer::TokenType::RBRACKET, "Expected ']'");
                ast::Type list_type(ast::TypeKind::List);
                list_type.element_type = std::make_shared<ast::Type>(elem_type);
                return list_type;
            }
            return ast::Type(ast::TypeKind::List);
        }

        if (module_prefix.empty() && type_name == "dict") {
            // Phase 2.4.1: Support both Dict<K, V> (angle brackets) and dict[K, V] (square brackets)
            if (match(lexer::TokenType::LT)) {
                // Dict<K, V> syntax
                auto key_type = parseType();
                expect(lexer::TokenType::COMMA, "Expected ',' in dict type");
                auto val_type = parseType();
                expectGTOrSplitGTGT("Expected '>' after dict value type");
                ast::Type dict_type(ast::TypeKind::Dict);
                dict_type.key_value_types = std::make_shared<std::pair<ast::Type, ast::Type>>(
                    key_type, val_type
                );
                return dict_type;
            } else if (match(lexer::TokenType::LBRACKET)) {
                // Legacy dict[K, V] syntax
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

        // Phase 2.4.1: Check if it's a type parameter (single uppercase letter like T, U, etc.)
        if (module_prefix.empty() && type_name.length() == 1 && std::isupper(type_name[0])) {
            // Likely a type parameter
            ast::Type type_param(ast::TypeKind::TypeParameter, "", is_nullable, is_reference);
            type_param.type_parameter_name = type_name;
            return type_param;
        }

        // Phase 4.1: Check if this is an enum type (only if no module prefix)
        if (module_prefix.empty() && enum_names_.count(type_name) > 0) {
            // This is an enum type
            return ast::Type::makeEnum(type_name);
        }

        // Struct type or Block type
        // Phase 2.4.1: Check for generic type arguments (e.g., Pair<int, string>)
        ast::Type struct_type(ast::TypeKind::Struct, type_name, is_nullable, is_reference);
        struct_type.module_prefix = module_prefix;  // ISS-024 Fix: Store module prefix
        if (match(lexer::TokenType::LT)) {
            // Parse generic type arguments
            do {
                struct_type.type_arguments.push_back(parseType());
            } while (match(lexer::TokenType::COMMA));
            expectGTOrSplitGTGT("Expected '>' after generic type arguments");
        }
        return struct_type;
    }

    throw ParseError(formatError("Expected type name", current()));
}

// Phase 2.4.2: Parse type with union support (int | string)
ast::Type Parser::parseType() {
    // Parse first type
    ast::Type first_type = parseBaseType();

    // Check for union operator (|)
    ast::Type result_type = first_type;  // Initialize with first_type
    if (check(lexer::TokenType::PIPE)) {
        // This is a union type
        std::vector<ast::Type> union_members;
        union_members.push_back(first_type);

        // Parse additional types separated by |
        while (match(lexer::TokenType::PIPE)) {
            union_members.push_back(parseBaseType());
        }

        // Create union type
        result_type = ast::Type(ast::TypeKind::Union);
        result_type.union_types = std::move(union_members);
    } else {
        // Not a union, use the single type
        result_type = first_type;
    }

    // Phase 2.4.5: Check for nullable suffix (int? or (int | string)?)
    if (match(lexer::TokenType::QUESTION)) {
        result_type.is_nullable = true;
    }

    return result_type;
}

// ============================================================================
// Phase 2.1: Parser Context Helpers for Enhanced Error Hints
// ============================================================================

void Parser::updateParserContext() {
    // Update current, previous, and next tokens
    parser_context_->current_token = !isAtEnd() ? &current() : nullptr;
    parser_context_->previous_token = pos_ > 0 ? &tokens_[pos_ - 1] : nullptr;
    parser_context_->next_token = (pos_ + 1 < tokens_.size()) ? &tokens_[pos_ + 1] : nullptr;

    // Keep track of recent tokens (last 5)
    if (parser_context_->current_token) {
        parser_context_->recent_tokens.push_back(parser_context_->current_token);
        if (parser_context_->recent_tokens.size() > 5) {
            parser_context_->recent_tokens.erase(parser_context_->recent_tokens.begin());
        }
    }
}

void Parser::setContextFlag(bool& flag, bool value) {
    flag = value;
}

std::vector<std::string> Parser::getErrorHints(
    const lexer::Token& unexpected,
    const std::string& expected
) {
    // Update context before generating hints
    updateParserContext();

    // Use ErrorHints to generate helpful suggestions
    return ErrorHints::getHintsForParseError(unexpected, expected, *parser_context_);
}

} // namespace parser
} // namespace naab
