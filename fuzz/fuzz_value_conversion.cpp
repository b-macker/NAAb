// Fuzzer for Value Conversion
// Tests type marshaling and conversion between C++ and polyglot values
// Week 2, Task 2.2: FFI/Polyglot Boundary Fuzzing

#include "naab/interpreter.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <random>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Need at least some data to work with
    if (size < 8) {
        return 0;
    }

    // Skip very large inputs
    if (size > 10000) {
        return 0;
    }

    // Use fuzzer input to create various Value types
    // This tests edge cases in value creation and conversion
    try {
        // Test 1: Integer values from raw bytes
        if (size >= 8) {
            int64_t i = *reinterpret_cast<const int64_t*>(data);
            // Cast to int to avoid ambiguous constructor
            auto v1 = std::make_shared<naab::interpreter::Value>(static_cast<int>(i));

            // Try conversions
            try {
                v1->toInt();
                v1->toFloat();
                v1->toString();
                v1->toBool();
            } catch (...) {
                // Expected for some conversions
            }
        }

        // Test 2: Float values from raw bytes
        if (size >= 8) {
            double d = *reinterpret_cast<const double*>(data);
            auto v2 = std::make_shared<naab::interpreter::Value>(d);

            try {
                v2->toInt();
                v2->toFloat();
                v2->toString();
            } catch (...) {
                // Expected for some conversions
            }
        }

        // Test 3: String values from fuzzer input
        if (size >= 1 && size <= 1000) {
            std::string str(reinterpret_cast<const char*>(data), size);
            auto v3 = std::make_shared<naab::interpreter::Value>(str);

            try {
                v3->toString();
                v3->toInt();    // May fail
                v3->toFloat();  // May fail
            } catch (...) {
                // Expected for non-numeric strings
            }
        }

        // Test 4: List creation and access
        std::vector<std::shared_ptr<naab::interpreter::Value>> list;
        for (size_t i = 0; i < std::min(size, size_t(10)); i++) {
            int val = static_cast<int>(data[i]);
            list.push_back(std::make_shared<naab::interpreter::Value>(val));
        }
        auto v4 = std::make_shared<naab::interpreter::Value>(list);

        try {
            // Access list - Value doesn't have toList(), just test toString()
            v4->toString();
        } catch (...) {
            // Some conversions may fail
        }

        // Successfully tested - fuzzer is looking for:
        // - Buffer overflows in conversions
        // - Type confusion bugs
        // - Memory leaks or use-after-free
        // - Crashes in edge cases (NaN, infinity, etc.)
    } catch (const std::exception& e) {
        // Some exceptions are expected
    } catch (...) {
        // Catch any other exceptions
    }

    return 0;
}
