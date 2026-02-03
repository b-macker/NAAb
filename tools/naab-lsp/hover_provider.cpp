#include "hover_provider.h"
#include <sstream>
#include <iostream>

namespace naab {
namespace lsp {

// ============================================================================
// HoverContents
// ============================================================================

json HoverContents::toJson() const {
    return {
        {"kind", "markdown"},
        {"value", "```" + language + "\n" + value + "\n```"}
    };
}

// ============================================================================
// Hover
// ============================================================================

json Hover::toJson() const {
    json j = {
        {"contents", contents.toJson()}
    };

    if (range) {
        j["range"] = range->toJson();
    }

    return j;
}

// ============================================================================
// HoverProvider
// ============================================================================

HoverProvider::HoverProvider() = default;

std::optional<Hover> HoverProvider::getHover(const Document& doc, const Position& pos) {
    // Find symbol at position
    auto symbol = findSymbolAtPosition(doc, pos);
    if (!symbol) {
        return std::nullopt;
    }

    // Format hover content
    Hover hover;
    hover.contents.value = formatSymbol(*symbol);

    // Set range (highlight the symbol)
    hover.range = Range{
        {static_cast<int>(symbol->location.line), static_cast<int>(symbol->location.column)},
        {static_cast<int>(symbol->location.line), static_cast<int>(symbol->location.column) + static_cast<int>(symbol->name.length())}
    };

    return hover;
}

std::optional<semantic::Symbol> HoverProvider::findSymbolAtPosition(
    const Document& doc,
    const Position& pos) {

    const auto& symbol_table = doc.getSymbolTable();

    std::cerr << "[HoverProvider] Looking for symbol at line " << pos.line << ", char " << pos.character << "\n";

    // Try to look up the identifier at this position
    // First, get the line of text
    std::string line_text = doc.getLineText(pos.line);
    std::cerr << "[HoverProvider] Line text: '" << line_text << "'\n";

    // Extract identifier at cursor position
    std::string identifier;
    int start = pos.character;
    int end = pos.character;

    // Expand backwards
    while (start > 0 && (std::isalnum(line_text[start - 1]) || line_text[start - 1] == '_')) {
        --start;
    }

    // Expand forwards
    while (end < static_cast<int>(line_text.length()) &&
           (std::isalnum(line_text[end]) || line_text[end] == '_')) {
        ++end;
    }

    if (start < end) {
        identifier = line_text.substr(start, end - start);
        std::cerr << "[HoverProvider] Extracted identifier: '" << identifier << "'\n";

        // Look up symbol in symbol table
        auto result = symbol_table.lookup(identifier);
        if (result) {
            std::cerr << "[HoverProvider] Found symbol: " << result->name << ": " << result->type << "\n";
        } else {
            std::cerr << "[HoverProvider] Symbol not found in table\n";
        }
        return result;
    }

    std::cerr << "[HoverProvider] No identifier at position\n";
    return std::nullopt;
}

std::string HoverProvider::formatSymbol(const semantic::Symbol& symbol) {
    switch (symbol.kind) {
        case semantic::SymbolKind::Function:
            return formatFunction(symbol);
        case semantic::SymbolKind::Variable:
        case semantic::SymbolKind::Parameter:
            return formatVariable(symbol);
        case semantic::SymbolKind::Class:
            return formatStruct(symbol);
        default:
            return symbol.name + ": " + symbol.type;
    }
}

std::string HoverProvider::formatFunction(const semantic::Symbol& symbol) {
    std::ostringstream ss;
    ss << "fn " << symbol.name << symbol.type;

    // TODO: Add documentation if available

    return ss.str();
}

std::string HoverProvider::formatVariable(const semantic::Symbol& symbol) {
    std::ostringstream ss;

    if (symbol.kind == semantic::SymbolKind::Parameter) {
        ss << "parameter ";
    } else {
        ss << "let ";
    }

    ss << symbol.name << ": " << symbol.type;

    return ss.str();
}

std::string HoverProvider::formatStruct(const semantic::Symbol& symbol) {
    return "struct " + symbol.name;
}

} // namespace lsp
} // namespace naab
