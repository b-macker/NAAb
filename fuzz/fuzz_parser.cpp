// NAAb Parser Fuzz Harness — libFuzzer
// Feeds random/malformed input through Lexer → Parser.
// All exceptions are expected for invalid input; only crashes,
// ASAN/UBSAN violations, and timeouts indicate real bugs.

#include "naab/lexer.h"
#include "naab/parser.h"
#include <cstdint>
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Skip excessively large inputs to avoid slow runs
    if (size > 100000) return 0;

    std::string source(reinterpret_cast<const char*>(data), size);

    try {
        // Stage 1: Tokenize
        naab::lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        // Stage 2: Parse
        naab::parser::Parser parser(tokens);
        parser.setSource(source, "fuzz_input.naab");
        auto program = parser.parseProgram();
    } catch (...) {
        // ParseError, runtime_error, InputSizeException,
        // RecursionLimitException — all expected for malformed input.
    }

    return 0;
}
