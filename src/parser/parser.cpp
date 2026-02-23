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

// ============================================================================
// Name token helpers - centralized keyword-as-name handling
// ============================================================================

// Helper: Check if a token type can be used as a name (variable, parameter, etc.)
// Many keywords are valid names in context (e.g., 'config', 'init', 'module')
static bool isAllowedNameToken(lexer::TokenType type) {
    switch (type) {
        case lexer::TokenType::IDENTIFIER:
        case lexer::TokenType::CONFIG:
        case lexer::TokenType::INIT:
        case lexer::TokenType::MODULE:
        case lexer::TokenType::FROM:
        case lexer::TokenType::DEFAULT:
        case lexer::TokenType::MATCH:
        case lexer::TokenType::METHOD:
        case lexer::TokenType::NEW:
        case lexer::TokenType::CLASS:
        case lexer::TokenType::ENUM:
        case lexer::TokenType::AS:
        case lexer::TokenType::IN:
        case lexer::TokenType::ASYNC:
        case lexer::TokenType::AWAIT:
        case lexer::TokenType::IMPORT:
        case lexer::TokenType::EXPORT:
        case lexer::TokenType::USE:
        case lexer::TokenType::REF:
        case lexer::TokenType::FUNCTION:  // 'func'/'fn'/'def' used as param names
        case lexer::TokenType::STRUCT:    // 'struct' used as name
        case lexer::TokenType::TRY:
        case lexer::TokenType::CATCH:
        case lexer::TokenType::THROW:
        case lexer::TokenType::FINALLY:
            return true;
        default:
            return false;
    }
}

// Helper: Check if a token type can be used as a member name after '.'
// More permissive than variable names - allows almost all keywords
static bool isAllowedMemberToken(lexer::TokenType type) {
    if (isAllowedNameToken(type)) return true;
    switch (type) {
        case lexer::TokenType::IF:
        case lexer::TokenType::ELSE:
        case lexer::TokenType::FOR:
        case lexer::TokenType::WHILE:
        case lexer::TokenType::BREAK:
        case lexer::TokenType::CONTINUE:
        case lexer::TokenType::RETURN:
        case lexer::TokenType::TRY:
        case lexer::TokenType::CATCH:
        case lexer::TokenType::THROW:
        case lexer::TokenType::FINALLY:
        case lexer::TokenType::LET:
        case lexer::TokenType::CONST:
        case lexer::TokenType::FUNCTION:
        case lexer::TokenType::STRUCT:
        case lexer::TokenType::MAIN:
            return true;
        default:
            return false;
    }
}

// Reserved keywords that cannot be used as variable/parameter names
// Note: 'func'/'fn'/'def' map to FUNCTION token but ARE allowed as param names
// (e.g., `fn apply(func: function)` is valid - 'func' is the param name)
static const std::unordered_set<std::string> forbidden_names = {
    "if", "else", "for", "while", "break", "continue",
    "return", "let", "const", "main", "true", "false", "null"
};

// Helper: Format a helpful error when a reserved keyword is used as a name
static std::string formatReservedNameError(const std::string& name, const std::string& context) {
    std::string msg = fmt::format(
        "'{}' is a reserved keyword and cannot be used as a {} name\n\n"
        "  Help: '{}' is used for control flow in NAAb.\n"
        "  Try a descriptive alternative instead:\n\n",
        name, context, name
    );

    if (name == "if" || name == "else") {
        msg += "  Example:\n"
               "    ✗ Wrong: let if = true\n"
               "    ✓ Right: let condition = true\n"
               "    ✓ Right: let is_ready = true\n";
    } else if (name == "for" || name == "while") {
        msg += "  Example:\n"
               "    ✗ Wrong: let for = items\n"
               "    ✓ Right: let items = [1, 2, 3]\n"
               "    ✓ Right: let loop_count = 10\n";
    } else if (name == "return") {
        msg += "  Example:\n"
               "    ✗ Wrong: let return = getValue()\n"
               "    ✓ Right: let result = getValue()\n"
               "    ✓ Right: let output = getValue()\n";
    } else if (name == "function" || name == "fn" || name == "func" || name == "def") {
        msg += "  Example:\n"
               "    ✗ Wrong: let func = someFunction\n"
               "    ✓ Right: let handler = someFunction\n"
               "    ✓ Right: let callback = someFunction\n";
    } else if (name == "let" || name == "const") {
        msg += "  Example:\n"
               "    ✗ Wrong: func process(let: int)\n"
               "    ✓ Right: func process(value: int)\n";
    } else if (name == "true" || name == "false" || name == "null") {
        msg += "  Example:\n"
               "    ✗ Wrong: let true = 1\n"
               "    ✓ Right: let is_valid = true\n"
               "    ✓ Right: let enabled = false\n";
    } else {
        msg += fmt::format(
               "  Example:\n"
               "    ✗ Wrong: let {} = value\n"
               "    ✓ Right: let my_{} = value\n",
               name, name
        );
    }

    msg += "\n  Note: These keywords ARE allowed as names: config, init, module,\n"
           "        from, default, match, method, new, class, enum, as, in,\n"
           "        async, await, import, export, use, ref";

    return msg;
}

// Helper: Format a helpful error when an unexpected token is found where a name is expected
static std::string formatUnexpectedNameError(const lexer::Token& tok, const std::string& context) {
    std::string msg = fmt::format(
        "Expected {} name, got '{}' ({})\n\n",
        context, tok.value,
        tok.type == lexer::TokenType::LBRACE ? "opening brace" :
        tok.type == lexer::TokenType::LPAREN ? "opening parenthesis" :
        tok.type == lexer::TokenType::RPAREN ? "closing parenthesis" :
        tok.type == lexer::TokenType::COLON ? "colon" :
        tok.type == lexer::TokenType::EQ ? "equals sign" :
        tok.type == lexer::TokenType::COMMA ? "comma" :
        "unexpected token"
    );

    if (context == "variable") {
        msg += "  Help: Variable names must be identifiers.\n\n"
               "  Example:\n"
               "    let myVariable = 10\n"
               "    let user_name = \"Alice\"\n"
               "    let config = {\"key\": \"value\"}\n";
    } else if (context == "parameter") {
        msg += "  Help: Parameter names must be identifiers.\n\n"
               "  Example:\n"
               "    func process(input: string, count: int) {\n"
               "        // ...\n"
               "    }\n";
    } else if (context == "loop variable") {
        msg += "  Help: For-loop variable must be an identifier.\n\n"
               "  Example:\n"
               "    for item in items {\n"
               "        print(item)\n"
               "    }\n";
    }

    return msg;
}

// ============================================================================

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
        // Track brace positions for better error messages
        if (type == lexer::TokenType::LBRACE) {
            brace_stack_.push_back(current().line);
        } else if (type == lexer::TokenType::RBRACE && !brace_stack_.empty()) {
            brace_stack_.pop_back();
        }
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
        // Track brace positions for better error messages
        if (type == lexer::TokenType::LBRACE) {
            brace_stack_.push_back(current().line);
        } else if (type == lexer::TokenType::RBRACE && !brace_stack_.empty()) {
            brace_stack_.pop_back();
        }
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

    // Enhanced error for missing '}' - show where the opening '{' was
    if (type == lexer::TokenType::RBRACE && !brace_stack_.empty()) {
        size_t open_line = brace_stack_.back();

        // Count total braces in file for diagnostic
        size_t total_open = 0, total_close = 0;
        for (const auto& t : tokens_) {
            if (t.type == lexer::TokenType::LBRACE) total_open++;
            else if (t.type == lexer::TokenType::RBRACE) total_close++;
        }

        std::string enhanced_msg = fmt::format(
            "{}\n\n"
            "  The opening '{{' was at line {}.\n"
            "  Brace count in file: {} opening '{{' vs {} closing '}}' ({} missing)\n\n"
            "  Help:\n"
            "  - You are missing {} closing '}}' brace(s)\n"
            "  - Check that all blocks (if/for/while/func/main) between line {} and EOF are properly closed\n"
            "  - This is NOT a parser limitation — the file just has mismatched braces",
            formatError(msg, token), open_line,
            total_open, total_close,
            total_open > total_close ? total_open - total_close : total_close - total_open,
            total_open > total_close ? total_open - total_close : total_close - total_open,
            open_line);
        throw ParseError(enhanced_msg);
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

// Helper: format location string with optional filename
std::string Parser::formatLocation(int line, int column) {
    if (!filename_.empty()) {
        return fmt::format("in {} at line {}, column {}", filename_, line, column);
    }
    return fmt::format("at line {}, column {}", line, column);
}

std::string Parser::formatError(const std::string& msg, const lexer::Token& token) {
    return fmt::format("Parse error {}: {}\n  Got: '{}'",
                      formatLocation(token.line, token.column), msg, token.value);
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
                            "Parse error {}: '{}' statements must be inside a 'main {{}}' block or function.\n"
                            "  Hint: Top level can only contain: use, import, export, struct, enum, function, main",
                            formatLocation(tok.line, tok.column), tok.value
                        )
                    );
                }
                // 'var' keyword - suggest 'let'
                else if (tok.type == lexer::TokenType::IDENTIFIER && tok.value == "var") {
                    throw std::runtime_error(
                        fmt::format(
                            "Parse error {}: NAAb uses 'let' instead of 'var' for variable declarations.\n\n"
                            "  Also, variables must be inside a 'main {{}}' block or function.\n\n"
                            "  \xE2\x9C\x97 Wrong:\n"
                            "    var x = 10\n\n"
                            "  \xE2\x9C\x93 Right:\n"
                            "    main {{\n"
                            "        let x = 10\n"
                            "    }}\n\n"
                            "  Hint: Top level can only contain: use, import, export, struct, enum, function, main",
                            formatLocation(tok.line, tok.column)
                        )
                    );
                }
                // 'block' keyword - explain NAAb structure
                else if (tok.type == lexer::TokenType::IDENTIFIER && tok.value == "block") {
                    throw std::runtime_error(
                        fmt::format(
                            "Parse error {}: 'block' is not a top-level construct in NAAb.\n\n"
                            "  NAAb uses 'function' for reusable code and 'main' for the entry point.\n\n"
                            "  \xE2\x9C\x97 Wrong:\n"
                            "    block MyModule {{\n"
                            "        // ...\n"
                            "    }}\n\n"
                            "  \xE2\x9C\x93 Right - use functions:\n"
                            "    function my_function(param: string) -> string {{\n"
                            "        return param\n"
                            "    }}\n\n"
                            "  \xE2\x9C\x93 Right - use main for entry point:\n"
                            "    main {{\n"
                            "        let result = my_function(\"hello\")\n"
                            "        print(result)\n"
                            "    }}\n\n"
                            "  Hint: Top level can only contain: use, import, export, struct, enum, function, main",
                            formatLocation(tok.line, tok.column)
                        )
                    );
                }
                // 'class' keyword - explain NAAb uses 'struct'
                else if (tok.type == lexer::TokenType::CLASS) {
                    throw std::runtime_error(
                        fmt::format(
                            "Parse error {}: NAAb uses 'struct' instead of 'class'.\n\n"
                            "  \xE2\x9C\x97 Wrong:\n"
                            "    class Person {{\n"
                            "        name: string\n"
                            "    }}\n\n"
                            "  \xE2\x9C\x93 Right:\n"
                            "    struct Person {{\n"
                            "        name: string\n"
                            "    }}\n\n"
                            "  Hint: Top level can only contain: use, import, export, struct, enum, function, main",
                            formatLocation(tok.line, tok.column)
                        )
                    );
                }
                else {
                    throw std::runtime_error(
                        fmt::format(
                            "Parse error {}: Unexpected '{}' at top level.\n\n"
                            "  Hint: Top level can only contain: use, import, export, struct, enum, function, main\n\n"
                            "  All other statements (let, print, for, if, etc.) must be inside a 'main {{}}' block or function.\n\n"
                            "  Example:\n"
                            "    main {{\n"
                            "        // your code here\n"
                            "    }}",
                            formatLocation(tok.line, tok.column), tok.value
                        )
                    );
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

    optionalSemicolon();  // Allow optional semicolon after use statement

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

    optionalSemicolon();  // Allow optional semicolon after import

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
    // Detect common mistake: use "path/to/file.naab" (string literal instead of module name)
    if (check(lexer::TokenType::STRING)) {
        auto& tok = current();
        throw ParseError(fmt::format(
            "Parse error {}: 'use' requires a module name, not a file path string.\n\n"
            "  \xE2\x9C\x97 Wrong:  use \"{}\"\n"
            "  \xE2\x9C\x93 Right:  use modules.risk_engine\n\n"
            "  NAAb resolves modules relative to the script file's directory.\n"
            "  If your script is at /project/output/script.naab and you need modules/risk_engine.naab,\n"
            "  move the script to /project/script.naab, then 'use modules.risk_engine' will work.\n\n"
            "  There is no way to use absolute file paths in 'use' statements.\n"
            "  Place your script next to the modules/ directory instead.",
            formatLocation(tok.line, tok.column), tok.value));
    }
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

    optionalSemicolon();  // Allow optional semicolon after use statement

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

    // Detect 'function main()' or 'func main()' pattern - suggest 'main { ... }' syntax
    if (check(lexer::TokenType::MAIN)) {
        auto& tok = current();
        throw std::runtime_error(
            fmt::format(
                "Parse error {}: NAAb uses 'main {{}}' as the entry point, not 'function main()'.\n\n"
                "  \xE2\x9C\x97 Wrong:\n"
                "    function main() {{\n"
                "        // ...\n"
                "    }}\n\n"
                "  \xE2\x9C\x93 Right:\n"
                "    main {{\n"
                "        // your code here\n"
                "    }}\n\n"
                "  Note: 'main' is a special block, not a function declaration.",
                formatLocation(tok.line, tok.column)
            )
        );
    }

    // Allow keywords like 'init', 'config', etc. as function names
    if (!isAllowedNameToken(current().type)) {
        throw std::runtime_error(
            fmt::format("Parse error at {}: Expected function name, got '{}'",
                formatLocation(current().line, current().column),
                current().value)
        );
    }
    std::string name = current().value;
    advance();

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
            // Accept identifiers and most keywords as parameter names
            // (keywords like 'config', 'init', 'module' are commonly used as param names)
            auto& param_tok = current();
            std::string param_name_str;
            if (isAllowedNameToken(param_tok.type)) {
                param_name_str = param_tok.value;
                advance();
                // Check for forbidden names
                if (forbidden_names.count(param_name_str)) {
                    throw ParseError(formatError(
                        formatReservedNameError(param_name_str, "parameter"),
                        param_tok
                    ));
                }
            } else {
                throw ParseError(formatError(
                    formatUnexpectedNameError(param_tok, "parameter"),
                    param_tok
                ));
            }

            // Optional type annotation: param: type (type defaults to Any if omitted)
            ast::Type param_type = ast::Type::makeAny();
            if (match(lexer::TokenType::COLON)) {
                param_type = parseType();
            }

            // Optional default value
            std::optional<std::unique_ptr<ast::Expr>> default_value;
            if (match(lexer::TokenType::EQ)) {
                default_value = parseExpression();
            }

            params.push_back({param_name_str, param_type, std::move(default_value)});

            skipNewlines();
        } while (match(lexer::TokenType::COMMA));
    }

    expect(lexer::TokenType::RPAREN, "Expected ')'");

    // Optional return type: -> type or : type (both accepted for LLM compatibility)
    // Phase 2.4.4 Phase 2: Use Any to mark "needs inference"
    ast::Type return_type = ast::Type::makeAny();  // Default to Any (will be inferred)
    if (match(lexer::TokenType::ARROW)) {
        return_type = parseType();  // Explicit return type provided
    } else if (check(lexer::TokenType::COLON)) {
        // Accept ': ReturnType' as alternative to '-> ReturnType'
        advance();  // consume ':'
        return_type = parseType();
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

    // Allow keywords as struct names
    if (!isAllowedNameToken(current().type)) {
        throw std::runtime_error(
            fmt::format("Parse error at {}: Expected struct name, got '{}'",
                formatLocation(current().line, current().column),
                current().value)
        );
    }
    std::string struct_name = current().value;
    advance();

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

    // Allow keywords as enum names
    if (!isAllowedNameToken(current().type)) {
        throw std::runtime_error(
            fmt::format("Parse error at {}: Expected enum name, got '{}'",
                formatLocation(current().line, current().column),
                current().value)
        );
    }
    std::string enum_name = current().value;
    advance();

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
    // Disambiguate: `func myFunc(...)` = declaration, `func(...)` or `func(x)` = expression
    if (check(lexer::TokenType::FUNCTION)) {
        // If FUNCTION followed by IDENTIFIER, it's a function declaration
        if (pos_ + 1 < tokens_.size() && isAllowedNameToken(tokens_[pos_ + 1].type) &&
            tokens_[pos_ + 1].type != lexer::TokenType::FUNCTION) {
            auto func_decl = parseFunctionDecl();
            auto loc = func_decl->getLocation();
            return std::make_unique<ast::FunctionDeclStmt>(std::move(func_decl), loc);
        }
        // Otherwise fall through to expression statement (lambda or variable call)
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
    // Phase 12: runtime name = language.start()
    if (check(lexer::TokenType::RUNTIME)) {
        return parseRuntimeDeclStmt();
    }

    // Match expression used as statement
    if (check(lexer::TokenType::MATCH)) {
        auto expr = parseMatchExpr();
        optionalSemicolon();
        return std::make_unique<ast::ExprStmt>(std::move(expr), ast::SourceLocation());
    }

    // Detect 'var' keyword and suggest 'let'
    if (check(lexer::TokenType::IDENTIFIER) && current().value == "var") {
        auto& tok = current();
        throw std::runtime_error(
            fmt::format(
                "Parse error {}: NAAb uses 'let' instead of 'var' for variable declarations.\n\n"
                "  \xE2\x9C\x97 Wrong:  var x = 10\n"
                "  \xE2\x9C\x93 Right:  let x = 10\n\n"
                "  For constants, use 'const':\n"
                "    const PI = 3.14159",
                formatLocation(tok.line, tok.column)
            )
        );
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
    if (!check(lexer::TokenType::NEWLINE) && !check(lexer::TokenType::RBRACE)
        && !check(lexer::TokenType::SEMICOLON)) {
        value = parseExpression();
    }

    optionalSemicolon();  // Allow optional semicolon after return

    return std::make_unique<ast::ReturnStmt>(
        std::move(value),
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::BreakStmt> Parser::parseBreakStmt() {
    auto start = current();
    expect(lexer::TokenType::BREAK, "Expected 'break'");

    optionalSemicolon();  // Allow optional semicolon after break

    return std::make_unique<ast::BreakStmt>(
        ast::SourceLocation(start.line, start.column)
    );
}

std::unique_ptr<ast::ContinueStmt> Parser::parseContinueStmt() {
    auto start = current();
    expect(lexer::TokenType::CONTINUE, "Expected 'continue'");

    optionalSemicolon();  // Allow optional semicolon after continue

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

    // Allow optional parentheses: `for (x in items)` or `for x in items`
    bool has_parens = match(lexer::TokenType::LPAREN);

    // Accept identifiers and common keywords as loop variable names
    auto& var_tok = current();
    std::string var;
    if (isAllowedNameToken(var_tok.type)) {
        var = var_tok.value;
        advance();
    } else {
        throw ParseError(formatError(
            formatUnexpectedNameError(var_tok, "loop variable"),
            var_tok
        ));
    }

    expect(lexer::TokenType::IN, "Expected 'in'");

    auto iterable = parseExpression();

    if (has_parens) {
        expect(lexer::TokenType::RPAREN, "Expected ')' to close for loop parentheses");
    }

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
    if (check(lexer::TokenType::IDENTIFIER) || isAllowedNameToken(current().type)) {
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
    // Allow optional type annotation: catch (e: Exception) - type is ignored
    if (match(lexer::TokenType::COLON)) {
        parseType();  // Consume and discard the type
    }
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

// Phase 12: Parse runtime declarations
// Syntax: runtime name = language.start()
std::unique_ptr<ast::RuntimeDeclStmt> Parser::parseRuntimeDeclStmt() {
    auto start = current();
    expect(lexer::TokenType::RUNTIME, "Expected 'runtime'");

    auto name_tok = current();
    expect(lexer::TokenType::IDENTIFIER, "Expected runtime name after 'runtime'");
    std::string name = name_tok.value;

    expect(lexer::TokenType::EQ, "Expected '=' after runtime name");

    // Parse: language.start()
    auto lang_tok = current();
    expect(lexer::TokenType::IDENTIFIER, "Expected language name (e.g., 'python')");
    std::string language = lang_tok.value;

    expect(lexer::TokenType::DOT, "Expected '.start()' after language name");

    auto method_tok = current();
    expect(lexer::TokenType::IDENTIFIER, "Expected 'start' method");
    if (method_tok.value != "start") {
        throw std::runtime_error(
            fmt::format("Parse error {}: Expected 'start()' but got '{}'.\n\n"
                        "  Usage: runtime name = language.start()\n"
                        "  Example: runtime py = python.start()\n",
                        formatLocation(method_tok.line, method_tok.column),
                        method_tok.value));
    }

    expect(lexer::TokenType::LPAREN, "Expected '(' after 'start'");
    expect(lexer::TokenType::RPAREN, "Expected ')' after '('");

    return std::make_unique<ast::RuntimeDeclStmt>(
        name, language,
        ast::SourceLocation(start.line, start.column));
}

std::unique_ptr<ast::VarDeclStmt> Parser::parseVarDeclStmt() {
    auto start = current();
    bool is_const = match(lexer::TokenType::CONST);
    if (!is_const) {
        expect(lexer::TokenType::LET, "Expected 'let' or 'const'");
    }

    // Accept identifiers and commonly-used keywords as variable names
    auto& name_token = current();
    std::string name;
    if (isAllowedNameToken(name_token.type)) {
        name = name_token.value;
        advance();
        // Reject truly problematic keywords (control flow, etc.)
        if (forbidden_names.count(name)) {
            throw ParseError(formatError(
                formatReservedNameError(name, "variable"),
                name_token
            ));
        }
    } else {
        throw ParseError(formatError(
            formatUnexpectedNameError(name_token, "variable"),
            name_token
        ));
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
        skipNewlines();
        auto value = parseAssignment();  // Right-associative
        expr = std::make_unique<ast::BinaryExpr>(
            ast::BinaryOp::Assign,
            std::move(expr),
            std::move(value),
            ast::SourceLocation()
        );
    }
    // Compound assignment: x += y  →  x = x + y (same for -=, *=, /=, %=)
    else if (check(lexer::TokenType::PLUS_EQ) ||
             check(lexer::TokenType::MINUS_EQ) ||
             check(lexer::TokenType::STAR_EQ) ||
             check(lexer::TokenType::SLASH_EQ) ||
             check(lexer::TokenType::PERCENT_EQ)) {
        auto op_token = current();
        advance();
        skipNewlines();

        // Determine the arithmetic operation
        ast::BinaryOp arith_op;
        switch (op_token.type) {
            case lexer::TokenType::PLUS_EQ:    arith_op = ast::BinaryOp::Add; break;
            case lexer::TokenType::MINUS_EQ:   arith_op = ast::BinaryOp::Sub; break;
            case lexer::TokenType::STAR_EQ:    arith_op = ast::BinaryOp::Mul; break;
            case lexer::TokenType::SLASH_EQ:   arith_op = ast::BinaryOp::Div; break;
            case lexer::TokenType::PERCENT_EQ: arith_op = ast::BinaryOp::Mod; break;
            default: arith_op = ast::BinaryOp::Add; break; // unreachable
        }

        auto value = parseAssignment();  // Right-associative

        // Clone the left-hand side for the arithmetic expression
        auto* id = dynamic_cast<ast::IdentifierExpr*>(expr.get());
        auto* member = dynamic_cast<ast::MemberExpr*>(expr.get());

        std::unique_ptr<ast::Expr> lhs_copy;
        if (id) {
            lhs_copy = std::make_unique<ast::IdentifierExpr>(id->getName(), ast::SourceLocation());
        } else if (member) {
            auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member->getObject());
            if (obj_id) {
                auto obj_copy = std::make_unique<ast::IdentifierExpr>(obj_id->getName(), ast::SourceLocation());
                lhs_copy = std::make_unique<ast::MemberExpr>(std::move(obj_copy), member->getMember(), ast::SourceLocation());
            }
        }

        if (!lhs_copy) {
            throw ParseError(formatError(
                "Compound assignment (+=, -=, etc.) requires a simple target:\n"
                "  variable or member access (obj.field)\n\n"
                "  Example:\n"
                "    x += 1           // variable\n"
                "    obj.count += 1   // member access",
                op_token));
        }

        // Build: x = x + y
        auto arith_expr = std::make_unique<ast::BinaryExpr>(
            arith_op,
            std::move(lhs_copy),
            std::move(value),
            ast::SourceLocation()
        );
        expr = std::make_unique<ast::BinaryExpr>(
            ast::BinaryOp::Assign,
            std::move(expr),
            std::move(arith_expr),
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
        skipNewlines(); // Add this line
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
        skipNewlines(); // Add this line
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
        skipNewlines(); // Add this line
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
        skipNewlines(); // Add this line
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
        skipNewlines(); // Add this line
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
        skipNewlines(); // Add this line
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
        skipNewlines(); // Add this line
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
        skipNewlines(); // Add this line
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

            // Allow keywords as member names (e.g., obj.init, obj.type, obj.match)
            // Many keywords are valid member/method names in practice
            auto& member_tok = current();
            std::string member_name;
            if (isAllowedMemberToken(member_tok.type)) {
                member_name = member_tok.value;
                advance();
            } else {
                throw ParseError(formatError(
                    fmt::format(
                        "Expected member name after '.', got '{}'\n\n"
                        "  Help: Member names can be identifiers or keywords.\n\n"
                        "  Example:\n"
                        "    obj.name      // identifier\n"
                        "    obj.init()    // keyword as method name\n"
                        "    obj.config    // keyword as property name\n",
                        member_tok.value
                    ),
                    member_tok
                ));
            }
            expr = std::make_unique<ast::MemberExpr>(
                std::move(expr),
                member_name,
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

        // Phase 12: Extract optional return type (->TYPE suffix)
        std::string return_type;
        size_t arrow_pos = language_part.find("->");
        if (arrow_pos != std::string::npos) {
            return_type = language_part.substr(arrow_pos + 2);
            language_part = language_part.substr(0, arrow_pos);
        }

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
                    // Skip whitespace (including newlines for multi-line binding lists)
                    while (start < var_list.size() && (var_list[start] == ' ' || var_list[start] == '\t' ||
                           var_list[start] == '\n' || var_list[start] == '\r')) {
                        start++;
                    }

                    // Find next comma or end
                    size_t comma = var_list.find(',', start);
                    size_t end = (comma != std::string::npos) ? comma : var_list.size();

                    // Extract variable name
                    std::string var_name = var_list.substr(start, end - start);

                    // Trim trailing whitespace (including newlines)
                    while (!var_name.empty() && (var_name.back() == ' ' || var_name.back() == '\t' ||
                           var_name.back() == '\n' || var_name.back() == '\r')) {
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

        auto inline_expr = std::make_unique<ast::InlineCodeExpr>(language, code, bound_vars, ast::SourceLocation());
        if (!return_type.empty()) {
            inline_expr->setReturnType(return_type);
        }
        return inline_expr;
    }

    // Identifier (and keywords used as variable names like 'config', 'init', 'module', etc.)
    // Excluded: NEW (struct literals), FUNCTION (lambdas), STRUCT/TRY/CATCH/THROW/FINALLY
    // (these have special handling elsewhere or are only valid in declaration context)
    if (current().type != lexer::TokenType::IDENTIFIER &&
        current().type != lexer::TokenType::NEW &&
        current().type != lexer::TokenType::FUNCTION &&
        current().type != lexer::TokenType::STRUCT &&
        current().type != lexer::TokenType::TRY &&
        current().type != lexer::TokenType::CATCH &&
        current().type != lexer::TokenType::THROW &&
        current().type != lexer::TokenType::FINALLY &&
        current().type != lexer::TokenType::MATCH &&
        isAllowedNameToken(current().type)) {
        // Keyword token being used as a variable name in expression context
        auto& token = current();
        std::string name = token.value;
        ast::SourceLocation loc(token.line, token.column, filename_);
        advance();
        return std::make_unique<ast::IdentifierExpr>(name, loc);
    }
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

    // Match expression: match value { pattern => expr, ... }
    if (check(lexer::TokenType::MATCH)) {
        return parseMatchExpr();
    }

    // If expression: if condition { expr } else { expr }
    if (check(lexer::TokenType::IF)) {
        return parseIfExpr();
    }

    // Lambda expression: function(params) { body } or func(params) { body }
    // Disambiguate from function CALL on a variable named 'func'/'fn'/'def':
    //   func(x) { body }    → lambda (has body block after params)
    //   func(x)             → call expression (no body, variable reference)
    if (check(lexer::TokenType::FUNCTION)) {
        if (pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].type == lexer::TokenType::LPAREN) {
            // Lookahead: scan past matching ')' to check for '{' (lambda body)
            size_t lookahead = pos_ + 2;  // skip FUNCTION and LPAREN
            int paren_depth = 1;
            bool is_lambda = false;
            while (lookahead < tokens_.size() && paren_depth > 0) {
                if (tokens_[lookahead].type == lexer::TokenType::LPAREN) paren_depth++;
                else if (tokens_[lookahead].type == lexer::TokenType::RPAREN) paren_depth--;
                lookahead++;
            }
            // After matching ')': skip optional return type annotation (-> type or : type)
            // then check for '{' (lambda body)
            while (lookahead < tokens_.size() &&
                   tokens_[lookahead].type == lexer::TokenType::NEWLINE) {
                lookahead++;
            }
            if (lookahead < tokens_.size()) {
                auto next_type = tokens_[lookahead].type;
                // Lambda has: -> ReturnType { body } or : ReturnType { body } or { body }
                if (next_type == lexer::TokenType::ARROW ||
                    next_type == lexer::TokenType::COLON ||
                    next_type == lexer::TokenType::LBRACE) {
                    is_lambda = true;
                }
            }
            if (is_lambda) {
                return parseLambdaExpr();
            } else {
                // Not a lambda - treat FUNCTION token as identifier (variable reference)
                auto& token = current();
                std::string name = token.value;
                ast::SourceLocation loc(token.line, token.column, filename_);
                advance();
                return std::make_unique<ast::IdentifierExpr>(name, loc);
            }
        }
        // FUNCTION not followed by '(' - treat as identifier
        auto& token = current();
        std::string name = token.value;
        ast::SourceLocation loc(token.line, token.column, filename_);
        advance();
        return std::make_unique<ast::IdentifierExpr>(name, loc);
    }

    // Provide helpful hints for common mistakes
    auto& tok = current();
    std::string hint;

    // Operator at start of line = likely multi-line expression
    if (tok.type == lexer::TokenType::PLUS || tok.type == lexer::TokenType::MINUS ||
        tok.type == lexer::TokenType::STAR || tok.type == lexer::TokenType::SLASH) {
        hint = "\n\n  Help: Operator '" + tok.value + "' at start of expression is not allowed.\n"
               "  For multi-line expressions, put the operator at the END of the previous line:\n\n"
               "  ✗ Wrong:\n"
               "    let x = \"hello\"\n"
               "        " + tok.value + " \"world\"\n\n"
               "  ✓ Right:\n"
               "    let x = \"hello\" " + tok.value + "\n"
               "        \"world\"\n";
    }
    // 'as' keyword = type casting attempt (not supported in expressions)
    else if (tok.type == lexer::TokenType::AS) {
        hint = "\n\n  Help: NAAb does not support 'as' for type casting in expressions.\n"
               "  NAAb is dynamically typed - values are converted automatically.\n\n"
               "  ✗ Wrong:\n"
               "    return result as MyStruct;\n"
               "    let x = value as int;\n\n"
               "  ✓ Right - just use the value directly:\n"
               "    return result\n"
               "    let x = value\n\n"
               "  Note: 'as' is only used in import statements:\n"
               "    use math_utils as math\n";
    }
    // NEWLINE token = unexpected line break
    else if (tok.type == lexer::TokenType::NEWLINE) {
        hint = "\n\n  Help: Unexpected end of expression.\n"
               "  If continuing on the next line, put the operator at the end:\n"
               "    let x = a +\n"
               "        b\n";
    }

    throw ParseError(formatError("Unexpected token in expression", tok) + hint);
}

// If expression: if condition { expr } else { expr }
std::unique_ptr<ast::Expr> Parser::parseIfExpr() {
    auto start = current();
    expect(lexer::TokenType::IF, "Expected 'if'");

    auto condition = parseExpression();
    skipNewlines();

    expect(lexer::TokenType::LBRACE, "Expected '{' after if condition in if expression");
    skipNewlines();

    auto then_expr = parseExpression();
    skipNewlines();

    expect(lexer::TokenType::RBRACE, "Expected '}' after then expression");
    skipNewlines();

    expect(lexer::TokenType::ELSE, "if expression requires an 'else' branch");
    skipNewlines();

    expect(lexer::TokenType::LBRACE, "Expected '{' after else");
    skipNewlines();

    auto else_expr = parseExpression();
    skipNewlines();

    expect(lexer::TokenType::RBRACE, "Expected '}' after else expression");

    return std::make_unique<ast::IfExpr>(
        std::move(condition),
        std::move(then_expr),
        std::move(else_expr),
        ast::SourceLocation(start.line, start.column, filename_)
    );
}

// Match expression: match subject { pattern => expr, ... }
std::unique_ptr<ast::Expr> Parser::parseMatchExpr() {
    auto start = current();
    expect(lexer::TokenType::MATCH, "Expected 'match'");

    auto subject = parseExpression();
    skipNewlines();

    expect(lexer::TokenType::LBRACE, "Expected '{' after match subject");
    skipNewlines();

    std::vector<ast::MatchArm> arms;

    while (!check(lexer::TokenType::RBRACE) && !check(lexer::TokenType::END_OF_FILE)) {
        std::unique_ptr<ast::Expr> pattern;

        // Check for wildcard '_'
        if (check(lexer::TokenType::IDENTIFIER) && current().value == "_") {
            advance();  // consume '_'
            pattern = nullptr;  // nullptr signals wildcard
        } else {
            // Use parseLogicalOr to avoid consuming across newlines (parsePipeline skips newlines)
            pattern = parseLogicalOr();
        }

        expect(lexer::TokenType::FAT_ARROW, "Expected '=>' after match pattern");
        skipNewlines();

        // Use parseLogicalOr for body to avoid greedy newline consumption
        auto body = parseLogicalOr();
        skipNewlines();

        arms.push_back(ast::MatchArm{std::move(pattern), std::move(body)});

        // Optional comma or newline between arms
        if (check(lexer::TokenType::COMMA)) {
            advance();
            skipNewlines();
        }
    }

    expect(lexer::TokenType::RBRACE, "Expected '}' to close match expression");

    if (arms.empty()) {
        throw ParseError(formatError(
            "Match error: match expression must have at least one arm\n\n"
            "  Example:\n"
            "    match value {\n"
            "        1 => \"one\"\n"
            "        _ => \"other\"\n"
            "    }\n",
            start
        ));
    }

    return std::make_unique<ast::MatchExpr>(
        std::move(subject),
        std::move(arms),
        ast::SourceLocation(start.line, start.column, filename_)
    );
}

// Lambda expression: function(params) -> type { body }
// Type annotations are optional: function(x, y) { return x + y } also works
std::unique_ptr<ast::Expr> Parser::parseLambdaExpr() {
    auto start = current();
    expect(lexer::TokenType::FUNCTION, "Expected 'function'/'func'/'def'/'fn'");
    expect(lexer::TokenType::LPAREN, "Expected '(' after function keyword");

    // Parse parameters (type annotations are optional for lambdas)
    std::vector<ast::Parameter> params;
    std::vector<ast::Type> param_types;

    if (!check(lexer::TokenType::RPAREN)) {
        do {
            skipNewlines();
            // Accept identifiers and keywords as parameter names (same as function decl)
            auto& param_tok = current();
            std::string param_name_str;
            if (isAllowedNameToken(param_tok.type)) {
                param_name_str = param_tok.value;
                advance();
                if (forbidden_names.count(param_name_str)) {
                    throw ParseError(formatError(
                        formatReservedNameError(param_name_str, "parameter"),
                        param_tok
                    ));
                }
            } else {
                throw ParseError(formatError(
                    formatUnexpectedNameError(param_tok, "parameter"),
                    param_tok
                ));
            }

            // Optional type annotation: param: type
            ast::Type param_type = ast::Type::makeAny();
            if (match(lexer::TokenType::COLON)) {
                param_type = parseType();
            }

            // Optional default value
            std::optional<std::unique_ptr<ast::Expr>> default_value;
            if (match(lexer::TokenType::EQ)) {
                default_value = parseExpression();
            }

            params.push_back(ast::Parameter{param_name_str, param_type, std::move(default_value)});
            param_types.push_back(param_type);
            skipNewlines();
        } while (match(lexer::TokenType::COMMA));
    }

    expect(lexer::TokenType::RPAREN, "Expected ')' after parameters");

    // Optional return type: -> type or : type (both accepted for LLM compatibility)
    ast::Type return_type = ast::Type::makeAny();
    if (match(lexer::TokenType::ARROW)) {
        return_type = parseType();
    } else if (check(lexer::TokenType::COLON) && !check(lexer::TokenType::LBRACE)) {
        // Accept ': ReturnType' as alternative to '-> ReturnType' (common in other languages)
        advance();  // consume ':'
        return_type = parseType();
    }

    // Parse body
    auto body = parseCompoundStmt();

    return std::make_unique<ast::LambdaExpr>(
        std::move(params),
        std::move(param_types),
        std::move(return_type),
        std::move(body),
        ast::SourceLocation(start.line, start.column, filename_)
    );
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

        // LLM-friendly type aliases: silently map common alternative type names
        // Java/TypeScript/Python style → NAAb style
        static const std::unordered_map<std::string, std::string> type_aliases = {
            // Capitalized primitives
            {"String", "string"}, {"Int", "int"}, {"Float", "float"},
            {"Bool", "bool"}, {"Boolean", "bool"}, {"Void", "void"},
            {"Any", "any"}, {"Object", "any"},
            // ALL-CAPS primitives
            {"INT", "int"}, {"FLOAT", "float"}, {"STRING", "string"},
            {"BOOL", "bool"}, {"VOID", "void"}, {"ANY", "any"},
            // Collection aliases
            {"Map", "dict"}, {"HashMap", "dict"}, {"Dictionary", "dict"},
            {"Dict", "dict"}, {"Record", "dict"},
            {"List", "list"}, {"Array", "list"}, {"Vec", "list"},
            {"Vector", "list"}, {"Slice", "list"},
            // Special types
            {"Double", "float"}, {"Number", "float"},
            {"Integer", "int"}, {"Long", "int"},
            {"Str", "string"}, {"Char", "string"},
            {"Exception", "any"}, {"Error", "any"},
            {"Callable", "function"}, {"Function", "function"},
            {"Func", "function"},
        };

        auto alias_it = type_aliases.find(type_name);
        if (alias_it != type_aliases.end()) {
            type_name = alias_it->second;
        }

        // Built-in types (only if no module prefix)
        if (module_prefix.empty()) {
            if (type_name == "int") return ast::Type(ast::TypeKind::Int, "", is_nullable, is_reference);
            if (type_name == "float") return ast::Type(ast::TypeKind::Float, "", is_nullable, is_reference);
            if (type_name == "string") return ast::Type(ast::TypeKind::String, "", is_nullable, is_reference);
            if (type_name == "bool") return ast::Type(ast::TypeKind::Bool, "", is_nullable, is_reference);
            if (type_name == "void") return ast::Type(ast::TypeKind::Void, "", is_nullable, is_reference);
            if (type_name == "any") return ast::Type(ast::TypeKind::Any, "", is_nullable, is_reference);
            if (type_name == "function") return ast::Type(ast::TypeKind::Function, "", is_nullable, is_reference);
            // Note: 'function' keyword token is also handled above before IDENTIFIER check
        }

        // List or Dict with type parameters (only if no module prefix)
        // After alias resolution, "Map", "List", etc. are mapped to "dict", "list"
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
