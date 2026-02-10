// NAAb Lexer - Token scanner
// Ported from Python implementation (naab/compiler/lexer.py)

#include "naab/lexer.h"
#include "naab/limits.h"  // Week 1, Task 1.2: Input size caps
#include <cctype>
#include <stdexcept>

namespace naab {
namespace lexer {

// Keywords mapping
const std::unordered_map<std::string, TokenType> Lexer::keywords_ = {
    {"use", TokenType::USE},
    {"as", TokenType::AS},
    {"function", TokenType::FUNCTION},
    {"fn", TokenType::FUNCTION},  // Alias for function
    {"async", TokenType::ASYNC},
    {"method", TokenType::METHOD},
    {"return", TokenType::RETURN},
    {"if", TokenType::IF},
    {"else", TokenType::ELSE},
    {"for", TokenType::FOR},
    {"in", TokenType::IN},
    {"while", TokenType::WHILE},
    {"break", TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"match", TokenType::MATCH},
    {"try", TokenType::TRY},       // Phase 4.1
    {"catch", TokenType::CATCH},   // Phase 4.1
    {"throw", TokenType::THROW},   // Phase 4.1
    {"finally", TokenType::FINALLY}, // Phase 4.1
    {"struct", TokenType::STRUCT},
    {"class", TokenType::CLASS},
    {"init", TokenType::INIT},
    {"module", TokenType::MODULE},
    {"export", TokenType::EXPORT},
    {"import", TokenType::IMPORT},
    {"from", TokenType::FROM},
    {"default", TokenType::DEFAULT},
    {"new", TokenType::NEW},
    {"config", TokenType::CONFIG},
    {"main", TokenType::MAIN},
    {"let", TokenType::LET},
    {"const", TokenType::CONST},
    {"await", TokenType::AWAIT},
    {"null", TokenType::NULL_LITERAL},
    {"ref", TokenType::REF},  // Phase 2.1: Reference types
    {"enum", TokenType::ENUM},  // Phase 2.4.3: Enum types
    {"true", TokenType::BOOLEAN},
    {"false", TokenType::BOOLEAN},
};

Lexer::Lexer(const std::string& source)
    : source_(source), pos_(0), line_(1), column_(1) {
    // Week 1, Task 1.2: Check input size to prevent DoS
    limits::checkStringSize(source.size(), "Source file");
}

std::optional<char> Lexer::currentChar() const {
    if (pos_ < source_.length()) {
        return source_[pos_];
    }
    return std::nullopt;
}

std::optional<char> Lexer::peekChar(int offset) const {
    size_t peek_pos = pos_ + offset;
    if (peek_pos < source_.length()) {
        return source_[peek_pos];
    }
    return std::nullopt;
}

void Lexer::advance() {
    if (pos_ < source_.length()) {
        if (source_[pos_] == '\n') {
            line_++;
            column_ = 1;
        } else {
            column_++;
        }
        pos_++;
    }
}

void Lexer::skipWhitespace() {
    while (currentChar() && (*currentChar() == ' ' || *currentChar() == '\t' || *currentChar() == '\r')) {
        advance();
    }
}

void Lexer::skipComment() {
    // Handle # style comments (Python)
    if (currentChar() && *currentChar() == '#') {
        while (currentChar() && *currentChar() != '\n') {
            advance();
        }
        return;
    }

    // Handle // style comments (C++/JavaScript)
    if (currentChar() && *currentChar() == '/' &&
        pos_ + 1 < source_.length() && source_[pos_ + 1] == '/') {
        advance();  // Skip first /
        advance();  // Skip second /
        while (currentChar() && *currentChar() != '\n') {
            advance();
        }
        return;
    }

    // Handle /* */ style block comments (C/C++/JavaScript)
    if (currentChar() && *currentChar() == '/' &&
        pos_ + 1 < source_.length() && source_[pos_ + 1] == '*') {
        advance();  // Skip /
        advance();  // Skip *
        while (currentChar()) {
            if (*currentChar() == '*' && pos_ + 1 < source_.length() && source_[pos_ + 1] == '/') {
                advance();  // Skip *
                advance();  // Skip /
                return;
            }
            advance();
        }
    }
}

std::string Lexer::readIdentifier() {
    size_t start = pos_;
    while (currentChar() && (std::isalnum(*currentChar()) || *currentChar() == '_')) {
        advance();
    }
    return source_.substr(start, pos_ - start);
}

std::string Lexer::readBlockId() {
    size_t start = pos_;

    // BLOCK
    while (currentChar() && std::isupper(*currentChar())) {
        advance();
    }

    // -
    if (currentChar() && *currentChar() == '-') {
        advance();
    }

    // LANG
    while (currentChar() && (std::isupper(*currentChar()) || *currentChar() == '+' || *currentChar() == '-')) {
        advance();
    }

    // -
    if (currentChar() && *currentChar() == '-') {
        advance();
    }

    // NUMBER
    while (currentChar() && std::isdigit(*currentChar())) {
        advance();
    }

    return source_.substr(start, pos_ - start);
}

std::string Lexer::readNumber() {
    size_t start = pos_;
    bool has_dot = false;

    // Handle leading dot (like .123)
    if (currentChar() && *currentChar() == '.') {
        has_dot = true;
        advance();
    }

    // Read digits and optional decimal point
    while (currentChar() && (std::isdigit(*currentChar()) || *currentChar() == '.')) {
        if (*currentChar() == '.') {
            // Check if this is the range operator (..)
            auto next = peekChar();
            if (next && *next == '.') {
                // This is .., not a decimal point - stop reading the number
                break;
            }
            if (has_dot) {
                // Already have a dot, stop here
                break;
            }
            has_dot = true;
        }
        advance();
    }

    std::string number = source_.substr(start, pos_ - start);

    // Handle trailing dot (like 123.) - treat as 123.0
    if (number.length() > 0 && number[number.length() - 1] == '.') {
        number += "0";
    }

    // Handle leading dot without digits after (just . followed by non-digit) - shouldn't happen due to check above
    if (number == ".") {
        // This shouldn't happen anymore, but handle gracefully
        number = "0.0";
    }

    return number;
}

std::string Lexer::readString() {
    char quote = *currentChar();
    advance();  // Skip opening quote

    std::string value;
    while (currentChar() && *currentChar() != quote) {
        if (*currentChar() == '\\') {
            advance();  // Skip backslash
            if (currentChar()) {
                // Interpret escape sequences
                char escaped = *currentChar();
                switch (escaped) {
                    case 'n':  value += '\n'; break;  // Newline
                    case 't':  value += '\t'; break;  // Tab
                    case 'r':  value += '\r'; break;  // Carriage return
                    case '\\': value += '\\'; break;  // Backslash
                    case '"':  value += '"';  break;  // Double quote
                    case '\'': value += '\''; break;  // Single quote
                    case '0':  value += '\0'; break;  // Null character
                    default:
                        // Unknown escape sequence - keep the backslash and character
                        value += '\\';
                        value += escaped;
                        break;
                }
                advance();
            }
        } else {
            value += *currentChar();
            advance();
        }
    }

    if (currentChar() && *currentChar() == quote) {
        advance();  // Skip closing quote
    }

    return value;
}

std::string Lexer::readInlineCode() {
    // Called after we've seen "<<", should read until ">>"
    //
    // IMPORTANT: Only treat >> as closing delimiter when at line start
    // This allows >> to appear in code (bitwise shift, bash redirect, etc.)
    // without prematurely closing the polyglot block.
    //
    // Example that works correctly:
    //   <<python
    //   x = 8 >> 1  # Right shift (>> not at line start)
    //   >>          # Closes block (>> at line start)
    //
    size_t start = pos_;
    bool at_line_start = true;  // We start right after the newline following language name

    while (currentChar()) {
        char ch = *currentChar();

        // Check if we're at line start and found closing >>
        if (at_line_start && ch == '>' && peekChar() && *peekChar() == '>') {
            // Found the closing >> at line start
            std::string code = source_.substr(start, pos_ - start);
            return code;
        }

        // Update line start tracking
        if (ch == '\n') {
            at_line_start = true;  // Next char will be at line start
            advance();
        } else if (ch == ' ' || ch == '\t' || ch == '\r') {
            // Whitespace doesn't change line start status
            advance();
        } else {
            // Non-whitespace character - no longer at line start
            at_line_start = false;
            advance();
        }
    }

    // If we get here, we never found the closing >>
    throw std::runtime_error(
        "Unclosed polyglot code block starting at line " + std::to_string(line_) + "\n\n"
        "  Help:\n"
        "  - Make sure your polyglot block has a closing >> at the start of a line\n"
        "  - The closing >> must be at the beginning of a line (optionally after spaces/tabs)\n"
        "  - If you have >> in your code (like bitwise shift or bash redirect), that's OK!\n"
        "  - Only >> at line start closes the block\n\n"
        "  Example:\n"
        "    let result = <<python\n"
        "    x = 8 >> 1  # This >> is fine (not at line start)\n"
        "    result = x * 2\n"
        "    >>  # This >> closes the block (at line start)\n"
    );
}

std::vector<Token> Lexer::tokenize() {
    tokens_.clear();

    while (currentChar()) {
        char ch = *currentChar();

        // Skip whitespace
        if (ch == ' ' || ch == '\t' || ch == '\r') {
            skipWhitespace();
            continue;
        }

        // Skip comments (#, //, /* */)
        if (ch == '#') {
            skipComment();
            continue;
        }
        if (ch == '/' && pos_ + 1 < source_.length() &&
            (source_[pos_ + 1] == '/' || source_[pos_ + 1] == '*')) {
            skipComment();
            continue;
        }

        // Newline
        if (ch == '\n') {
            tokens_.emplace_back(TokenType::NEWLINE, "\n", line_, column_);
            advance();
            continue;
        }

        // Block ID
        if (ch == 'B' && pos_ + 6 <= source_.length() &&
            source_.substr(pos_, 6) == "BLOCK-") {
            int line = line_, col = column_;
            std::string block_id = readBlockId();
            tokens_.emplace_back(TokenType::BLOCK_ID, block_id, line, col);
            continue;
        }

        // Identifier or keyword
        if (std::isalpha(ch) || ch == '_') {
            int line = line_, col = column_;
            std::string identifier = readIdentifier();

            // Check if keyword
            auto it = keywords_.find(identifier);
            TokenType type = (it != keywords_.end()) ? it->second : TokenType::IDENTIFIER;
            tokens_.emplace_back(type, identifier, line, col);
            continue;
        }

        // Number (including leading decimal like .123)
        if (std::isdigit(ch)) {
            int line = line_, col = column_;
            std::string number = readNumber();
            tokens_.emplace_back(TokenType::NUMBER, number, line, col);
            continue;
        }

        // Leading decimal number (like .123)
        auto next_char = peekChar();
        if (ch == '.' && next_char && std::isdigit(*next_char)) {
            int line = line_, col = column_;
            std::string number = readNumber();
            tokens_.emplace_back(TokenType::NUMBER, number, line, col);
            continue;
        }

        // String
        if (ch == '"' || ch == '\'') {
            int line = line_, col = column_;
            std::string str = readString();
            tokens_.emplace_back(TokenType::STRING, str, line, col);
            continue;
        }

        // Two-character operators
        auto next = peekChar();
        int line = line_, col = column_;

        if (ch == '=' && next && *next == '=') {
            tokens_.emplace_back(TokenType::EQEQ, "==", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '!' && next && *next == '=') {
            tokens_.emplace_back(TokenType::NE, "!=", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '<' && next && *next == '<') {
            // Inline code block: <<language ... >>
            advance();  // Skip first <
            advance();  // Skip second <

            // Skip whitespace after <<
            while (currentChar() && (*currentChar() == ' ' || *currentChar() == '\t')) {
                advance();
            }

            // Read language name
            if (!currentChar() || !std::isalpha(*currentChar())) {
                throw std::runtime_error("Expected language name after '<<' at line " + std::to_string(line_));
            }

            std::string language = readIdentifier();

            // Phase 2.2: Check for optional variable binding list [var1, var2]
            std::string var_list;
            if (currentChar() && *currentChar() == '[') {
                advance();  // Skip [

                // Read everything until ]
                while (currentChar() && *currentChar() != ']') {
                    var_list += *currentChar();
                    advance();
                }

                if (!currentChar() || *currentChar() != ']') {
                    throw std::runtime_error("Expected ']' after variable list at line " + std::to_string(line_));
                }
                advance();  // Skip ]
            }

            // Skip only newlines after language name (or var list)
            // Don't skip spaces/tabs - they're part of the code's indentation
            while (currentChar() && (*currentChar() == '\n' || *currentChar() == '\r')) {
                if (*currentChar() == '\n') {
                    line_++;
                    column_ = 1;
                }
                advance();
            }

            // Read the inline code
            std::string code = readInlineCode();

            // Skip the closing >>
            if (currentChar() && *currentChar() == '>' && peekChar() && *peekChar() == '>') {
                advance();  // Skip first >
                advance();  // Skip second >
            }

            // Phase 2.2: Create INLINE_CODE token with format "language[var1,var2]:code" or "language:code"
            std::string value;
            if (!var_list.empty()) {
                value = language + "[" + var_list + "]:" + code;
            } else {
                value = language + ":" + code;
            }
            tokens_.emplace_back(TokenType::INLINE_CODE, value, line, col);
            continue;
        }

        if (ch == '<' && next && *next == '=') {
            tokens_.emplace_back(TokenType::LE, "<=", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '>' && next && *next == '>') {
            tokens_.emplace_back(TokenType::GT_GT, ">>", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '>' && next && *next == '=') {
            tokens_.emplace_back(TokenType::GE, ">=", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '-' && next && *next == '>') {
            tokens_.emplace_back(TokenType::ARROW, "->", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '=' && next && *next == '>') {
            tokens_.emplace_back(TokenType::FAT_ARROW, "=>", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '|' && next && *next == '>') {
            tokens_.emplace_back(TokenType::PIPELINE, "|>", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '|' && next && *next == '|') {
            tokens_.emplace_back(TokenType::OR, "||", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '&' && next && *next == '&') {
            tokens_.emplace_back(TokenType::AND, "&&", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == ':' && next && *next == ':') {
            tokens_.emplace_back(TokenType::DOUBLE_COLON, "::", line, col);
            advance();
            advance();
            continue;
        }

        if (ch == '.' && next && *next == '.') {
            // Check for ..= (inclusive range)
            auto third = peekChar(2);
            if (third && *third == '=') {
                tokens_.emplace_back(TokenType::DOTDOT_EQ, "..=", line, col);
                advance();
                advance();
                advance();
                continue;
            }
            // Otherwise it's .. (exclusive range)
            tokens_.emplace_back(TokenType::DOTDOT, "..", line, col);
            advance();
            advance();
            continue;
        }

        // Single-character tokens
        switch (ch) {
            case '+': tokens_.emplace_back(TokenType::PLUS, "+", line, col); break;
            case '-': tokens_.emplace_back(TokenType::MINUS, "-", line, col); break;
            case '*': tokens_.emplace_back(TokenType::STAR, "*", line, col); break;
            case '/': tokens_.emplace_back(TokenType::SLASH, "/", line, col); break;
            case '%': tokens_.emplace_back(TokenType::PERCENT, "%", line, col); break;
            case '=': tokens_.emplace_back(TokenType::EQ, "=", line, col); break;
            case '<': tokens_.emplace_back(TokenType::LT, "<", line, col); break;
            case '>': tokens_.emplace_back(TokenType::GT, ">", line, col); break;
            case '|': tokens_.emplace_back(TokenType::PIPE, "|", line, col); break;
            case '&': tokens_.emplace_back(TokenType::AMPERSAND, "&", line, col); break;
            case '!': tokens_.emplace_back(TokenType::NOT, "!", line, col); break;
            case '.': tokens_.emplace_back(TokenType::DOT, ".", line, col); break;
            case '?': tokens_.emplace_back(TokenType::QUESTION, "?", line, col); break;
            case ':': tokens_.emplace_back(TokenType::COLON, ":", line, col); break;
            case '(': tokens_.emplace_back(TokenType::LPAREN, "(", line, col); break;
            case ')': tokens_.emplace_back(TokenType::RPAREN, ")", line, col); break;
            case '{': tokens_.emplace_back(TokenType::LBRACE, "{", line, col); break;
            case '}': tokens_.emplace_back(TokenType::RBRACE, "}", line, col); break;
            case '[': tokens_.emplace_back(TokenType::LBRACKET, "[", line, col); break;
            case ']': tokens_.emplace_back(TokenType::RBRACKET, "]", line, col); break;
            case ',': tokens_.emplace_back(TokenType::COMMA, ",", line, col); break;
            case ';': tokens_.emplace_back(TokenType::SEMICOLON, ";", line, col); break;
            default:
                throw std::runtime_error("Unexpected character '" + std::string(1, ch) +
                                       "' at line " + std::to_string(line_) +
                                       ", column " + std::to_string(column_));
        }

        advance();
    }

    // Add EOF token
    tokens_.emplace_back(TokenType::END_OF_FILE, "", line_, column_);
    return tokens_;
}

} // namespace lexer
} // namespace naab
