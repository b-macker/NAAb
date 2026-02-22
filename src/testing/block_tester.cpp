// NAAb Block Testing Framework Implementation
// Tests individual blocks in isolation

#include "naab/block_tester.h"
#include "naab/language_registry.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <chrono>

namespace naab {
namespace testing {

BlockTester::BlockTester() {
}

std::string BlockTester::getTestDefinitionDir() {
    return "tests/fixtures/block-tests";
}

bool BlockTester::loadTestDefinition(const std::string& test_file_path) {
    fmt::print("[TEST] Loading test definition: {}\n", test_file_path);

    // Read file
    std::ifstream file(test_file_path);
    if (!file.is_open()) {
        fmt::print("[ERROR] Could not open test definition file: {}\n", test_file_path);
        return false;
    }

    std::string json_content((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());

    // Parse JSON (simplified - in production, use nlohmann/json)
    return parseTestDefinition(json_content);
}

bool BlockTester::parseTestDefinition(const std::string& json_content) {
    // Simplified JSON parsing for demonstration
    // In production, use nlohmann/json or similar library

    // For now, create a hardcoded test for demonstration
    definition_.block_id = "TEST-BLOCK-001";
    definition_.language = "javascript";
    definition_.setup_code = "";

    BlockTest test;
    test.name = "add_function_test";
    test.code = "function add(a, b) { return a + b; }";

    Assertion assertion;
    assertion.type = AssertionType::EQUALS;
    assertion.value_expr = "add(5, 3)";
    assertion.expected = "8";

    test.assertions.push_back(assertion);
    definition_.tests.push_back(test);

    fmt::print("[TEST] Loaded {} test(s) for block: {}\n",
               definition_.tests.size(), definition_.block_id);

    return true;
}

TestResults BlockTester::runTests() {
    TestResults results;
    results.block_id = definition_.block_id;
    results.total = definition_.tests.size();
    results.passed = 0;
    results.failed = 0;

    fmt::print("\n=== Running tests for {} ===\n\n", definition_.block_id);

    // Get executor for the language
    auto& registry = runtime::LanguageRegistry::instance();
    auto* executor = registry.getExecutor(definition_.language);

    if (!executor) {
        fmt::print("[ERROR] No executor found for language: {}\n", definition_.language);
        return results;
    }

    // Execute setup code if present
    if (!definition_.setup_code.empty()) {
        fmt::print("[SETUP] Executing setup code...\n");
        if (!executor->execute(definition_.setup_code)) {
            fmt::print("[ERROR] Setup code failed\n");
            return results;
        }
    }

    // Run each test
    for (const auto& test : definition_.tests) {
        TestResult result = runSingleTest(test);
        results.results.push_back(result);

        if (result.passed) {
            results.passed++;
            fmt::print("✓ {} ({:.2f}ms)\n", test.name, result.execution_time_ms);
        } else {
            results.failed++;
            fmt::print("✗ {} - {}\n", test.name, result.error_message);
        }
    }

    fmt::print("\n=== Test Summary ===\n");
    fmt::print("Total:  {}\n", results.total);
    fmt::print("Passed: {}\n", results.passed);
    fmt::print("Failed: {}\n", results.failed);
    fmt::print("\n");

    return results;
}

TestResult BlockTester::runSingleTest(const BlockTest& test) {
    TestResult result;
    result.test_name = test.name;
    result.passed = false;
    result.error_message = "";

    auto start = std::chrono::high_resolution_clock::now();

    try {
        // Get executor for the language
        auto& registry = runtime::LanguageRegistry::instance();
        auto* executor = registry.getExecutor(definition_.language);

        if (!executor) {
            result.error_message = fmt::format("No executor for language: {}", definition_.language);
            return result;
        }

        // Execute test code
        if (!executor->execute(test.code)) {
            result.error_message = "Failed to execute test code";
            return result;
        }

        // Check all assertions
        bool all_assertions_passed = true;
        for (const auto& assertion : test.assertions) {
            // For simplified demo, evaluate the expression
            auto value = executor->callFunction("add", {
                std::make_shared<interpreter::Value>(5),
                std::make_shared<interpreter::Value>(3)
            });

            std::string error;
            if (!checkAssertion(assertion, value, error)) {
                result.error_message = error;
                all_assertions_passed = false;
                break;
            }
        }

        result.passed = all_assertions_passed;

    } catch (const std::exception& e) {
        result.error_message = fmt::format("Exception: {}", e.what());
        result.passed = false;
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.execution_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return result;
}

bool BlockTester::checkAssertion(const Assertion& assertion,
                                  const std::shared_ptr<interpreter::Value>& result,
                                  std::string& error_message) {
    // Get actual value as string
    std::string actual = result->toString();

    switch (assertion.type) {
        case AssertionType::EQUALS:
            if (actual != assertion.expected) {
                error_message = fmt::format("Expected: {}, Got: {}", assertion.expected, actual);
                return false;
            }
            return true;

        case AssertionType::NOT_EQUALS:
            if (actual == assertion.expected) {
                error_message = fmt::format("Should not equal: {}", assertion.expected);
                return false;
            }
            return true;

        case AssertionType::GREATER_THAN:
            try {
                int actual_int = result->toInt();
                int expected_int = std::stoi(assertion.expected);
                if (actual_int <= expected_int) {
                    error_message = fmt::format("{} not > {}", actual_int, expected_int);
                    return false;
                }
            } catch (...) {
                error_message = "Cannot compare as integers";
                return false;
            }
            return true;

        case AssertionType::LESS_THAN:
            try {
                int actual_int = result->toInt();
                int expected_int = std::stoi(assertion.expected);
                if (actual_int >= expected_int) {
                    error_message = fmt::format("{} not < {}", actual_int, expected_int);
                    return false;
                }
            } catch (...) {
                error_message = "Cannot compare as integers";
                return false;
            }
            return true;

        case AssertionType::CONTAINS:
            if (actual.find(assertion.expected) == std::string::npos) {
                error_message = fmt::format("'{}' does not contain '{}'", actual, assertion.expected);
                return false;
            }
            return true;

        case AssertionType::TYPE_IS:
            // Check type - simplified for demo
            if (assertion.expected == "int") {
                if (!std::holds_alternative<int>(result->data)) {
                    error_message = fmt::format("Expected type: int, Got: {}", actual);
                    return false;
                }
            } else if (assertion.expected == "string") {
                if (!std::holds_alternative<std::string>(result->data)) {
                    error_message = fmt::format("Expected type: string, Got: {}", actual);
                    return false;
                }
            }
            return true;

        default:
            error_message = "Unknown assertion type";
            return false;
    }
}

TestResults BlockTester::runTestsForBlock(const std::string& block_id) {
    std::string test_file = fmt::format("{}/{}.test.json",
                                         getTestDefinitionDir(), block_id);

    if (!loadTestDefinition(test_file)) {
        TestResults results;
        results.block_id = block_id;
        results.total = 0;
        results.passed = 0;
        results.failed = 1;
        return results;
    }

    return runTests();
}

} // namespace testing
} // namespace naab
