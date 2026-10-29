// The string module's functions, called directly: 12 functions, about five
// cases each.
//
// Was tests/unit/test_string_methods.cpp, a standalone program in the default
// build target that stopped compiling at the NaabVal migration and that
// nothing built or ran (A58). Ported to NaabVal; each group is one test, and
// a failing case prints its own name (CHECK_*), as the program did.

#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <gtest/gtest.h>
#include <iostream>

using namespace naab;

#define CHECK_EQ(actual, expected, test_name) \
    do { \
        if ((actual) != (expected)) { \
            fmt::print("  ✗ FAIL: {} (expected: {}, got: {})\n", test_name, expected, actual); \
            return false; \
        } \
        tests_passed++; \
    } while (0)

#define CHECK_TRUE(condition, test_name) \
    do { \
        if (!(condition)) { \
            fmt::print("  ✗ FAIL: {} (expected true)\n", test_name); \
            return false; \
        } \
        tests_passed++; \
    } while (0)

#define CHECK_FALSE(condition, test_name) \
    do { \
        if (condition) { \
            fmt::print("  ✗ FAIL: {} (expected false)\n", test_name); \
            return false; \
        } \
        tests_passed++; \
    } while (0)

namespace {

static int tests_passed = 0;

// Helper to create string value
interpreter::NaabVal makeStr(const std::string& s) {
    return interpreter::NaabVal::makeString(s);
}

// Helper to create int value
interpreter::NaabVal makeInt(int i) {
    return interpreter::NaabVal::makeInt(i);
}

// Helper to get string array from result
std::vector<std::string> getStringArray(const interpreter::NaabVal& val) {
    std::vector<std::string> result;
    for (const auto& item : val.asListConst()) {
        result.push_back(item.asString());
    }
    return result;
}

// Module::call takes its arguments by non-const reference.
interpreter::NaabVal callFn(const std::shared_ptr<stdlib::Module>& module, const std::string& name,
                            std::vector<interpreter::NaabVal> args) {
    return module->call(name, args);
}

// ============================================================================
// Test 1: string.length() - 5 tests
// ============================================================================
bool test_string_length(stdlib::StdLib& stdlib_instance) {
    fmt::print("\n=== Test Group 1: string.length() ===\n");
    auto module = stdlib_instance.getModule("string");

    // Test 1.1: Basic string
    {
        auto result = callFn(module, "length", {makeStr("hello")});
        CHECK_EQ(result.toInt(), 5, "length('hello')");
    }

    // Test 1.2: Empty string
    {
        auto result = callFn(module, "length", {makeStr("")});
        CHECK_EQ(result.toInt(), 0, "length('')");
    }

    // Test 1.3: Single character
    {
        auto result = callFn(module, "length", {makeStr("a")});
        CHECK_EQ(result.toInt(), 1, "length('a')");
    }

    // Test 1.4: String with spaces
    {
        auto result = callFn(module, "length", {makeStr("hello world")});
        CHECK_EQ(result.toInt(), 11, "length('hello world')");
    }

    // Test 1.5: Unicode/special characters
    {
        auto result = callFn(module, "length", {makeStr("Hello123!@#")});
        CHECK_EQ(result.toInt(), 11, "length('Hello123!@#')");
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
        auto result = callFn(module, "upper", {makeStr("hello")});
        CHECK_EQ(result.toString(), "HELLO", "upper('hello')");
    }

    // Test 2.2: Already uppercase
    {
        auto result = callFn(module, "upper", {makeStr("HELLO")});
        CHECK_EQ(result.toString(), "HELLO", "upper('HELLO')");
    }

    // Test 2.3: Mixed case
    {
        auto result = callFn(module, "upper", {makeStr("HeLLo")});
        CHECK_EQ(result.toString(), "HELLO", "upper('HeLLo')");
    }

    // Test 2.4: Empty string
    {
        auto result = callFn(module, "upper", {makeStr("")});
        CHECK_EQ(result.toString(), "", "upper('')");
    }

    // Test 2.5: With numbers and symbols
    {
        auto result = callFn(module, "upper", {makeStr("hello123!@#")});
        CHECK_EQ(result.toString(), "HELLO123!@#", "upper('hello123!@#')");
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
        auto result = callFn(module, "lower", {makeStr("HELLO")});
        CHECK_EQ(result.toString(), "hello", "lower('HELLO')");
    }

    // Test 3.2: Already lowercase
    {
        auto result = callFn(module, "lower", {makeStr("hello")});
        CHECK_EQ(result.toString(), "hello", "lower('hello')");
    }

    // Test 3.3: Mixed case
    {
        auto result = callFn(module, "lower", {makeStr("HeLLo")});
        CHECK_EQ(result.toString(), "hello", "lower('HeLLo')");
    }

    // Test 3.4: Empty string
    {
        auto result = callFn(module, "lower", {makeStr("")});
        CHECK_EQ(result.toString(), "", "lower('')");
    }

    // Test 3.5: With numbers and symbols
    {
        auto result = callFn(module, "lower", {makeStr("HELLO123!@#")});
        CHECK_EQ(result.toString(), "hello123!@#", "lower('HELLO123!@#')");
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
        auto result = callFn(module, "trim", {makeStr("  hello  ")});
        CHECK_EQ(result.toString(), "hello", "trim('  hello  ')");
    }

    // Test 4.2: Only leading spaces
    {
        auto result = callFn(module, "trim", {makeStr("  hello")});
        CHECK_EQ(result.toString(), "hello", "trim('  hello')");
    }

    // Test 4.3: Only trailing spaces
    {
        auto result = callFn(module, "trim", {makeStr("hello  ")});
        CHECK_EQ(result.toString(), "hello", "trim('hello  ')");
    }

    // Test 4.4: No spaces to trim
    {
        auto result = callFn(module, "trim", {makeStr("hello")});
        CHECK_EQ(result.toString(), "hello", "trim('hello')");
    }

    // Test 4.5: Trim tabs and newlines
    {
        auto result = callFn(module, "trim", {makeStr("\t\nhello\r\n")});
        CHECK_EQ(result.toString(), "hello", "trim('\\t\\nhello\\r\\n')");
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
        auto result = callFn(module, "substring", {makeStr("hello world"), makeInt(0), makeInt(5)});
        CHECK_EQ(result.toString(), "hello", "substring('hello world', 0, 5)");
    }

    // Test 5.2: Middle substring
    {
        auto result = callFn(module, "substring", {makeStr("hello world"), makeInt(6), makeInt(11)});
        CHECK_EQ(result.toString(), "world", "substring('hello world', 6, 11)");
    }

    // Test 5.3: Full string
    {
        auto result = callFn(module, "substring", {makeStr("hello"), makeInt(0), makeInt(5)});
        CHECK_EQ(result.toString(), "hello", "substring('hello', 0, 5)");
    }

    // Test 5.4: Empty substring (start == end)
    {
        auto result = callFn(module, "substring", {makeStr("hello"), makeInt(2), makeInt(2)});
        CHECK_EQ(result.toString(), "", "substring('hello', 2, 2)");
    }

    // Test 5.5: Out of bounds (should be clamped)
    {
        auto result = callFn(module, "substring", {makeStr("hello"), makeInt(0), makeInt(100)});
        CHECK_EQ(result.toString(), "hello", "substring('hello', 0, 100)");
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
        auto result = callFn(module, "split", {makeStr("a,b,c"), makeStr(",")});
        auto parts = getStringArray(result);
        CHECK_EQ(parts.size(), 3, "split('a,b,c', ',').size");
        CHECK_EQ(parts[0], "a", "split('a,b,c', ',')[0]");
        CHECK_EQ(parts[1], "b", "split('a,b,c', ',')[1]");
        CHECK_EQ(parts[2], "c", "split('a,b,c', ',')[2]");
    }

    // Test 6.2: Split by space
    {
        auto result = callFn(module, "split", {makeStr("hello world"), makeStr(" ")});
        auto parts = getStringArray(result);
        CHECK_EQ(parts.size(), 2, "split('hello world', ' ').size");
        CHECK_EQ(parts[0], "hello", "split('hello world', ' ')[0]");
        CHECK_EQ(parts[1], "world", "split('hello world', ' ')[1]");
    }

    // Test 6.3: Split with no delimiter (chars)
    {
        auto result = callFn(module, "split", {makeStr("abc"), makeStr("")});
        auto parts = getStringArray(result);
        CHECK_EQ(parts.size(), 3, "split('abc', '').size");
        CHECK_EQ(parts[0], "a", "split('abc', '')[0]");
    }

    // Test 6.4: No split needed
    {
        auto result = callFn(module, "split", {makeStr("hello"), makeStr(",")});
        auto parts = getStringArray(result);
        CHECK_EQ(parts.size(), 1, "split('hello', ',').size");
        CHECK_EQ(parts[0], "hello", "split('hello', ',')[0]");
    }

    // Test 6.5: Empty string
    {
        auto result = callFn(module, "split", {makeStr(""), makeStr(",")});
        auto parts = getStringArray(result);
        CHECK_EQ(parts.size(), 1, "split('', ',').size");
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
        std::vector<interpreter::NaabVal> vec;
        for (const auto& s : strs) {
            vec.push_back(makeStr(s));
        }
        return interpreter::NaabVal::makeList(std::move(vec));
    };

    // Test 7.1: Join with comma
    {
        auto arr = makeStrArray({"a", "b", "c"});
        auto result = callFn(module, "join", {arr, makeStr(",")});
        CHECK_EQ(result.toString(), "a,b,c", "join(['a','b','c'], ',')");
    }

    // Test 7.2: Join with space
    {
        auto arr = makeStrArray({"hello", "world"});
        auto result = callFn(module, "join", {arr, makeStr(" ")});
        CHECK_EQ(result.toString(), "hello world", "join(['hello','world'], ' ')");
    }

    // Test 7.3: Join with empty delimiter
    {
        auto arr = makeStrArray({"a", "b", "c"});
        auto result = callFn(module, "join", {arr, makeStr("")});
        CHECK_EQ(result.toString(), "abc", "join(['a','b','c'], '')");
    }

    // Test 7.4: Single element
    {
        auto arr = makeStrArray({"hello"});
        auto result = callFn(module, "join", {arr, makeStr(",")});
        CHECK_EQ(result.toString(), "hello", "join(['hello'], ',')");
    }

    // Test 7.5: Empty array
    {
        auto arr = makeStrArray({});
        auto result = callFn(module, "join", {arr, makeStr(",")});
        CHECK_EQ(result.toString(), "", "join([], ',')");
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
        auto result = callFn(module, "replace", {makeStr("hello world"), makeStr("world"), makeStr("there")});
        CHECK_EQ(result.toString(), "hello there", "replace('hello world', 'world', 'there')");
    }

    // Test 8.2: Replace multiple occurrences
    {
        auto result = callFn(module, "replace", {makeStr("aa bb aa"), makeStr("aa"), makeStr("cc")});
        CHECK_EQ(result.toString(), "cc bb cc", "replace('aa bb aa', 'aa', 'cc')");
    }

    // Test 8.3: Replace with empty string
    {
        auto result = callFn(module, "replace", {makeStr("hello"), makeStr("l"), makeStr("")});
        CHECK_EQ(result.toString(), "heo", "replace('hello', 'l', '')");
    }

    // Test 8.4: No match
    {
        auto result = callFn(module, "replace", {makeStr("hello"), makeStr("x"), makeStr("y")});
        CHECK_EQ(result.toString(), "hello", "replace('hello', 'x', 'y')");
    }

    // Test 8.5: Replace empty old string (no-op)
    {
        auto result = callFn(module, "replace", {makeStr("hello"), makeStr(""), makeStr("x")});
        CHECK_EQ(result.toString(), "hello", "replace('hello', '', 'x')");
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
        auto result = callFn(module, "contains", {makeStr("hello world"), makeStr("world")});
        CHECK_TRUE(result.toBool(), "contains('hello world', 'world')");
    }

    // Test 9.2: Does not contain
    {
        auto result = callFn(module, "contains", {makeStr("hello"), makeStr("world")});
        CHECK_FALSE(result.toBool(), "contains('hello', 'world')");
    }

    // Test 9.3: Contains at start
    {
        auto result = callFn(module, "contains", {makeStr("hello"), makeStr("hel")});
        CHECK_TRUE(result.toBool(), "contains('hello', 'hel')");
    }

    // Test 9.4: Contains at end
    {
        auto result = callFn(module, "contains", {makeStr("hello"), makeStr("llo")});
        CHECK_TRUE(result.toBool(), "contains('hello', 'llo')");
    }

    // Test 9.5: Empty substring (always contains)
    {
        auto result = callFn(module, "contains", {makeStr("hello"), makeStr("")});
        CHECK_TRUE(result.toBool(), "contains('hello', '')");
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
        auto result = callFn(module, "starts_with", {makeStr("hello world"), makeStr("hello")});
        CHECK_TRUE(result.toBool(), "starts_with('hello world', 'hello')");
    }

    // Test 10.2: Does not start with
    {
        auto result = callFn(module, "starts_with", {makeStr("hello"), makeStr("world")});
        CHECK_FALSE(result.toBool(), "starts_with('hello', 'world')");
    }

    // Test 10.3: Exact match
    {
        auto result = callFn(module, "starts_with", {makeStr("hello"), makeStr("hello")});
        CHECK_TRUE(result.toBool(), "starts_with('hello', 'hello')");
    }

    // Test 10.4: Empty prefix (always true)
    {
        auto result = callFn(module, "starts_with", {makeStr("hello"), makeStr("")});
        CHECK_TRUE(result.toBool(), "starts_with('hello', '')");
    }

    // Test 10.5: Prefix longer than string
    {
        auto result = callFn(module, "starts_with", {makeStr("hi"), makeStr("hello")});
        CHECK_FALSE(result.toBool(), "starts_with('hi', 'hello')");
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
        auto result = callFn(module, "ends_with", {makeStr("hello world"), makeStr("world")});
        CHECK_TRUE(result.toBool(), "ends_with('hello world', 'world')");
    }

    // Test 11.2: Does not end with
    {
        auto result = callFn(module, "ends_with", {makeStr("hello"), makeStr("world")});
        CHECK_FALSE(result.toBool(), "ends_with('hello', 'world')");
    }

    // Test 11.3: Exact match
    {
        auto result = callFn(module, "ends_with", {makeStr("hello"), makeStr("hello")});
        CHECK_TRUE(result.toBool(), "ends_with('hello', 'hello')");
    }

    // Test 11.4: Empty suffix (always true)
    {
        auto result = callFn(module, "ends_with", {makeStr("hello"), makeStr("")});
        CHECK_TRUE(result.toBool(), "ends_with('hello', '')");
    }

    // Test 11.5: Suffix longer than string
    {
        auto result = callFn(module, "ends_with", {makeStr("hi"), makeStr("hello")});
        CHECK_FALSE(result.toBool(), "ends_with('hi', 'hello')");
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
        auto result = callFn(module, "concat", {makeStr("hello"), makeStr(" world")});
        CHECK_EQ(result.toString(), "hello world", "concat('hello', ' world')");
    }

    // Test 12.2: Concatenate with empty string
    {
        auto result = callFn(module, "concat", {makeStr("hello"), makeStr("")});
        CHECK_EQ(result.toString(), "hello", "concat('hello', '')");
    }

    // Test 12.3: Both empty strings
    {
        auto result = callFn(module, "concat", {makeStr(""), makeStr("")});
        CHECK_EQ(result.toString(), "", "concat('', '')");
    }

    // Test 12.4: Numbers and symbols
    {
        auto result = callFn(module, "concat", {makeStr("test"), makeStr("123")});
        CHECK_EQ(result.toString(), "test123", "concat('test', '123')");
    }

    // Test 12.5: Multiple words
    {
        auto result = callFn(module, "concat", {makeStr("foo bar"), makeStr(" baz")});
        CHECK_EQ(result.toString(), "foo bar baz", "concat('foo bar', ' baz')");
    }

    fmt::print("  ✓ All concat() tests passed (5/5)\n");
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================
}  // namespace

TEST(StringFunctionsTest, Length) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_length(stdlib_instance));
}

TEST(StringFunctionsTest, Upper) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_upper(stdlib_instance));
}

TEST(StringFunctionsTest, Lower) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_lower(stdlib_instance));
}

TEST(StringFunctionsTest, Trim) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_trim(stdlib_instance));
}

TEST(StringFunctionsTest, Substring) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_substring(stdlib_instance));
}

TEST(StringFunctionsTest, Split) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_split(stdlib_instance));
}

TEST(StringFunctionsTest, Join) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_join(stdlib_instance));
}

TEST(StringFunctionsTest, Replace) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_replace(stdlib_instance));
}

TEST(StringFunctionsTest, Contains) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_contains(stdlib_instance));
}

TEST(StringFunctionsTest, StartsWith) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_starts_with(stdlib_instance));
}

TEST(StringFunctionsTest, EndsWith) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_ends_with(stdlib_instance));
}

TEST(StringFunctionsTest, Concat) {
    stdlib::StdLib stdlib_instance;
    EXPECT_TRUE(test_string_concat(stdlib_instance));
}
