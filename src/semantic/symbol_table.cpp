#include "naab/symbol_table.h"
#include <algorithm>

namespace naab {
namespace semantic {

// ============================================================================
// Scope Implementation
// ============================================================================

void Scope::define(const std::string& name, Symbol symbol) {
    symbols_[name] = std::move(symbol);
}

std::optional<Symbol> Scope::lookup_local(const std::string& name) const {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<Symbol> Scope::lookup(const std::string& name) const {
    // Check this scope
    auto symbol = lookup_local(name);
    if (symbol) return symbol;

    // Check parent scopes
    if (parent_) {
        return parent_->lookup(name);
    }

    return std::nullopt;
}

bool Scope::has_local(const std::string& name) const {
    return symbols_.find(name) != symbols_.end();
}

std::vector<Symbol> Scope::get_all_symbols() const {
    std::vector<Symbol> result;
    result.reserve(symbols_.size());
    for (const auto& [name, symbol] : symbols_) {
        result.push_back(symbol);
    }
    return result;
}

// ============================================================================
// SymbolTable Implementation
// ============================================================================

SymbolTable::SymbolTable()
    : global_scope_(std::make_shared<Scope>()),
      current_scope_(global_scope_) {
}

void SymbolTable::push_scope() {
    current_scope_ = std::make_shared<Scope>(current_scope_);
}

void SymbolTable::pop_scope() {
    if (current_scope_ && current_scope_->parent()) {
        current_scope_ = current_scope_->parent();
    }
}

void SymbolTable::define(const std::string& name, Symbol symbol) {
    current_scope_->define(name, std::move(symbol));
}

std::optional<Symbol> SymbolTable::lookup(const std::string& name) const {
    return current_scope_->lookup(name);
}

bool SymbolTable::has(const std::string& name) const {
    return current_scope_->lookup(name).has_value();
}

std::vector<Symbol> SymbolTable::get_all_symbols() const {
    std::vector<Symbol> result;

    // Collect symbols from current scope
    auto current_symbols = current_scope_->get_all_symbols();
    result.insert(result.end(), current_symbols.begin(), current_symbols.end());

    // Collect from parent scopes
    auto scope = current_scope_->parent();
    while (scope) {
        auto symbols = scope->get_all_symbols();
        result.insert(result.end(), symbols.begin(), symbols.end());
        scope = scope->parent();
    }

    return result;
}

std::optional<Symbol> SymbolTable::find_symbol_at(
    const std::string& filename,
    size_t line,
    size_t column) const {

    // Search all scopes for symbol at location
    auto all_symbols = get_all_symbols();

    for (const auto& symbol : all_symbols) {
        if (symbol.location.filename == filename &&
            symbol.location.line == line &&
            symbol.location.column == column) {
            return symbol;
        }
    }

    return std::nullopt;
}

std::vector<SourceLocation> SymbolTable::get_references(const std::string& name) const {
    auto it = references_.find(name);
    if (it != references_.end()) {
        return it->second;
    }
    return {};
}

} // namespace semantic
} // namespace naab
