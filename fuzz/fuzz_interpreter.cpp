// NAAb Interpreter Fuzz Harness — libFuzzer
// Feeds valid-syntax but adversarial programs through Lexer → Parser → Interpreter.
// Inputs that fail to parse are cheaply skipped. Only crashes, ASAN/UBSAN
// violations, and timeouts (-timeout=10) indicate real bugs.

#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include <cstdint>
#include <cstddef>
#include <string>

// Disable leak detection — the interpreter uses shared_ptr cycles cleaned by GC,
// and libpython has its own allocator. Both cause benign leak reports.
extern "C" const char* __asan_default_options() {
    return "detect_leaks=0";
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Tighter cap than parser fuzzer — interpreter is slower per iteration
    if (size > 50000) return 0;

    std::string source(reinterpret_cast<const char*>(data), size);

    try {
        // Stage 1: Lex + Parse (skip if invalid syntax)
        naab::lexer::Lexer lexer(source);
        auto tokens = lexer.tokenize();

        naab::parser::Parser parser(tokens);
        parser.setSource(source, "fuzz_input.naab");
        auto program = parser.parseProgram();

        // Stage 2: Interpret
        // Governance disabled — no govern.json filesystem probing.
        // Polyglot blocks throw at runtime (no executor) — caught below.
        // Python not initialized — Python paths throw, not crash.
        // Infinite loops killed by libFuzzer -timeout flag.
        naab::interpreter::Interpreter interpreter;
        interpreter.disableGovernance();
        interpreter.setSourceCode(source, "fuzz_input.naab");
        interpreter.execute(*program);
    } catch (...) {
        // All exceptions expected for adversarial input:
        // runtime_error, NaabError, InputSizeException,
        // RecursionLimitException, etc.
    }

    return 0;
}
