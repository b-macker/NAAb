#pragma once

#include "document_manager.h"
#include "naab/symbol_table.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace naab {
namespace lsp {

using json = nlohmann::json;

// Hover contents
struct HoverContents {
    std::string language = "naab";
    std::string value;

    json toJson() const;
};

// Hover response
struct Hover {
    HoverContents contents;
    std::optional<Range> range;

    json toJson() const;
};

// Hover provider - provides type information on hover
class HoverProvider {
public:
    HoverProvider();

    // Get hover information at position
    std::optional<Hover> getHover(const Document& doc, const Position& pos);

private:
    // Find symbol at position
    std::optional<semantic::Symbol> findSymbolAtPosition(
        const Document& doc,
        const Position& pos
    );

    // Format symbol information
    std::string formatSymbol(const semantic::Symbol& symbol);
    std::string formatFunction(const semantic::Symbol& symbol);
    std::string formatVariable(const semantic::Symbol& symbol);
    std::string formatStruct(const semantic::Symbol& symbol);
};

} // namespace lsp
} // namespace naab
