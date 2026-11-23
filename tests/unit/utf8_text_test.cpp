// Evidence text is valid UTF-8 (include/naab/utf8_text.h, A207, Bundle 20 PR 5).
//
// The rule: one U+FFFD per byte that does not belong to a well-formed sequence
// (RFC 3629), valid text unchanged. Each expected value is written out by hand.

#include <gtest/gtest.h>

#include "naab/utf8_text.h"

#include <nlohmann/json.hpp>

#include <string>

namespace tx = naab::text;

namespace {
const std::string R = "\xEF\xBF\xBD";   // U+FFFD
}

TEST(Utf8Text, ValidTextIsUnchanged) {
    const std::string s = "plain ascii, caf\xC3\xA9, \xE2\x82\xAC, \xF0\x9F\x98\x80";
    EXPECT_TRUE(tx::isValidUtf8(s));
    EXPECT_EQ(tx::validUtf8(s), s);
    EXPECT_TRUE(tx::isValidUtf8(""));
    EXPECT_EQ(tx::validUtf8(""), "");
}

TEST(Utf8Text, EachInvalidByteIsOneReplacement) {
    EXPECT_EQ(tx::validUtf8("caf\xE9 x"), "caf" + R + " x");          // Latin-1
    EXPECT_EQ(tx::validUtf8("\x80"), R);                               // a stray continuation
    EXPECT_EQ(tx::validUtf8("\xE2\x82"), R + R);                       // truncated: two bytes, two
    EXPECT_EQ(tx::validUtf8("\xE2\x82x"), R + R + "x");
    EXPECT_EQ(tx::validUtf8("\xFF\xFE"), R + R);
}

TEST(Utf8Text, OverlongSurrogateAndOutOfRangeAreInvalid) {
    EXPECT_FALSE(tx::isValidUtf8("\xC0\xAF"));          // overlong '/'
    EXPECT_FALSE(tx::isValidUtf8("\xE0\x80\xAF"));      // overlong, three bytes
    EXPECT_FALSE(tx::isValidUtf8("\xED\xA0\x80"));      // U+D800, a surrogate
    EXPECT_FALSE(tx::isValidUtf8("\xF4\x90\x80\x80"));  // above U+10FFFF
    EXPECT_TRUE(tx::isValidUtf8("\xF4\x8F\xBF\xBF"));   // U+10FFFF itself
    EXPECT_TRUE(tx::isValidUtf8("\xED\x9F\xBF"));       // U+D7FF, below the surrogates
}

TEST(Utf8Text, JsonValuesAndKeysAreMadeValid) {
    nlohmann::json j;
    j["message"] = std::string("caf\xE9");
    j["list"] = nlohmann::json::array({std::string("a\xFF"), 3, "ok"});
    j[std::string("k\xE9y")] = "v";
    tx::makeValidUtf8(j);
    EXPECT_EQ(j["message"].get<std::string>(), "caf" + R);
    EXPECT_EQ(j["list"][0].get<std::string>(), "a" + R);
    EXPECT_EQ(j["list"][1].get<int>(), 3);
    EXPECT_TRUE(j.contains("k" + R + "y"));
    EXPECT_FALSE(j.contains(std::string("k\xE9y")));
}

TEST(Utf8Text, DumpEvidenceNeverThrowsAndAgreesWithTheHelper) {
    nlohmann::json j;
    j["text"] = std::string("Caf\xE9 rule");
    std::string out;
    EXPECT_NO_THROW(out = tx::dumpEvidence(j));
    // The recorded text, read back, is what validUtf8() makes of the input:
    // the agreement a guidance delivery's recorded_sha256 depends on.
    EXPECT_EQ(nlohmann::json::parse(out)["text"].get<std::string>(),
              tx::validUtf8(std::string("Caf\xE9 rule")));
    // A valid document serialises exactly as nlohmann's own dump.
    nlohmann::json ok = {{"a", "b"}, {"n", 1}};
    EXPECT_EQ(tx::dumpEvidence(ok), ok.dump());
}

TEST(Utf8Text, TruncateCutsOnACharacterBoundary) {
    // A209: a tool result cut by tool_result_max_chars inside a multi-byte
    // character used to be cut by byte, which made the request invalid.
    const std::string e_acute = "caf\xC3\xA9";          // 5 bytes, 4 characters
    EXPECT_EQ(tx::truncateUtf8(e_acute, 10), e_acute);  // no cut
    EXPECT_EQ(tx::truncateUtf8(e_acute, 5), e_acute);   // the whole string fits
    EXPECT_EQ(tx::truncateUtf8(e_acute, 4), "caf");     // inside the 2-byte sequence
    EXPECT_EQ(tx::truncateUtf8(e_acute, 3), "caf");     // exactly on the boundary
    const std::string euro = "a\xE2\x82\xAC" "b";       // 3-byte sequence
    EXPECT_EQ(tx::truncateUtf8(euro, 2), "a");
    EXPECT_EQ(tx::truncateUtf8(euro, 3), "a");
    EXPECT_EQ(tx::truncateUtf8(euro, 4), "a\xE2\x82\xAC");
    EXPECT_EQ(tx::truncateUtf8(euro, 0), "");
    // The cut of a valid string is valid.
    for (size_t m = 0; m <= euro.size(); ++m) EXPECT_TRUE(tx::isValidUtf8(tx::truncateUtf8(euro, m)));
}

TEST(Utf8Text, DumpRequestRepairsAndNeverThrows) {
    nlohmann::json j = {{"contents", {{{"parts", {{{"text", std::string("caf\xE9")}}}}}}}};
    std::string out;
    EXPECT_NO_THROW(out = tx::dumpRequest(j));
    EXPECT_TRUE(tx::isValidUtf8(out));
    EXPECT_NE(out.find("caf\xEF\xBF\xBD"), std::string::npos);
    nlohmann::json ok = {{"a", "b"}};
    EXPECT_EQ(tx::dumpRequest(ok), ok.dump());
}
