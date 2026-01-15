#ifndef NAAB_LEXER_H
#define NAAB_LEXER_H

// NAAb Block Assembly Language - Lexer
// Ported from Python implementation (naab/compiler/lexer.py)

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace naab {
namespace lexer {

// Token types for .naab language
enum class TokenType {
    // Keywords
    USE, AS, FUNCTION, ASYNC, METHOD, RETURN,
    IF, ELSE, FOR, IN, WHILE, BREAK, CONTINUE,
    MATCH, TRY, CATCH, THROW, FINALLY,  // Phase 4.1: Exception handling
    STRUCT, CLASS, INIT, MODULE, EXPORT, IMPORT, NEW,
    CONFIG, MAIN, LET, CONST, AWAIT,

    // Literals
    IDENTIFIER,
    BLOCK_ID,      // BLOCK-CPP-00123
    NUMBER,
    STRING,
    BOOLEAN,
    INLINE_CODE,   // Raw code inside << ... >>

    // Operators
    PLUS,          // +
    MINUS,         // -
    STAR,          // *
    SLASH,         // /
    PERCENT,       // %
    EQ,            // =
    EQEQ,          // ==
    NE,            // !=
    LT,            // <
    LE,            // <=
    GT,            // >
    GE,            // >=
    LT_LT,         // << (inline code start)
    GT_GT,         // >> (inline code end)
    PIPE,          // |
    PIPELINE,      // |>
    AMPERSAND,     // &
    AND,           // &&
    OR,            // ||
    NOT,           // !
    DOT,           // .
    ARROW,         // ->
    FAT_ARROW,     // =>
    QUESTION,      // ?
    COLON,         // :
    DOUBLE_COLON,  // ::

    // Delimiters
    LPAREN,        // (
    RPAREN,        // )
    LBRACE,        // {
    RBRACE,        // }
    LBRACKET,      // [
    RBRACKET,      // ]
    COMMA,         // ,
    SEMICOLON,     // ;

    // Special
    NEWLINE,
    END_OF_FILE,
    COMMENT,
};

// A lexical token
struct Token {
    TokenType type;
    std::string value;
    int line;
    int column;

    Token(TokenType t, std::string v, int l, int c)
        : type(t), value(std::move(v)), line(l), column(c) {}
};

// Lexer for .naab language
class Lexer {
public:
    explicit Lexer(const std::string& source);

    // Tokenize the source code
    std::vector<Token> tokenize();

    // Get current position
    int getLine() const { return line_; }
    int getColumn() const { return column_; }

private:
    std::string source_;
    size_t pos_;
    int line_;
    int column_;
    std::vector<Token> tokens_;

    // Character navigation
    std::optional<char> currentChar() const;
    std::optional<char> peekChar(int offset = 1) const;
    void advance();

    // Whitespace and comments
    void skipWhitespace();
    void skipComment();

    // Token readers
    std::string readIdentifier();
    std::string readBlockId();
    std::string readNumber();
    std::string readString();
    std::string readInlineCode();  // Read code between << and >>

    // Keywords map
    static const std::unordered_map<std::string, TokenType> keywords_;
};

} // namespace lexer
} // namespace naab

#endif // NAAB_LEXER_H
