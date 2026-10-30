// naab::rx -- see include/naab/rx.h for why this exists (A89).
#include "naab/rx.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <ostream>
#include <utility>

#if defined(NAAB_RX_LIBREGEXP)
extern "C" {
#include "quickjs.h"
#include "libregexp.h"
}
#endif

namespace naab {
namespace rx {

namespace {

// A pattern that does not compile. A regex_error, so every existing catch site
// applies, carrying the matcher's own message (a regex_error carries only a code).
class pattern_error : public std::regex_error {
public:
    explicit pattern_error(std::string msg)
        : std::regex_error(std::regex_constants::error_badrepeat), msg_(std::move(msg)) {}
    const char* what() const noexcept override { return msg_.c_str(); }
private:
    std::string msg_;
};

// A group's span as byte offsets into the searched range; -1 when unmatched.
struct Span { long begin = -1, end = -1; };

#if defined(NAAB_RX_LIBREGEXP)

// Per-thread allocator for libregexp. lre_realloc and lre_check_stack_overflow
// (defined in quickjs.c) read their opaque argument as a JSContext, so each
// thread gets a raw context of its own -- made on that thread, so its stack
// limit is that thread's (a runtime made on another thread checks the wrong
// stack: A61). The memory limit bounds the backtracking state a single search
// may hold; a search that reaches it throws resource_error.
constexpr size_t kSearchMemoryLimit = 256u << 20;

struct LreContext {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    ~LreContext() {
        if (ctx) JS_FreeContext(ctx);
        if (rt) JS_FreeRuntime(rt);
    }
};

JSContext* lreContext() {
    thread_local LreContext c;
    if (!c.ctx) {
        c.rt = JS_NewRuntime();
        if (!c.rt) throw resource_error("regular expression: could not allocate a matcher");
        JS_SetMemoryLimit(c.rt, kSearchMemoryLimit);
        c.ctx = JS_NewContextRaw(c.rt);
        if (!c.ctx) throw resource_error("regular expression: could not allocate a matcher");
    }
    return c.ctx;
}

void appendHexByte(std::string& out, unsigned char b) {
    static const char* hex = "0123456789ABCDEF";
    out += "\\x";
    out += hex[b >> 4];
    out += hex[b & 15];
}

// The pattern as libregexp should read it to match like std::regex<char>:
//  - every byte >= 0x80 (and NUL) as \xHH, so the pattern is matched byte by
//    byte -- libregexp reads a pattern as UTF-8 code points and refuses a
//    malformed sequence, while the input is matched as bytes;
//  - \s and \S as the C locale's six space characters: libregexp's \s also
//    matches 0xA0, which in UTF-8 text is a continuation byte. Inside a
//    character class \S is left as it is (it cannot be expanded there); the
//    difference is the byte 0xA0 alone.
std::string translatePattern(const std::string& p) {
    std::string out;
    out.reserve(p.size() + 16);
    bool in_class = false;
    for (size_t i = 0; i < p.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == '\\' && i + 1 < p.size()) {
            unsigned char n = static_cast<unsigned char>(p[i + 1]);
            ++i;
            if (n == 's') { out += in_class ? "\\t\\n\\v\\f\\r " : "[\\t\\n\\v\\f\\r ]"; continue; }
            if (n == 'S' && !in_class) { out += "[^\\t\\n\\v\\f\\r ]"; continue; }
            if (n >= 0x80 || n == 0) { appendHexByte(out, n); continue; }
            out += '\\';
            out += static_cast<char>(n);
            continue;
        }
        if (c >= 0x80 || c == 0) { appendHexByte(out, c); continue; }
        if (!in_class && c == '[') { in_class = true; out += '['; continue; }
        if (in_class && c == ']') { in_class = false; out += ']'; continue; }
        out += static_cast<char>(c);
    }
    return out;
}

std::vector<uint8_t> compileLre(const std::string& translated, int flags, std::string& err) {
    int len = 0;
    char msg[128] = {0};
    uint8_t* bc = lre_compile(&len, msg, sizeof msg, translated.c_str(), translated.size(),
                              flags, lreContext());
    if (!bc) { err = msg; return {}; }
    std::vector<uint8_t> out(bc, bc + len);   // owned here, not by this thread's runtime
    js_free(lreContext(), bc);
    return out;
}

#endif  // NAAB_RX_LIBREGEXP

}  // namespace

struct regex::Impl {
#if defined(NAAB_RX_LIBREGEXP)
    std::vector<uint8_t> search_bc;
    unsigned groups = 1;              // including group 0
    std::string translated;
    int lre_flags = 0;
    mutable std::once_flag full_once;
    mutable std::vector<uint8_t> full_bc;   // ^(?:P)$, for regex_match
#else
    std::regex re;
    unsigned groups = 1;
#endif
#if defined(NAAB_REGEX_DIFFERENTIAL)
    std::shared_ptr<std::regex> oracle;   // null when std::regex rejects the pattern
#endif
};

namespace {

#if defined(NAAB_REGEX_DIFFERENTIAL)
// Test builds only (cmake -DNAAB_REGEX_DIFFERENTIAL=ON): every search also
// runs std::regex on the same input -- below 4 KB, where std::regex does not
// overflow -- and any disagreement is appended to $NAAB_REGEX_DIFF_LOG. The
// whole test suite run this way is the evidence that the dialects agree on
// the inputs the engine actually sees.
constexpr size_t kOracleMaxInput = 4096;

std::string escapeForLog(const std::string& s, size_t max) {
    std::string out;
    for (size_t i = 0; i < s.size() && i < max; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\\') out += "\\\\";
        else if (c >= 0x20 && c < 0x7f) out += static_cast<char>(c);
        else { char b[8]; std::snprintf(b, sizeof b, "\\x%02X", c); out += b; }
    }
    if (s.size() > max) out += "...";
    return out;
}

void diffLog(const std::string& line) {
    const char* path = std::getenv("NAAB_REGEX_DIFF_LOG");
    if (!path || !*path) return;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (FILE* f = std::fopen(path, "a")) { std::fprintf(f, "%s\n", line.c_str()); std::fclose(f); }
}

std::string spansText(bool matched, const std::vector<Span>& s) {
    if (!matched) return "nomatch";
    std::string out;
    for (const auto& sp : s) out += "[" + std::to_string(sp.begin) + "," + std::to_string(sp.end) + ")";
    return out;
}
#endif

// Search [base, base+len) from offset `start`; fill `out` with every group's
// span. `full` asks for a match of the whole range (regex_match).
bool execSpans(const regex& re, const char* base, size_t len, size_t start, bool full,
               std::vector<Span>& out) {
    const regex::Impl* im = re.impl();
    if (!im) { out.clear(); return false; }   // as std::regex(): matches nothing
    out.assign(im->groups, Span{});
#if defined(NAAB_RX_LIBREGEXP)
    if (len > static_cast<size_t>(INT_MAX))
        throw resource_error("regular expression: input too large to search");
    const std::vector<uint8_t>* bc = &im->search_bc;
    if (full) {
        std::call_once(im->full_once, [im] {
            std::string err;
            im->full_bc = compileLre("^(?:" + im->translated + ")$", im->lre_flags, err);
        });
        if (im->full_bc.empty())
            throw pattern_error("regular expression: could not compile a whole-input match");
        bc = &im->full_bc;
    }
    std::vector<uint8_t*> cap(im->groups * 2, nullptr);
    static const uint8_t kEmpty[1] = {0};
    const uint8_t* cbuf = len ? reinterpret_cast<const uint8_t*>(base) : kEmpty;
    int r = lre_exec(cap.data(), bc->data(), cbuf, static_cast<int>(start),
                     static_cast<int>(len), 0, lreContext());
    if (r < 0)
        throw resource_error("regular expression: the search ran out of its memory budget");
    bool matched = r == 1;
    if (matched) {
        for (unsigned g = 0; g < im->groups; ++g) {
            if (cap[2 * g] && cap[2 * g + 1]) {
                out[g].begin = cap[2 * g] - cbuf;
                out[g].end = cap[2 * g + 1] - cbuf;
            }
        }
    }
#else
    const char* first = base + start;
    const char* last = base + len;
    std::cmatch m;
    auto flags = std::regex_constants::match_default;
    if (start > 0) flags |= std::regex_constants::match_prev_avail;
    bool matched = full ? std::regex_match(base, last, m, im->re)
                        : std::regex_search(first, last, m, im->re, flags);
    if (matched) {
        for (unsigned g = 0; g < im->groups && g < m.size(); ++g) {
            if (m[g].matched) {
                out[g].begin = m[g].first - base;
                out[g].end = m[g].second - base;
            }
        }
    }
#endif
#if defined(NAAB_REGEX_DIFFERENTIAL)
    if (im->oracle && len <= kOracleMaxInput) {
        std::cmatch m;
        auto flags = std::regex_constants::match_default;
        if (start > 0) flags |= std::regex_constants::match_prev_avail;
        bool sm = full ? std::regex_match(base, base + len, m, *im->oracle)
                       : std::regex_search(base + start, base + len, m, *im->oracle, flags);
        std::vector<Span> ss(im->groups, Span{});
        if (sm) {
            for (unsigned g = 0; g < im->groups && g < m.size(); ++g)
                if (m[g].matched) { ss[g].begin = m[g].first - base; ss[g].end = m[g].second - base; }
        }
        bool same = sm == matched;
        if (same && matched)
            for (unsigned g = 0; g < im->groups; ++g)
                if (ss[g].begin != out[g].begin || ss[g].end != out[g].end) { same = false; break; }
        if (!same)
            diffLog("MISMATCH " + std::string(full ? "match" : "search") +
                    " flags=" + std::to_string(re.flags()) + " start=" + std::to_string(start) +
                    " pattern=" + escapeForLog(re.pattern(), 300) +
                    " input=" + escapeForLog(std::string(base, len), 300) +
                    " rx=" + spansText(matched, out) + " std=" + spansText(sm, ss));
    }
#endif
    return matched;
}

}  // namespace

// ---- regex ------------------------------------------------------------------

regex::regex() = default;

regex::regex(const char* pattern, flag_type flags) : regex(std::string(pattern ? pattern : ""), flags) {}

regex::regex(const std::string& pattern, flag_type flags) : pattern_(pattern), flags_(flags) {
    auto im = std::make_shared<Impl>();
#if defined(NAAB_RX_LIBREGEXP)
    im->translated = translatePattern(pattern);
    im->lre_flags = (flags & icase) ? LRE_FLAG_IGNORECASE : 0;
    std::string err;
    im->search_bc = compileLre(im->translated, im->lre_flags, err);
    if (im->search_bc.empty()) {
#if defined(NAAB_REGEX_DIFFERENTIAL)
        diffLog("COMPILE rx-rejects pattern=" + escapeForLog(pattern, 300) + " error=" + err);
#endif
        throw pattern_error("regular expression: " + err);
    }
    im->groups = static_cast<unsigned>(lre_get_capture_count(im->search_bc.data()));
#else
    auto f = std::regex::ECMAScript;
    if (flags & icase) f |= std::regex::icase;
    im->re = std::regex(pattern, f);
    im->groups = static_cast<unsigned>(im->re.mark_count()) + 1;
#endif
#if defined(NAAB_REGEX_DIFFERENTIAL)
    try {
        auto f = std::regex::ECMAScript;
        if (flags & icase) f |= std::regex::icase;
        im->oracle = std::make_shared<std::regex>(pattern, f);
    } catch (const std::regex_error&) {
        diffLog("COMPILE std-rejects pattern=" + escapeForLog(pattern, 300));
    }
#endif
    impl_ = std::move(im);
}

unsigned regex::mark_count() const { return impl_ ? impl_->groups - 1 : 0; }

// ---- sub_match / smatch -----------------------------------------------------

bool operator==(const sub_match& a, const std::string& b) { return a.str() == b; }
bool operator!=(const sub_match& a, const std::string& b) { return a.str() != b; }
std::ostream& operator<<(std::ostream& os, const sub_match& m) { return os << m.str(); }

const sub_match& smatch::operator[](std::size_t i) const {
    return i < subs_.size() ? subs_[i] : unmatched_;
}

std::ptrdiff_t smatch::position(std::size_t i) const {
    // As std: an unmatched group's first is the end of the range.
    return (*this)[i].first - base_;
}

struct MatchAccess {
    // Fill `m` from spans over the range starting at `range_begin`; position()
    // counts from `pos_base`, prefix() starts at `prefix_from`.
    static void fill(smatch& m, std::string::const_iterator range_begin,
                     std::string::const_iterator range_end,
                     std::string::const_iterator pos_base,
                     std::string::const_iterator prefix_from,
                     const std::vector<Span>& spans) {
        m.subs_.assign(spans.size(), sub_match{});
        for (size_t g = 0; g < spans.size(); ++g) {
            if (spans[g].begin < 0) {
                m.subs_[g].first = m.subs_[g].second = range_end;
                continue;
            }
            m.subs_[g].first = range_begin + spans[g].begin;
            m.subs_[g].second = range_begin + spans[g].end;
            m.subs_[g].matched = true;
        }
        m.base_ = pos_base;
        m.prefix_.first = prefix_from;
        m.prefix_.second = m.subs_[0].first;
        m.prefix_.matched = m.prefix_.first != m.prefix_.second;
        m.suffix_.first = m.subs_[0].second;
        m.suffix_.second = range_end;
        m.suffix_.matched = m.suffix_.first != m.suffix_.second;
        m.unmatched_ = sub_match{};
        m.unmatched_.first = m.unmatched_.second = range_end;
        m.ready_ = true;
    }
    static void clear(smatch& m) {
        m.subs_.clear();
        m.prefix_ = m.suffix_ = m.unmatched_ = sub_match{};
        m.ready_ = true;
    }
};

// ---- search / match ---------------------------------------------------------

static const char* rangeBase(std::string::const_iterator first, std::string::const_iterator last) {
    return first == last ? "" : &*first;
}

bool regex_search(std::string::const_iterator first, std::string::const_iterator last,
                  smatch& m, const regex& re, match_flag_type) {
    std::vector<Span> spans;
    const size_t len = static_cast<size_t>(last - first);
    if (!execSpans(re, rangeBase(first, last), len, 0, false, spans)) {
        MatchAccess::clear(m);
        return false;
    }
    MatchAccess::fill(m, first, last, first, first, spans);
    return true;
}

bool regex_search(std::string::const_iterator first, std::string::const_iterator last,
                  const regex& re, match_flag_type) {
    std::vector<Span> spans;
    return execSpans(re, rangeBase(first, last), static_cast<size_t>(last - first), 0, false, spans);
}

bool regex_search(const std::string& s, smatch& m, const regex& re, match_flag_type f) {
    return regex_search(s.begin(), s.end(), m, re, f);
}

bool regex_search(const std::string& s, const regex& re, match_flag_type f) {
    return regex_search(s.begin(), s.end(), re, f);
}

bool regex_match(const std::string& s, smatch& m, const regex& re, match_flag_type) {
    std::vector<Span> spans;
    if (!execSpans(re, s.data(), s.size(), 0, true, spans)) {
        MatchAccess::clear(m);
        return false;
    }
    MatchAccess::fill(m, s.begin(), s.end(), s.begin(), s.begin(), spans);
    return true;
}

bool regex_match(const std::string& s, const regex& re, match_flag_type) {
    std::vector<Span> spans;
    return execSpans(re, s.data(), s.size(), 0, true, spans);
}

// ---- iterator ---------------------------------------------------------------

sregex_iterator::sregex_iterator(std::string::const_iterator first,
                                 std::string::const_iterator last, const regex& re)
    : begin_(first), end_(last), re_(&re), at_end_(false) {
    searchFrom(0);
}

void sregex_iterator::searchFrom(std::size_t offset) {
    const size_t len = static_cast<size_t>(end_ - begin_);
    std::string::const_iterator prefix_from =
        match_.ready() && !match_.empty() ? match_[0].second : begin_;
    if (offset > len) { at_end_ = true; return; }
    std::vector<Span> spans;
    if (!execSpans(*re_, rangeBase(begin_, end_), len, offset, false, spans)) {
        at_end_ = true;
        return;
    }
    MatchAccess::fill(match_, begin_, end_, begin_, prefix_from, spans);
}

sregex_iterator& sregex_iterator::operator++() {
    if (at_end_) return *this;
    const size_t end_off = static_cast<size_t>(match_[0].second - begin_);
    // After an empty match, resume one byte later (JavaScript's rule; see rx.h).
    const bool empty = match_[0].first == match_[0].second;
#if defined(NAAB_REGEX_DIFFERENTIAL)
    if (empty && end_off < static_cast<size_t>(end_ - begin_)) {
        std::string line = "EMPTY-ADVANCE pattern=" + escapeForLog(re_->pattern(), 300);
        diffLog(line);
    }
#endif
    searchFrom(empty ? end_off + 1 : end_off);
    return *this;
}

bool sregex_iterator::operator==(const sregex_iterator& o) const {
    if (at_end_ || o.at_end_) return at_end_ == o.at_end_;
    return begin_ == o.begin_ && end_ == o.end_ && re_ == o.re_ &&
           match_[0].first == o.match_[0].first && match_[0].second == o.match_[0].second;
}

// ---- replace ----------------------------------------------------------------

// libstdc++'s match_results::format, ECMAScript rules: $$, $&, $`, $', and $n
// or $nn read greedily as two digits, a group past the end giving nothing.
static void formatOne(std::string& out, const smatch& m, const std::string& fmt) {
    for (size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (c != '$' || i + 1 >= fmt.size()) { out += c; continue; }
        char n = fmt[i + 1];
        if (n == '$') { out += '$'; ++i; }
        else if (n == '&') { out += m.str(0); ++i; }
        else if (n == '`') { out += m.prefix().str(); ++i; }
        else if (n == '\'') { out += m.suffix().str(); ++i; }
        else if (n >= '0' && n <= '9') {
            long num = n - '0';
            ++i;
            if (i + 1 < fmt.size() && fmt[i + 1] >= '0' && fmt[i + 1] <= '9') {
                num = num * 10 + (fmt[i + 1] - '0');
                ++i;
            }
            if (num >= 0 && static_cast<size_t>(num) < m.size()) out += m.str(static_cast<size_t>(num));
        } else {
            out += '$';
        }
    }
}

std::string regex_replace(const std::string& s, const regex& re, const std::string& fmt,
                          match_flag_type flags) {
    const bool first_only = (flags & std::regex_constants::format_first_only) != 0;
    std::string out;
    std::string::const_iterator copied = s.begin();
    for (sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) {
        out.append(copied, (*it)[0].first);
        formatOne(out, *it, fmt);
        copied = (*it)[0].second;
        if (first_only) break;
    }
    out.append(copied, s.end());
    return out;
}

std::string regex_replace(const std::string& s, const regex& re, const char* fmt,
                          match_flag_type flags) {
    return regex_replace(s, re, std::string(fmt ? fmt : ""), flags);
}

}  // namespace rx
}  // namespace naab
