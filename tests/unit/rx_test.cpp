// naab::rx -- the engine's regular expressions (A89). Each case states the
// std::regex behaviour it keeps, or the A89 input std::regex crashed on.
#include "naab/rx.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace rx = naab::rx;

TEST(RxTest, SearchReportsGroupsPositionsAndUnmatchedGroups) {
    rx::regex re("(\\w+)@(\\w+)(\\.org)?");
    std::string s = "mail: alice@example now";
    rx::smatch m;
    ASSERT_TRUE(rx::regex_search(s, m, re));
    EXPECT_EQ(m.size(), 4u);
    EXPECT_EQ(m.str(0), "alice@example");
    EXPECT_EQ(m[1].str(), "alice");
    EXPECT_EQ(m.str(2), "example");
    EXPECT_FALSE(m[3].matched);
    EXPECT_EQ(m[3].str(), "");
    EXPECT_EQ(m.position(0), 6);
    EXPECT_EQ(m.length(1), 5);
    EXPECT_EQ(m.prefix().str(), "mail: ");
    EXPECT_EQ(m.suffix().str(), " now");
    EXPECT_EQ(re.mark_count(), 3u);
}

TEST(RxTest, NoMatchAndIcase) {
    EXPECT_FALSE(rx::regex_search(std::string("nothing here"), rx::regex("TODO")));
    EXPECT_TRUE(rx::regex_search(std::string("// todo: later"), rx::regex("TODO", rx::regex::icase)));
    auto flags = rx::regex::ECMAScript;
    flags |= rx::regex::icase;   // the engine's own spelling of a combination
    EXPECT_TRUE(rx::regex_search(std::string("FixMe"), rx::regex("fixme", flags)));
}

// A89: std::regex recursed per character and segfaulted from ~50,000 bytes.
TEST(RxTest, LongInputIsAnOrdinarySearch) {
    std::string big(1000000, 'a');
    EXPECT_TRUE(rx::regex_search(big, rx::regex("^[a-z ]+$")));
    rx::smatch m;
    ASSERT_TRUE(rx::regex_match(big, m, rx::regex("[a-z]+")));
    EXPECT_EQ(m.length(0), 1000000);
    std::string words;
    for (int i = 0; i < 60000; ++i) words += "word ";
    EXPECT_FALSE(rx::regex_search(words, rx::regex("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}")));
    words += "x@example.org";
    EXPECT_TRUE(rx::regex_search(words, rx::regex("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}")));
}

// std::regex<char>'s \s is the C locale's six spaces; 0xA0 is a UTF-8
// continuation byte (the second byte of "\xC3\xA0"), never a space.
TEST(RxTest, SpaceClassIsTheCLocaleSet) {
    std::string nbsp_byte = std::string("x") + '\xA0' + "y";
    EXPECT_FALSE(rx::regex_search(nbsp_byte, rx::regex("x\\sy")));
    EXPECT_TRUE(rx::regex_search(nbsp_byte, rx::regex("x\\Sy")));
    EXPECT_TRUE(rx::regex_search(std::string("x\vy"), rx::regex("x\\sy")));
    EXPECT_TRUE(rx::regex_search(std::string("a b"), rx::regex("a[\\s]b")));
    EXPECT_TRUE(rx::regex_search(nbsp_byte, rx::regex("x[^\\s]y")));
}

// A non-ASCII byte in a pattern stands for that byte, as in std::regex<char>.
TEST(RxTest, NonAsciiPatternBytesMatchTheirBytes) {
    std::string cafe = "caf\xC3\xA9 au lait";
    rx::smatch m;
    ASSERT_TRUE(rx::regex_search(cafe, m, rx::regex("caf\xC3\xA9")));
    EXPECT_EQ(m.length(0), 5);
    EXPECT_FALSE(rx::regex_search(std::string("caf\xE9"), rx::regex("caf\xC3\xA9")));
}

TEST(RxTest, MatchIsWholeInput) {
    EXPECT_TRUE(rx::regex_match(std::string("ab"), rx::regex("a|ab")));
    EXPECT_FALSE(rx::regex_match(std::string("abc"), rx::regex("a|ab")));
    EXPECT_TRUE(rx::regex_search(std::string("abc"), rx::regex("a|ab")));
    rx::smatch m;
    const std::string date = "2026-10-30";
    ASSERT_TRUE(rx::regex_match(date, m, rx::regex("(\\d+)-(\\d+)-(\\d+)")));
    EXPECT_EQ(m.str(2), "10");
}

// libstdc++'s format rules: $& $` $' $$, and $n / $nn read greedily, a group
// past the end giving nothing.
TEST(RxTest, ReplaceFollowsTheStdFormat) {
    rx::regex re("(\\w)(\\d)");
    EXPECT_EQ(rx::regex_replace(std::string("a1 b2"), re, "[$2$1]"), "[1a] [2b]");
    EXPECT_EQ(rx::regex_replace(std::string("a1"), re, "<$&>"), "<a1>");
    EXPECT_EQ(rx::regex_replace(std::string("a1"), re, "$$"), "$");
    EXPECT_EQ(rx::regex_replace(std::string("a1"), re, "$9"), "");
    EXPECT_EQ(rx::regex_replace(std::string("x-a1-y"), re, "($`|$')"), "x-(x-|-y)-y");
    EXPECT_EQ(rx::regex_replace(std::string("a1 b2"), re, "_",
                                std::regex_constants::format_first_only), "_ b2");
    EXPECT_EQ(rx::regex_replace(std::string("no digits"), re, "_"), "no digits");
}

TEST(RxTest, IteratorWalksEveryMatch) {
    std::string s = "a1b22c333";
    rx::regex re("\\d+");
    std::vector<std::string> got;
    std::vector<long> pos;
    std::vector<std::string> prefixes;
    for (rx::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) {
        got.push_back(it->str());
        pos.push_back(static_cast<long>(it->position()));
        prefixes.push_back((*it).prefix().str());
    }
    EXPECT_EQ(got, (std::vector<std::string>{"1", "22", "333"}));
    EXPECT_EQ(pos, (std::vector<long>{1, 3, 6}));
    EXPECT_EQ(prefixes, (std::vector<std::string>{"a", "b", "c"}));
    // A pattern that only matches empty strings still terminates.
    int n = 0;
    std::string abc = "abc";
    const rx::regex xs("x*");
    for (rx::sregex_iterator it(abc.begin(), abc.end(), xs), end; it != end; ++it) ++n;
    EXPECT_EQ(n, 4);
}

TEST(RxTest, BadPatternIsARegexError) {
    bool caught = false;
    try {
        rx::regex bad("(unclosed");
    } catch (const std::regex_error& e) {
        caught = true;
        EXPECT_NE(std::string(e.what()), "");
    }
    EXPECT_TRUE(caught);
    // Running out of the search's memory budget is deliberately NOT a
    // regex_error: catch sites that skip a bad pattern must not skip a search
    // they could not finish.
    static_assert(!std::is_base_of<std::regex_error, rx::resource_error>::value,
                  "resource_error must not be caught as a regex_error");
}

TEST(RxTest, DefaultConstructedMatchesNothing) {
    rx::regex none;
    EXPECT_FALSE(rx::regex_search(std::string("anything"), none));
}

// The engine shares compiled patterns (static tables) across threads.
TEST(RxTest, OneRegexServesManyThreads) {
    static const rx::regex re("(key)=(\\d+)", rx::regex::icase);
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([t, &ok] {
            for (int i = 0; i < 500; ++i) {
                std::string s = "x KEY=" + std::to_string(t * 1000 + i) + " y";
                rx::smatch m;
                if (rx::regex_search(s, m, re) && m.str(2) == std::to_string(t * 1000 + i)) ++ok;
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(ok.load(), 8 * 500);
}

// A89, the second half: a backtracking matcher rescans from every start
// position, so this unanchored pattern over a run with no '@' was quadratic
// (libregexp: 155 s for 80,000 bytes, observed). RE2 is linear.
TEST(RxTest, UnanchoredRunIsLinear) {
    rx::regex re("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}");
    EXPECT_TRUE(re.linear_time());
    std::string run(200000, 'a');
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(rx::regex_search(run, re));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 5000) << "a linear search of 200,000 bytes took " << ms << " ms";
    run += "x@example.org";
    EXPECT_TRUE(rx::regex_search(run, re));
}

// What RE2 cannot run still works, on the fallback, and says so.
TEST(RxTest, LookaroundFallsBackAndStillMatches) {
    rx::regex la("pickle\\.load(?!s)");
    EXPECT_FALSE(la.linear_time());
    rx::smatch m;
    const std::string s = "pickle.loads pickle.load(";
    ASSERT_TRUE(rx::regex_search(s, m, la));
    EXPECT_EQ(m.position(0), 13);
    EXPECT_FALSE(rx::regex("(a)\\1").linear_time());          // backreference
    EXPECT_TRUE(rx::regex_search(std::string("xaay"), rx::regex("(a)\\1")));
}

// The RE2 path keeps std::regex's dialect where RE2's own differs.
TEST(RxTest, Re2PathKeepsTheEcmaScriptDialect) {
    rx::regex dot("a.b");
    EXPECT_TRUE(dot.linear_time());
    EXPECT_FALSE(rx::regex_search(std::string("a\rb"), dot));   // RE2's '.' would match \r
    EXPECT_FALSE(rx::regex_search(std::string("a\nb"), dot));
    EXPECT_TRUE(rx::regex_search(std::string("axb"), dot));
    rx::regex sp("x\\sy");
    EXPECT_TRUE(sp.linear_time());
    EXPECT_TRUE(rx::regex_search(std::string("x\vy"), sp));     // RE2's \s lacks \v
    // \A is the letter A in ECMAScript (Annex B); RE2 would read start-of-text.
    rx::regex a("x\\Ay");
    EXPECT_FALSE(a.linear_time());
    EXPECT_TRUE(rx::regex_search(std::string("xAy"), a));
    // [] matches nothing in ECMAScript; RE2 would read a literal ']'.
    EXPECT_FALSE(rx::regex("[]a]").linear_time());
}
