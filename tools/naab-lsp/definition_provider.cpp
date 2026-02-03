#include "definition_provider.h"
#include <iostream>

namespace naab {
namespace lsp {

// ============================================================================
// Location
// ============================================================================

json Location::toJson() const {
    return {
        {"uri", uri},
        {"range", range.toJson()}
    };
}

// ============================================================================
// DefinitionProvider
// ============================================================================

DefinitionProvider::DefinitionProvider() = default;

std::vector<Location> DefinitionProvider::getDefinition(const Document& doc, const Position& pos) {
    std::cerr << "[DefinitionProvider] Getting definition at line " << pos.line << ", char " << pos.character << "\n";

    // Find symbol at cursor
    auto symbol = findSymbolAtPosition(doc, pos);
    if (!symbol) {
        std::cerr << "[DefinitionProvider] No symbol found at position\n";
        return {};
    }

    std::cerr << "[DefinitionProvider] Found symbol: " << symbol->name << " at line " << symbol->location.line << "\n";

    // Create location from symbol
    Location loc;
    loc.uri = symbol->location.filename;
    loc.range = Range{
        {static_cast<int>(symbol->location.line), static_cast<int>(symbol->location.column)},
        {static_cast<int>(symbol->location.line), static_cast<int>(symbol->location.column) + static_cast<int>(symbol->name.length())}
    };

    return {loc};
}

std::optional<semantic::Symbol> DefinitionProvider::findSymbolAtPosition(
    const Document& doc,
    const Position& pos) {

    const auto& symbol_table = doc.getSymbolTable();

    std::cerr << "[DefinitionProvider] Looking for symbol at line " << pos.line << ", char " << pos.character << "\n";

    // Get line text
    std::string line_text = doc.getLineText(pos.line);
    std::cerr << "[DefinitionProvider] Line text: '" << line_text << "'\n";

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
        std::cerr << "[DefinitionProvider] Extracted identifier: '" << identifier << "'\n";

        // Look up symbol in symbol table
        auto result = symbol_table.lookup(identifier);
        if (result) {
            std::cerr << "[DefinitionProvider] Found in symbol table: " << result->name << "\n";
        } else {
            std::cerr << "[DefinitionProvider] Not found in symbol table\n";
        }
        return result;
    }

    std::cerr << "[DefinitionProvider] No identifier at position\n";
    return std::nullopt;
}

} // namespace lsp
} // namespace naab
