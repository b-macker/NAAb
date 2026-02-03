#include "symbol_provider.h"
#include <sstream>
#include <iostream>

namespace naab {
namespace lsp {

// ============================================================================
// DocumentSymbol
// ============================================================================

json DocumentSymbol::toJson() const {
    json j = {
        {"name", name},
        {"kind", static_cast<int>(kind)},
        {"range", range.toJson()},
        {"selectionRange", selectionRange.toJson()}
    };

    if (!detail.empty()) {
        j["detail"] = detail;
    }

    if (!children.empty()) {
        json children_json = json::array();
        for (const auto& child : children) {
            children_json.push_back(child.toJson());
        }
        j["children"] = children_json;
    }

    return j;
}

// ============================================================================
// SymbolProvider
// ============================================================================

SymbolProvider::SymbolProvider() = default;

// Helper function to convert ast::Type to string
static std::string typeToString(const ast::Type& type) {
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
                return "list[" + typeToString(*type.element_type) + "]";
            }
            return "list";
        case ast::TypeKind::Dict:
            if (type.key_value_types) {
                return "dict[" + typeToString(type.key_value_types->first) + ", " +
                       typeToString(type.key_value_types->second) + "]";
            }
            return "dict";
        case ast::TypeKind::Function: return "function";
        case ast::TypeKind::TypeParameter:
            return type.type_parameter_name.empty() ? "T" : type.type_parameter_name;
        case ast::TypeKind::Union: {
            std::string result;
            for (size_t i = 0; i < type.union_types.size(); ++i) {
                if (i > 0) result += " | ";
                result += typeToString(type.union_types[i]);
            }
            return result.empty() ? "union" : result;
        }
        default: return "unknown";
    }
}

std::vector<DocumentSymbol> SymbolProvider::getDocumentSymbols(const Document& doc) {
    std::vector<DocumentSymbol> symbols;

    ast::Program* program = doc.getAST();
    if (!program) {
        std::cerr << "[SymbolProvider] AST is null\n";
        return symbols;
    }

    std::cerr << "[SymbolProvider] Program has:\n";
    std::cerr << "  Functions: " << program->getFunctions().size() << "\n";
    std::cerr << "  Structs: " << program->getStructs().size() << "\n";
    std::cerr << "  Enums: " << program->getEnums().size() << "\n";
    std::cerr << "  Main block: " << (program->getMainBlock() ? "yes" : "no") << "\n";

    // Extract functions
    for (const auto& func : program->getFunctions()) {
        symbols.push_back(extractFunction(func.get()));
    }

    // Extract structs
    for (const auto& struct_decl : program->getStructs()) {
        symbols.push_back(extractStruct(struct_decl.get()));
    }

    // Extract enums
    for (const auto& enum_decl : program->getEnums()) {
        symbols.push_back(extractEnum(enum_decl.get()));
    }

    // Main block
    if (program->getMainBlock()) {
        DocumentSymbol main_symbol;
        main_symbol.name = "main";
        main_symbol.kind = SymbolKind::Function;
        auto loc = program->getMainBlock()->getLocation();
        main_symbol.range = createRange(loc.line, loc.column);
        main_symbol.selectionRange = main_symbol.range;
        symbols.push_back(main_symbol);
    }

    std::cerr << "[SymbolProvider] Returning " << symbols.size() << " symbols\n";

    return symbols;
}

DocumentSymbol SymbolProvider::extractFunction(const ast::FunctionDecl* func) {
    DocumentSymbol symbol;
    symbol.name = func->getName();
    symbol.kind = SymbolKind::Function;

    // Create signature detail
    std::ostringstream sig;
    sig << "(";
    for (size_t i = 0; i < func->getParams().size(); ++i) {
        if (i > 0) sig << ", ";
        const auto& param = func->getParams()[i];
        sig << param.name << ": " << typeToString(param.type);
    }
    sig << ") -> " << typeToString(func->getReturnType());
    symbol.detail = sig.str();

    auto loc = func->getLocation();
    symbol.range = createRange(loc.line, loc.column);
    symbol.selectionRange = symbol.range;

    // TODO: Extract local variables as children

    return symbol;
}

DocumentSymbol SymbolProvider::extractStruct(const ast::StructDecl* struct_decl) {
    DocumentSymbol symbol;
    symbol.name = struct_decl->getName();
    symbol.kind = SymbolKind::Class;  // Using Class for structs
    auto loc = struct_decl->getLocation();
    symbol.range = createRange(loc.line, loc.column);
    symbol.selectionRange = symbol.range;

    // Add fields as children
    for (const auto& field : struct_decl->getFields()) {
        DocumentSymbol field_symbol;
        field_symbol.name = field.name;
        field_symbol.kind = SymbolKind::Field;
        field_symbol.detail = typeToString(field.type);
        field_symbol.range = createRange(loc.line, loc.column);
        field_symbol.selectionRange = field_symbol.range;
        symbol.children.push_back(field_symbol);
    }

    return symbol;
}

DocumentSymbol SymbolProvider::extractEnum(const ast::EnumDecl* enum_decl) {
    DocumentSymbol symbol;
    symbol.name = enum_decl->getName();
    symbol.kind = SymbolKind::Enum;
    auto loc = enum_decl->getLocation();
    symbol.range = createRange(loc.line, loc.column);
    symbol.selectionRange = symbol.range;

    // Add variants as children
    for (const auto& variant : enum_decl->getVariants()) {
        DocumentSymbol variant_symbol;
        variant_symbol.name = variant.name;
        variant_symbol.kind = SymbolKind::Constant;
        variant_symbol.range = createRange(loc.line, loc.column);
        variant_symbol.selectionRange = variant_symbol.range;
        symbol.children.push_back(variant_symbol);
    }

    return symbol;
}

DocumentSymbol SymbolProvider::extractVariable(const ast::VarDeclStmt* var_decl) {
    DocumentSymbol symbol;
    symbol.name = var_decl->getName();
    symbol.kind = SymbolKind::Variable;

    // Get type if available
    if (var_decl->getType().has_value()) {
        symbol.detail = typeToString(*var_decl->getType());
    }

    auto loc = var_decl->getLocation();
    symbol.range = createRange(loc.line, loc.column);
    symbol.selectionRange = symbol.range;
    return symbol;
}

Range SymbolProvider::createRange(size_t line, size_t column) {
    int l = static_cast<int>(line);
    int c = static_cast<int>(column);

    // TODO: Calculate end position (for now, single character)
    return Range{
        {l, c},
        {l, c + 1}
    };
}

// Overload to accept int directly (to avoid warnings from SourceLocation)
Range SymbolProvider::createRange(int line, int column) {
    // TODO: Calculate end position (for now, single character)
    return Range{
        {line, column},
        {line, column + 1}
    };
}

} // namespace lsp
} // namespace naab
