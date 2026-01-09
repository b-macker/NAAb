// Test program for Performance Profiler
#include "naab/profiler.h"
#include <fmt/core.h>
#include <thread>
#include <chrono>

using namespace naab;

// Simulate some work
void simulateWork(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Test function with manual profiling
void testFunction1() {
    auto& profiler = profiling::Profiler::instance();

    profiler.startFunction("testFunction1");
    simulateWork(10);
    profiler.endFunction("testFunction1");
}

// Test function with RAII profiling
void testFunction2() {
    PROFILE_FUNCTION();
    simulateWork(20);
}

// Test nested profiling
void testNested() {
    PROFILE_FUNCTION();

    simulateWork(5);

    {
        profiling::ScopedProfile inner("innerFunction", "function");
        simulateWork(15);
    }

    simulateWork(5);
}

// Test block profiling
void testBlockLoading() {
    auto& profiler = profiling::Profiler::instance();

    profiler.startBlock("BLOCK-JS-001");
    simulateWork(30);
    profiler.endBlock("BLOCK-JS-001");

    profiler.startBlock("BLOCK-CPP-001");
    simulateWork(50);
    profiler.endBlock("BLOCK-CPP-001");
}

int main() {
    fmt::print("=== Performance Profiler Test ===\n\n");

    auto& profiler = profiling::Profiler::instance();

    // Test 1: Enable/disable profiling
    fmt::print("Test 1: Enable/disable profiling\n");
    fmt::print("================================\n");

    fmt::print("  Initial state: {}\n", profiler.isEnabled() ? "enabled" : "disabled");
    profiler.enable();
    fmt::print("  After enable: {}\n", profiler.isEnabled() ? "enabled" : "disabled");
    profiler.disable();
    fmt::print("  After disable: {}\n", profiler.isEnabled() ? "disabled" : "enabled");
    fmt::print("  ✓ PASS\n\n");

    // Test 2: Manual function profiling
    fmt::print("Test 2: Manual function profiling\n");
    fmt::print("==================================\n");

    profiler.enable();
    profiler.clear();

    fmt::print("  Calling testFunction1 (10ms)...\n");
    testFunction1();

    auto report = profiler.generateReport();
    fmt::print("  Entries recorded: {}\n", report.total_entries);

    if (report.total_entries == 1 && !report.function_stats.empty()) {
        fmt::print("  Function: {}\n", report.function_stats[0].name);
        fmt::print("  Duration: {:.2f}ms\n", report.function_stats[0].total_ms);
        fmt::print("  ✓ PASS\n\n");
    } else {
        fmt::print("  ✗ FAIL (expected 1 entry)\n\n");
        return 1;
    }

    // Test 3: RAII profiling
    fmt::print("Test 3: RAII profiling (ScopedProfile)\n");
    fmt::print("=======================================\n");

    profiler.clear();

    fmt::print("  Calling testFunction2 (20ms)...\n");
    testFunction2();

    report = profiler.generateReport();
    fmt::print("  Entries recorded: {}\n", report.total_entries);

    if (report.total_entries == 1) {
        fmt::print("  Function: {}\n", report.function_stats[0].name);
        fmt::print("  Duration: {:.2f}ms\n", report.function_stats[0].total_ms);
        fmt::print("  ✓ PASS\n\n");
    } else {
        fmt::print("  ✗ FAIL\n\n");
        return 1;
    }

    // Test 4: Multiple calls
    fmt::print("Test 4: Multiple calls (statistics)\n");
    fmt::print("====================================\n");

    profiler.clear();

    fmt::print("  Calling testFunction1 3 times...\n");
    testFunction1();
    testFunction1();
    testFunction1();

    report = profiler.generateReport();
    fmt::print("  Entries recorded: {}\n", report.total_entries);
    fmt::print("  Call count: {}\n", report.function_stats[0].call_count);
    fmt::print("  Total time: {:.2f}ms\n", report.function_stats[0].total_ms);
    fmt::print("  Avg time: {:.2f}ms\n", report.function_stats[0].avg_ms);
    fmt::print("  Min time: {:.2f}ms\n", report.function_stats[0].min_ms);
    fmt::print("  Max time: {:.2f}ms\n", report.function_stats[0].max_ms);

    if (report.function_stats[0].call_count == 3) {
        fmt::print("  ✓ PASS\n\n");
    } else {
        fmt::print("  ✗ FAIL (expected 3 calls)\n\n");
        return 1;
    }

    // Test 5: Nested profiling
    fmt::print("Test 5: Nested profiling\n");
    fmt::print("=========================\n");

    profiler.clear();

    fmt::print("  Calling testNested (outer: 25ms, inner: 15ms)...\n");
    testNested();

    report = profiler.generateReport();
    fmt::print("  Entries recorded: {}\n", report.total_entries);
    fmt::print("  Functions profiled: {}\n", report.function_stats.size());

    if (report.total_entries == 2 && report.function_stats.size() == 2) {
        for (const auto& stat : report.function_stats) {
            fmt::print("    {}: {:.2f}ms\n", stat.name, stat.total_ms);
        }
        fmt::print("  ✓ PASS\n\n");
    } else {
        fmt::print("  ✗ FAIL\n\n");
        return 1;
    }

    // Test 6: Block profiling
    fmt::print("Test 6: Block profiling\n");
    fmt::print("========================\n");

    profiler.clear();

    fmt::print("  Loading blocks...\n");
    testBlockLoading();

    report = profiler.generateReport();
    fmt::print("  Entries recorded: {}\n", report.total_entries);
    fmt::print("  Blocks profiled: {}\n", report.block_stats.size());

    if (!report.block_stats.empty()) {
        for (const auto& stat : report.block_stats) {
            fmt::print("    {}: {:.2f}ms\n", stat.name, stat.total_ms);
        }
        fmt::print("  ✓ PASS\n\n");
    } else {
        fmt::print("  ✗ FAIL (no blocks profiled)\n\n");
        return 1;
    }

    // Test 7: Full report
    fmt::print("Test 7: Full report generation\n");
    fmt::print("===============================\n");

    profiler.clear();

    // Mix of functions and blocks
    testFunction1();
    testFunction2();
    testBlockLoading();

    report = profiler.generateReport();

    fmt::print("\n");
    fmt::print("{}", report.toString());

    if (report.total_entries > 0) {
        fmt::print("  ✓ PASS\n\n");
    } else {
        fmt::print("  ✗ FAIL (no entries)\n\n");
        return 1;
    }

    // Test 8: Clear profiling data
    fmt::print("Test 8: Clear profiling data\n");
    fmt::print("=============================\n");

    fmt::print("  Entries before clear: {}\n", report.total_entries);
    profiler.clear();

    report = profiler.generateReport();
    fmt::print("  Entries after clear: {}\n", report.total_entries);

    if (report.total_entries == 0) {
        fmt::print("  ✓ PASS\n\n");
    } else {
        fmt::print("  ✗ FAIL (entries not cleared)\n\n");
        return 1;
    }

    fmt::print("=== All Profiler Tests Passed! ===\n");

    return 0;
}
