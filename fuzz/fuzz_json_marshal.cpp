// Fuzzer for JSON Marshaling
// Tests JSON parsing and serialization at FFI boundaries
// Week 2, Task 2.2: FFI/Polyglot Boundary Fuzzing

#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include <cstdint>
#include <cstddef>
#include <string>

// External JSON parsing (if available)
// This tests the JSON module's parsing of untrusted input

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip empty inputs
    if (size == 0) {
        return 0;
    }

    // Skip very large inputs
    if (size > 10000) {
        return 0;
    }

    // Convert to string (will be treated as JSON)
    std::string json_str(reinterpret_cast<const char*>(data), size);

    try {
        // Test JSON parsing via NAAb interpreter
        // This simulates: json.parse(fuzzer_input)

        // Create minimal test program with JSON parsing
        std::string code = "use json\nmain { let x = json.parse(\"\"\"" +
                          json_str + "\"\"\") }";

        // Parse and execute
        naab::lexer::Lexer lexer(code);
        auto tokens = lexer.tokenize();

        naab::parser::Parser parser(tokens);
        parser.setSource(code, "fuzz_json");
        auto program = parser.parseProgram();

        naab::interpreter::Interpreter interp;
        interp.setSourceCode(code, "fuzz_json");
        interp.execute(*program);

        // Successfully parsed and executed
        // Fuzzer is looking for:
        // - Buffer overflows in JSON parser
        // - Stack overflow from deeply nested JSON
        // - Memory leaks
        // - Crashes on malformed JSON
    } catch (const std::exception& e) {
        // Expected - most random inputs won't be valid JSON
    } catch (...) {
        // Catch any other exceptions
    }

    return 0;
}
