// Test Example Programs Parsing
#include "naab/lexer.h"
#include "naab/parser.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool testParse(const std::string& name, const std::string& path) {
    fmt::print("\n--- Testing: {} ---\n", name);
    fmt::print("File: {}\n", path);

    std::string source = readFile(path);
    if (source.empty()) {
        fmt::print("[ERROR] Failed to read file\n");
        return false;
    }

    fmt::print("Source: {} bytes\n", source.size());

    try {
        naab::lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();
        fmt::print("Tokens: {}\n", tokens.size());

        naab::parser::Parser parser(tokens);
        auto program = parser.parseProgram();
        fmt::print("[SUCCESS] Parsed successfully\n");

        // Show program structure
        fmt::print("Imports: {}\n", program->getImports().size());
        fmt::print("Functions: {}\n", program->getFunctions().size());
        fmt::print("Has main: {}\n", program->getMainBlock() ? "yes" : "no");

        return true;

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Parse failed: {}\n", e.what());
        return false;
    }
}

int main() {
    fmt::print("=== Example Programs Parse Test ===\n");

    int passed = 0;
    int total = 0;

    // Test 1: cpp_math.naab
    total++;
    if (testParse("cpp_math.naab",
                  "examples/cpp_math.naab")) {
        passed++;
    }

    // Test 2: js_utils.naab
    total++;
    if (testParse("js_utils.naab",
                  "examples/js_utils.naab")) {
        passed++;
    }

    // Test 3: polyglot.naab
    total++;
    if (testParse("polyglot.naab",
                  "examples/polyglot.naab")) {
        passed++;
    }

    fmt::print("\n=== Results ===\n");
    fmt::print("Passed: {}/{}\n", passed, total);
    fmt::print("Success Rate: {:.1f}%\n", (passed * 100.0) / total);

    return (passed == total) ? 0 : 1;
}
