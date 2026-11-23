// Text as an evidence writer records it (include/naab/utf8_text.h, A207).
#include "naab/utf8_text.h"

namespace naab {
namespace text {

namespace {

// Length of the well-formed sequence starting at s[i], or 0 when the byte at
// s[i] does not start one that completes within `s` (RFC 3629, table 3-7 of
// the Unicode standard).
size_t sequenceLength(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const size_t n = s.size();
    const unsigned char c = b(i);
    if (c < 0x80) return 1;
    auto cont = [&](size_t k, unsigned char lo = 0x80, unsigned char hi = 0xBF) {
        return k < n && b(k) >= lo && b(k) <= hi;
    };
    if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
    if (c == 0xE0) return (cont(i + 1, 0xA0, 0xBF) && cont(i + 2)) ? 3 : 0;
    if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF)
        return (cont(i + 1) && cont(i + 2)) ? 3 : 0;
    if (c == 0xED) return (cont(i + 1, 0x80, 0x9F) && cont(i + 2)) ? 3 : 0;  // no surrogates
    if (c == 0xF0) return (cont(i + 1, 0x90, 0xBF) && cont(i + 2) && cont(i + 3)) ? 4 : 0;
    if (c >= 0xF1 && c <= 0xF3) return (cont(i + 1) && cont(i + 2) && cont(i + 3)) ? 4 : 0;
    if (c == 0xF4) return (cont(i + 1, 0x80, 0x8F) && cont(i + 2) && cont(i + 3)) ? 4 : 0;
    return 0;  // 0x80-0xC1 (a stray continuation or an overlong lead), 0xF5-0xFF
}

}  // namespace

bool isValidUtf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const size_t len = sequenceLength(s, i);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

std::string validUtf8(std::string_view s) {
    if (isValidUtf8(s)) return std::string(s);
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        const size_t len = sequenceLength(s, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(s.data() + i, len);
            i += len;
        }
    }
    return out;
}

std::string truncateUtf8(std::string_view s, size_t max) {
    if (s.size() <= max) return std::string(s);
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return std::string(s.substr(0, cut));
}

}  // namespace text
}  // namespace naab
