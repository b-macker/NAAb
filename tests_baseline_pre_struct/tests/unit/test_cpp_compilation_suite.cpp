// Comprehensive C++ Compilation Test Suite
// Tests compilation of blocks with various library dependencies

#include "naab/cpp_executor.h"
#include "naab/block_enricher.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <vector>
#include <string>

using namespace naab;

// Test result structure
struct TestResult {
    std::string name;
    bool passed;
    std::string message;
};

std::vector<TestResult> results;

void recordTest(const std::string& name, bool passed, const std::string& msg = "") {
    results.push_back({name, passed, msg});
    if (passed) {
        fmt::print("  ✅ {}\n", name);
    } else {
        fmt::print("  ❌ {} - {}\n", name, msg);
    }
}

// Test 1: Simple standalone block (no libraries)
void test_simple_block() {
    fmt::print("\nTest 1: Simple Standalone Block\n");
    fmt::print("--------------------------------\n");

    runtime::CppExecutor executor;
    tools::BlockEnricher enricher;

    std::string code = R"(
        extern "C" {
            int add(int a, int b) {
                return a + b;
            }
        }
    )";

    auto libs = enricher.detectLibraries(code);
    bool compiled = executor.compileBlock("TEST-SIMPLE", code, "add", libs);

    recordTest("Simple block compiles", compiled);

    if (compiled) {
        // Try to call the function
        std::vector<std::shared_ptr<interpreter::Value>> args;
        args.push_back(std::make_shared<interpreter::Value>(10));
        args.push_back(std::make_shared<interpreter::Value>(20));

        try {
            auto result = executor.callFunction("TEST-SIMPLE", "add", args);
            int value = std::get<int>(result->data);
            recordTest("Simple block executes correctly", value == 30,
                      fmt::format("Expected 30, got {}", value));
        } catch (const std::exception& e) {
            recordTest("Simple block executes correctly", false, e.what());
        }
    }
}

// Test 2: Block with standard library only
void test_stdlib_block() {
    fmt::print("\nTest 2: Standard Library Block\n");
    fmt::print("-------------------------------\n");

    runtime::CppExecutor executor;
    tools::BlockEnricher enricher;

    std::string code = R"(
        #include <string>
        #include <vector>
        #include <cmath>

        extern "C" {
            double compute_magnitude() {
                return std::sqrt(3.0 * 3.0 + 4.0 * 4.0);
            }
        }
    )";

    auto libs = enricher.detectLibraries(code);
    recordTest("Stdlib block has no detected libs", libs.empty());

    bool compiled = executor.compileBlock("TEST-STDLIB", code, "compute_magnitude", libs);
    recordTest("Stdlib block compiles", compiled);
}

// Test 3: Library detection accuracy
void test_library_detection() {
    fmt::print("\nTest 3: Library Detection Accuracy\n");
    fmt::print("-----------------------------------\n");

    tools::BlockEnricher enricher;

    // Test various include patterns
    struct DetectionTest {
        std::string code;
        std::string expected_lib;
        std::string description;
    };

    std::vector<DetectionTest> tests = {
        {"#include <spdlog/spdlog.h>", "spdlog", "spdlog header"},
        {"#include \"llvm/IR/Value.h\"", "llvm", "LLVM header"},
        {"#include <clang/AST/Decl.h>", "clang", "Clang header"},
        {"#include <fmt/core.h>", "fmt", "fmt header"},
        {"#include <omp.h>", "openmp", "OpenMP header"},
        {"#include <pthread.h>", "pthread", "pthread header"},
        {"#include <sqlite3.h>", "sqlite3", "SQLite header"},
        {"#include <curl/curl.h>", "curl", "curl header"},
    };

    for (const auto& test : tests) {
        auto libs = enricher.detectLibraries(test.code);
        bool found = false;
        for (const auto& lib : libs) {
            if (lib == test.expected_lib) {
                found = true;
                break;
            }
        }
        recordTest(fmt::format("Detects {}", test.description), found,
                  found ? "" : fmt::format("Expected '{}', got {} libs", test.expected_lib, libs.size()));
    }
}

// Test 4: Multiple libraries
void test_multiple_libraries() {
    fmt::print("\nTest 4: Multiple Library Detection\n");
    fmt::print("-----------------------------------\n");

    tools::BlockEnricher enricher;

    std::string code = R"(
        #include <spdlog/spdlog.h>
        #include <fmt/core.h>
        #include <pthread.h>
        #include <sqlite3.h>
    )";

    auto libs = enricher.detectLibraries(code);

    recordTest("Detects multiple libraries", libs.size() >= 3,
              fmt::format("Expected >=3, got {}", libs.size()));

    // Check for expected libraries
    bool has_spdlog = false, has_pthread = false, has_sqlite = false;
    for (const auto& lib : libs) {
        if (lib == "spdlog") has_spdlog = true;
        if (lib == "pthread") has_pthread = true;
        if (lib == "sqlite3") has_sqlite = true;
    }

    recordTest("Found spdlog", has_spdlog);
    recordTest("Found pthread", has_pthread);
    recordTest("Found sqlite3", has_sqlite);
}

// Test 5: Compilation with external includes (detection only)
void test_external_includes() {
    fmt::print("\nTest 5: External Library Includes\n");
    fmt::print("----------------------------------\n");

    tools::BlockEnricher enricher;

    // Test that external library includes are detected
    std::string code = R"(
        #include <boost/filesystem.hpp>
        #include <gtest/gtest.h>
        #include <Eigen/Dense>
    )";

    auto libs = enricher.detectLibraries(code);

    recordTest("Detects boost", std::find(libs.begin(), libs.end(), "boost") != libs.end());
    recordTest("Detects gtest", std::find(libs.begin(), libs.end(), "gtest") != libs.end());
    recordTest("Detects eigen", std::find(libs.begin(), libs.end(), "eigen") != libs.end());
}

// Test 7: Deduplication
void test_deduplication() {
    fmt::print("\nTest 7: Library Deduplication\n");
    fmt::print("------------------------------\n");

    tools::BlockEnricher enricher;

    std::string code = R"(
        #include <spdlog/spdlog.h>
        #include <spdlog/async.h>
        #include <spdlog/sinks/stdout_sinks.h>
        #include <fmt/core.h>
        #include <fmt/format.h>
    )";

    auto libs = enricher.detectLibraries(code);

    // Should only have 2 libs (spdlog, fmt) even with 5 includes
    int spdlog_count = 0, fmt_count = 0;
    for (const auto& lib : libs) {
        if (lib == "spdlog") spdlog_count++;
        if (lib == "fmt") fmt_count++;
    }

    recordTest("Deduplicates spdlog", spdlog_count == 1);
    recordTest("Deduplicates fmt", fmt_count == 1);
    recordTest("Total libs is small", libs.size() <= 3,
              fmt::format("Expected <=3, got {}", libs.size()));
}

void print_summary() {
    fmt::print("\n");
    fmt::print("================================================================\n");
    fmt::print("  Test Summary\n");
    fmt::print("================================================================\n\n");

    int passed = 0;
    int total = results.size();

    for (const auto& result : results) {
        if (result.passed) passed++;
    }

    fmt::print("Total Tests: {}\n", total);
    fmt::print("Passed: {} ({}%)\n", passed, passed * 100 / total);
    fmt::print("Failed: {}\n\n", total - passed);

    if (passed == total) {
        fmt::print("✅ ALL TESTS PASSED!\n");
    } else {
        fmt::print("❌ Some tests failed:\n\n");
        for (const auto& result : results) {
            if (!result.passed) {
                fmt::print("  - {}: {}\n", result.name, result.message);
            }
        }
    }

    fmt::print("\n================================================================\n");
}

int main() {
    fmt::print("================================================================\n");
    fmt::print("  C++ Compilation Test Suite\n");
    fmt::print("  Phase 3: Testing Excellence\n");
    fmt::print("================================================================\n");

    // Run all tests
    test_simple_block();
    test_stdlib_block();
    test_library_detection();
    test_multiple_libraries();
    test_external_includes();
    test_deduplication();

    // Print summary
    print_summary();

    return results.size() == 0 ? 1 : (results.back().passed ? 0 : 1);
}
