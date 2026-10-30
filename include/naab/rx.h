// naab/rx.h -- the engine's regular expressions (A89)
//
// libstdc++'s std::regex recurses once per character on an unbounded
// repetition, so a long enough input exhausts the stack and the process dies
// with SIGSEGV: no verdict, no report, exit 139. The inputs reaching it were
// chosen by the governed side -- a model reply under an operator's
// output_contract regex, a long reply under the engine's own no_pii scan, a
// program holding a long string literal (A89, docs/open-investigations.md).
//
// naab::rx is the subset of the std::regex API the engine uses. A pattern RE2
// can read exactly as std::regex does is matched by RE2, in time linear in the
// input; the rest (lookaround, backreferences, ECMAScript-only escapes) by
// QuickJS's libregexp, which keeps its backtracking state on the heap -- no
// crash, but superlinear on hostile input. A 1 MB input is an ordinary search.
// It is drop-in by design -- naab::rx::regex for std::regex, naab::rx::smatch
// for std::smatch, and so on -- and it matches
// std::regex's dialect where the engine depends on it: the pattern is matched
// byte by byte (a non-ASCII byte in the pattern stands for that byte, as in
// std::regex<char>), and \s / \S are the C locale's six space characters
// (libregexp's \s would also match the byte 0xA0, a UTF-8 continuation byte).
// Known differences, kept: lookbehind and named groups compile (std::regex
// rejected them); after an EMPTY match an iterator resumes one byte later
// (JavaScript's rule; std tries a non-empty match at the same position first);
// under icase a byte >= 0x80 folds as Latin-1. On the fallback matcher a
// hostile input can still take superlinear time (no time hook in this
// libregexp) -- recorded in A89.
//
// Errors: a pattern that does not compile throws std::regex_error, so every
// existing `catch (const std::regex_error&)` still applies. A search that runs
// out of its memory budget throws naab::rx::resource_error, which is NOT a
// regex_error on purpose: those catch sites skip the pattern and carry on, and
// a detection check that skipped its pattern would have passed text it never
// scanned.
//
// Work limit. The fallback counts its steps (a patched libregexp; A89), so
// no input can keep it busy without end: a search takes at most kSearchSteps,
// or, while a ScanBudget is open on its thread, draws on that budget with every
// other search under it. Past the limit it throws resource_error. RE2 needs no
// limit.
//
// Built without QuickJS (MSVC, where QuickJS does not compile), the fallback
// for what RE2 cannot read is std::regex, with its limits and no step count.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <iterator>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace naab {
namespace rx {

// A search that ran out of its memory budget. Deliberately not a regex_error.
class resource_error : public std::runtime_error {
public:
    explicit resource_error(const std::string& what) : std::runtime_error(what) {}
};

// Steps one fallback search may take when no ScanBudget is open.
constexpr std::uint64_t kSearchSteps = 100000000;

// A step budget shared by every fallback search on this thread while it is
// open -- a check that runs many patterns over one text is bounded as a whole.
// A budget opened inside another gets at most what the outer one has left,
// and what it spends is spent from the outer one too.
class ScanBudget {
public:
    explicit ScanBudget(std::uint64_t steps);
    ~ScanBudget();
    ScanBudget(const ScanBudget&) = delete;
    ScanBudget& operator=(const ScanBudget&) = delete;
    std::uint64_t remaining() const;

private:
    bool outer_scoped_ = false;
    std::uint64_t outer_remaining_ = 0;
    std::uint64_t granted_ = 0;
};

class regex {
public:
    using flag_type = unsigned;
    static constexpr flag_type ECMAScript = 0;
    static constexpr flag_type icase = 1u << 0;

    regex();
    explicit regex(const std::string& pattern, flag_type flags = ECMAScript);
    explicit regex(const char* pattern, flag_type flags = ECMAScript);

    unsigned mark_count() const;   // capture groups, not counting group 0
    // True when RE2 matches this pattern (linear in the input). False when it
    // fell back to the backtracking matcher: lookaround, backreferences,
    // ECMAScript-only escapes -- A89's recorded residual.
    bool linear_time() const;
    flag_type flags() const { return flags_; }
    const std::string& pattern() const { return pattern_; }

    struct Impl;
    const Impl* impl() const { return impl_.get(); }

private:
    std::shared_ptr<const Impl> impl_;
    std::string pattern_;
    flag_type flags_ = ECMAScript;
};

class sub_match {
public:
    using iterator = std::string::const_iterator;
    iterator first{};
    iterator second{};
    bool matched = false;

    std::string str() const { return matched ? std::string(first, second) : std::string(); }
    operator std::string() const { return str(); }
    std::ptrdiff_t length() const { return matched ? second - first : 0; }
    int compare(const std::string& s) const { return str().compare(s); }
};

bool operator==(const sub_match& a, const std::string& b);
bool operator!=(const sub_match& a, const std::string& b);
std::ostream& operator<<(std::ostream& os, const sub_match& m);

class smatch {
public:
    using const_iterator = std::vector<sub_match>::const_iterator;

    bool ready() const { return ready_; }
    bool empty() const { return subs_.empty(); }
    std::size_t size() const { return subs_.size(); }
    const sub_match& operator[](std::size_t i) const;
    std::string str(std::size_t i = 0) const { return (*this)[i].str(); }
    std::ptrdiff_t position(std::size_t i = 0) const;
    std::ptrdiff_t length(std::size_t i = 0) const { return (*this)[i].length(); }
    const sub_match& prefix() const { return prefix_; }
    const sub_match& suffix() const { return suffix_; }
    const_iterator begin() const { return subs_.begin(); }
    const_iterator end() const { return subs_.end(); }

private:
    friend struct MatchAccess;
    std::vector<sub_match> subs_;
    sub_match prefix_, suffix_, unmatched_;
    sub_match::iterator base_{};   // what position() counts from
    bool ready_ = false;
};

using match_flag_type = std::regex_constants::match_flag_type;

bool regex_search(const std::string& s, smatch& m, const regex& re,
                  match_flag_type flags = std::regex_constants::match_default);
bool regex_search(const std::string& s, const regex& re,
                  match_flag_type flags = std::regex_constants::match_default);
bool regex_search(std::string::const_iterator first, std::string::const_iterator last,
                  smatch& m, const regex& re,
                  match_flag_type flags = std::regex_constants::match_default);
bool regex_search(std::string::const_iterator first, std::string::const_iterator last,
                  const regex& re,
                  match_flag_type flags = std::regex_constants::match_default);
// As std: a match result must not point into a temporary.
bool regex_search(const std::string&&, smatch&, const regex&,
                  match_flag_type = std::regex_constants::match_default) = delete;

bool regex_match(const std::string& s, smatch& m, const regex& re,
                 match_flag_type flags = std::regex_constants::match_default);
bool regex_match(const std::string& s, const regex& re,
                 match_flag_type flags = std::regex_constants::match_default);
bool regex_match(const std::string&&, smatch&, const regex&,
                 match_flag_type = std::regex_constants::match_default) = delete;

// ECMAScript format: $& $1..$99 $` $' $$. format_first_only replaces once.
std::string regex_replace(const std::string& s, const regex& re, const std::string& fmt,
                          match_flag_type flags = std::regex_constants::format_default);
std::string regex_replace(const std::string& s, const regex& re, const char* fmt,
                          match_flag_type flags = std::regex_constants::format_default);

class sregex_iterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = smatch;
    using difference_type = std::ptrdiff_t;
    using pointer = const smatch*;
    using reference = const smatch&;

    sregex_iterator() = default;   // the end iterator
    sregex_iterator(std::string::const_iterator first, std::string::const_iterator last,
                    const regex& re);
    // As std (C++14): the iterator keeps a pointer to the regex, so a temporary
    // would dangle before the first ++.
    sregex_iterator(std::string::const_iterator, std::string::const_iterator,
                    const regex&&) = delete;

    reference operator*() const { return match_; }
    pointer operator->() const { return &match_; }
    sregex_iterator& operator++();
    sregex_iterator operator++(int) { sregex_iterator t = *this; ++*this; return t; }
    bool operator==(const sregex_iterator& o) const;
    bool operator!=(const sregex_iterator& o) const { return !(*this == o); }

private:
    void searchFrom(std::size_t offset);
    std::string::const_iterator begin_{}, end_{};
    const regex* re_ = nullptr;
    smatch match_;
    bool at_end_ = true;
};

}  // namespace rx
}  // namespace naab
