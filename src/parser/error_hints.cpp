#include "error_hints.h"
#include "naab/lexer.h"
#include <unordered_map>
#include <unordered_set>

namespace naab {
namespace parser {

// ============================================================================
// Reserved Keywords and Alternatives
// ============================================================================

static const std::unordered_map<std::string, std::string> KEYWORD_ALTERNATIVES = {
    {"config", "cfg, configuration, settings, options"},
    {"module", "mod"},
    {"function", "fn"},
    {"interface", "trait (if defining interface-like behavior)"},
    {"class", "struct (NAAb uses structs)"},
    {"import", "use (NAAb uses 'use' for imports)"},
    {"export", "export (already valid, but ensure correct syntax)"},
    {"var", "let (use 'let' for variables)"},
    {"const", "let (NAAb uses 'let' for all bindings)"},
    {"void", "omit return type or use '-> ()'"},
    {"null", "null (valid in NAAb)"},
    {"undefined", "null (NAAb doesn't have undefined)"},
    {"async", "async (valid keyword)"},
    {"await", "await (valid keyword)"},
    {"try", "try (valid keyword)"},
    {"catch", "catch (valid keyword)"},
    {"throw", "throw (valid keyword)"},
};

static const std::unordered_set<std::string> RESERVED_KEYWORDS = {
    "use", "as", "function", "fn", "async", "await",
    "struct", "enum", "type", "trait",
    "let", "mut", "const",
    "if", "else", "for", "while", "loop", "match",
    "break", "continue", "return",
    "try", "catch", "finally", "throw",
    "export", "import", "module",
    "true", "false", "null",
    "main", "config",
    "and", "or", "not",
    "in", "is",
};

// ============================================================================
// Pattern Detection
// ============================================================================

bool ErrorHints::looksLikeMainFunction(const ParserContext& ctx) {
    // Check if user wrote "fn main()" instead of "main {}"
    if (ctx.previous_token && ctx.current_token) {
        std::string prev_text = ctx.previous_token->value;
        std::string curr_text = ctx.current_token->value;

        // Pattern: "fn" followed by "main"
        if (prev_text == "fn" && curr_text == "main") {
            return true;
        }
    }
    return false;
}

bool ErrorHints::looksLikeDictLiteral(const ParserContext& ctx) {
    return ctx.in_dict_literal;
}

bool ErrorHints::looksLikeMissingBrace(const ParserContext& ctx) {
    // Check for common patterns where braces might be missing
    if (ctx.in_function_body && ctx.expecting_statement) {
        return true;
    }
    return false;
}

bool ErrorHints::looksLikeReservedKeywordUsage(const std::string& name) {
    return RESERVED_KEYWORDS.count(name) > 0;
}

bool ErrorHints::looksLikeUnquotedDictKey(const ParserContext& ctx) {
    // Check if we're in a dict literal and encountered an identifier followed by colon
    if (ctx.in_dict_literal && ctx.current_token && ctx.next_token) {
        return ctx.current_token->type == lexer::TokenType::IDENTIFIER &&
               ctx.next_token->type == lexer::TokenType::COLON;
    }
    return false;
}

bool ErrorHints::looksLikeJavaScriptImport(const ParserContext& ctx) {
    // Pattern: "import" keyword (instead of "use")
    if (ctx.current_token) {
        return ctx.current_token->value == "import";
    }
    return false;
}

// ============================================================================
// Hint Generators
// ============================================================================

std::vector<std::string> ErrorHints::hintForMainFunction(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "NAAb uses 'main {}' as the entry point, not 'fn main()'.",
        "",
        "Did you mean:",
        "    main {",
        "        // your code",
        "    }",
        "",
        "Instead of:",
        "    fn main() {  // ❌ This doesn't work",
        "        // your code",
        "    }",
    };
}

std::vector<std::string> ErrorHints::hintForUnquotedDictKey(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "Dictionary keys must be quoted strings in NAAb.",
        "",
        "Did you mean:",
        "    let person = {",
        "        \"name\": \"Alice\",  // ✅ Quoted keys",
        "        \"age\": 30",
        "    }",
        "",
        "Instead of:",
        "    let person = {",
        "        name: \"Alice\",  // ❌ Unquoted keys",
        "        age: 30",
        "    }",
        "",
        "Note: Use structs for fixed schemas, dictionaries for dynamic data.",
    };
}

std::vector<std::string> ErrorHints::hintForDotNotationOnDict(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "Dictionaries use bracket notation, not dot notation.",
        "",
        "Did you mean:",
        "    let name = person[\"name\"]  // ✅ Bracket notation for dicts",
        "",
        "Instead of:",
        "    let name = person.name  // ❌ Dot notation only for structs",
        "",
        "Note: If you need dot notation, use a struct instead:",
        "    struct Person { name: string, age: int }",
        "    let person = Person { name: \"Alice\", age: 30 }",
        "    let name = person.name  // ✅ Dot notation works",
    };
}

std::vector<std::string> ErrorHints::hintForReservedKeyword(const std::string& name) {
    std::vector<std::string> hints;

    hints.push_back("'" + name + "' is a reserved keyword in NAAb.");

    auto it = KEYWORD_ALTERNATIVES.find(name);
    if (it != KEYWORD_ALTERNATIVES.end()) {
        hints.push_back("");
        hints.push_back("Suggested alternatives:");
        hints.push_back("    - " + it->second);
    }

    hints.push_back("");
    hints.push_back("Example:");
    hints.push_back("    let cfg = loadSettings()  // ✅");

    return hints;
}

std::vector<std::string> ErrorHints::hintForIncorrectImport(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "NAAb uses 'use' for imports, not 'import'.",
        "",
        "Did you mean:",
        "    use io  // ✅ For stdlib",
        "    use my_module as mod  // ✅ For custom modules",
        "",
        "Instead of:",
        "    import io from \"std\"  // ❌ Not JavaScript!",
    };
}

std::vector<std::string> ErrorHints::hintForMissingSemicolon(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "Multi-statement lines require semicolons.",
        "",
        "Did you mean:",
        "    let x = 42; let y = 10  // ✅ Semicolon separates statements",
        "",
        "Or use newlines:",
        "    let x = 42",
        "    let y = 10  // ✅ Newline separates statements",
    };
}

std::vector<std::string> ErrorHints::hintForMissingBrace(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "Missing closing brace '}'.",
        "",
        "Tip: Check that all opening braces '{' have matching closing braces '}'.",
    };
}

std::vector<std::string> ErrorHints::hintForMissingReturn(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "Function with non-void return type must return a value.",
        "",
        "Add a return statement:",
        "    return result",
    };
}

std::vector<std::string> ErrorHints::hintForDictVsStruct(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "Dictionary vs Struct usage:",
        "",
        "Use dictionaries for dynamic data:",
        "    let data = {\"key\": value}",
        "    let value = data[\"key\"]",
        "",
        "Use structs for fixed schemas:",
        "    struct Person { name: string, age: int }",
        "    let p = Person { name: \"Alice\", age: 30 }",
        "    let name = p.name",
    };
}

std::vector<std::string> ErrorHints::hintForMissingTypeAnnotation(const ParserContext& ctx) {
    (void)ctx;  // Unused parameter
    return {
        "Type annotation required in this context.",
        "",
        "Example:",
        "    let x: int = 42",
        "    fn process(data: list<int>) -> int { ... }",
    };
}

// ============================================================================
// Main Entry Point
// ============================================================================

std::vector<std::string> ErrorHints::getHintsForParseError(
    const lexer::Token& unexpected_token,
    const std::string& expected,
    const ParserContext& context
) {
    std::vector<std::string> hints;

    // Check for specific patterns
    if (looksLikeMainFunction(context)) {
        return hintForMainFunction(context);
    }

    if (looksLikeUnquotedDictKey(context)) {
        return hintForUnquotedDictKey(context);
    }

    if (looksLikeJavaScriptImport(context)) {
        return hintForIncorrectImport(context);
    }

    if (looksLikeReservedKeywordUsage(unexpected_token.value)) {
        return hintForReservedKeyword(unexpected_token.value);
    }

    // Expected token hints
    if (expected.find("'}'") != std::string::npos) {
        return hintForMissingBrace(context);
    }

    if (expected.find("semicolon") != std::string::npos) {
        return hintForMissingSemicolon(context);
    }

    // Generic hint
    hints.push_back("Expected " + expected + " but got '" + unexpected_token.value + "'");

    return hints;
}

// ============================================================================
// Helper Functions
// ============================================================================

std::vector<std::string> ErrorHints::suggestKeywordAlternatives(const std::string& keyword) {
    auto it = KEYWORD_ALTERNATIVES.find(keyword);
    if (it != KEYWORD_ALTERNATIVES.end()) {
        std::vector<std::string> alternatives;
        std::string alt_str = it->second;

        // Parse comma-separated alternatives
        size_t start = 0;
        size_t comma = alt_str.find(',');
        while (comma != std::string::npos) {
            alternatives.push_back(alt_str.substr(start, comma - start));
            start = comma + 2; // Skip ", "
            comma = alt_str.find(',', start);
        }
        alternatives.push_back(alt_str.substr(start));

        return alternatives;
    }
    return {};
}

std::string ErrorHints::formatCodeExample(const std::string& code, bool is_good) {
    if (is_good) {
        return "    " + code + "  // ✅";
    } else {
        return "    " + code + "  // ❌";
    }
}

} // namespace parser
} // namespace naab
