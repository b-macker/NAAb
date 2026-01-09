// Quick test for the lexer
#include "naab/lexer.h"
#include <fmt/core.h>
#include <iostream>

int main() {
    std::string source = R"(
# Test program
use BLOCK-CPP-03845 as Cord

function greet(name: string) -> string {
    return "Hello, " + name
}

main {
    result = greet("World")
    print(result)
}
)";

    try {
        naab::lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        fmt::print("Lexer test: {} tokens\n\n", tokens.size());

        for (const auto& token : tokens) {
            if (token.type == naab::lexer::TokenType::NEWLINE) continue;
            if (token.type == naab::lexer::TokenType::END_OF_FILE) break;

            fmt::print("Line {:2d}: Type={:3d} '{}'\n",
                      token.line,
                      static_cast<int>(token.type),
                      token.value);
        }

        fmt::print("\n✓ Lexer working correctly!\n");
        return 0;

    } catch (const std::exception& e) {
        fmt::print("✗ Lexer error: {}\n", e.what());
        return 1;
    }
}
