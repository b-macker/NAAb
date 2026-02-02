// Fuzzer for NAAb Interpreter
// Tests full execution pipeline with randomly generated inputs
// Week 2, Task 2.1: Fuzzing Infrastructure

#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include <cstdint>
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty inputs
    if (size == 0) {
        return 0;
    }

    // Skip very large inputs (handled by limits.h)
    if (size > 50000) {  // Smaller limit for interpreter (more expensive)
        return 0;
    }

    // Convert to string
    std::string input(reinterpret_cast<const char*>(data), size);

    try {
        // Tokenize
        naab::lexer::Lexer lexer(input);
        auto tokens = lexer.tokenize();

        // Parse
        naab::parser::Parser parser(tokens);
        parser.setSource(input, "fuzz_input");
        auto program = parser.parseProgram();

        // Execute (with timeout protection from resource_limits.h)
        naab::interpreter::Interpreter interp;
        interp.setSourceCode(input, "fuzz_input");
        interp.execute(*program);

        // Successfully executed - good!
        // The fuzzer is looking for crashes, hangs, or sanitizer violations
    } catch (const std::exception& e) {
        // Expected exceptions are fine (runtime errors, limits, etc.)
        // Sanitizers will catch memory issues
    } catch (...) {
        // Catch any other exceptions
    }

    return 0;
}
