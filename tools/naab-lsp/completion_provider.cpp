#include "completion_provider.h"
#include <algorithm>
#include <cctype>
#include <iostream>

namespace naab {
namespace lsp {

// ============================================================================
// CompletionItem
// ============================================================================

json CompletionItem::toJson() const {
    json j = {
        {"label", label},
        {"kind", static_cast<int>(kind)}
    };

    if (!detail.empty()) {
        j["detail"] = detail;
    }

    if (!documentation.empty()) {
        j["documentation"] = documentation;
    }

    if (!insertText.empty()) {
        j["insertText"] = insertText;
    }

    return j;
}

// ============================================================================
// CompletionList
// ============================================================================

json CompletionList::toJson() const {
    json items_json = json::array();
    for (const auto& item : items) {
        items_json.push_back(item.toJson());
    }

    return {
        {"isIncomplete", isIncomplete},
        {"items", items_json}
    };
}

// ============================================================================
// CompletionProvider
// ============================================================================

CompletionProvider::CompletionProvider() = default;

CompletionList CompletionProvider::getCompletions(const Document& doc, const Position& pos) {
    std::cerr << "[CompletionProvider] Getting completions at line " << pos.line << ", char " << pos.character << "\n";

    // Check cache
    CacheKey key{doc.getUri(), pos.line, pos.character, doc.getVersion()};
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        std::cerr << "[CompletionProvider] Cache hit! Returning " << it->second.items.size() << " cached items\n";
        return it->second;
    }

    // Analyze context
    CompletionContext ctx = analyzeContext(doc, pos);

    std::cerr << "[CompletionProvider] Context type: " << static_cast<int>(ctx.type) << "\n";
    std::cerr << "[CompletionProvider] Prefix: '" << ctx.prefix << "'\n";

    // Dispatch to appropriate completion generator
    CompletionList result;
    switch (ctx.type) {
        case CompletionContextType::Expression:
            result = completeExpression(doc, ctx);
            break;
        case CompletionContextType::MemberAccess:
            result = completeMemberAccess(doc, ctx);
            break;
        case CompletionContextType::TypeAnnotation:
            result = completeTypeAnnotation(doc, ctx);
            break;
        default:
            result = CompletionList{false, {}};
    }

    // Cache result
    cache_[key] = result;
    std::cerr << "[CompletionProvider] Cached " << result.items.size() << " items\n";

    return result;
}

CompletionContext CompletionProvider::analyzeContext(const Document& doc, const Position& pos) {
    CompletionContext ctx;
    ctx.position = pos;

    // Get line text up to cursor
    std::string line = doc.getLineText(pos.line);
    std::string prefix = line.substr(0, std::min(static_cast<size_t>(pos.character), line.length()));

    std::cerr << "[CompletionProvider] Line text: '" << line << "'\n";
    std::cerr << "[CompletionProvider] Full prefix: '" << prefix << "'\n";

    ctx.prefix = prefix;

    // Determine context type
    if (prefix.size() >= 1 && prefix.back() == '.') {
        // Member access: obj.|
        ctx.type = CompletionContextType::MemberAccess;

        // Get identifier before dot
        size_t dot_pos = prefix.length() - 1;
        size_t start = dot_pos;
        while (start > 0 && (std::isalnum(prefix[start-1]) || prefix[start-1] == '_')) {
            --start;
        }

        if (start < dot_pos) {
            std::string object_name = prefix.substr(start, dot_pos - start);
            std::cerr << "[CompletionProvider] Member access on: '" << object_name << "'\n";
            // TODO: Look up object type in symbol table
            ctx.object_type = "unknown";
        }

    } else if (prefix.find("let ") != std::string::npos && prefix.find(':') != std::string::npos) {
        // Type annotation: let x: |
        ctx.type = CompletionContextType::TypeAnnotation;
        std::cerr << "[CompletionProvider] Type annotation context\n";

    } else {
        // General expression
        ctx.type = CompletionContextType::Expression;
        std::cerr << "[CompletionProvider] Expression context\n";
    }

    return ctx;
}

// ============================================================================
// Completion Generators
// ============================================================================

CompletionList CompletionProvider::completeExpression(
    const Document& doc,
    const CompletionContext& ctx) {

    CompletionList list;
    list.isIncomplete = false;

    // Get prefix (last word before cursor)
    std::string prefix;
    for (int i = static_cast<int>(ctx.prefix.length()) - 1; i >= 0; --i) {
        if (std::isalnum(ctx.prefix[i]) || ctx.prefix[i] == '_') {
            prefix = ctx.prefix[i] + prefix;
        } else {
            break;
        }
    }

    std::cerr << "[CompletionProvider] Word prefix: '" << prefix << "'\n";

    // Keywords
    auto keywords = getKeywordCompletions(prefix);
    list.items.insert(list.items.end(), keywords.begin(), keywords.end());
    std::cerr << "[CompletionProvider] Added " << keywords.size() << " keywords\n";

    // Symbols (variables, functions)
    auto symbols = getSymbolCompletions(doc, prefix);
    list.items.insert(list.items.end(), symbols.begin(), symbols.end());
    std::cerr << "[CompletionProvider] Added " << symbols.size() << " symbols\n";

    std::cerr << "[CompletionProvider] Total completions: " << list.items.size() << "\n";

    return list;
}

CompletionList CompletionProvider::completeMemberAccess(
    const Document& doc,
    const CompletionContext& ctx) {

    CompletionList list;
    list.isIncomplete = false;

    std::cerr << "[CompletionProvider] Member access completions for: " << ctx.object_type << "\n";

    // Get members of object type
    auto members = getMemberCompletions(ctx.object_type);
    list.items.insert(list.items.end(), members.begin(), members.end());

    return list;
}

CompletionList CompletionProvider::completeTypeAnnotation(
    const Document& doc,
    const CompletionContext& ctx) {

    CompletionList list;
    list.isIncomplete = false;

    std::cerr << "[CompletionProvider] Type annotation completions\n";

    // Get type names
    auto types = getTypeCompletions("");
    list.items.insert(list.items.end(), types.begin(), types.end());

    return list;
}

// ============================================================================
// Helper Functions
// ============================================================================

std::vector<CompletionItem> CompletionProvider::getKeywordCompletions(const std::string& prefix) {
    static const std::vector<std::string> keywords = {
        "let", "fn", "if", "else", "for", "while", "return",
        "struct", "enum", "use", "import", "export",
        "try", "catch", "throw", "true", "false", "null",
        "main", "break", "continue", "match"
    };

    std::vector<CompletionItem> items;

    for (const auto& keyword : keywords) {
        if (prefix.empty() || keyword.find(prefix) == 0) {
            CompletionItem item;
            item.label = keyword;
            item.kind = CompletionItemKind::Keyword;
            items.push_back(item);
        }
    }

    return items;
}

std::vector<CompletionItem> CompletionProvider::getSymbolCompletions(
    const Document& doc,
    const std::string& prefix) {

    std::vector<CompletionItem> items;

    const auto& symbol_table = doc.getSymbolTable();
    auto symbols = symbol_table.get_all_symbols();

    std::cerr << "[CompletionProvider] Symbol table has " << symbols.size() << " symbols\n";

    for (const auto& symbol : symbols) {
        if (prefix.empty() || symbol.name.find(prefix) == 0) {
            CompletionItem item;
            item.label = symbol.name;

            switch (symbol.kind) {
                case semantic::SymbolKind::Function:
                    item.kind = CompletionItemKind::Function;
                    item.detail = symbol.type;
                    break;
                case semantic::SymbolKind::Variable:
                case semantic::SymbolKind::Parameter:
                    item.kind = CompletionItemKind::Variable;
                    item.detail = symbol.type;
                    break;
                case semantic::SymbolKind::Class:
                    item.kind = CompletionItemKind::Class;
                    item.detail = "struct";
                    break;
                case semantic::SymbolKind::Enum:
                    item.kind = CompletionItemKind::Enum;
                    item.detail = "enum";
                    break;
                default:
                    item.kind = CompletionItemKind::Text;
            }

            items.push_back(item);
        }
    }

    return items;
}

std::vector<CompletionItem> CompletionProvider::getTypeCompletions(const std::string& prefix) {
    static const std::vector<std::string> builtin_types = {
        "int", "float", "bool", "string", "void",
        "list", "dict", "Result", "Option"
    };

    std::vector<CompletionItem> items;

    for (const auto& type : builtin_types) {
        if (prefix.empty() || type.find(prefix) == 0) {
            CompletionItem item;
            item.label = type;
            item.kind = CompletionItemKind::Class;
            items.push_back(item);
        }
    }

    return items;
}

std::vector<CompletionItem> CompletionProvider::getMemberCompletions(const std::string& type_name) {
    // TODO: Query struct fields, enum variants, etc.
    // For now, return empty
    std::cerr << "[CompletionProvider] getMemberCompletions not yet implemented\n";
    return {};
}

void CompletionProvider::invalidateCache(const std::string& uri) {
    // Remove all cached entries for this URI
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->first.uri == uri) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    std::cerr << "[CompletionProvider] Cache invalidated for: " << uri << "\n";
}

} // namespace lsp
} // namespace naab
