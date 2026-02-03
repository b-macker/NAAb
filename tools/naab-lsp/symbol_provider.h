#pragma once

#include "document_manager.h"
#include "naab/ast.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

namespace naab {
namespace lsp {

using json = nlohmann::json;

// LSP SymbolKind enum
enum class SymbolKind {
    File = 1,
    Module = 2,
    Namespace = 3,
    Package = 4,
    Class = 5,
    Method = 6,
    Property = 7,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Interface = 11,
    Function = 12,
    Variable = 13,
    Constant = 14,
    String = 15,
    Number = 16,
    Boolean = 17,
    Array = 18
};

// Document symbol
struct DocumentSymbol {
    std::string name;
    std::string detail;
    SymbolKind kind;
    Range range;
    Range selectionRange;
    std::vector<DocumentSymbol> children;

    json toJson() const;
};

// Symbol provider - extracts symbols from AST
class SymbolProvider {
public:
    SymbolProvider();

    // Get all symbols in document (for outline view)
    std::vector<DocumentSymbol> getDocumentSymbols(const Document& doc);

private:
    // AST visitors to extract symbols
    DocumentSymbol extractFunction(const ast::FunctionDecl* func);
    DocumentSymbol extractStruct(const ast::StructDecl* struct_decl);
    DocumentSymbol extractEnum(const ast::EnumDecl* enum_decl);
    DocumentSymbol extractVariable(const ast::VarDeclStmt* var_decl);

    // Helper to create range from AST node
    Range createRange(size_t line, size_t column);
    Range createRange(int line, int column);  // Overload for SourceLocation
};

} // namespace lsp
} // namespace naab
