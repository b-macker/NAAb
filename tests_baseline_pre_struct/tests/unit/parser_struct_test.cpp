// Parser Struct Edge Case Tests
// Week 8, Task 59

#include <gtest/gtest.h>
#include "naab/parser.h"
#include "naab/lexer.h"
#include "naab/ast.h"

using namespace naab;

TEST(ParserStructTest, EmptyStruct) {
    std::string source = R"(
        struct Empty {
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    ASSERT_EQ(program->getStructs().size(), 1);
    auto& struct_decl = program->getStructs()[0];
    ASSERT_EQ(struct_decl->getName(), "Empty");
    ASSERT_EQ(struct_decl->getFields().size(), 0);
}

TEST(ParserStructTest, SingleField) {
    std::string source = R"(
        struct Single {
            value: INT;
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    ASSERT_EQ(program->getStructs().size(), 1);
    auto& struct_decl = program->getStructs()[0];
    ASSERT_EQ(struct_decl->getName(), "Single");
    ASSERT_EQ(struct_decl->getFields().size(), 1);
    ASSERT_EQ(struct_decl->getFields()[0].name, "value");
}

TEST(ParserStructTest, MultipleFields) {
    std::string source = R"(
        struct Point3D {
            x: INT;
            y: INT;
            z: INT;
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    ASSERT_EQ(program->getStructs().size(), 1);
    auto& struct_decl = program->getStructs()[0];
    ASSERT_EQ(struct_decl->getFields().size(), 3);
}

TEST(ParserStructTest, StructLiteralBasic) {
    std::string source = R"(
        main {
            let p = new Point { x: 10, y: 20 }
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    // Should parse without error
    ASSERT_NE(program, nullptr);
}

TEST(ParserStructTest, StructLiteralSingleField) {
    std::string source = R"(
        main {
            let s = new Single { value: 42 }
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    ASSERT_NE(program, nullptr);
}

TEST(ParserStructTest, StructLiteralNestedInArray) {
    std::string source = R"(
        main {
            let points = [
                new Point { x: 0, y: 0 },
                new Point { x: 1, y: 1 },
                new Point { x: 2, y: 2 }
            ]
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    ASSERT_NE(program, nullptr);
}

TEST(ParserStructTest, StructLiteralInMap) {
    std::string source = R"(
        main {
            let map = {
                origin: new Point { x: 0, y: 0 },
                destination: new Point { x: 100, y: 200 }
            }
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    ASSERT_NE(program, nullptr);
}

TEST(ParserStructTest, NestedStructLiteral) {
    std::string source = R"(
        main {
            let line = new Line {
                start: new Point { x: 0, y: 0 },
                end: new Point { x: 10, y: 10 }
            }
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    ASSERT_NE(program, nullptr);
}

TEST(ParserStructTest, MissingNewKeywordError) {
    std::string source = R"(
        main {
            let p = Point { x: 10, y: 20 }
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    // Should parse as map literal, not struct literal
    // (Point becomes an identifier, {...} is a map)
    auto program = p.parseProgram();
    ASSERT_NE(program, nullptr);
}

TEST(ParserStructTest, VariousFieldTypes) {
    std::string source = R"(
        struct Mixed {
            int_field: INT;
            float_field: FLOAT;
            string_field: STRING;
            bool_field: BOOL;
        }
    )";

    lexer::Lexer lex(source);
    auto tokens = lex.tokenize();
    parser::Parser p(tokens);

    auto program = p.parseProgram();
    ASSERT_EQ(program->getStructs().size(), 1);
    auto& struct_decl = program->getStructs()[0];
    ASSERT_EQ(struct_decl->getFields().size(), 4);
}
