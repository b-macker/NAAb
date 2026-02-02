// Standalone test for error reporter
#include "naab/error_reporter.h"
#include <fmt/core.h>

int main() {
    using namespace naab::error;

    // Sample source code
    std::string source = R"(# Test program
main {
    let x = 42
    let bad = x + "hello"
    let y = undefined_var
    print(y)
})";

    fmt::print("=== Error Reporter Demo ===\n\n");

    // Create error reporter
    ErrorReporter reporter;
    reporter.setSource(source, "test.naab");

    // Report some errors
    reporter.error("Cannot add int and string", 4, 15);
    reporter.addSuggestion("Convert the string to int using int()");
    reporter.addSuggestion("Or convert the int to string using str()");

    reporter.error("Undefined variable 'undefined_var'", 5, 13);
    reporter.addSuggestion("Did you mean 'x'?");
    reporter.addSuggestion("Define the variable before using it");

    reporter.warning("Variable 'y' is used before being properly initialized", 6, 11);

    // Print all diagnostics with source context
    fmt::print("\n");
    reporter.printAllWithSource();

    // Summary
    fmt::print("Summary: {} error(s), {} warning(s)\n",
               reporter.errorCount(), reporter.warningCount());

    return 0;
}
