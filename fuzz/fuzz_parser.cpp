// Fuzzer for NAAb Parser
// Tests parsing with randomly generated inputs
// Week 2, Task 2.1: Fuzzing Infrastructure

#include "naab/lexer.h"
#include "naab/parser.h"
#include <cstdint>
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty inputs
    if (size == 0) {
        return 0;
    }

    // Skip very large inputs (handled by limits.h)
    if (size > 100000) {
        return 0;
    }

    // Convert to string
    std::string input(reinterpret_cast<const char*>(data), size);

    try {
        // Tokenize first
        naab::lexer::Lexer lexer(input);
        auto tokens = lexer.tokenize();

        // Parse tokens
        naab::parser::Parser parser(tokens);
        parser.setSource(input, "fuzz_input");
        auto program = parser.parseProgram();

        // Successfully parsed - good!
        // The fuzzer is looking for crashes, hangs, or sanitizer violations
    } catch (const std::exception& e) {
        // Expected exceptions are fine (parse errors, recursion limits, etc.)
        // Sanitizers will catch memory issues
    } catch (...) {
        // Catch any other exceptions
    }

    return 0;
}
