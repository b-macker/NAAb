// Lexer Unit Tests
// Tests tokenization of NAAb source code

#include <gtest/gtest.h>
#include "naab/lexer.h"
#include <vector>
#include <string>

using namespace naab::lexer;

// ============================================================================
// Basic Token Tests
// ============================================================================

TEST(LexerTest, EmptySource) {
    Lexer lexer("");
    auto tokens = lexer.tokenize();
    ASSERT_EQ(tokens.size(), 1);  // EOF token
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(LexerTest, Whitespace) {
    Lexer lexer("   \t\n  ");
    auto tokens = lexer.tokenize();
    ASSERT_EQ(tokens.size(), 1);  // Only EOF
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(LexerTest, SingleLineComment) {
    Lexer lexer("// this is a comment\n");
    auto tokens = lexer.tokenize();
    ASSERT_EQ(tokens.size(), 1);  // Only EOF (comments ignored)
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(LexerTest, MultiLineComment) {
    Lexer lexer("/* this is\na multi-line\ncomment */");
    auto tokens = lexer.tokenize();
    ASSERT_EQ(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

// ============================================================================
// Keyword Tests
// ============================================================================

TEST(LexerTest, KeywordLet) {
    Lexer lexer("let");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::LET);
    EXPECT_EQ(tokens[0].value, "let");
}

TEST(LexerTest, KeywordFunction) {
    Lexer lexer("function");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::FUNCTION);
}

TEST(LexerTest, KeywordReturn) {
    Lexer lexer("return");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::RETURN);
}

TEST(LexerTest, KeywordIf) {
    Lexer lexer("if");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::IF);
}

TEST(LexerTest, KeywordElse) {
    Lexer lexer("else");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::ELSE);
}

TEST(LexerTest, KeywordFor) {
    Lexer lexer("for");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::FOR);
}

TEST(LexerTest, KeywordWhile) {
    Lexer lexer("while");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::WHILE);
}

TEST(LexerTest, KeywordBreak) {
    Lexer lexer("break");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::BREAK);
}

TEST(LexerTest, KeywordContinue) {
    Lexer lexer("continue");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::CONTINUE);
}

TEST(LexerTest, KeywordTrue) {
    Lexer lexer("true");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::BOOLEAN);
}

TEST(LexerTest, KeywordFalse) {
    Lexer lexer("false");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::BOOLEAN);
}

TEST(LexerTest, KeywordUse) {
    Lexer lexer("use");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::USE);
}

TEST(LexerTest, KeywordImport) {
    Lexer lexer("import");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::IMPORT);
}

TEST(LexerTest, KeywordExport) {
    Lexer lexer("export");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::EXPORT);
}

TEST(LexerTest, KeywordTry) {
    Lexer lexer("try");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::TRY);
}

TEST(LexerTest, KeywordCatch) {
    Lexer lexer("catch");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::CATCH);
}

TEST(LexerTest, KeywordFinally) {
    Lexer lexer("finally");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::FINALLY);
}

TEST(LexerTest, KeywordThrow) {
    Lexer lexer("throw");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::THROW);
}

// ============================================================================
// Identifier Tests
// ============================================================================

TEST(LexerTest, SimpleIdentifier) {
    Lexer lexer("variable");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].value, "variable");
}

TEST(LexerTest, IdentifierWithNumbers) {
    Lexer lexer("var123");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].value, "var123");
}

TEST(LexerTest, IdentifierWithUnderscore) {
    Lexer lexer("my_variable");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].value, "my_variable");
}

TEST(LexerTest, IdentifierStartsWithUnderscore) {
    Lexer lexer("_private");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].value, "_private");
}

// ============================================================================
// Literal Tests
// ============================================================================

TEST(LexerTest, IntegerLiteral) {
    Lexer lexer("42");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].value, "42");
}

TEST(LexerTest, ZeroLiteral) {
    Lexer lexer("0");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].value, "0");
}

TEST(LexerTest, FloatLiteral) {
    Lexer lexer("3.14");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].value, "3.14");
}

TEST(LexerTest, FloatWithLeadingZero) {
    Lexer lexer("0.5");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[0].value, "0.5");
}

TEST(LexerTest, StringLiteralDoubleQuotes) {
    Lexer lexer("\"hello world\"");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].value, "hello world");
}

TEST(LexerTest, StringLiteralSingleQuotes) {
    Lexer lexer("'hello world'");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].value, "hello world");
}

TEST(LexerTest, EmptyString) {
    Lexer lexer("\"\"");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].value, "");
}

TEST(LexerTest, StringWithEscapes) {
    Lexer lexer("\"hello\\nworld\"");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
}

// ============================================================================
// Operator Tests
// ============================================================================

TEST(LexerTest, PlusOperator) {
    Lexer lexer("+");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::PLUS);
}

TEST(LexerTest, MinusOperator) {
    Lexer lexer("-");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::MINUS);
}

TEST(LexerTest, StarOperator) {
    Lexer lexer("*");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::STAR);
}

TEST(LexerTest, SlashOperator) {
    Lexer lexer("/");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::SLASH);
}

TEST(LexerTest, PercentOperator) {
    Lexer lexer("%");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::PERCENT);
}

TEST(LexerTest, EqualOperator) {
    Lexer lexer("=");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::EQ);
}

TEST(LexerTest, EqualEqualOperator) {
    Lexer lexer("==");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::EQEQ);
}

TEST(LexerTest, BangOperator) {
    Lexer lexer("!");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::NOT);
}

TEST(LexerTest, BangEqualOperator) {
    Lexer lexer("!=");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::NE);
}

TEST(LexerTest, LessOperator) {
    Lexer lexer("<");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::LT);
}

TEST(LexerTest, LessEqualOperator) {
    Lexer lexer("<=");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::LE);
}

TEST(LexerTest, GreaterOperator) {
    Lexer lexer(">");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::GT);
}

TEST(LexerTest, GreaterEqualOperator) {
    Lexer lexer(">=");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::GE);
}

TEST(LexerTest, AndOperator) {
    Lexer lexer("&&");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::AND);
}

TEST(LexerTest, OrOperator) {
    Lexer lexer("||");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::OR);
}

TEST(LexerTest, PipeOperator) {
    Lexer lexer("|>");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::PIPE);
}

// ============================================================================
// Delimiter Tests
// ============================================================================

TEST(LexerTest, LeftParen) {
    Lexer lexer("(");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::LPAREN);
}

TEST(LexerTest, RightParen) {
    Lexer lexer(")");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::RPAREN);
}

TEST(LexerTest, LeftBrace) {
    Lexer lexer("{");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::LBRACE);
}

TEST(LexerTest, RightBrace) {
    Lexer lexer("}");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::RBRACE);
}

TEST(LexerTest, LeftBracket) {
    Lexer lexer("[");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::LBRACKET);
}

TEST(LexerTest, RightBracket) {
    Lexer lexer("]");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::RBRACKET);
}

TEST(LexerTest, Comma) {
    Lexer lexer(",");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::COMMA);
}

TEST(LexerTest, Dot) {
    Lexer lexer(".");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::DOT);
}

TEST(LexerTest, Colon) {
    Lexer lexer(":");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 1);
    EXPECT_EQ(tokens[0].type, TokenType::COLON);
}

// ============================================================================
// Complex Expression Tests
// ============================================================================

TEST(LexerTest, SimpleExpression) {
    Lexer lexer("x + y");
    auto tokens = lexer.tokenize();
    ASSERT_EQ(tokens.size(), 4);  // x, +, y, EOF
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].type, TokenType::PLUS);
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[3].type, TokenType::END_OF_FILE);
}

TEST(LexerTest, FunctionCall) {
    Lexer lexer("print(\"hello\")");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 5);  // print, (, "hello", ), EOF
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].value, "print");
    EXPECT_EQ(tokens[1].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[2].type, TokenType::STRING);
    EXPECT_EQ(tokens[3].type, TokenType::RPAREN);
}

TEST(LexerTest, ArrayLiteral) {
    Lexer lexer("[1, 2, 3]");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 8);  // [, 1, ,, 2, ,, 3, ], EOF
    EXPECT_EQ(tokens[0].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[1].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[2].type, TokenType::COMMA);
    EXPECT_EQ(tokens[6].type, TokenType::RBRACKET);
}

TEST(LexerTest, DictLiteral) {
    Lexer lexer("{\"key\": \"value\"}");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 6);  // {, "key", :, "value", }, EOF
    EXPECT_EQ(tokens[0].type, TokenType::LBRACE);
    EXPECT_EQ(tokens[1].type, TokenType::STRING);
    EXPECT_EQ(tokens[2].type, TokenType::COLON);
    EXPECT_EQ(tokens[3].type, TokenType::STRING);
    EXPECT_EQ(tokens[4].type, TokenType::RBRACE);
}

TEST(LexerTest, VariableDeclaration) {
    Lexer lexer("let x = 42");
    auto tokens = lexer.tokenize();
    ASSERT_EQ(tokens.size(), 5);  // let, x, =, 42, EOF
    EXPECT_EQ(tokens[0].type, TokenType::LET);
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].type, TokenType::EQ);
    EXPECT_EQ(tokens[3].type, TokenType::NUMBER);
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST(LexerTest, MultipleStatementsOnOneLine) {
    Lexer lexer("let x = 1; let y = 2");
    auto tokens = lexer.tokenize();
    ASSERT_GE(tokens.size(), 9);
    EXPECT_EQ(tokens[0].type, TokenType::LET);
    EXPECT_EQ(tokens[4].type, TokenType::LET);
}

TEST(LexerTest, NumbersWithoutSpaces) {
    Lexer lexer("123+456");
    auto tokens = lexer.tokenize();
    ASSERT_EQ(tokens.size(), 4);  // 123, +, 456, EOF
    EXPECT_EQ(tokens[0].type, TokenType::NUMBER);
    EXPECT_EQ(tokens[1].type, TokenType::PLUS);
    EXPECT_EQ(tokens[2].type, TokenType::NUMBER);
}

TEST(LexerTest, LineNumberTracking) {
    Lexer lexer("let x = 1\nlet y = 2");
    auto tokens = lexer.tokenize();
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_GT(tokens[5].line, tokens[0].line);
}

// Total: 80+ lexer tests
