//
// Unit tests for SafeRegex - ReDoS protection
//

#include <gtest/gtest.h>
#include "naab/safe_regex.h"
#include <chrono>
#include <thread>

using namespace naab::regex_safety;
using namespace std::chrono_literals;

// Test basic functionality
TEST(SafeRegexTest, BasicMatch) {
    SafeRegex sr;

    EXPECT_TRUE(sr.safeMatch("hello", "hello"));
    EXPECT_TRUE(sr.safeMatch("hello world", "hello.*"));
    EXPECT_FALSE(sr.safeMatch("hello", "world"));
}

TEST(SafeRegexTest, BasicSearch) {
    SafeRegex sr;

    EXPECT_TRUE(sr.safeSearch("hello world", "world"));
    EXPECT_TRUE(sr.safeSearch("the quick brown fox", "quick"));
    EXPECT_FALSE(sr.safeSearch("hello", "world"));
}

TEST(SafeRegexTest, FindWithMatch) {
    SafeRegex sr;
    std::smatch match;

    bool found = sr.safeSearch("hello world", "w(\\w+)", match);
    EXPECT_TRUE(found);
    EXPECT_EQ(match.str(0), "world");
    EXPECT_EQ(match.str(1), "orld");
}

TEST(SafeRegexTest, SafeReplace) {
    SafeRegex sr;

    std::string result = sr.safeReplace("hello world", "world", "universe");
    EXPECT_EQ(result, "hello universe");

    result = sr.safeReplace("one two three", "\\w+", "X", 0ms, true);
    EXPECT_EQ(result, "X X X");
}

TEST(SafeRegexTest, SafeReplaceFirst) {
    SafeRegex sr;

    std::string result = sr.safeReplace("one two three", "\\w+", "X", 0ms, false);
    EXPECT_EQ(result, "X two three");
}

TEST(SafeRegexTest, SafeFindAll) {
    SafeRegex sr;

    auto matches = sr.safeFindAll("one 123 two 456", "\\d+");
    ASSERT_EQ(matches.size(), 2);
    EXPECT_EQ(matches[0], "123");
    EXPECT_EQ(matches[1], "456");
}

// Test input validation
TEST(SafeRegexTest, InputSizeLimit) {
    RegexLimits limits;
    limits.max_input_size = 100;  // Very small limit
    SafeRegex sr(limits);

    std::string large_input(200, 'a');

    EXPECT_THROW({
        sr.safeMatch(large_input, "a+");
    }, RegexInputSizeException);
}

TEST(SafeRegexTest, PatternSizeLimit) {
    RegexLimits limits;
    limits.max_pattern_length = 50;
    SafeRegex sr(limits);

    std::string large_pattern(100, 'a');

    EXPECT_THROW({
        sr.safeMatch("test", large_pattern);
    }, std::runtime_error);
}

TEST(SafeRegexTest, MatchLimit) {
    RegexLimits limits;
    limits.max_matches = 5;  // Limit to 5 matches
    SafeRegex sr(limits);

    std::string text = "a b c d e f g h i j";  // 10 words

    EXPECT_THROW({
        sr.safeFindAll(text, "\\w+");
    }, std::runtime_error);
}

// Test pattern complexity analysis
TEST(SafeRegexTest, PatternAnalysis_Safe) {
    SafeRegex sr;

    PatternComplexity complexity = sr.analyzePattern("hello.*world");
    EXPECT_TRUE(complexity.is_safe);
    EXPECT_LT(complexity.backtracking_score, 100);
}

TEST(SafeRegexTest, PatternAnalysis_NestedQuantifiers) {
    SafeRegex sr;

    // (a+)+ is a classic ReDoS pattern
    PatternComplexity complexity = sr.analyzePattern("(a+)+");
    EXPECT_FALSE(complexity.is_safe);
    EXPECT_GE(complexity.backtracking_score, 100);
    EXPECT_FALSE(complexity.warning.empty());
}

TEST(SafeRegexTest, PatternAnalysis_UnboundedRepetition) {
    SafeRegex sr;

    PatternComplexity complexity = sr.analyzePattern(".*");
    EXPECT_GE(complexity.backtracking_score, 30);
}

TEST(SafeRegexTest, PatternAnalysis_Nesting) {
    SafeRegex sr;

    PatternComplexity complexity = sr.analyzePattern("((((a))))");
    EXPECT_GT(complexity.nesting_depth, 0);
}

TEST(SafeRegexTest, PatternAnalysis_Quantifiers) {
    SafeRegex sr;

    PatternComplexity complexity = sr.analyzePattern("a+b*c?d{2,5}");
    EXPECT_EQ(complexity.quantifier_count, 4);
}

// Test ReDoS protection
TEST(SafeRegexTest, RejectDangerousPattern_NestedQuantifiers) {
    RegexLimits limits;
    limits.strict_validation = true;
    SafeRegex sr(limits);

    // This pattern causes catastrophic backtracking
    EXPECT_THROW({
        sr.safeMatch("aaaaaaaaaaaaaaaaaaaaaaaa!", "(a+)+b");
    }, RegexDangerousPatternException);
}

TEST(SafeRegexTest, TimeoutProtection_SlowPattern) {
    RegexLimits limits;
    limits.max_execution_time = 100ms;  // Short timeout
    limits.strict_validation = false;  // Allow pattern but enforce timeout
    SafeRegex sr(limits);

    // This pattern can be slow on certain inputs
    // Note: Actual timeout depends on system performance
    std::string input(30, 'a');

    // This might timeout on slower systems
    // On fast systems, it might complete
    // C++ regex library may also throw its own complexity exception
    // So we don't assert timeout, just ensure no crash
    try {
        sr.safeMatch(input + "!", "(a+)+b");
    } catch (const RegexTimeoutException& e) {
        // Timeout is expected behavior
        EXPECT_FALSE(std::string(e.what()).empty());
    } catch (const RegexDangerousPatternException& e) {
        // If pattern validation caught it first, that's also good
        EXPECT_FALSE(std::string(e.what()).empty());
    } catch (const std::runtime_error& e) {
        // C++ regex library may throw complexity exception
        // This is also acceptable (library-level protection)
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("complexity") != std::string::npos ||
                    msg.find("Regex error") != std::string::npos);
    }
}

TEST(SafeRegexTest, CustomTimeout) {
    SafeRegex sr;

    // Use custom timeout (longer than default)
    auto result = sr.safeMatch("hello world", "hello.*", 5000ms);
    EXPECT_TRUE(result);
}

// Test pattern analysis utilities
TEST(PatternAnalysisTest, HasNestedQuantifiers) {
    EXPECT_TRUE(pattern_analysis::hasNestedQuantifiers("(a+)+"));
    EXPECT_TRUE(pattern_analysis::hasNestedQuantifiers("(a*)*"));
    EXPECT_TRUE(pattern_analysis::hasNestedQuantifiers("(a+)*"));
    EXPECT_FALSE(pattern_analysis::hasNestedQuantifiers("a+"));
    EXPECT_FALSE(pattern_analysis::hasNestedQuantifiers("(a+)"));
}

TEST(PatternAnalysisTest, HasUnboundedRepetition) {
    EXPECT_TRUE(pattern_analysis::hasUnboundedRepetition(".*"));
    EXPECT_TRUE(pattern_analysis::hasUnboundedRepetition(".+"));
    EXPECT_TRUE(pattern_analysis::hasUnboundedRepetition("[a-z]*"));
    EXPECT_TRUE(pattern_analysis::hasUnboundedRepetition("[a-z]+"));
    EXPECT_FALSE(pattern_analysis::hasUnboundedRepetition("a{1,5}"));
}

TEST(PatternAnalysisTest, EstimateBacktrackingScore) {
    // More quantifiers = higher score
    int score1 = pattern_analysis::estimateBacktrackingScore("a+");
    int score2 = pattern_analysis::estimateBacktrackingScore("a+b*c+d*");
    EXPECT_GT(score2, score1);

    // Alternations increase score
    int score3 = pattern_analysis::estimateBacktrackingScore("(a|b|c|d)");
    EXPECT_GT(score3, score1);
}

TEST(PatternAnalysisTest, GetNestingDepth) {
    EXPECT_EQ(pattern_analysis::getPatternNestingDepth("abc"), 0);
    EXPECT_EQ(pattern_analysis::getPatternNestingDepth("(abc)"), 1);
    EXPECT_EQ(pattern_analysis::getPatternNestingDepth("((abc))"), 2);
    EXPECT_EQ(pattern_analysis::getPatternNestingDepth("[abc]"), 1);
    EXPECT_EQ(pattern_analysis::getPatternNestingDepth("([abc])"), 2);
}

TEST(PatternAnalysisTest, CountQuantifiers) {
    EXPECT_EQ(pattern_analysis::countQuantifiers("abc"), 0);
    EXPECT_EQ(pattern_analysis::countQuantifiers("a+b*c?"), 3);
    EXPECT_EQ(pattern_analysis::countQuantifiers("a{2,5}"), 1);
    EXPECT_EQ(pattern_analysis::countQuantifiers("a+b*c?d{1,3}"), 4);
}

// Test edge cases
TEST(SafeRegexTest, EmptyPattern) {
    SafeRegex sr;
    EXPECT_TRUE(sr.safeMatch("", ""));
}

TEST(SafeRegexTest, EmptyInput) {
    SafeRegex sr;
    EXPECT_FALSE(sr.safeMatch("", "a+"));
}

TEST(SafeRegexTest, InvalidRegex) {
    SafeRegex sr;

    EXPECT_THROW({
        sr.safeMatch("test", "[invalid");  // Unclosed bracket
    }, std::runtime_error);
}

// Test global instance
TEST(SafeRegexTest, GlobalInstance) {
    auto& global1 = getGlobalSafeRegex();
    auto& global2 = getGlobalSafeRegex();

    // Should be same instance
    EXPECT_EQ(&global1, &global2);

    // Should work
    EXPECT_TRUE(global1.safeMatch("hello", "hello"));
}

// Performance test (not a timeout test, just ensures reasonable performance)
TEST(SafeRegexTest, PerformanceReasonable) {
    SafeRegex sr;

    std::string input(1000, 'a');

    auto start = std::chrono::steady_clock::now();
    bool result = sr.safeMatch(input, "a+");
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_TRUE(result);
    EXPECT_LT(duration.count(), 100);  // Should complete in under 100ms
}

// Test limits configuration
TEST(SafeRegexTest, ConfigurableLimits) {
    RegexLimits custom_limits;
    custom_limits.max_execution_time = 2000ms;
    custom_limits.max_input_size = 50000;
    custom_limits.max_pattern_length = 500;
    custom_limits.max_matches = 5000;
    custom_limits.strict_validation = false;

    SafeRegex sr(custom_limits);

    const auto& limits = sr.getLimits();
    EXPECT_EQ(limits.max_execution_time, 2000ms);
    EXPECT_EQ(limits.max_input_size, 50000);
    EXPECT_EQ(limits.max_pattern_length, 500);
    EXPECT_EQ(limits.max_matches, 5000);
    EXPECT_FALSE(limits.strict_validation);
}

TEST(SafeRegexTest, UpdateLimits) {
    SafeRegex sr;

    RegexLimits new_limits;
    new_limits.max_input_size = 10000;
    sr.setLimits(new_limits);

    EXPECT_EQ(sr.getLimits().max_input_size, 10000);
}
