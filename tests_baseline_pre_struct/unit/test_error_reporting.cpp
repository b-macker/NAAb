// Test Enhanced Error Reporting (Phase 1.3)
// Validates that parser and interpreter produce helpful error messages

#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/error_reporter.h"
#include <iostream>
#include <cassert>

#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL: " << message << std::endl; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

int main() {
    std::cout << "=== Testing Enhanced Error Reporting (Phase 1.3) ===" << std::endl;
    std::cout << std::endl;

    // Test 1: Parse error with source context
    std::cout << "Test 1: Parse error with source context..." << std::endl;
    try {
        std::string source = R"(
main {
    let x =
    print(x)
}
)";

        naab::lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        naab::parser::Parser parser(tokens);
        parser.setSource(source, "test.naab");

        try {
            auto program = parser.parseProgram();
            std::cout << "  ✗ Expected parse error but parsing succeeded" << std::endl;
        } catch (const naab::parser::ParseError& e) {
            std::cout << "  ✓ Caught parse error: " << e.what() << std::endl;

            // Check that error reporter has diagnostics
            const auto& reporter = parser.getErrorReporter();
            ASSERT(reporter.hasErrors(), "Error reporter should have errors");
            std::cout << "  ✓ Error reporter has " << reporter.errorCount() << " error(s)" << std::endl;

            // Print enhanced error message
            std::cout << "\n  Enhanced error output:" << std::endl;
            std::cout << "  " << std::string(60, '-') << std::endl;
            reporter.printAllWithSource();
            std::cout << "  " << std::string(60, '-') << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "  ✗ Unexpected exception: " << e.what() << std::endl;
        return 1;
    }

    // Test 2: Multiple errors
    std::cout << "\nTest 2: Multiple syntax errors..." << std::endl;
    try {
        std::string source = R"(
main {
    let x = 10
    let y =
    let z = 30
}
)";

        naab::lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        naab::parser::Parser parser(tokens);
        parser.setSource(source, "test2.naab");

        try {
            auto program = parser.parseProgram();
        } catch (const naab::parser::ParseError& e) {
            std::cout << "  ✓ Caught parse error" << std::endl;
            const auto& reporter = parser.getErrorReporter();
            std::cout << "  ✓ Error count: " << reporter.errorCount() << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "  ✗ Unexpected exception: " << e.what() << std::endl;
        return 1;
    }

    // Test 3: Verify error reporter API
    std::cout << "\nTest 3: Error reporter API..." << std::endl;
    {
        naab::error::ErrorReporter reporter;
        std::string source = "let x = undefined_variable\nprint(x)\n";
        reporter.setSource(source, "api_test.naab");

        reporter.error("Undefined variable: undefined_variable", 1, 9);
        reporter.addSuggestion("Did you mean 'x'?");

        ASSERT(reporter.hasErrors(), "Should have errors");
        ASSERT(reporter.errorCount() == 1, "Should have exactly 1 error");
        std::cout << "  ✓ Error reporter API works correctly" << std::endl;

        std::cout << "\n  Sample error output:" << std::endl;
        std::cout << "  " << std::string(60, '-') << std::endl;
        reporter.printAllWithSource();
        std::cout << "  " << std::string(60, '-') << std::endl;
    }

    // Test 4: Warnings
    std::cout << "\nTest 4: Warnings..." << std::endl;
    {
        naab::error::ErrorReporter reporter;
        std::string source = "let unused_var = 42\n";
        reporter.setSource(source, "warnings.naab");

        reporter.warning("Unused variable: unused_var", 1, 5);
        reporter.addSuggestion("Consider removing this variable or using it");

        ASSERT(!reporter.hasErrors(), "Should not have errors");
        ASSERT(reporter.warningCount() == 1, "Should have exactly 1 warning");
        std::cout << "  ✓ Warnings work correctly" << std::endl;

        std::cout << "\n  Sample warning output:" << std::endl;
        std::cout << "  " << std::string(60, '-') << std::endl;
        reporter.printAllWithSource();
        std::cout << "  " << std::string(60, '-') << std::endl;
    }

    std::cout << "\n=== All Error Reporting Tests Passed! ===" << std::endl;
    return 0;
}
