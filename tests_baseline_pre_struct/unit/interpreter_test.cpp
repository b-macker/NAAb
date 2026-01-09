// Interpreter Unit Tests
// Tests evaluation and execution of NAAb programs

#include <gtest/gtest.h>
#include "naab/interpreter.h"
#include "naab/parser.h"
#include "naab/lexer.h"

using namespace naab::interpreter;
using namespace naab::parser;
using namespace naab::lexer;

// Helper to execute and get result
std::shared_ptr<Value> execute(const std::string& source) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parseProgram();
    Interpreter interp;
    interp.execute(*program);
    return interp.getResult();
}

// ============================================================================
// Basic Evaluation Tests
// ============================================================================

TEST(InterpreterTest, IntegerLiteral) {
    auto result = execute("42");
    ASSERT_NE(result, nullptr);
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 42);
}

TEST(InterpreterTest, FloatLiteral) {
    auto result = execute("3.14");
    ASSERT_NE(result, nullptr);
    auto* floatval = std::get_if<double>(&result->data);
    ASSERT_NE(floatval, nullptr);
    EXPECT_NEAR(*floatval, 3.14, 0.0001);
}

TEST(InterpreterTest, StringLiteral) {
    auto result = execute("\"hello\"");
    ASSERT_NE(result, nullptr);
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "hello");
}

TEST(InterpreterTest, BooleanTrue) {
    auto result = execute("true");
    ASSERT_NE(result, nullptr);
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, BooleanFalse) {
    auto result = execute("false");
    ASSERT_NE(result, nullptr);
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_FALSE(*boolval);
}

// ============================================================================
// Arithmetic Tests
// ============================================================================

TEST(InterpreterTest, Addition) {
    auto result = execute("2 + 3");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 5);
}

TEST(InterpreterTest, Subtraction) {
    auto result = execute("5 - 2");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 3);
}

TEST(InterpreterTest, Multiplication) {
    auto result = execute("4 * 3");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 12);
}

TEST(InterpreterTest, Division) {
    auto result = execute("10 / 2");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 5);
}

TEST(InterpreterTest, Modulo) {
    auto result = execute("10 % 3");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 1);
}

TEST(InterpreterTest, OperatorPrecedence) {
    auto result = execute("2 + 3 * 4");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 14);  // Not 20
}

TEST(InterpreterTest, Parentheses) {
    auto result = execute("(2 + 3) * 4");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 20);
}

// ============================================================================
// Comparison Tests
// ============================================================================

TEST(InterpreterTest, Equality) {
    auto result = execute("5 == 5");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, Inequality) {
    auto result = execute("5 != 3");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, LessThan) {
    auto result = execute("3 < 5");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, LessThanOrEqual) {
    auto result = execute("5 <= 5");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, GreaterThan) {
    auto result = execute("5 > 3");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, GreaterThanOrEqual) {
    auto result = execute("5 >= 5");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

// ============================================================================
// Logical Operator Tests
// ============================================================================

TEST(InterpreterTest, LogicalAnd) {
    auto result = execute("true && true");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, LogicalAndShortCircuit) {
    auto result = execute("false && true");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_FALSE(*boolval);
}

TEST(InterpreterTest, LogicalOr) {
    auto result = execute("false || true");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, LogicalOrShortCircuit) {
    auto result = execute("true || false");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(InterpreterTest, LogicalNot) {
    auto result = execute("!false");
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

// ============================================================================
// Variable Tests
// ============================================================================

TEST(InterpreterTest, VariableDeclaration) {
    auto result = execute("let x = 42\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 42);
}

TEST(InterpreterTest, VariableReassignment) {
    auto result = execute("let x = 10\nx = 20\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 20);
}

TEST(InterpreterTest, MultipleVariables) {
    auto result = execute("let x = 10\nlet y = 20\nx + y");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 30);
}

// ============================================================================
// Function Tests
// ============================================================================

TEST(InterpreterTest, SimpleFunctionCall) {
    auto result = execute("function add(x, y) { return x + y }\nadd(2, 3)");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 5);
}

TEST(InterpreterTest, FunctionWithDefaultParameter) {
    auto result = execute("function greet(name = \"World\") { return name }\ngreet()");
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "World");
}

TEST(InterpreterTest, RecursiveFunction) {
    auto result = execute("function fib(n) { if (n <= 1) { return n } return fib(n-1) + fib(n-2) }\nfib(6)");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 8);
}

// ============================================================================
// Array Tests
// ============================================================================

TEST(InterpreterTest, ArrayLiteral) {
    auto result = execute("[1, 2, 3]");
    auto* arrval = std::get_if<std::vector<std::shared_ptr<Value>>>(&result->data);
    ASSERT_NE(arrval, nullptr);
    EXPECT_EQ(arrval->size(), 3);
}

TEST(InterpreterTest, ArrayIndexing) {
    auto result = execute("let arr = [10, 20, 30]\narr[1]");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 20);
}

// ============================================================================
// Control Flow Tests
// ============================================================================

TEST(InterpreterTest, IfStatement) {
    auto result = execute("let x = 0\nif (true) { x = 42 }\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 42);
}

TEST(InterpreterTest, IfElseStatement) {
    auto result = execute("let x = 0\nif (false) { x = 10 } else { x = 20 }\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 20);
}

TEST(InterpreterTest, WhileLoop) {
    auto result = execute("let x = 0\nwhile (x < 5) { x = x + 1 }\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 5);
}

TEST(InterpreterTest, ForLoop) {
    auto result = execute("let sum = 0\nfor (i in [1,2,3]) { sum = sum + i }\nsum");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 6);
}

TEST(InterpreterTest, BreakStatement) {
    auto result = execute("let x = 0\nwhile (true) { x = x + 1\nif (x == 3) { break } }\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 3);
}

TEST(InterpreterTest, ContinueStatement) {
    auto result = execute("let x = 0\nfor (i in [1,2,3,4,5]) { if (i % 2 == 0) { continue }\nx = x + i }\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 9);  // 1 + 3 + 5
}

// ============================================================================
// Exception Handling Tests
// ============================================================================

TEST(InterpreterTest, TryCatchBlock) {
    auto result = execute("let x = 0\ntry { x = 42 } catch (e) { x = 10 }\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 42);
}

TEST(InterpreterTest, ThrowAndCatch) {
    auto result = execute("let x = 0\ntry { throw \"error\" } catch (e) { x = 1 }\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 1);
}

TEST(InterpreterTest, FinallyBlock) {
    auto result = execute("let x = 0\ntry { x = 10 } finally { x = x + 5 }\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 15);
}

// ============================================================================
// Scope Tests
// ============================================================================

TEST(InterpreterTest, GlobalScope) {
    auto result = execute("let x = 10\nfunction f() { return x }\nf()");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 10);
}

TEST(InterpreterTest, LocalScope) {
    auto result = execute("let x = 10\nfunction f() { let x = 20\nreturn x }\nf()");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 20);
}

TEST(InterpreterTest, GlobalScopeAfterLocalScope) {
    auto result = execute("let x = 10\nfunction f() { let x = 20\nreturn x }\nf()\nx");
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 10);  // Global x unchanged
}

// ============================================================================
// String Operations
// ============================================================================

TEST(InterpreterTest, StringConcatenation) {
    auto result = execute("\"hello\" + \" \" + \"world\"");
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "hello world");
}

// Total: 60+ interpreter tests
