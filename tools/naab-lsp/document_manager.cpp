#include "document_manager.h"
#include <sstream>
#include <algorithm>
#include <iostream>

namespace naab {
namespace lsp {

// ============================================================================
// Position
// ============================================================================

Position Position::fromJson(const json& j) {
    return Position{
        j["line"].get<int>(),
        j["character"].get<int>()
    };
}

json Position::toJson() const {
    return {
        {"line", line},
        {"character", character}
    };
}

// ============================================================================
// Range
// ============================================================================

Range Range::fromJson(const json& j) {
    return Range{
        Position::fromJson(j["start"]),
        Position::fromJson(j["end"])
    };
}

json Range::toJson() const {
    return {
        {"start", start.toJson()},
        {"end", end.toJson()}
    };
}

// ============================================================================
// Diagnostic
// ============================================================================

json Diagnostic::toJson() const {
    return {
        {"range", range.toJson()},
        {"severity", static_cast<int>(severity)},
        {"code", code},
        {"message", message},
        {"source", source}
    };
}

// ============================================================================
// Document
// ============================================================================

Document::Document(const std::string& uri, const std::string& text, int version)
    : uri_(uri), text_(text), version_(version) {
    parse();
    typeCheck();
}

void Document::update(const std::string& new_text, int new_version) {
    text_ = new_text;
    version_ = new_version;

    // Re-analyze
    parse();
    typeCheck();
}

void Document::parse() {
    std::cerr << "[Document::parse] Starting parse for " << uri_ << "\n";
    diagnostics_.clear();

    try {
        // Tokenize
        std::cerr << "[Document::parse] Tokenizing...\n";
        lexer::Lexer lexer(text_);
        tokens_ = lexer.tokenize();
        std::cerr << "[Document::parse] Got " << tokens_.size() << " tokens\n";

        // Parse
        std::cerr << "[Document::parse] Parsing...\n";
        parser::Parser parser(tokens_);
        parser.setSource(text_, uri_);
        ast_ = parser.parseProgram();
        std::cerr << "[Document::parse] AST created: " << (ast_ ? "yes" : "no") << "\n";

        // Build symbol table
        std::cerr << "[Document::parse] Calling buildSymbolTable...\n";
        buildSymbolTable();

        // Collect parse errors
        collectDiagnostics();

    } catch (const std::exception& e) {
        std::cerr << "[Document::parse] Exception caught: " << e.what() << "\n";
        // Add diagnostic for parse failure
        Diagnostic diag;
        diag.range = Range{{0, 0}, {0, 0}};
        diag.severity = DiagnosticSeverity::Error;
        diag.code = "parse-error";
        diag.message = std::string("Parse error: ") + e.what();
        diagnostics_.push_back(diag);
    } catch (...) {
        std::cerr << "[Document::parse] Unknown exception caught!\n";
        Diagnostic diag;
        diag.range = Range{{0, 0}, {0, 0}};
        diag.severity = DiagnosticSeverity::Error;
        diag.code = "parse-error";
        diag.message = "Unknown parse error";
        diagnostics_.push_back(diag);
    }
}

void Document::typeCheck() {
    if (!ast_) return;

    try {
        // Run type checker
        typecheck::TypeChecker checker;

        // Create a non-owning shared_ptr for type checker
        std::shared_ptr<ast::Program> ast_ptr(ast_.get(), [](ast::Program*){});
        auto type_errors = checker.check(ast_ptr);

        // Convert type errors to diagnostics
        for (const auto& err : type_errors) {
            Diagnostic diag;
            diag.range = Range{
                {static_cast<int>(err.line), static_cast<int>(err.column)},
                {static_cast<int>(err.line), static_cast<int>(err.column) + 1}
            };
            diag.severity = DiagnosticSeverity::Error;
            diag.code = "type-error";
            diag.message = err.message;
            diagnostics_.push_back(diag);
        }

    } catch (const std::exception& e) {
        // Type checker failure (should not happen)
        Diagnostic diag;
        diag.range = Range{{0, 0}, {0, 0}};
        diag.severity = DiagnosticSeverity::Error;
        diag.code = "type-check-error";
        diag.message = std::string("Type check error: ") + e.what();
        diagnostics_.push_back(diag);
    }
}

void Document::collectDiagnostics() {
    // TODO: Extract diagnostics from parser's error reporter
    // For now, if we got here, parsing succeeded
}

// Forward declaration
static void extractVariablesFromStmt(ast::Stmt* stmt, semantic::SymbolTable& symbol_table, const std::string& uri);

// Helper function to convert ast::Type to string for symbol table
static std::string astTypeToString(const ast::Type& type) {
    switch (type.kind) {
        case ast::TypeKind::Void: return "void";
        case ast::TypeKind::Int: return "int";
        case ast::TypeKind::Float: return "float";
        case ast::TypeKind::String: return "string";
        case ast::TypeKind::Bool: return "bool";
        case ast::TypeKind::Any: return "any";
        case ast::TypeKind::Block: return "block";
        case ast::TypeKind::Struct:
            return type.struct_name.empty() ? "struct" : type.struct_name;
        case ast::TypeKind::Enum:
            return type.enum_name.empty() ? "enum" : type.enum_name;
        case ast::TypeKind::List:
            if (type.element_type) {
                return "list[" + astTypeToString(*type.element_type) + "]";
            }
            return "list";
        case ast::TypeKind::Dict:
            if (type.key_value_types) {
                return "dict[" + astTypeToString(type.key_value_types->first) + ", " +
                       astTypeToString(type.key_value_types->second) + "]";
            }
            return "dict";
        case ast::TypeKind::Function: return "function";
        case ast::TypeKind::TypeParameter:
            return type.type_parameter_name.empty() ? "T" : type.type_parameter_name;
        default: return "unknown";
    }
}

void Document::buildSymbolTable() {
    if (!ast_) {
        std::cerr << "[Document] AST is null, cannot build symbol table\n";
        return;
    }

    // Clear existing symbols
    symbol_table_ = semantic::SymbolTable();

    std::cerr << "[Document] Building symbol table for " << uri_ << "\n";

    // Walk AST and populate symbol table
    ast::Program* program = ast_.get();

    // Functions
    for (const auto& func : program->getFunctions()) {
        auto loc = func->getLocation();

        // Create function signature type string
        std::ostringstream sig;
        sig << "(";
        for (size_t i = 0; i < func->getParams().size(); ++i) {
            if (i > 0) sig << ", ";
            const auto& param = func->getParams()[i];
            sig << param.name << ": " << astTypeToString(param.type);
        }
        sig << ") -> " << astTypeToString(func->getReturnType());

        semantic::Symbol symbol(
            func->getName(),
            semantic::SymbolKind::Function,
            sig.str(),
            semantic::SourceLocation(uri_, loc.line, loc.column)
        );
        symbol_table_.define(func->getName(), std::move(symbol));
        std::cerr << "  Added function: " << func->getName() << " " << sig.str() << "\n";

        // TODO: Add parameters and local variables in function scope
    }

    // Structs
    for (const auto& struct_decl : program->getStructs()) {
        auto loc = struct_decl->getLocation();
        semantic::Symbol symbol(
            struct_decl->getName(),
            semantic::SymbolKind::Class,
            "struct",
            semantic::SourceLocation(uri_, loc.line, loc.column)
        );
        symbol_table_.define(struct_decl->getName(), std::move(symbol));
        std::cerr << "  Added struct: " << struct_decl->getName() << "\n";
    }

    // Enums
    for (const auto& enum_decl : program->getEnums()) {
        auto loc = enum_decl->getLocation();
        semantic::Symbol symbol(
            enum_decl->getName(),
            semantic::SymbolKind::Enum,
            "enum",
            semantic::SourceLocation(uri_, loc.line, loc.column)
        );
        symbol_table_.define(enum_decl->getName(), std::move(symbol));
        std::cerr << "  Added enum: " << enum_decl->getName() << "\n";
    }

    // Main block - extract top-level variables
    if (program->getMainBlock()) {
        extractVariablesFromStmt(program->getMainBlock()->getBody(), symbol_table_, uri_);
    }

    std::cerr << "[Document] Symbol table has " << symbol_table_.get_all_symbols().size() << " symbols\n";
}

// Helper to extract variable declarations from statements
static void extractVariablesFromStmt(ast::Stmt* stmt, semantic::SymbolTable& symbol_table, const std::string& uri) {
    if (!stmt) return;

    // Check if this is a VarDeclStmt
    if (stmt->getKind() == ast::NodeKind::VarDeclStmt) {
        ast::VarDeclStmt* var_decl = static_cast<ast::VarDeclStmt*>(stmt);
        auto loc = var_decl->getLocation();

        std::string type_str = "any";
        if (var_decl->getType().has_value()) {
            type_str = astTypeToString(*var_decl->getType());
        }

        semantic::Symbol symbol(
            var_decl->getName(),
            semantic::SymbolKind::Variable,
            type_str,
            semantic::SourceLocation(uri, loc.line, loc.column)
        );
        symbol_table.define(var_decl->getName(), std::move(symbol));
        std::cerr << "  Added variable: " << var_decl->getName() << ": " << type_str << "\n";
    }
    // If compound statement, walk all children
    else if (stmt->getKind() == ast::NodeKind::CompoundStmt) {
        ast::CompoundStmt* compound = static_cast<ast::CompoundStmt*>(stmt);
        for (const auto& child_stmt : compound->getStatements()) {
            extractVariablesFromStmt(child_stmt.get(), symbol_table, uri);
        }
    }
    // TODO: Handle if statements, for loops, etc. that have nested scopes
}

std::string Document::getLineText(int line) const {
    std::istringstream iss(text_);
    std::string line_text;

    for (int i = 0; i <= line && std::getline(iss, line_text); ++i) {
        if (i == line) {
            return line_text;
        }
    }

    return "";
}

Position Document::offsetToPosition(size_t offset) const {
    int line = 0;
    int character = 0;

    for (size_t i = 0; i < offset && i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            ++line;
            character = 0;
        } else {
            ++character;
        }
    }

    return Position{line, character};
}

size_t Document::positionToOffset(const Position& pos) const {
    std::istringstream iss(text_);
    std::string line_text;
    size_t offset = 0;

    for (int line = 0; line < pos.line && std::getline(iss, line_text); ++line) {
        offset += line_text.length() + 1;  // +1 for newline
    }

    offset += static_cast<size_t>(pos.character);
    return offset;
}

// ============================================================================
// DocumentManager
// ============================================================================

DocumentManager::DocumentManager() = default;

void DocumentManager::open(const std::string& uri, const std::string& text, int version) {
    documents_[uri] = std::make_unique<Document>(uri, text, version);
}

void DocumentManager::update(const std::string& uri, const std::string& text, int version) {
    auto it = documents_.find(uri);
    if (it != documents_.end()) {
        it->second->update(text, version);
    }
}

void DocumentManager::close(const std::string& uri) {
    documents_.erase(uri);
}

Document* DocumentManager::getDocument(const std::string& uri) {
    auto it = documents_.find(uri);
    return (it != documents_.end()) ? it->second.get() : nullptr;
}

bool DocumentManager::hasDocument(const std::string& uri) const {
    return documents_.find(uri) != documents_.end();
}

std::vector<Document*> DocumentManager::getAllDocuments() {
    std::vector<Document*> docs;
    for (auto& [uri, doc] : documents_) {
        docs.push_back(doc.get());
    }
    return docs;
}

} // namespace lsp
} // namespace naab
