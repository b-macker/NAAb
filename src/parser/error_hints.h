#pragma once

#include <string>
#include <vector>
#include <memory>

// Forward declarations
namespace naab {
namespace lexer {
    struct Token;
}
namespace parser {
    class Parser;
}
}

namespace naab {
namespace parser {

// Context information for error hint generation
struct ParserContext {
    const lexer::Token* current_token = nullptr;
    const lexer::Token* previous_token = nullptr;
    const lexer::Token* next_token = nullptr;

    // State flags
    bool in_main_block = false;
    bool in_function_body = false;
    bool in_struct_literal = false;
    bool in_dict_literal = false;
    bool expecting_statement = false;
    bool expecting_expression = false;

    // Recent tokens (for pattern detection)
    std::vector<const lexer::Token*> recent_tokens;
};

// Enhanced error hint generator for parser errors
class ErrorHints {
public:
    // Main entry point: Get hints for a parse error
    static std::vector<std::string> getHintsForParseError(
        const lexer::Token& unexpected_token,
        const std::string& expected,
        const ParserContext& context
    );

    // Specific hint generators for common mistakes
    static std::vector<std::string> hintForMissingBrace(const ParserContext& ctx);
    static std::vector<std::string> hintForReservedKeyword(const std::string& name);
    static std::vector<std::string> hintForDictVsStruct(const ParserContext& ctx);
    static std::vector<std::string> hintForMainFunction(const ParserContext& ctx);
    static std::vector<std::string> hintForMissingReturn(const ParserContext& ctx);
    static std::vector<std::string> hintForMissingSemicolon(const ParserContext& ctx);
    static std::vector<std::string> hintForUnquotedDictKey(const ParserContext& ctx);
    static std::vector<std::string> hintForDotNotationOnDict(const ParserContext& ctx);
    static std::vector<std::string> hintForIncorrectImport(const ParserContext& ctx);
    static std::vector<std::string> hintForMissingTypeAnnotation(const ParserContext& ctx);

private:
    // Pattern detection helpers
    static bool looksLikeMainFunction(const ParserContext& ctx);
    static bool looksLikeDictLiteral(const ParserContext& ctx);
    static bool looksLikeMissingBrace(const ParserContext& ctx);
    static bool looksLikeReservedKeywordUsage(const std::string& name);
    static bool looksLikeUnquotedDictKey(const ParserContext& ctx);
    static bool looksLikeJavaScriptImport(const ParserContext& ctx);

    // Suggestion helpers
    static std::vector<std::string> suggestKeywordAlternatives(const std::string& keyword);
    static std::string formatCodeExample(const std::string& code, bool is_good);
};

} // namespace parser
} // namespace naab

