// Safe Time Unit Tests
// Tests time/counter wraparound detection and safe arithmetic

#include <gtest/gtest.h>
#include "naab/safe_time.h"
#include <limits>
#include <chrono>

using namespace naab::time;
using namespace std::chrono;

// ============================================================================
// Safe Time Addition Tests
// ============================================================================

TEST(SafeTimeTest, SafeTimeAdd_Normal) {
    int64_t timestamp = 1000000;
    int64_t delta = 500000;

    int64_t result = safeTimeAdd(timestamp, delta);
    EXPECT_EQ(result, 1500000);
}

TEST(SafeTimeTest, SafeTimeAdd_ZeroDelta) {
    int64_t timestamp = 1000000;

    int64_t result = safeTimeAdd(timestamp, 0);
    EXPECT_EQ(result, 1000000);
}

TEST(SafeTimeTest, SafeTimeAdd_NegativeDelta) {
    int64_t timestamp = 1000000;
    int64_t delta = -500000;

    int64_t result = safeTimeAdd(timestamp, delta);
    EXPECT_EQ(result, 500000);
}

TEST(SafeTimeTest, SafeTimeAdd_Overflow) {
    int64_t timestamp = std::numeric_limits<int64_t>::max();
    int64_t delta = 1;

    EXPECT_THROW(safeTimeAdd(timestamp, delta), TimeWraparoundException);
}

TEST(SafeTimeTest, SafeTimeAdd_LargePositiveValues) {
    int64_t timestamp = std::numeric_limits<int64_t>::max() - 1000;
    int64_t delta = 500;

    int64_t result = safeTimeAdd(timestamp, delta);
    EXPECT_EQ(result, std::numeric_limits<int64_t>::max() - 500);
}

TEST(SafeTimeTest, SafeTimeAdd_OverflowWithMaxAndOne) {
    int64_t timestamp = std::numeric_limits<int64_t>::max();
    int64_t delta = std::numeric_limits<int64_t>::max();

    EXPECT_THROW(safeTimeAdd(timestamp, delta), TimeWraparoundException);
}

// ============================================================================
// Safe Time Subtraction Tests
// ============================================================================

TEST(SafeTimeTest, SafeTimeSub_Normal) {
    int64_t timestamp = 1000000;
    int64_t delta = 500000;

    int64_t result = safeTimeSub(timestamp, delta);
    EXPECT_EQ(result, 500000);
}

TEST(SafeTimeTest, SafeTimeSub_ZeroDelta) {
    int64_t timestamp = 1000000;

    int64_t result = safeTimeSub(timestamp, 0);
    EXPECT_EQ(result, 1000000);
}

TEST(SafeTimeTest, SafeTimeSub_Underflow) {
    int64_t timestamp = std::numeric_limits<int64_t>::min();
    int64_t delta = 1;

    EXPECT_THROW(safeTimeSub(timestamp, delta), TimeWraparoundException);
}

TEST(SafeTimeTest, SafeTimeSub_NegativeResult) {
    int64_t timestamp = 100;
    int64_t delta = 200;

    int64_t result = safeTimeSub(timestamp, delta);
    EXPECT_EQ(result, -100);
}

// ============================================================================
// Safe Time Multiplication Tests
// ============================================================================

TEST(SafeTimeTest, SafeTimeMul_Normal) {
    int64_t time = 1000;
    int64_t multiplier = 5;

    int64_t result = safeTimeMul(time, multiplier);
    EXPECT_EQ(result, 5000);
}

TEST(SafeTimeTest, SafeTimeMul_Zero) {
    int64_t time = 1000;
    int64_t multiplier = 0;

    int64_t result = safeTimeMul(time, multiplier);
    EXPECT_EQ(result, 0);
}

TEST(SafeTimeTest, SafeTimeMul_Overflow) {
    int64_t time = std::numeric_limits<int64_t>::max();
    int64_t multiplier = 2;

    EXPECT_THROW(safeTimeMul(time, multiplier), TimeWraparoundException);
}

TEST(SafeTimeTest, SafeTimeMul_LargeValues) {
    // Use values that will definitely overflow int64_t
    int64_t time = std::numeric_limits<int64_t>::max() / 2;
    int64_t multiplier = 3;  // This will overflow

    EXPECT_THROW(safeTimeMul(time, multiplier), TimeWraparoundException);
}

// ============================================================================
// Counter Increment Tests
// ============================================================================

TEST(SafeTimeTest, SafeCounterIncrement_Normal) {
    uint64_t counter = 0;

    counter = safeCounterIncrement(counter);
    EXPECT_EQ(counter, 1);

    counter = safeCounterIncrement(counter, 10);
    EXPECT_EQ(counter, 11);
}

TEST(SafeTimeTest, SafeCounterIncrement_LargeIncrement) {
    uint64_t counter = 100;

    counter = safeCounterIncrement(counter, 1000000);
    EXPECT_EQ(counter, 1000100);
}

TEST(SafeTimeTest, SafeCounterIncrement_NearMax) {
    uint64_t counter = UINT64_MAX - 100;

    counter = safeCounterIncrement(counter, 50);
    EXPECT_EQ(counter, UINT64_MAX - 50);
}

TEST(SafeTimeTest, SafeCounterIncrement_Overflow) {
    uint64_t counter = UINT64_MAX;

    EXPECT_THROW(safeCounterIncrement(counter), CounterOverflowException);
}

TEST(SafeTimeTest, SafeCounterIncrement_OverflowWithLargeIncrement) {
    uint64_t counter = UINT64_MAX - 5;

    EXPECT_THROW(safeCounterIncrement(counter, 10), CounterOverflowException);
}

TEST(SafeTimeTest, SafeCounterIncrement_ZeroIncrement) {
    uint64_t counter = 100;

    counter = safeCounterIncrement(counter, 0);
    EXPECT_EQ(counter, 100);
}

// ============================================================================
// Counter Near Overflow Tests
// ============================================================================

TEST(SafeTimeTest, IsCounterNearOverflow_NotNear) {
    uint64_t counter = 1000;
    EXPECT_FALSE(isCounterNearOverflow(counter));
}

TEST(SafeTimeTest, IsCounterNearOverflow_Near90Percent) {
    // Use integer arithmetic to avoid float conversion warning
    uint64_t counter = UINT64_MAX - (UINT64_MAX / 10);  // ~90%
    EXPECT_TRUE(isCounterNearOverflow(counter));
}

TEST(SafeTimeTest, IsCounterNearOverflow_AtMax) {
    uint64_t counter = UINT64_MAX;
    EXPECT_TRUE(isCounterNearOverflow(counter));
}

TEST(SafeTimeTest, IsCounterNearOverflow_CustomThreshold) {
    // Use integer arithmetic to avoid float conversion warning
    uint64_t counter = UINT64_MAX - (UINT64_MAX / 6);  // ~85%

    // Should not be near with 90% threshold
    EXPECT_FALSE(isCounterNearOverflow(counter, 0.9));

    // Should be near with 80% threshold
    EXPECT_TRUE(isCounterNearOverflow(counter, 0.8));
}

TEST(SafeTimeTest, IsCounterNearOverflow_ExactThreshold) {
    // Use integer arithmetic to avoid float conversion warning
    uint64_t counter = UINT64_MAX - (UINT64_MAX / 10);  // 90%
    EXPECT_TRUE(isCounterNearOverflow(counter, 0.9));
}

// ============================================================================
// Counter Difference Tests
// ============================================================================

TEST(SafeTimeTest, SafeCounterDiff_Normal) {
    uint64_t newer = 1000;
    uint64_t older = 500;

    uint64_t diff = safeCounterDiff(newer, older);
    EXPECT_EQ(diff, 500);
}

TEST(SafeTimeTest, SafeCounterDiff_Equal) {
    uint64_t newer = 1000;
    uint64_t older = 1000;

    uint64_t diff = safeCounterDiff(newer, older);
    EXPECT_EQ(diff, 0);
}

TEST(SafeTimeTest, SafeCounterDiff_Wraparound) {
    uint64_t newer = 100;  // Wrapped around
    uint64_t older = UINT64_MAX - 100;

    uint64_t diff = safeCounterDiff(newer, older);
    EXPECT_EQ(diff, 201);  // (UINT64_MAX - older) + newer + 1
}

TEST(SafeTimeTest, SafeCounterDiff_LargeGap) {
    uint64_t newer = UINT64_MAX;
    uint64_t older = 0;

    uint64_t diff = safeCounterDiff(newer, older);
    EXPECT_EQ(diff, UINT64_MAX);
}

// ============================================================================
// Chrono Integration Tests
// ============================================================================

TEST(SafeTimeTest, SafeDurationAdd_Milliseconds) {
    auto d1 = milliseconds(1000);
    auto d2 = milliseconds(500);

    auto result = safeDurationAdd(d1, d2);
    EXPECT_EQ(result.count(), 1500);
}

TEST(SafeTimeTest, SafeDurationAdd_Seconds) {
    auto d1 = seconds(60);
    auto d2 = seconds(30);

    auto result = safeDurationAdd(d1, d2);
    EXPECT_EQ(result.count(), 90);
}

TEST(SafeTimeTest, SafeDurationAdd_Overflow) {
    auto d1 = milliseconds(std::numeric_limits<int64_t>::max());
    auto d2 = milliseconds(1);

    EXPECT_THROW(safeDurationAdd(d1, d2), TimeWraparoundException);
}

TEST(SafeTimeTest, SafeDeadline_Normal) {
    using Clock = std::chrono::system_clock;
    using Duration = milliseconds;

    auto now = time_point_cast<Duration>(Clock::now());
    auto timeout = Duration(30000);  // 30 seconds in ms

    auto deadline = safeDeadline(now, timeout);
    EXPECT_GT(deadline.time_since_epoch().count(), now.time_since_epoch().count());
}

TEST(SafeTimeTest, SafeDeadline_Overflow) {
    using Duration = milliseconds;
    using TimePoint = std::chrono::time_point<std::chrono::system_clock, Duration>;

    auto now = TimePoint(Duration(std::numeric_limits<int64_t>::max()));
    auto timeout = Duration(1);

    EXPECT_THROW(safeDeadline(now, timeout), TimeWraparoundException);
}

// ============================================================================
// Timestamp Validation Tests
// ============================================================================

TEST(SafeTimeTest, ValidateTimestamp_Valid) {
    int64_t timestamp = 1609459200;  // 2021-01-01

    EXPECT_NO_THROW(validateTimestamp(timestamp));
}

TEST(SafeTimeTest, ValidateTimestamp_TooEarly) {
    int64_t timestamp = -1;

    EXPECT_THROW(validateTimestamp(timestamp), std::invalid_argument);
}

TEST(SafeTimeTest, ValidateTimestamp_TooLate) {
    int64_t timestamp = 5000000000;  // Beyond 2100

    EXPECT_THROW(validateTimestamp(timestamp), std::invalid_argument);
}

TEST(SafeTimeTest, ValidateTimestamp_CustomRange) {
    int64_t timestamp = 1000;

    EXPECT_NO_THROW(validateTimestamp(timestamp, 500, 2000));
    EXPECT_THROW(validateTimestamp(timestamp, 1500, 2000), std::invalid_argument);
}

TEST(SafeTimeTest, ValidateTimestamp_AtBoundaries) {
    int64_t min_valid = 0;
    int64_t max_valid = 4102444800;  // 2100-01-01

    EXPECT_NO_THROW(validateTimestamp(min_valid));
    EXPECT_NO_THROW(validateTimestamp(max_valid));
}

// ============================================================================
// Time Monotonicity Tests
// ============================================================================

TEST(SafeTimeTest, IsTimeGoingBackwards_Normal) {
    int64_t newer = 1000;
    int64_t older = 500;

    EXPECT_FALSE(isTimeGoingBackwards(newer, older));
}

TEST(SafeTimeTest, IsTimeGoingBackwards_Backwards) {
    int64_t newer = 500;
    int64_t older = 1000;

    EXPECT_TRUE(isTimeGoingBackwards(newer, older));
}

TEST(SafeTimeTest, IsTimeGoingBackwards_Equal) {
    int64_t newer = 1000;
    int64_t older = 1000;

    EXPECT_FALSE(isTimeGoingBackwards(newer, older));
}

// ============================================================================
// Counter Guard Tests
// ============================================================================

TEST(SafeTimeTest, CounterGuard_NormalIncrement) {
    uint64_t counter = 100;

    {
        CounterGuard guard(counter);
        counter = safeCounterIncrement(counter, 10);
        EXPECT_EQ(counter, 110);
    }  // Guard destructor runs - should not warn
}

TEST(SafeTimeTest, CounterGuard_NoChange) {
    uint64_t counter = 100;

    {
        CounterGuard guard(counter);
        // Counter unchanged
    }  // Guard destructor runs

    EXPECT_EQ(counter, 100);
}

TEST(SafeTimeTest, CounterGuard_WraparoundDetection) {
    uint64_t counter = 1000;

    {
        CounterGuard guard(counter);
        // Simulate wraparound (this shouldn't happen in real code)
        counter = 500;
    }  // Guard destructor runs - should print warning to stderr

    EXPECT_EQ(counter, 500);
}

TEST(SafeTimeTest, CounterGuard_NearOverflowWarning) {
    // Use integer arithmetic to avoid float conversion warning
    uint64_t counter = UINT64_MAX - (UINT64_MAX / 20);  // ~95%

    {
        CounterGuard guard(counter);
        // Counter is near overflow - guard should warn on destruction
    }
}

// ============================================================================
// Exception Message Tests
// ============================================================================

TEST(SafeTimeTest, TimeWraparoundException_Message) {
    try {
        safeTimeAdd(std::numeric_limits<int64_t>::max(), 1);
        FAIL() << "Expected TimeWraparoundException";
    } catch (const TimeWraparoundException& e) {
        std::string msg(e.what());
        EXPECT_TRUE(msg.find("wraparound") != std::string::npos ||
                   msg.find("overflow") != std::string::npos);
    }
}

TEST(SafeTimeTest, CounterOverflowException_Message) {
    try {
        safeCounterIncrement(UINT64_MAX);
        FAIL() << "Expected CounterOverflowException";
    } catch (const CounterOverflowException& e) {
        std::string msg(e.what());
        EXPECT_TRUE(msg.find("overflow") != std::string::npos);
        EXPECT_TRUE(msg.find("UINT64_MAX") != std::string::npos);
    }
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST(SafeTimeTest, EdgeCase_MaxMinusOne) {
    uint64_t counter = UINT64_MAX - 1;

    // Should succeed
    counter = safeCounterIncrement(counter, 1);
    EXPECT_EQ(counter, UINT64_MAX);

    // Should fail
    EXPECT_THROW(safeCounterIncrement(counter, 1), CounterOverflowException);
}

TEST(SafeTimeTest, EdgeCase_ZeroCounter) {
    uint64_t counter = 0;

    counter = safeCounterIncrement(counter);
    EXPECT_EQ(counter, 1);
}

TEST(SafeTimeTest, EdgeCase_NegativeTimeValues) {
    int64_t timestamp = -1000;
    int64_t delta = -500;

    int64_t result = safeTimeAdd(timestamp, delta);
    EXPECT_EQ(result, -1500);
}

TEST(SafeTimeTest, EdgeCase_MinInt64Plus1) {
    int64_t timestamp = std::numeric_limits<int64_t>::min() + 1;
    int64_t delta = 1;

    int64_t result = safeTimeSub(timestamp, delta);
    EXPECT_EQ(result, std::numeric_limits<int64_t>::min());
}
