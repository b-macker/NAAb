// Unit test for exception handling (Phase 4.1)
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace naab;

void test_basic_try_catch() {
    std::cout << "Test 1: Basic try/catch... ";

    std::string source = R"(
try {
    throw "Test error"
} catch (e) {
    let result = "Caught: " + e
}
)";

    try {
        lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        parser::Parser parser(tokens);
        auto program = parser.parseProgram();

        interpreter::Interpreter interp;
        interp.execute(*program);

        std::cout << "PASS\n";
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
        throw;
    }
}

void test_try_catch_finally() {
    std::cout << "Test 2: Try/catch/finally... ";

    std::string source = R"(
let cleanup = false
try {
    throw "Error"
} catch (e) {
    let msg = e
} finally {
    cleanup = true
}
)";

    try {
        lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        parser::Parser parser(tokens);
        auto program = parser.parseProgram();

        interpreter::Interpreter interp;
        interp.execute(*program);

        std::cout << "PASS\n";
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
        throw;
    }
}

void test_no_error() {
    std::cout << "Test 3: Try block with no error... ";

    std::string source = R"(
try {
    let x = 42
} catch (e) {
    let msg = "Should not execute"
}
)";

    try {
        lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        parser::Parser parser(tokens);
        auto program = parser.parseProgram();

        interpreter::Interpreter interp;
        interp.execute(*program);

        std::cout << "PASS\n";
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
        throw;
    }
}

void test_throw_number() {
    std::cout << "Test 4: Throw number value... ";

    std::string source = R"(
try {
    throw 404
} catch (code) {
    let error_code = code
}
)";

    try {
        lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        parser::Parser parser(tokens);
        auto program = parser.parseProgram();

        interpreter::Interpreter interp;
        interp.execute(*program);

        std::cout << "PASS\n";
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
        throw;
    }
}

int main() {
    std::cout << "=== Exception Handling Tests (Phase 4.1) ===\n\n";

    try {
        test_basic_try_catch();
        test_try_catch_finally();
        test_no_error();
        test_throw_number();

        std::cout << "\n✓ All exception handling tests passed!\n";
        return 0;
    } catch (...) {
        std::cout << "\n✗ Exception handling tests failed\n";
        return 1;
    }
}
