#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <map>
#include <vector>
#include "naab/ast.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/type_checker.h"
#include "naab/symbol_table.h"

namespace naab {
namespace lsp {

using json = nlohmann::json;

// Source location (line, character)
struct Position {
    int line;
    int character;

    static Position fromJson(const json& j);
    json toJson() const;
};

// Source range
struct Range {
    Position start;
    Position end;

    static Range fromJson(const json& j);
    json toJson() const;
};

// Diagnostic severity
enum class DiagnosticSeverity {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4
};

// Diagnostic message
struct Diagnostic {
    Range range;
    DiagnosticSeverity severity;
    std::string code;
    std::string message;
    std::string source = "naab";

    json toJson() const;
};

// Represents an open document
class Document {
public:
    Document(const std::string& uri, const std::string& text, int version);

    // Update document content
    void update(const std::string& new_text, int new_version);

    // Parse and analyze document
    void parse();
    void typeCheck();

    // Getters
    const std::string& getUri() const { return uri_; }
    const std::string& getText() const { return text_; }
    int getVersion() const { return version_; }
    ast::Program* getAST() const { return ast_.get(); }
    const semantic::SymbolTable& getSymbolTable() const { return symbol_table_; }
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics_; }

    // Query
    std::string getLineText(int line) const;
    Position offsetToPosition(size_t offset) const;
    size_t positionToOffset(const Position& pos) const;

private:
    std::string uri_;
    std::string text_;
    int version_;

    // Parsed data
    std::unique_ptr<ast::Program> ast_;
    semantic::SymbolTable symbol_table_;
    std::vector<lexer::Token> tokens_;
    std::vector<Diagnostic> diagnostics_;

    // Symbol table population
    void buildSymbolTable();

    // Parse errors
    void collectDiagnostics();
};

// Manages all open documents
class DocumentManager {
public:
    DocumentManager();

    // Document lifecycle
    void open(const std::string& uri, const std::string& text, int version);
    void update(const std::string& uri, const std::string& text, int version);
    void close(const std::string& uri);

    // Query
    Document* getDocument(const std::string& uri);
    bool hasDocument(const std::string& uri) const;

    // Get all documents
    std::vector<Document*> getAllDocuments();

private:
    std::map<std::string, std::unique_ptr<Document>> documents_;
};

} // namespace lsp
} // namespace naab
