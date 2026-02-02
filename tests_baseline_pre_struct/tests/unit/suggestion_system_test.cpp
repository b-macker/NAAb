// Suggestion System Unit Tests (Phase 4.1.4 - Task 4.1.26)

#include <gtest/gtest.h>
#include "naab/suggestion_system.h"

using namespace naab::error;

// ============================================================================
// Levenshtein Distance Tests (Task 4.1.22)
// ============================================================================

TEST(SuggestionSystemTest, FindClosestMatch_ExactMatch) {
    std::vector<std::string> candidates = {"count", "total", "index"};
    auto result = SuggestionSystem::findClosestMatch("count", candidates);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "count");
}

TEST(SuggestionSystemTest, FindClosestMatch_Distance1) {
    std::vector<std::string> candidates = {"count", "total", "index"};
    auto result = SuggestionSystem::findClosestMatch("cont", candidates);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "count");  // Missing 'u'
}

TEST(SuggestionSystemTest, FindClosestMatch_Distance2) {
    std::vector<std::string> candidates = {"count", "total", "index"};
    auto result = SuggestionSystem::findClosestMatch("cont", candidates, 2);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "count");
}

TEST(SuggestionSystemTest, FindClosestMatch_BeyondThreshold) {
    std::vector<std::string> candidates = {"count", "total", "index"};
    auto result = SuggestionSystem::findClosestMatch("xyz", candidates, 2);

    EXPECT_FALSE(result.has_value());  // No close match
}

TEST(SuggestionSystemTest, FindClosestMatch_EmptyCandidates) {
    std::vector<std::string> candidates;
    auto result = SuggestionSystem::findClosestMatch("count", candidates);

    EXPECT_FALSE(result.has_value());
}

TEST(SuggestionSystemTest, FindClosestMatch_Typo) {
    std::vector<std::string> candidates = {"variable", "function", "module"};
    auto result = SuggestionSystem::findClosestMatch("variabel", candidates);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "variable");  // Swapped 'l' and 'e'
}

// ============================================================================
// Variable Suggestion Tests (Task 4.1.23)
// ============================================================================

TEST(SuggestionSystemTest, SuggestVariable_Found) {
    std::vector<std::string> scope = {"count", "total", "index", "value"};
    std::string suggestion = SuggestionSystem::suggestVariable("cont", scope);

    EXPECT_EQ(suggestion, "Did you mean 'count'?");
}

TEST(SuggestionSystemTest, SuggestVariable_NotFound) {
    std::vector<std::string> scope = {"count", "total", "index"};
    std::string suggestion = SuggestionSystem::suggestVariable("xyz", scope);

    EXPECT_NE(suggestion.find("not defined"), std::string::npos);
    EXPECT_NE(suggestion.find("Check spelling"), std::string::npos);
}

// ============================================================================
// Type Conversion Suggestion Tests (Task 4.1.23)
// ============================================================================

TEST(SuggestionSystemTest, SuggestTypeConversion_StringToInt) {
    std::string suggestion = SuggestionSystem::suggestTypeConversion("int", "string");

    EXPECT_NE(suggestion.find("int()"), std::string::npos);
}

TEST(SuggestionSystemTest, SuggestTypeConversion_IntToString) {
    std::string suggestion = SuggestionSystem::suggestTypeConversion("string", "int");

    EXPECT_NE(suggestion.find("string()"), std::string::npos);
}

TEST(SuggestionSystemTest, SuggestTypeConversion_IntToDouble) {
    std::string suggestion = SuggestionSystem::suggestTypeConversion("double", "int");

    EXPECT_NE(suggestion.find("automatically converted"), std::string::npos);
}

TEST(SuggestionSystemTest, SuggestTypeConversion_ToBool) {
    std::string suggestion = SuggestionSystem::suggestTypeConversion("bool", "int");

    EXPECT_NE(suggestion.find("explicit boolean conversion"), std::string::npos);
}

TEST(SuggestionSystemTest, SuggestTypeConversion_Generic) {
    std::string suggestion = SuggestionSystem::suggestTypeConversion("custom_type", "other_type");

    EXPECT_NE(suggestion.find("cannot be used"), std::string::npos);
}

// ============================================================================
// Import Suggestion Tests (Task 4.1.24)
// ============================================================================

TEST(SuggestionSystemTest, SuggestImport) {
    std::string suggestion = SuggestionSystem::suggestImport("math");

    EXPECT_EQ(suggestion, "Add 'import math' at the top of your file");
}

TEST(SuggestionSystemTest, SuggestImport_CustomModule) {
    std::string suggestion = SuggestionSystem::suggestImport("my_custom_module");

    EXPECT_NE(suggestion.find("import my_custom_module"), std::string::npos);
}

// ============================================================================
// Real-World Scenario Tests (Task 4.1.26 - 20 error cases)
// ============================================================================

TEST(SuggestionSystemTest, RealWorld_MisspelledVariable1) {
    std::vector<std::string> scope = {"userName", "userEmail", "userId"};
    auto result = SuggestionSystem::findClosestMatch("userNam", scope);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "userName");
}

TEST(SuggestionSystemTest, RealWorld_MisspelledVariable2) {
    std::vector<std::string> scope = {"response", "request", "result"};
    auto result = SuggestionSystem::findClosestMatch("responce", scope);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "response");
}

TEST(SuggestionSystemTest, RealWorld_CamelCaseTypo) {
    std::vector<std::string> scope = {"getResponse", "getRequest", "getData"};
    auto result = SuggestionSystem::findClosestMatch("getRespose", scope);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "getResponse");
}

TEST(SuggestionSystemTest, RealWorld_SimilarNames) {
    std::vector<std::string> scope = {"count1", "count2", "count3"};
    auto result = SuggestionSystem::findClosestMatch("count", scope);
    ASSERT_TRUE(result.has_value());
    // Should suggest one of them (closest)
}

TEST(SuggestionSystemTest, RealWorld_OffByOne) {
    std::vector<std::string> scope = {"index", "value", "total"};
    auto result = SuggestionSystem::findClosestMatch("indx", scope);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "index");
}
