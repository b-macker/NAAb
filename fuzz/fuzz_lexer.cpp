// Fuzzer for NAAb Lexer
// Tests tokenization with randomly generated inputs
// Week 2, Task 2.1: Fuzzing Infrastructure

#include "naab/lexer.h"
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
        // Create lexer and tokenize
        naab::lexer::Lexer lexer(input);
        auto tokens = lexer.tokenize();

        // Successfully tokenized - good!
        // The fuzzer is looking for crashes, hangs, or sanitizer violations
        // Normal exceptions are fine
    } catch (const std::exception& e) {
        // Expected exceptions are fine
        // Sanitizers will catch memory issues, buffer overflows, etc.
    } catch (...) {
        // Catch any other exceptions
    }

    return 0;
}
