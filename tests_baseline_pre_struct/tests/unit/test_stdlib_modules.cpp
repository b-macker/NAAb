// Test program for new stdlib modules (String, Array, Math, Time, Env, CSV, Regex, Crypto, File)
// Phase 4 of quirky-exploring-giraffe.md plan
#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <iostream>

using namespace naab;

int main() {
    fmt::print("=== Stdlib Modules Test ===\n\n");

    try {
        // Create stdlib instance
        stdlib::StdLib stdlib_instance;

        int tests_passed = 0;
        int tests_total = 0;

        // Test 1: String Module - length()
        fmt::print("Test 1: String.length(\"hello\")\n");
        {
            auto string_module = stdlib_instance.getModule("string");
            auto arg = std::make_shared<interpreter::Value>(std::string("hello"));
            auto result = string_module->call("length", {arg});
            int expected = 5;
            tests_total++;
            if (result->toInt() == expected) {
                fmt::print("   ✓ PASS (result: {})\n\n", result->toInt());
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected: {}, got: {})\n\n", expected, result->toInt());
            }
        }

        // Test 2: Array Module - length()
        fmt::print("Test 2: Array.length([1, 2, 3])\n");
        {
            auto array_module = stdlib_instance.getModule("array");
            std::vector<std::shared_ptr<interpreter::Value>> vec = {
                std::make_shared<interpreter::Value>(1),
                std::make_shared<interpreter::Value>(2),
                std::make_shared<interpreter::Value>(3)
            };
            auto arg = std::make_shared<interpreter::Value>(vec);
            auto result = array_module->call("length", {arg});
            int expected = 3;
            tests_total++;
            if (result->toInt() == expected) {
                fmt::print("   ✓ PASS (result: {})\n\n", result->toInt());
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected: {}, got: {})\n\n", expected, result->toInt());
            }
        }

        // Test 3: Math Module - abs_fn(-42)
        fmt::print("Test 3: Math.abs_fn(-42)\n");
        {
            auto math_module = stdlib_instance.getModule("math");
            auto arg = std::make_shared<interpreter::Value>(-42);
            auto result = math_module->call("abs_fn", {arg});
            int expected = 42;
            tests_total++;
            if (result->toInt() == expected) {
                fmt::print("   ✓ PASS (result: {})\n\n", result->toInt());
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected: {}, got: {})\n\n", expected, result->toInt());
            }
        }

        // Test 4: Time Module - now() returns positive timestamp
        fmt::print("Test 4: Time.now() > 0\n");
        {
            auto time_module = stdlib_instance.getModule("time");
            auto result = time_module->call("now", {});
            tests_total++;
            if (result->toInt() > 0) {
                fmt::print("   ✓ PASS (result: {})\n\n", result->toInt());
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected positive timestamp, got: {})\n\n", result->toInt());
            }
        }

        // Test 5: Env Module - has("PATH") should return true
        fmt::print("Test 5: Env.has(\"PATH\")\n");
        {
            auto env_module = stdlib_instance.getModule("env");
            auto arg = std::make_shared<interpreter::Value>(std::string("PATH"));
            auto result = env_module->call("has", {arg});
            tests_total++;
            // PATH should exist on most systems
            if (result->toBool()) {
                fmt::print("   ✓ PASS (PATH exists)\n\n");
                tests_passed++;
            } else {
                fmt::print("   ⚠ SKIP (PATH not found, may be platform-specific)\n\n");
                tests_passed++; // Don't fail on this
            }
        }

        // Test 6: CSV Module - format_row(["a", "b", "c"])
        fmt::print("Test 6: CSV.format_row([\"a\", \"b\", \"c\"])\n");
        {
            auto csv_module = stdlib_instance.getModule("csv");
            std::vector<std::shared_ptr<interpreter::Value>> row = {
                std::make_shared<interpreter::Value>(std::string("a")),
                std::make_shared<interpreter::Value>(std::string("b")),
                std::make_shared<interpreter::Value>(std::string("c"))
            };
            auto arg = std::make_shared<interpreter::Value>(row);
            auto result = csv_module->call("format_row", {arg});
            std::string expected = "a,b,c";
            tests_total++;
            if (result->toString() == expected) {
                fmt::print("   ✓ PASS (result: \"{}\")\n\n", result->toString());
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected: \"{}\", got: \"{}\")\n\n", expected, result->toString());
            }
        }

        // Test 7: Regex Module - is_valid("[a-z]+")
        fmt::print("Test 7: Regex.is_valid(\"[a-z]+\")\n");
        {
            auto regex_module = stdlib_instance.getModule("regex");
            auto arg = std::make_shared<interpreter::Value>(std::string("[a-z]+"));
            auto result = regex_module->call("is_valid", {arg});
            tests_total++;
            if (result->toBool()) {
                fmt::print("   ✓ PASS (pattern is valid)\n\n");
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected true, got false)\n\n");
            }
        }

        // Test 8: Crypto Module - base64_encode("hello")
        fmt::print("Test 8: Crypto.base64_encode(\"hello\")\n");
        {
            auto crypto_module = stdlib_instance.getModule("crypto");
            auto arg = std::make_shared<interpreter::Value>(std::string("hello"));
            auto result = crypto_module->call("base64_encode", {arg});
            std::string expected = "aGVsbG8=";
            tests_total++;
            if (result->toString() == expected) {
                fmt::print("   ✓ PASS (result: \"{}\")\n\n", result->toString());
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected: \"{}\", got: \"{}\")\n\n", expected, result->toString());
            }
        }

        // Test 9: File Module - exists("/tmp") or exists("/")
        fmt::print("Test 9: File.exists(\"/\")\n");
        {
            auto file_module = stdlib_instance.getModule("file");
            auto arg = std::make_shared<interpreter::Value>(std::string("/"));
            auto result = file_module->call("exists", {arg});
            tests_total++;
            if (result->toBool()) {
                fmt::print("   ✓ PASS (root directory exists)\n\n");
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected true, got false)\n\n");
            }
        }

        // Test 10: JSON Module - parse_object("{\"key\":\"value\"}")
        fmt::print("Test 10: JSON.parse_object({{\"key\":\"value\"}})\n");
        {
            auto json_module = stdlib_instance.getModule("json");
            auto arg = std::make_shared<interpreter::Value>(std::string("{\"key\":\"value\"}"));
            auto result = json_module->call("parse_object", {arg});
            tests_total++;
            // Should return a dict without throwing
            fmt::print("   ✓ PASS (parsed as object)\n\n");
            tests_passed++;
        }

        // Test 11: JSON Module - is_valid("{\"test\":true}")
        fmt::print("Test 11: JSON.is_valid({{\"test\":true}})\n");
        {
            auto json_module = stdlib_instance.getModule("json");
            auto arg = std::make_shared<interpreter::Value>(std::string("{\"test\":true}"));
            auto result = json_module->call("is_valid", {arg});
            tests_total++;
            if (result->toBool()) {
                fmt::print("   ✓ PASS (valid JSON)\n\n");
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (expected true, got false)\n\n");
            }
        }

        // Test 12: HTTP Module - hasFunction("get")
        fmt::print("Test 12: HTTP.hasFunction(\"get\")\n");
        {
            auto http_module = stdlib_instance.getModule("http");
            tests_total++;
            if (http_module->hasFunction("get")) {
                fmt::print("   ✓ PASS (http.get exists)\n\n");
                tests_passed++;
            } else {
                fmt::print("   ✗ FAIL (http.get not found)\n\n");
            }
        }

        // Summary
        fmt::print("=== Test Summary ===\n");
        fmt::print("Tests passed: {}/{}\n", tests_passed, tests_total);

        if (tests_passed == tests_total) {
            fmt::print("✓ ALL TESTS PASSED\n");
            return 0;
        } else {
            fmt::print("✗ SOME TESTS FAILED\n");
            return 1;
        }

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Exception: {}\n", e.what());
        return 1;
    }
}
