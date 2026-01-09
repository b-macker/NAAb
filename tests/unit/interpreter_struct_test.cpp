#include <gtest/gtest.h>
#include "naab/interpreter.h"
#include "naab/parser.h"
#include "naab/lexer.h"
#include "naab/struct_registry.h"

using namespace naab;

TEST(InterpreterStructTest, StructDeclRegistration) {
    runtime::StructRegistry::instance().clearForTesting();

    std::string source = R"(
        struct Point {
            x: INT;
            y: INT;
        }
    )";

    lexer::Lexer lexer(source);
    auto tokens = lexer.tokenize();
    parser::Parser parser(tokens);
    auto program = parser.parseProgram();

    interpreter::Interpreter interp;
    interp.execute(*program);

    ASSERT_TRUE(runtime::StructRegistry::instance().hasStruct("Point"));
}

TEST(InterpreterStructTest, StructLiteralCreation) {
    runtime::StructRegistry::instance().clearForTesting();

    std::string source = R"(
        struct Point {
            x: INT;
            y: INT;
        }

        main {
            let p = new Point { x: 10, y: 20 }
        }
    )";

    lexer::Lexer lexer(source);
    auto tokens = lexer.tokenize();
    parser::Parser parser(tokens);
    auto program = parser.parseProgram();

    interpreter::Interpreter interp;
    ASSERT_NO_THROW(interp.execute(*program));
}

TEST(InterpreterStructTest, StructMemberAccess) {
    runtime::StructRegistry::instance().clearForTesting();

    std::string source = R"(
        struct Point {
            x: INT;
            y: INT;
        }

        main {
            let p = new Point { x: 42, y: 100 }
            let x_val = p.x
        }
    )";

    lexer::Lexer lexer(source);
    auto tokens = lexer.tokenize();
    parser::Parser parser(tokens);
    auto program = parser.parseProgram();

    interpreter::Interpreter interp;
    ASSERT_NO_THROW(interp.execute(*program));
}

TEST(InterpreterStructTest, StructMissingFieldError) {
    runtime::StructRegistry::instance().clearForTesting();

    std::string source = R"(
        struct Point {
            x: INT;
            y: INT;
        }

        main {
            let p = new Point { x: 10 }
        }
    )";

    lexer::Lexer lexer(source);
    auto tokens = lexer.tokenize();
    parser::Parser parser(tokens);
    auto program = parser.parseProgram();

    interpreter::Interpreter interp;
    ASSERT_THROW(interp.execute(*program), std::runtime_error);
}

TEST(InterpreterStructTest, StructUnknownFieldError) {
    runtime::StructRegistry::instance().clearForTesting();

    std::string source = R"(
        struct Point {
            x: INT;
            y: INT;
        }

        main {
            let p = new Point { x: 10, y: 20, z: 30 }
        }
    )";

    lexer::Lexer lexer(source);
    auto tokens = lexer.tokenize();
    parser::Parser parser(tokens);
    auto program = parser.parseProgram();

    interpreter::Interpreter interp;
    ASSERT_THROW(interp.execute(*program), std::runtime_error);
}
