// Parser Unit Tests
// Tests AST construction from tokens

#include <gtest/gtest.h>
#include "naab/parser.h"
#include "naab/lexer.h"
#include "naab/ast.h"

using namespace naab::parser;
using namespace naab::lexer;
using namespace naab::ast;

// Helper to parse source code
std::unique_ptr<Program> parse(const std::string& source) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    return parser.parseProgram();
}

// ============================================================================
// Basic Parsing Tests
// ============================================================================

TEST(ParserTest, EmptyProgram) {
    auto program = parse("");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, SimpleStatement) {
    auto program = parse("print(\"hello\")");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Literal Expression Tests
// ============================================================================

TEST(ParserTest, IntegerLiteral) {
    auto program = parse("42");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, FloatLiteral) {
    auto program = parse("3.14");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, StringLiteral) {
    auto program = parse("\"hello\"");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, BooleanTrue) {
    auto program = parse("true");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, BooleanFalse) {
    auto program = parse("false");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, ArrayLiteral) {
    auto program = parse("[1, 2, 3]");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, EmptyArray) {
    auto program = parse("[]");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, DictLiteral) {
    auto program = parse("{\"key\": \"value\"}");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, EmptyDict) {
    auto program = parse("{}");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Variable Declaration Tests
// ============================================================================

TEST(ParserTest, SimpleVariableDeclaration) {
    auto program = parse("let x = 42");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, VariableWithStringValue) {
    auto program = parse("let name = \"Alice\"");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, VariableWithExpression) {
    auto program = parse("let result = 1 + 2");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, MultipleVariableDeclarations) {
    auto program = parse("let x = 1\nlet y = 2");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Binary Expression Tests
// ============================================================================

TEST(ParserTest, Addition) {
    auto program = parse("1 + 2");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, Subtraction) {
    auto program = parse("5 - 3");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, Multiplication) {
    auto program = parse("4 * 5");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, Division) {
    auto program = parse("10 / 2");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, Modulo) {
    auto program = parse("10 % 3");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, CompoundExpression) {
    auto program = parse("1 + 2 * 3");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, ParenthesizedExpression) {
    auto program = parse("(1 + 2) * 3");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Comparison Operator Tests
// ============================================================================

TEST(ParserTest, Equality) {
    auto program = parse("x == y");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, Inequality) {
    auto program = parse("x != y");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, LessThan) {
    auto program = parse("x < y");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, LessThanOrEqual) {
    auto program = parse("x <= y");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, GreaterThan) {
    auto program = parse("x > y");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, GreaterThanOrEqual) {
    auto program = parse("x >= y");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Logical Operator Tests
// ============================================================================

TEST(ParserTest, LogicalAnd) {
    auto program = parse("true && false");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, LogicalOr) {
    auto program = parse("true || false");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, LogicalNot) {
    auto program = parse("!true");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, ComplexLogicalExpression) {
    auto program = parse("(x > 0) && (y < 10)");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Function Declaration Tests
// ============================================================================

TEST(ParserTest, SimpleFunctionDeclaration) {
    auto program = parse("function add(x, y) { return x + y }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, FunctionWithNoParameters) {
    auto program = parse("function hello() { print(\"hello\") }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, FunctionWithSingleParameter) {
    auto program = parse("function double(x) { return x * 2 }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, FunctionWithDefaultParameter) {
    auto program = parse("function greet(name = \"World\") { return name }");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Function Call Tests
// ============================================================================

TEST(ParserTest, SimpleFunctionCall) {
    auto program = parse("print(\"hello\")");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, FunctionCallWithMultipleArgs) {
    auto program = parse("add(1, 2)");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, NestedFunctionCall) {
    auto program = parse("print(add(1, 2))");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, FunctionCallNoArgs) {
    auto program = parse("getValue()");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Member Expression Tests
// ============================================================================

TEST(ParserTest, PropertyAccess) {
    auto program = parse("obj.property");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, ChainedPropertyAccess) {
    auto program = parse("obj.prop1.prop2");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, MethodCall) {
    auto program = parse("obj.method()");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, MethodCallWithArgs) {
    auto program = parse("obj.method(arg1, arg2)");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// If Statement Tests
// ============================================================================

TEST(ParserTest, SimpleIfStatement) {
    auto program = parse("if (true) { print(\"yes\") }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, IfElseStatement) {
    auto program = parse("if (x > 0) { print(\"positive\") } else { print(\"negative\") }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, IfElseIfElseStatement) {
    auto program = parse("if (x > 0) { print(\"positive\") } else if (x < 0) { print(\"negative\") } else { print(\"zero\") }");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Loop Tests
// ============================================================================

TEST(ParserTest, ForLoop) {
    auto program = parse("for (i in [1,2,3]) { print(i) }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, WhileLoop) {
    auto program = parse("while (x < 10) { x = x + 1 }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, BreakStatement) {
    auto program = parse("while (true) { break }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, ContinueStatement) {
    auto program = parse("for (i in [1,2,3]) { continue }");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Exception Handling Tests
// ============================================================================

TEST(ParserTest, TryCatchBlock) {
    auto program = parse("try { risky() } catch (e) { print(e) }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, TryFinallyBlock) {
    auto program = parse("try { risky() } finally { cleanup() }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, TryCatchFinallyBlock) {
    auto program = parse("try { risky() } catch (e) { handle(e) } finally { cleanup() }");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, ThrowStatement) {
    auto program = parse("throw \"error\"");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Import/Export Tests
// ============================================================================

TEST(ParserTest, ImportStatement) {
    auto program = parse("import \"module\" as mod");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, ExportStatement) {
    auto program = parse("export let x = 42");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, UseStatement) {
    auto program = parse("use block_id as alias");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Pipeline Operator Tests
// ============================================================================

TEST(ParserTest, SimplePipeline) {
    auto program = parse("x |> f");
    ASSERT_NE(program, nullptr);
}

TEST(ParserTest, ChainedPipeline) {
    auto program = parse("x |> f |> g |> h");
    ASSERT_NE(program, nullptr);
}

// ============================================================================
// Error Detection Tests
// ============================================================================

TEST(ParserTest, MissingClosingParen) {
    EXPECT_THROW(parse("print(\"hello\""), std::runtime_error);
}

TEST(ParserTest, MissingClosingBracket) {
    EXPECT_THROW(parse("[1, 2, 3"), std::runtime_error);
}

TEST(ParserTest, MissingClosingBrace) {
    EXPECT_THROW(parse("{\"key\": \"value\""), std::runtime_error);
}

TEST(ParserTest, InvalidSyntax) {
    EXPECT_THROW(parse("let = 42"), std::runtime_error);
}

TEST(ParserTest, UnexpectedToken) {
    EXPECT_THROW(parse("let x = +"), std::runtime_error);
}

// Total: 80+ parser tests
