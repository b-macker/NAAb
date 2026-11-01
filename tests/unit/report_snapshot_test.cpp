// The report writers read check results while other threads record them (A86).
//
// Async fns and agent.batch workers record check results (every enforce() and
// pass) while the main thread writes a report -- at a HARD block in one async
// fn, the others are still running. The writers iterated check_results_ with
// no lock while those threads appended to it, and past MAX_CHECK_RESULTS the
// cap moves entries inside it, so a report could hold moved-from (empty)
// records, and reading through a reallocated buffer is undefined behaviour.
// They now iterate a copy taken under results_mutex_ (getCheckResults()).
//
// Here four threads record passes past the cap while the main thread writes
// JSON reports in a loop, the writers running until the reports are done;
// every record must have its rule name. Each writer
// records through a clean encoding check, which costs a byte scan. The
// ThreadSanitizer job runs this test too, where the unlocked read is reported
// directly.

#include <gtest/gtest.h>

#include "naab/governance.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using naab::governance::GovernanceEngine;

TEST(ReportSnapshot, AReportWrittenWhileOthersRecordHoldsWholeRecords) {
    GovernanceEngine engine;
    ASSERT_TRUE(engine.loadFromString(
        R"({"version": "3.0", "mode": "enforce",
            "code_quality": {"encoding": {"level": "hard"}}})"));

    // Control: a clean scan records a pass, so the writers below do append.
    const size_t before = engine.getCheckResults().size();
    ASSERT_EQ(engine.checkEncoding("let x = 1", 1), "");
    ASSERT_GT(engine.getCheckResults().size(), before);

    // The writers record until the reporter has written its reports, so every
    // report overlaps them however fast the build is. A fixed workload did not:
    // 4 x 3,000 records took about 8 s on the unoptimised local build and
    // finished on CI's Release build before more than 5 reports overlapped it
    // (#354). kMaxRecords only bounds a writer the reporter never stops.
    constexpr int kReports = 6;   // each holds ~10,000 records: ~2 s apiece unoptimised
    constexpr long kCap = 10000;          // MAX_CHECK_RESULTS
    constexpr long kMaxRecords = 4000000;
    std::atomic<bool> stop{false};
    std::atomic<long> recorded{0};
    std::atomic<int> writers_done{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; w++) {
        writers.emplace_back([&] {
            for (long i = 0; i < kMaxRecords && !stop.load(); i++) {
                engine.checkEncoding("let y = 2", static_cast<int>(i % 100000) + 1);
                recorded++;
            }
            writers_done++;
        });
    }
    // The report is built by nlohmann from the records it read, so a damaged
    // record shows as an empty rule name rather than as invalid JSON. Count the
    // records and the empty names in the report text (the unit-test target
    // has no JSON parser on its include path).
    auto count = [](const std::string& s, const std::string& needle) {
        size_t n = 0;
        for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) n++;
        return n;
    };
    // Past the cap first, so its compaction (which moves every record) runs
    // while the reports are written.
    while (recorded.load() < kCap && writers_done.load() < 4) std::this_thread::yield();
    int reports = 0, overlapped = 0;
    size_t empty_rules = 0, records = 0;
    while (reports < kReports && writers_done.load() < 4) {
        std::string report = engine.generateJsonReport();
        reports++;
        if (writers_done.load() == 0) overlapped++;
        records += count(report, "\"rule\":");
        empty_rules += count(report, "\"rule\": \"\"") + count(report, "\"rule\":\"\"");
    }
    stop = true;
    for (auto& t : writers) t.join();

    EXPECT_EQ(overlapped, kReports) << "a report was written after the writers stopped: it measures nothing";
    EXPECT_GT(recorded.load(), kCap) << "the writers never passed the cap: its compaction was not exercised";
    EXPECT_GT(records, 0u) << "the reports held no records: the probe sees nothing";
    EXPECT_EQ(empty_rules, 0u) << "records read while being moved or reallocated";
}
