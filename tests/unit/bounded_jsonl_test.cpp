// The bounded line reader evidence checks read untrusted files through
// (Bundle 14 PR 2, include/naab/bounded_jsonl.h).
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "naab/bounded_jsonl.h"

using naab::evidence::jsonNestingWithin;
using naab::evidence::readBoundedLine;

TEST(BoundedJsonl, ReadsLinesAndDropsCarriageReturn) {
    std::istringstream in("one\r\ntwo\n\nlast");
    std::string line;
    bool over = true;
    ASSERT_TRUE(readBoundedLine(in, line, 16, over));
    EXPECT_EQ(line, "one");
    EXPECT_FALSE(over);
    ASSERT_TRUE(readBoundedLine(in, line, 16, over));
    EXPECT_EQ(line, "two");
    ASSERT_TRUE(readBoundedLine(in, line, 16, over));
    EXPECT_EQ(line, "");
    ASSERT_TRUE(readBoundedLine(in, line, 16, over));
    EXPECT_EQ(line, "last");
    EXPECT_FALSE(readBoundedLine(in, line, 16, over));
}

TEST(BoundedJsonl, AnOversizedLineIsConsumedNotKept) {
    std::istringstream in(std::string(100, 'a') + "\nnext\n");
    std::string line;
    bool over = false;
    ASSERT_TRUE(readBoundedLine(in, line, 10, over));
    EXPECT_TRUE(over);
    EXPECT_TRUE(line.empty());
    // the reader is positioned at the next line, not inside the long one
    ASSERT_TRUE(readBoundedLine(in, line, 10, over));
    EXPECT_FALSE(over);
    EXPECT_EQ(line, "next");
}

TEST(BoundedJsonl, ALineOfExactlyTheCapIsKept) {
    std::istringstream in(std::string(10, 'a') + "\n" + std::string(11, 'b') + "\n");
    std::string line;
    bool over = false;
    ASSERT_TRUE(readBoundedLine(in, line, 10, over));
    EXPECT_FALSE(over);
    EXPECT_EQ(line.size(), 10u);
    ASSERT_TRUE(readBoundedLine(in, line, 10, over));
    EXPECT_TRUE(over);
}

TEST(BoundedJsonl, ZeroCapIsUnbounded) {
    std::istringstream in(std::string(5000, 'a') + "\n");
    std::string line;
    bool over = true;
    ASSERT_TRUE(readBoundedLine(in, line, 0, over));
    EXPECT_FALSE(over);
    EXPECT_EQ(line.size(), 5000u);
}

TEST(BoundedJsonl, NestingCountsBracketsOutsideStringsOnly) {
    EXPECT_TRUE(jsonNestingWithin(R"({"a":[1,{"b":2}]})", 3));
    EXPECT_FALSE(jsonNestingWithin(R"({"a":[1,{"b":2}]})", 2));
    // brackets inside a string, an escaped quote included, are text
    EXPECT_TRUE(jsonNestingWithin(R"({"a":"[[[[\"[[[["})", 1));
    EXPECT_FALSE(jsonNestingWithin(std::string(65, '['), 64));
    EXPECT_TRUE(jsonNestingWithin(std::string(64, '['), 64));
}
