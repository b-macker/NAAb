// The check-result cap (V-GOV-024) and what depends on it: A177, A107, A193.
//
// A177: the cap evicted one result per record once the list was full, by a
//   pass that moved every other result, so each record past 10,000 cost about
//   10,000 moves. It now compacts only when the list is CHECK_RESULTS_SLACK
//   over the cap. At a compaction point the results kept must be the ones the
//   per-record pass kept: the newest MAX_CHECK_RESULTS.
// A107: telemetry resumed from a POSITION in the list, and the cap erases from
//   the front, so results appended between two report writes were skipped. It
//   resumes by record number now; every result is written once, or counted in
//   results_not_written when it left memory before a write.
// A193: the run-wide counts (summary line, quality gate, wasBlocked) read only
//   what the cap had kept, so an advisory recorded early stopped counting.
//
// Results are recorded through checkEncoding(): a clean scan records a pass,
// a null byte at advisory records one advisory. Kill table, by mutant of the
// engine (run by hand from a copy of the tree):
//   U1 compact at every record again (no slack)    -> SizeStaysWithinCapPlusSlack
//                                                      (size == MAX after one over)
//   U2 compaction keeps the oldest record          -> RetainedSetIsTheNewest
//      (excess - 1 instead is an equivalent mutant: the function's second
//      branch evicts what the first loop left over)
//   U3 the tally not updated at eviction           -> EvictedResultsStillCount
//   U4 the quality gate ignores the tally          -> EvictedResultsStillCount
//   U5 telemetry resumes by position again         -> TelemetryWritesEachResultOnce
//   U6 results_not_written not counted             -> TelemetryCountsWhatLeftMemoryUnwritten
//   U7 wasBlocked() ignores the tally              -> AnEvictedRefusalStillCounts

#include <gtest/gtest.h>

#include "naab/governance.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using naab::governance::GovernanceEngine;

namespace {

constexpr size_t kCap = GovernanceEngine::MAX_CHECK_RESULTS;
constexpr size_t kSlack = GovernanceEngine::CHECK_RESULTS_SLACK;

// Record passes until the newest record number reaches `target`.
void recordPassesUntil(GovernanceEngine& e, uint64_t target) {
    for (;;) {
        auto r = e.getCheckResults();
        uint64_t last = r.empty() ? 0 : r.back().record_id;
        if (last >= target) return;
        // One call records one pass; record the gap in one go.
        for (uint64_t i = last; i < target; ++i) e.checkEncoding("let x = 1", 1);
    }
}

// Telemetry lines are one compact JSON object each; read the two facts the
// tests need as text (the unit-test target has no JSON library).
struct TelemetryCounts { size_t check_rows = 0; uint64_t not_written = 0; };
TelemetryCounts readTelemetry(const std::string& path) {
    TelemetryCounts c;
    std::ifstream in(path);
    std::string l;
    while (std::getline(in, l)) {
        if (l.find("\"event_type\":\"GovernanceCheck\"") != std::string::npos ||
            l.find("\"event_type\":\"RuleViolation\"") != std::string::npos) c.check_rows++;
        const std::string key = "\"results_not_written\":";
        auto k = l.find(key);
        if (l.find("\"event_type\":\"GovernanceCheckSummary\"") != std::string::npos &&
            k != std::string::npos)
            c.not_written += std::stoull(l.substr(k + key.size()));
    }
    return c;
}

std::string tempPath(const char* stem) {
    return (std::filesystem::temp_directory_path() /
            (std::string(stem) + "_" + std::to_string(::getpid()) + ".jsonl")).string();
}

}  // namespace

TEST(CheckResultCap, OneCallRecordsOneResult) {
    // The harness control: the arithmetic below assumes one result per call.
    GovernanceEngine e;
    ASSERT_TRUE(e.loadFromString(R"({"version": "3.0", "mode": "enforce",
        "code_quality": {"encoding": {"level": "hard"}}})"));
    ASSERT_EQ(e.checkEncoding("let x = 1", 1), "");
    auto r = e.getCheckResults();
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r.back().record_id, 1u);
    EXPECT_TRUE(r.back().passed);
}

TEST(CheckResultCap, SizeStaysWithinCapPlusSlack) {
    GovernanceEngine e;
    ASSERT_TRUE(e.loadFromString(R"({"version": "3.0", "mode": "enforce",
        "code_quality": {"encoding": {"level": "hard"}}})"));
    recordPassesUntil(e, kCap + kSlack);
    EXPECT_EQ(e.getCheckResults().size(), kCap + kSlack) << "no compaction before the slack is used";
    EXPECT_EQ(e.evictedCheckTally().total, 0u);
    e.checkEncoding("let x = 1", 1);   // one over: compaction
    EXPECT_EQ(e.getCheckResults().size(), kCap);
    EXPECT_EQ(e.evictedCheckTally().total, kSlack + 1);
    recordPassesUntil(e, 3 * kCap);
    EXPECT_LE(e.getCheckResults().size(), kCap + kSlack);
    EXPECT_GE(e.getCheckResults().size(), kCap);
}

TEST(CheckResultCap, RetainedSetIsTheNewest) {
    GovernanceEngine e;
    ASSERT_TRUE(e.loadFromString(R"({"version": "3.0", "mode": "enforce",
        "code_quality": {"encoding": {"level": "hard"}}})"));
    // Two compaction points: right after each, the per-record policy would
    // hold exactly the newest kCap results, in order.
    for (uint64_t point : {static_cast<uint64_t>(kCap + kSlack + 1),
                           static_cast<uint64_t>(kCap + 2 * kSlack + 2)}) {
        recordPassesUntil(e, point);
        auto r = e.getCheckResults();
        ASSERT_EQ(r.size(), kCap) << "at record " << point;
        for (size_t i = 0; i < r.size(); ++i) {
            ASSERT_EQ(r[i].record_id, point - kCap + 1 + i) << "at record " << point << ", index " << i;
        }
        EXPECT_EQ(e.evictedCheckTally().total, point - kCap);
    }
}

TEST(CheckResultCap, EvictedResultsStillCount) {
    GovernanceEngine e;
    ASSERT_TRUE(e.loadFromString(R"({"version": "3.0", "mode": "enforce",
        "code_quality": {"encoding": {"level": "advisory"}},
        "quality_gate": {"enabled": true, "conditions": [
            {"metric": "advisory_violations", "operator": ">", "threshold": 0}]}})"));
    const std::string bad("a\0b", 3);
    e.checkEncoding(bad, 1);   // advisory: warns, returns "" and records a failed result
    {
        auto r = e.getCheckResults();
        ASSERT_EQ(r.size(), 1u);
        ASSERT_FALSE(r[0].passed) << "control: the null byte is recorded as an advisory";
    }
    EXPECT_NE(e.evaluateQualityGate(), "") << "control: below the cap the gate fails";
    recordPassesUntil(e, 2 * kCap);
    // The advisory is the oldest result, so it is evicted ...
    for (const auto& r : e.getCheckResults()) ASSERT_TRUE(r.passed);
    EXPECT_EQ(e.evictedCheckTally().advisory, 1u);
    // ... and still counted.
    EXPECT_NE(e.evaluateQualityGate(), "") << "an evicted advisory still fails the gate";
    const std::string line = e.formatSummaryOneLine();
    EXPECT_NE(line.find("1 warned"), std::string::npos) << line;
    EXPECT_NE(line.find(std::to_string(2 * kCap - 1) + " passed"), std::string::npos) << line;
    EXPECT_FALSE(e.wasBlocked()) << "an advisory is not a block";
}

TEST(CheckResultCap, AnEvictedRefusalStillCounts) {
    GovernanceEngine e;
    ASSERT_TRUE(e.loadFromString(R"({"version": "3.0", "mode": "enforce",
        "code_quality": {"encoding": {"level": "hard"}}})"));
    const std::string bad("a\0b", 3);
    bool threw = false;
    try { e.checkEncoding(bad, 1); } catch (const std::exception&) { threw = true; }
    ASSERT_TRUE(threw) << "control: at hard the null byte is a refusal";
    ASSERT_TRUE(e.wasBlocked()) << "control: below the cap it is counted";
    recordPassesUntil(e, 2 * kCap);
    for (const auto& r : e.getCheckResults()) ASSERT_TRUE(r.passed);   // evicted
    EXPECT_TRUE(e.evictedCheckTally().blocked);
    EXPECT_TRUE(e.wasBlocked()) << "an evicted refusal still makes the run blocked";
}

TEST(CheckResultCap, TelemetryWritesEachResultOnce) {
    const std::string path = tempPath("naab_cap_telemetry");
    std::remove(path.c_str());
    GovernanceEngine e;
    ASSERT_TRUE(e.loadFromString(
        R"({"version": "3.0", "mode": "enforce",
            "code_quality": {"encoding": {"level": "hard"}},
            "telemetry": {"enabled": true, "output_file": ")" + path + R"("}})"));
    // Two report writes with a compaction between them, and no more than kCap
    // results between them, so every one is still in memory at the second:
    // it must write every result recorded since the first, by number. By
    // position (the old per-record cap holding exactly kCap) it wrote only
    // positions first.. of the list, the newest kCap - first results, and
    // skipped the kCap - first - 1 before them.
    const uint64_t first = kCap / 2;
    const uint64_t second = first + kCap - 1;   // past kCap + kSlack: a compaction
    recordPassesUntil(e, first);
    e.writeReports();
    recordPassesUntil(e, second);
    e.writeReports();

    ASSERT_TRUE(std::ifstream(path).good()) << "telemetry written to " << path;
    const TelemetryCounts t = readTelemetry(path);
    const size_t check_rows = t.check_rows;
    const uint64_t not_written = t.not_written;
    std::remove(path.c_str());
    // Every result recorded is written once, or counted as not written.
    EXPECT_EQ(check_rows + not_written, second)
        << "rows=" << check_rows << " not_written=" << not_written;
    // None left memory unwritten: the compaction keeps at least the newest
    // kCap, and the second dump needs only the newest kCap - 1.
    EXPECT_EQ(not_written, 0u);
    EXPECT_EQ(check_rows, second);
}

TEST(CheckResultCap, TelemetryCountsWhatLeftMemoryUnwritten) {
    const std::string path = tempPath("naab_cap_unwritten");
    std::remove(path.c_str());
    GovernanceEngine e;
    ASSERT_TRUE(e.loadFromString(
        R"({"version": "3.0", "mode": "enforce",
            "code_quality": {"encoding": {"level": "hard"}},
            "telemetry": {"enabled": true, "output_file": ")" + path + R"("}})"));
    const uint64_t total = 3 * kCap;
    recordPassesUntil(e, total);
    const size_t held = e.getCheckResults().size();
    e.writeReports();
    const TelemetryCounts t = readTelemetry(path);
    const size_t check_rows = t.check_rows;
    const uint64_t not_written = t.not_written;
    std::remove(path.c_str());
    EXPECT_EQ(check_rows, held);
    EXPECT_EQ(not_written, total - held) << "the evicted results are counted, not silent";
}
