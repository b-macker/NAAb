#pragma once

#include "document_manager.h"
#include "naab/symbol_table.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <map>

namespace naab {
namespace lsp {

using json = nlohmann::json;

// LSP CompletionItemKind
enum class CompletionItemKind {
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Property = 10,
    Unit = 11,
    Value = 12,
    Enum = 13,
    Keyword = 14,
    Snippet = 15,
    Color = 16,
    File = 17,
    Reference = 18
};

// Completion item
struct CompletionItem {
    std::string label;
    CompletionItemKind kind;
    std::string detail;
    std::string documentation;
    std::string insertText;

    json toJson() const;
};

// Completion list
struct CompletionList {
    bool isIncomplete;
    std::vector<CompletionItem> items;

    json toJson() const;
};

// Completion context - what are we completing?
enum class CompletionContextType {
    Expression,        // General expression (show variables, functions, keywords)
    MemberAccess,      // After dot: obj.|
    TypeAnnotation,    // After colon: let x: |
    ImportPath,        // After import: import { foo } from "|"
};

struct CompletionContext {
    CompletionContextType type;
    std::string prefix;              // Text before cursor
    std::string object_type;         // For member access, type of object
    Position position;
};

// Completion provider
class CompletionProvider {
public:
    CompletionProvider();

    // Main entry point
    CompletionList getCompletions(const Document& doc, const Position& pos);

    // Cache invalidation
    void invalidateCache(const std::string& uri);

private:
    // Context analysis
    CompletionContext analyzeContext(const Document& doc, const Position& pos);

    // Completion generators
    CompletionList completeExpression(const Document& doc, const CompletionContext& ctx);
    CompletionList completeMemberAccess(const Document& doc, const CompletionContext& ctx);
    CompletionList completeTypeAnnotation(const Document& doc, const CompletionContext& ctx);

    // Helpers
    std::vector<CompletionItem> getKeywordCompletions(const std::string& prefix);
    std::vector<CompletionItem> getSymbolCompletions(const Document& doc, const std::string& prefix);
    std::vector<CompletionItem> getTypeCompletions(const std::string& prefix);
    std::vector<CompletionItem> getMemberCompletions(const std::string& type_name);

    // Cache: (uri, line, character, version) -> CompletionList
    struct CacheKey {
        std::string uri;
        int line;
        int character;
        int version;

        bool operator<(const CacheKey& other) const {
            if (uri != other.uri) return uri < other.uri;
            if (line != other.line) return line < other.line;
            if (character != other.character) return character < other.character;
            return version < other.version;
        }
    };

    std::map<CacheKey, CompletionList> cache_;
};

} // namespace lsp
} // namespace naab
