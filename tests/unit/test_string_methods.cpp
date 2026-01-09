// Test String Methods - Phase 2.1
// Comprehensive tests for all 12 string functions (5 tests each = 60 total)

#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <iostream>
#include <cassert>

using namespace naab;

#define ASSERT_EQ(actual, expected, test_name) \
    do { \
        if ((actual) != (expected)) { \
            fmt::print("  ✗ FAIL: {} (expected: {}, got: {})\n", test_name, expected, actual); \
            return false; \
        } \
        tests_passed++; \
    } while (0)

#define ASSERT_TRUE(condition, test_name) \
    do { \
        if (!(condition)) { \
            fmt::print("  ✗ FAIL: {} (expected true)\n", test_name); \
            return false; \
        } \
        tests_passed++; \
    } while (0)

#define ASSERT_FALSE(condition, test_name) \
    do { \
        if (condition) { \
            fmt::print("  ✗ FAIL: {} (expected false)\n", test_name); \
            return false; \
        } \
        tests_passed++; \
    } while (0)

static int tests_passed = 0;

// Helper to create string value
auto makeStr(const std::string& s) {
    return std::make_shared<interpreter::Value>(s);
}

// Helper to create int value
auto makeInt(int i) {
    return std::make_shared<interpreter::Value>(i);
}

// Helper to get string array from result
std::vector<std::string> getStringArray(std::shared_ptr<interpreter::Value> val) {
    std::vector<std::string> result;
    auto vec = std::get<std::vector<std::shared_ptr<interpreter::Value>>>(val->data);
    for (const auto& item : vec) {
        result.push_back(std::get<std::string>(item->data));
    }
    return result;
}

// ============================================================================
// Test 1: string.length() - 5 tests
// ============================================================================
bool test_string_length(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 1: string.length() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 1.1: Basic string
    {
        auto result = module->call("length", {makeStr("hello")});
        ASSERT_EQ(result->toInt(), 5, "length('hello')");
    }

    // Test 1.2: Empty string
    {
        auto result = module->call("length", {makeStr("")});
        ASSERT_EQ(result->toInt(), 0, "length('')");
    }

    // Test 1.3: Single character
    {
        auto result = module->call("length", {makeStr("a")});
        ASSERT_EQ(result->toInt(), 1, "length('a')");
    }

    // Test 1.4: String with spaces
    {
        auto result = module->call("length", {makeStr("hello world")});
        ASSERT_EQ(result->toInt(), 11, "length('hello world')");
    }

    // Test 1.5: Unicode/special characters
    {
        auto result = module->call("length", {makeStr("Hello123!@#")});
        ASSERT_EQ(result->toInt(), 11, "length('Hello123!@#')");
    }

    fmt::print("  ✓ All length() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 2: string.upper() - 5 tests
// ============================================================================
bool test_string_upper(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 2: string.upper() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 2.1: Basic lowercase
    {
        auto result = module->call("upper", {makeStr("hello")});
        ASSERT_EQ(result->toString(), "HELLO", "upper('hello')");
    }

    // Test 2.2: Already uppercase
    {
        auto result = module->call("upper", {makeStr("HELLO")});
        ASSERT_EQ(result->toString(), "HELLO", "upper('HELLO')");
    }

    // Test 2.3: Mixed case
    {
        auto result = module->call("upper", {makeStr("HeLLo")});
        ASSERT_EQ(result->toString(), "HELLO", "upper('HeLLo')");
    }

    // Test 2.4: Empty string
    {
        auto result = module->call("upper", {makeStr("")});
        ASSERT_EQ(result->toString(), "", "upper('')");
    }

    // Test 2.5: With numbers and symbols
    {
        auto result = module->call("upper", {makeStr("hello123!@#")});
        ASSERT_EQ(result->toString(), "HELLO123!@#", "upper('hello123!@#')");
    }

    fmt::print("  ✓ All upper() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 3: string.lower() - 5 tests
// ============================================================================
bool test_string_lower(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 3: string.lower() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 3.1: Basic uppercase
    {
        auto result = module->call("lower", {makeStr("HELLO")});
        ASSERT_EQ(result->toString(), "hello", "lower('HELLO')");
    }

    // Test 3.2: Already lowercase
    {
        auto result = module->call("lower", {makeStr("hello")});
        ASSERT_EQ(result->toString(), "hello", "lower('hello')");
    }

    // Test 3.3: Mixed case
    {
        auto result = module->call("lower", {makeStr("HeLLo")});
        ASSERT_EQ(result->toString(), "hello", "lower('HeLLo')");
    }

    // Test 3.4: Empty string
    {
        auto result = module->call("lower", {makeStr("")});
        ASSERT_EQ(result->toString(), "", "lower('')");
    }

    // Test 3.5: With numbers and symbols
    {
        auto result = module->call("lower", {makeStr("HELLO123!@#")});
        ASSERT_EQ(result->toString(), "hello123!@#", "lower('HELLO123!@#')");
    }

    fmt::print("  ✓ All lower() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 4: string.trim() - 5 tests
// ============================================================================
bool test_string_trim(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 4: string.trim() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 4.1: Leading and trailing spaces
    {
        auto result = module->call("trim", {makeStr("  hello  ")});
        ASSERT_EQ(result->toString(), "hello", "trim('  hello  ')");
    }

    // Test 4.2: Only leading spaces
    {
        auto result = module->call("trim", {makeStr("  hello")});
        ASSERT_EQ(result->toString(), "hello", "trim('  hello')");
    }

    // Test 4.3: Only trailing spaces
    {
        auto result = module->call("trim", {makeStr("hello  ")});
        ASSERT_EQ(result->toString(), "hello", "trim('hello  ')");
    }

    // Test 4.4: No spaces to trim
    {
        auto result = module->call("trim", {makeStr("hello")});
        ASSERT_EQ(result->toString(), "hello", "trim('hello')");
    }

    // Test 4.5: Trim tabs and newlines
    {
        auto result = module->call("trim", {makeStr("\t\nhello\r\n")});
        ASSERT_EQ(result->toString(), "hello", "trim('\\t\\nhello\\r\\n')");
    }

    fmt::print("  ✓ All trim() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 5: string.substring() - 5 tests
// ============================================================================
bool test_string_substring(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 5: string.substring() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 5.1: Basic substring
    {
        auto result = module->call("substring", {makeStr("hello world"), makeInt(0), makeInt(5)});
        ASSERT_EQ(result->toString(), "hello", "substring('hello world', 0, 5)");
    }

    // Test 5.2: Middle substring
    {
        auto result = module->call("substring", {makeStr("hello world"), makeInt(6), makeInt(11)});
        ASSERT_EQ(result->toString(), "world", "substring('hello world', 6, 11)");
    }

    // Test 5.3: Full string
    {
        auto result = module->call("substring", {makeStr("hello"), makeInt(0), makeInt(5)});
        ASSERT_EQ(result->toString(), "hello", "substring('hello', 0, 5)");
    }

    // Test 5.4: Empty substring (start == end)
    {
        auto result = module->call("substring", {makeStr("hello"), makeInt(2), makeInt(2)});
        ASSERT_EQ(result->toString(), "", "substring('hello', 2, 2)");
    }

    // Test 5.5: Out of bounds (should be clamped)
    {
        auto result = module->call("substring", {makeStr("hello"), makeInt(0), makeInt(100)});
        ASSERT_EQ(result->toString(), "hello", "substring('hello', 0, 100)");
    }

    fmt::print("  ✓ All substring() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 6: string.split() - 5 tests
// ============================================================================
bool test_string_split(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 6: string.split() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 6.1: Split by comma
    {
        auto result = module->call("split", {makeStr("a,b,c"), makeStr(",")});
        auto parts = getStringArray(result);
        ASSERT_EQ(parts.size(), 3, "split('a,b,c', ',').size");
        ASSERT_EQ(parts[0], "a", "split('a,b,c', ',')[0]");
        ASSERT_EQ(parts[1], "b", "split('a,b,c', ',')[1]");
        ASSERT_EQ(parts[2], "c", "split('a,b,c', ',')[2]");
    }

    // Test 6.2: Split by space
    {
        auto result = module->call("split", {makeStr("hello world"), makeStr(" ")});
        auto parts = getStringArray(result);
        ASSERT_EQ(parts.size(), 2, "split('hello world', ' ').size");
        ASSERT_EQ(parts[0], "hello", "split('hello world', ' ')[0]");
        ASSERT_EQ(parts[1], "world", "split('hello world', ' ')[1]");
    }

    // Test 6.3: Split with no delimiter (chars)
    {
        auto result = module->call("split", {makeStr("abc"), makeStr("")});
        auto parts = getStringArray(result);
        ASSERT_EQ(parts.size(), 3, "split('abc', '').size");
        ASSERT_EQ(parts[0], "a", "split('abc', '')[0]");
    }

    // Test 6.4: No split needed
    {
        auto result = module->call("split", {makeStr("hello"), makeStr(",")});
        auto parts = getStringArray(result);
        ASSERT_EQ(parts.size(), 1, "split('hello', ',').size");
        ASSERT_EQ(parts[0], "hello", "split('hello', ',')[0]");
    }

    // Test 6.5: Empty string
    {
        auto result = module->call("split", {makeStr(""), makeStr(",")});
        auto parts = getStringArray(result);
        ASSERT_EQ(parts.size(), 1, "split('', ',').size");
    }

    fmt::print("  ✓ All split() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 7: string.join() - 5 tests
// ============================================================================
bool test_string_join(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 7: string.join() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Helper to create string array
    auto makeStrArray = [](std::vector<std::string> strs) {
        std::vector<std::shared_ptr<interpreter::Value>> vec;
        for (const auto& s : strs) {
            vec.push_back(makeStr(s));
        }
        return std::make_shared<interpreter::Value>(vec);
    };

    // Test 7.1: Join with comma
    {
        auto arr = makeStrArray({"a", "b", "c"});
        auto result = module->call("join", {arr, makeStr(",")});
        ASSERT_EQ(result->toString(), "a,b,c", "join(['a','b','c'], ',')");
    }

    // Test 7.2: Join with space
    {
        auto arr = makeStrArray({"hello", "world"});
        auto result = module->call("join", {arr, makeStr(" ")});
        ASSERT_EQ(result->toString(), "hello world", "join(['hello','world'], ' ')");
    }

    // Test 7.3: Join with empty delimiter
    {
        auto arr = makeStrArray({"a", "b", "c"});
        auto result = module->call("join", {arr, makeStr("")});
        ASSERT_EQ(result->toString(), "abc", "join(['a','b','c'], '')");
    }

    // Test 7.4: Single element
    {
        auto arr = makeStrArray({"hello"});
        auto result = module->call("join", {arr, makeStr(",")});
        ASSERT_EQ(result->toString(), "hello", "join(['hello'], ',')");
    }

    // Test 7.5: Empty array
    {
        auto arr = makeStrArray({});
        auto result = module->call("join", {arr, makeStr(",")});
        ASSERT_EQ(result->toString(), "", "join([], ',')");
    }

    fmt::print("  ✓ All join() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 8: string.replace() - 5 tests
// ============================================================================
bool test_string_replace(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 8: string.replace() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 8.1: Basic replace
    {
        auto result = module->call("replace", {makeStr("hello world"), makeStr("world"), makeStr("there")});
        ASSERT_EQ(result->toString(), "hello there", "replace('hello world', 'world', 'there')");
    }

    // Test 8.2: Replace multiple occurrences
    {
        auto result = module->call("replace", {makeStr("aa bb aa"), makeStr("aa"), makeStr("cc")});
        ASSERT_EQ(result->toString(), "cc bb cc", "replace('aa bb aa', 'aa', 'cc')");
    }

    // Test 8.3: Replace with empty string
    {
        auto result = module->call("replace", {makeStr("hello"), makeStr("l"), makeStr("")});
        ASSERT_EQ(result->toString(), "heo", "replace('hello', 'l', '')");
    }

    // Test 8.4: No match
    {
        auto result = module->call("replace", {makeStr("hello"), makeStr("x"), makeStr("y")});
        ASSERT_EQ(result->toString(), "hello", "replace('hello', 'x', 'y')");
    }

    // Test 8.5: Replace empty old string (no-op)
    {
        auto result = module->call("replace", {makeStr("hello"), makeStr(""), makeStr("x")});
        ASSERT_EQ(result->toString(), "hello", "replace('hello', '', 'x')");
    }

    fmt::print("  ✓ All replace() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 9: string.contains() - 5 tests
// ============================================================================
bool test_string_contains(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 9: string.contains() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 9.1: Contains substring
    {
        auto result = module->call("contains", {makeStr("hello world"), makeStr("world")});
        ASSERT_TRUE(result->toBool(), "contains('hello world', 'world')");
    }

    // Test 9.2: Does not contain
    {
        auto result = module->call("contains", {makeStr("hello"), makeStr("world")});
        ASSERT_FALSE(result->toBool(), "contains('hello', 'world')");
    }

    // Test 9.3: Contains at start
    {
        auto result = module->call("contains", {makeStr("hello"), makeStr("hel")});
        ASSERT_TRUE(result->toBool(), "contains('hello', 'hel')");
    }

    // Test 9.4: Contains at end
    {
        auto result = module->call("contains", {makeStr("hello"), makeStr("llo")});
        ASSERT_TRUE(result->toBool(), "contains('hello', 'llo')");
    }

    // Test 9.5: Empty substring (always contains)
    {
        auto result = module->call("contains", {makeStr("hello"), makeStr("")});
        ASSERT_TRUE(result->toBool(), "contains('hello', '')");
    }

    fmt::print("  ✓ All contains() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 10: string.starts_with() - 5 tests
// ============================================================================
bool test_string_starts_with(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 10: string.starts_with() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 10.1: Starts with prefix
    {
        auto result = module->call("starts_with", {makeStr("hello world"), makeStr("hello")});
        ASSERT_TRUE(result->toBool(), "starts_with('hello world', 'hello')");
    }

    // Test 10.2: Does not start with
    {
        auto result = module->call("starts_with", {makeStr("hello"), makeStr("world")});
        ASSERT_FALSE(result->toBool(), "starts_with('hello', 'world')");
    }

    // Test 10.3: Exact match
    {
        auto result = module->call("starts_with", {makeStr("hello"), makeStr("hello")});
        ASSERT_TRUE(result->toBool(), "starts_with('hello', 'hello')");
    }

    // Test 10.4: Empty prefix (always true)
    {
        auto result = module->call("starts_with", {makeStr("hello"), makeStr("")});
        ASSERT_TRUE(result->toBool(), "starts_with('hello', '')");
    }

    // Test 10.5: Prefix longer than string
    {
        auto result = module->call("starts_with", {makeStr("hi"), makeStr("hello")});
        ASSERT_FALSE(result->toBool(), "starts_with('hi', 'hello')");
    }

    fmt::print("  ✓ All starts_with() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 11: string.ends_with() - 5 tests
// ============================================================================
bool test_string_ends_with(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 11: string.ends_with() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 11.1: Ends with suffix
    {
        auto result = module->call("ends_with", {makeStr("hello world"), makeStr("world")});
        ASSERT_TRUE(result->toBool(), "ends_with('hello world', 'world')");
    }

    // Test 11.2: Does not end with
    {
        auto result = module->call("ends_with", {makeStr("hello"), makeStr("world")});
        ASSERT_FALSE(result->toBool(), "ends_with('hello', 'world')");
    }

    // Test 11.3: Exact match
    {
        auto result = module->call("ends_with", {makeStr("hello"), makeStr("hello")});
        ASSERT_TRUE(result->toBool(), "ends_with('hello', 'hello')");
    }

    // Test 11.4: Empty suffix (always true)
    {
        auto result = module->call("ends_with", {makeStr("hello"), makeStr("")});
        ASSERT_TRUE(result->toBool(), "ends_with('hello', '')");
    }

    // Test 11.5: Suffix longer than string
    {
        auto result = module->call("ends_with", {makeStr("hi"), makeStr("hello")});
        ASSERT_FALSE(result->toBool(), "ends_with('hi', 'hello')");
    }

    fmt::print("  ✓ All ends_with() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Test 12: string.concat() - 5 tests
// ============================================================================
bool test_string_concat(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 12: string.concat() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 12.1: Basic concatenation
    {
        auto result = module->call("concat", {makeStr("hello"), makeStr(" world")});
        ASSERT_EQ(result->toString(), "hello world", "concat('hello', ' world')");
    }

    // Test 12.2: Concatenate with empty string
    {
        auto result = module->call("concat", {makeStr("hello"), makeStr("")});
        ASSERT_EQ(result->toString(), "hello", "concat('hello', '')");
    }

    // Test 12.3: Both empty strings
    {
        auto result = module->call("concat", {makeStr(""), makeStr("")});
        ASSERT_EQ(result->toString(), "", "concat('', '')");
    }

    // Test 12.4: Numbers and symbols
    {
        auto result = module->call("concat", {makeStr("test"), makeStr("123")});
        ASSERT_EQ(result->toString(), "test123", "concat('test', '123')");
    }

    // Test 12.5: Multiple words
    {
        auto result = module->call("concat", {makeStr("foo bar"), makeStr(" baz")});
        ASSERT_EQ(result->toString(), "foo bar baz", "concat('foo bar', ' baz')");
    }

    fmt::print("  ✓ All concat() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    fmt::print("╔═══════════════════════════════════════════════════════════════╗\n");
    fmt::print("║  Phase 2.1: String Methods - Comprehensive Test Suite        ║\n");
    fmt::print("║  Testing all 12 functions with 5 tests each (60 total)       ║\n");
    fmt::print("╚═══════════════════════════════════════════════════════════════╝\n");

    try {
        stdlib::StdLib stdlib_instance;

        bool all_passed = true;
        all_passed &= test_string_length(stdlib_instance);
        all_passed &= test_string_upper(stdlib_instance);
        all_passed &= test_string_lower(stdlib_instance);
        all_passed &= test_string_trim(stdlib_instance);
        all_passed &= test_string_substring(stdlib_instance);
        all_passed &= test_string_split(stdlib_instance);
        all_passed &= test_string_join(stdlib_instance);
        all_passed &= test_string_replace(stdlib_instance);
        all_passed &= test_string_contains(stdlib_instance);
        all_passed &= test_string_starts_with(stdlib_instance);
        all_passed &= test_string_ends_with(stdlib_instance);
        all_passed &= test_string_concat(stdlib_instance);

        fmt::print("\n╔═══════════════════════════════════════════════════════════════╗\n");
        fmt::print("║                      Test Summary                             ║\n");
        fmt::print("╠═══════════════════════════════════════════════════════════════╣\n");
        fmt::print("║  Tests passed: {}/60                                          ║\n", tests_passed);

        if (all_passed && tests_passed == 60) {
            fmt::print("║  Status: ✓ ALL TESTS PASSED                                  ║\n");
            fmt::print("╚═══════════════════════════════════════════════════════════════╝\n");
            return 0;
        } else {
            fmt::print("║  Status: ✗ SOME TESTS FAILED                                 ║\n");
            fmt::print("╚═══════════════════════════════════════════════════════════════╝\n");
            return 1;
        }

    } catch (const std::exception& e) {
        fmt::print("\n[FATAL ERROR] Exception: {}\n", e.what());
        return 1;
    }
}
