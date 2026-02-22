#pragma once
// NAAb Safe Time Operations
// Phase 1: Path to 97% - Time/Counter Wraparound Detection
//
// Prevents time and counter wraparound vulnerabilities by:
// - Detecting wraparound in time calculations
// - Preventing counter overflow
// - Safe time arithmetic
// - Monotonic counter validation


#include "naab/safe_math.h"
#include <cstdint>
#include <chrono>
#include <stdexcept>
#include <fmt/core.h>

namespace naab {
namespace time {

// ============================================================================
// Time Wraparound Exceptions
// ============================================================================

class TimeWraparoundException : public std::runtime_error {
public:
    explicit TimeWraparoundException(const std::string& msg)
        : std::runtime_error(msg) {}
};

class CounterOverflowException : public std::runtime_error {
public:
    explicit CounterOverflowException(const std::string& msg)
        : std::runtime_error(msg) {}
};

// ============================================================================
// Safe Time Arithmetic
// ============================================================================

/**
 * Safe time addition with wraparound detection
 *
 * Prevents wraparound when adding time deltas
 * @param timestamp Base timestamp
 * @param delta Time delta to add
 * @return New timestamp
 * @throws TimeWraparoundException if wraparound would occur
 */
inline int64_t safeTimeAdd(int64_t timestamp, int64_t delta) {
    try {
        return math::safeAdd(timestamp, delta);
    } catch (const math::OverflowException& e) {
        throw TimeWraparoundException(
            fmt::format("Time wraparound: {} + {} would overflow", timestamp, delta)
        );
    }
}

/**
 * Safe time subtraction with wraparound detection
 *
 * @param timestamp Base timestamp
 * @param delta Time delta to subtract
 * @return New timestamp
 * @throws TimeWraparoundException if wraparound would occur
 */
inline int64_t safeTimeSub(int64_t timestamp, int64_t delta) {
    try {
        return math::safeSub(timestamp, delta);
    } catch (const math::UnderflowException& e) {
        throw TimeWraparoundException(
            fmt::format("Time wraparound: {} - {} would underflow", timestamp, delta)
        );
    }
}

/**
 * Safe time multiplication (for timeouts, delays)
 *
 * @param time Base time value
 * @param multiplier Multiplier
 * @return Scaled time
 * @throws TimeWraparoundException if overflow would occur
 */
inline int64_t safeTimeMul(int64_t time, int64_t multiplier) {
    try {
        return math::safeMul(time, multiplier);
    } catch (const math::OverflowException& e) {
        throw TimeWraparoundException(
            fmt::format("Time overflow: {} * {} exceeds range", time, multiplier)
        );
    }
}

// ============================================================================
// Counter Operations
// ============================================================================

/**
 * Safe counter increment with overflow detection
 *
 * Prevents counter wraparound in monotonic counters
 * @param counter Current counter value
 * @param increment Amount to increment
 * @return New counter value
 * @throws CounterOverflowException if counter would overflow
 */
inline uint64_t safeCounterIncrement(uint64_t counter, uint64_t increment = 1) {
    if (counter > UINT64_MAX - increment) {
        throw CounterOverflowException(
            fmt::format("Counter overflow: {} + {} exceeds UINT64_MAX", counter, increment)
        );
    }
    return counter + increment;
}

/**
 * Check if counter is approaching overflow
 *
 * Useful for early warning before overflow occurs
 * @param counter Current counter value
 * @param threshold Warning threshold (default: 90% of max)
 * @return true if counter is approaching overflow
 */
inline bool isCounterNearOverflow(uint64_t counter, double threshold = 0.9) {
    // Calculate threshold using integer arithmetic to avoid precision issues
    // For example: threshold 0.9 means counter > 90% of UINT64_MAX
    // We calculate: UINT64_MAX - (UINT64_MAX * (1.0 - threshold))
    // But to avoid float precision issues, we use: UINT64_MAX * threshold
    if (threshold >= 1.0) {
        return true;  // Always near overflow if threshold is 100%+
    }
    if (threshold <= 0.0) {
        return false;  // Never near overflow if threshold is 0%
    }
    // Use integer division to avoid float conversion warning
    uint64_t percentage = static_cast<uint64_t>(threshold * 100.0);
    uint64_t warning_level = UINT64_MAX / 100 * percentage;
    return counter >= warning_level;
}

/**
 * Safe counter difference calculation
 *
 * Handles wraparound in circular/wrapping counters
 * @param newer_value Newer counter value
 * @param older_value Older counter value
 * @return Difference, accounting for wraparound
 */
inline uint64_t safeCounterDiff(uint64_t newer_value, uint64_t older_value) {
    if (newer_value >= older_value) {
        return newer_value - older_value;
    } else {
        // Wraparound occurred
        return (UINT64_MAX - older_value) + newer_value + 1;
    }
}

// ============================================================================
// Chrono Integration
// ============================================================================

/**
 * Safe duration addition using std::chrono
 *
 * Type-safe time arithmetic using C++17 chrono
 */
template<typename Duration>
inline Duration safeDurationAdd(Duration d1, Duration d2) {
    try {
        auto count1 = d1.count();
        auto count2 = d2.count();
        auto result_count = math::safeAdd(count1, count2);
        return Duration(result_count);
    } catch (const math::OverflowException& e) {
        throw TimeWraparoundException(
            fmt::format("Duration overflow: adding {} + {} would overflow",
                       d1.count(), d2.count())
        );
    }
}

/**
 * Safe timeout calculation
 *
 * Prevents timeout overflow when calculating deadline
 * @param base_time Base time point
 * @param timeout Timeout duration
 * @return Deadline time point
 */
template<typename Clock, typename Duration>
inline std::chrono::time_point<Clock, Duration>
safeDeadline(std::chrono::time_point<Clock, Duration> base_time, Duration timeout) {
    try {
        auto base_count = base_time.time_since_epoch().count();
        auto timeout_count = timeout.count();
        auto deadline_count = math::safeAdd(base_count, timeout_count);
        return std::chrono::time_point<Clock, Duration>(Duration(deadline_count));
    } catch (const math::OverflowException& e) {
        throw TimeWraparoundException(
            fmt::format("Deadline overflow: base + timeout would overflow")
        );
    }
}

// ============================================================================
// Validation Helpers
// ============================================================================

/**
 * Validate timestamp is within reasonable range
 *
 * Prevents obviously invalid timestamps
 * @param timestamp Timestamp to validate
 * @param min_valid Minimum valid timestamp (e.g., Unix epoch of known past date)
 * @param max_valid Maximum valid timestamp (e.g., year 2100)
 * @throws std::invalid_argument if timestamp is out of range
 */
inline void validateTimestamp(int64_t timestamp,
                               int64_t min_valid = 0,
                               int64_t max_valid = 4102444800) { // 2100-01-01
    if (timestamp < min_valid || timestamp > max_valid) {
        throw std::invalid_argument(
            fmt::format("Invalid timestamp: {} not in range [{}, {}]",
                       timestamp, min_valid, max_valid)
        );
    }
}

/**
 * Check for time going backwards (monotonicity violation)
 *
 * Useful for detecting system clock adjustments or bugs
 * @param newer_time Should be newer timestamp
 * @param older_time Should be older timestamp
 * @return true if time went backwards
 */
inline bool isTimeGoingBackwards(int64_t newer_time, int64_t older_time) {
    return newer_time < older_time;
}

// ============================================================================
// RAII Guard for Counter Safety
// ============================================================================

/**
 * RAII guard for automatic counter overflow checking
 *
 * Usage:
 *   uint64_t counter = 0;
 *   {
 *       CounterGuard guard(counter);
 *       counter = safeCounterIncrement(counter);
 *       // Automatically validated on scope exit
 *   }
 */
class CounterGuard {
public:
    explicit CounterGuard(uint64_t& counter)
        : counter_(counter), initial_value_(counter) {}

    ~CounterGuard() {
        // Validate counter didn't wrap around during scope
        if (counter_ < initial_value_) {
            // Counter went backwards - likely wraparound
            fmt::print(stderr, "WARNING: Counter wraparound detected: {} -> {}\n",
                      initial_value_, counter_);
        }

        // Check if approaching overflow
        if (isCounterNearOverflow(counter_)) {
            fmt::print(stderr, "WARNING: Counter approaching overflow: {}\n", counter_);
        }
    }

    // Non-copyable, non-movable
    CounterGuard(const CounterGuard&) = delete;
    CounterGuard& operator=(const CounterGuard&) = delete;

private:
    uint64_t& counter_;
    uint64_t initial_value_;
};

} // namespace time
} // namespace naab

