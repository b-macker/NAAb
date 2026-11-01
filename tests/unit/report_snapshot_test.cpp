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
// JSON reports in a loop; every record must have its rule name. Each writer
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

    std::atomic<bool> stop{false};
    std::atomic<long> recorded{0};
    std::atomic<int> writers_done{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; w++) {
        writers.emplace_back([&] {
            // 4 x 3,000 passes the 10,000 cap, so the cap's compaction (which
            // moves every record) runs while reports are being written.
            for (int i = 0; i < 3000 && !stop.load(); i++) {
                engine.checkEncoding("let y = 2", i + 1);
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
    int reports = 0;
    size_t empty_rules = 0, records = 0;
    while (writers_done.load() < 4 && reports < 60) {
        std::string report = engine.generateJsonReport();
        reports++;
        records += count(report, "\"rule\":");
        empty_rules += count(report, "\"rule\": \"\"") + count(report, "\"rule\":\"\"");
    }
    stop = true;
    for (auto& t : writers) t.join();

    EXPECT_GT(reports, 5) << "too few reports overlapped the writers to measure anything";
    EXPECT_GT(records, 0u) << "the reports held no records: the probe sees nothing";
    EXPECT_EQ(empty_rules, 0u) << "records read while being moved or reallocated";
}
