#pragma once

#include "document_manager.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <optional>

namespace naab {
namespace lsp {

using json = nlohmann::json;

// Location (file + range)
struct Location {
    std::string uri;
    Range range;

    json toJson() const;
};

// Definition provider
class DefinitionProvider {
public:
    DefinitionProvider();

    // Get definition location(s) for symbol at position
    std::vector<Location> getDefinition(const Document& doc, const Position& pos);

private:
    // Find symbol at position
    std::optional<semantic::Symbol> findSymbolAtPosition(const Document& doc, const Position& pos);
};

} // namespace lsp
} // namespace naab
