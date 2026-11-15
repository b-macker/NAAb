// The performance profiler behind `naab-lang --profile` (naab_profiling).
//
// Folded in from tests/unit/test_profiler.cpp, a standalone program in the
// default build target that nothing ran (A58). Its checks are kept as they
// were and the ones it printed without asserting are asserted: enable/disable
// toggles the state, and each recorded duration is no shorter than the sleep
// it timed.
#include "naab/profiler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

using naab::profiling::Profiler;
using naab::profiling::ScopedProfile;

namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void manualFunction() {
    auto& p = Profiler::instance();
    p.startFunction("manualFunction");
    sleepMs(10);
    p.endFunction("manualFunction");
}

void scopedFunction() {
    PROFILE_FUNCTION();
    sleepMs(20);
}

void nestedFunction() {
    PROFILE_FUNCTION();
    sleepMs(5);
    {
        ScopedProfile inner("innerFunction", "function");
        sleepMs(15);
    }
    sleepMs(5);
}

void loadTwoBlocks() {
    auto& p = Profiler::instance();
    p.startBlock("BLOCK-JS-001");
    sleepMs(30);
    p.endBlock("BLOCK-JS-001");
    p.startBlock("BLOCK-CPP-001");
    sleepMs(50);
    p.endBlock("BLOCK-CPP-001");
}

// The profiler is a process-wide singleton: every test starts it enabled and
// empty, and leaves it disabled and empty for the tests that follow.
class ProfilerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Profiler::instance().enable();
        Profiler::instance().clear();
    }
    void TearDown() override {
        Profiler::instance().clear();
        Profiler::instance().disable();
    }
};

}  // namespace

TEST_F(ProfilerTest, EnableAndDisableToggleTheState) {
    auto& p = Profiler::instance();
    p.disable();
    EXPECT_FALSE(p.isEnabled());
    p.enable();
    EXPECT_TRUE(p.isEnabled());
    p.disable();
    EXPECT_FALSE(p.isEnabled());
}

TEST_F(ProfilerTest, ManualStartAndEndRecordOneEntry) {
    manualFunction();
    auto report = Profiler::instance().generateReport();
    ASSERT_EQ(report.total_entries, 1);
    ASSERT_EQ(report.function_stats.size(), 1u);
    EXPECT_EQ(report.function_stats[0].name, "manualFunction");
    EXPECT_GE(report.function_stats[0].total_ms, 10.0);
}

TEST_F(ProfilerTest, ScopedProfileRecordsOneEntry) {
    scopedFunction();
    auto report = Profiler::instance().generateReport();
    ASSERT_EQ(report.total_entries, 1);
    ASSERT_EQ(report.function_stats.size(), 1u);
    EXPECT_GE(report.function_stats[0].total_ms, 20.0);
}

TEST_F(ProfilerTest, RepeatedCallsAreCountedWithStatistics) {
    manualFunction();
    manualFunction();
    manualFunction();
    auto report = Profiler::instance().generateReport();
    ASSERT_EQ(report.function_stats.size(), 1u);
    const auto& s = report.function_stats[0];
    EXPECT_EQ(s.call_count, 3);
    EXPECT_GE(s.total_ms, 30.0);
    EXPECT_LE(s.min_ms, s.avg_ms);
    EXPECT_LE(s.avg_ms, s.max_ms);
}

TEST_F(ProfilerTest, NestedScopesAreRecordedSeparately) {
    nestedFunction();
    auto report = Profiler::instance().generateReport();
    EXPECT_EQ(report.total_entries, 2);
    ASSERT_EQ(report.function_stats.size(), 2u);
    double outer = -1, inner = -1;
    for (const auto& s : report.function_stats) {
        if (s.name == "innerFunction") inner = s.total_ms;
        else outer = s.total_ms;
    }
    ASSERT_GE(inner, 15.0);
    EXPECT_GE(outer, inner);  // the outer scope contains the inner one
}

TEST_F(ProfilerTest, BlocksAreRecordedApartFromFunctions) {
    loadTwoBlocks();
    auto report = Profiler::instance().generateReport();
    EXPECT_EQ(report.block_stats.size(), 2u);
    EXPECT_TRUE(report.function_stats.empty());
}

TEST_F(ProfilerTest, FullReportRendersAndClearEmptiesIt) {
    manualFunction();
    scopedFunction();
    loadTwoBlocks();
    auto report = Profiler::instance().generateReport();
    EXPECT_GT(report.total_entries, 0);
    EXPECT_FALSE(report.toString().empty());
    Profiler::instance().clear();
    EXPECT_EQ(Profiler::instance().generateReport().total_entries, 0);
}

TEST_F(ProfilerTest, NothingIsRecordedWhileDisabled) {
    Profiler::instance().disable();
    manualFunction();
    scopedFunction();
    EXPECT_EQ(Profiler::instance().generateReport().total_entries, 0);
}
