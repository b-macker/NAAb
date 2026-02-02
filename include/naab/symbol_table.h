#pragma once

#include "absl/container/flat_hash_map.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace naab {
namespace semantic {

// Symbol kind
enum class SymbolKind {
    Variable,
    Function,
    Parameter,
    Module,
    Class,  // Future
    Enum,   // Future
};

// Symbol location
struct SourceLocation {
    std::string filename;
    size_t line;
    size_t column;

    SourceLocation(std::string file, size_t l, size_t c)
        : filename(std::move(file)), line(l), column(c) {}
};

// Symbol information
struct Symbol {
    std::string name;
    SymbolKind kind;
    std::string type;  // Type annotation (e.g., "int", "string")
    SourceLocation location;
    bool is_exported;
    bool is_mutable;  // let vs const

    Symbol(std::string n, SymbolKind k, std::string t, SourceLocation loc)
        : name(std::move(n)), kind(k), type(std::move(t)),
          location(std::move(loc)), is_exported(false), is_mutable(true) {}
};

// Scope
class Scope {
public:
    Scope(std::shared_ptr<Scope> parent = nullptr)
        : parent_(parent) {}

    // Add symbol to this scope
    void define(const std::string& name, Symbol symbol);

    // Look up symbol in this scope only
    std::optional<Symbol> lookup_local(const std::string& name) const;

    // Look up symbol in this scope and parent scopes
    std::optional<Symbol> lookup(const std::string& name) const;

    // Check if symbol exists in this scope
    bool has_local(const std::string& name) const;

    // Get all symbols in this scope
    std::vector<Symbol> get_all_symbols() const;

    // Get parent scope
    std::shared_ptr<Scope> parent() const { return parent_; }

private:
    absl::flat_hash_map<std::string, Symbol> symbols_;
    std::shared_ptr<Scope> parent_;
};

// Symbol Table - manages scopes
class SymbolTable {
public:
    SymbolTable();

    // Scope management
    void push_scope();
    void pop_scope();
    std::shared_ptr<Scope> current_scope() const { return current_scope_; }
    std::shared_ptr<Scope> global_scope() const { return global_scope_; }

    // Symbol operations
    void define(const std::string& name, Symbol symbol);
    std::optional<Symbol> lookup(const std::string& name) const;
    bool has(const std::string& name) const;

    // Get all symbols (for LSP autocomplete)
    std::vector<Symbol> get_all_symbols() const;

    // Find symbol by location (for LSP hover)
    std::optional<Symbol> find_symbol_at(const std::string& filename,
                                        size_t line, size_t column) const;

    // Get all references to symbol (for LSP references)
    std::vector<SourceLocation> get_references(const std::string& name) const;

private:
    std::shared_ptr<Scope> global_scope_;
    std::shared_ptr<Scope> current_scope_;

    // Track all symbol references for LSP
    absl::flat_hash_map<std::string, std::vector<SourceLocation>> references_;
};

} // namespace semantic
} // namespace naab
