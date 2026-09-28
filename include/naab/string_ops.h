// String operations shared by BOTH engines' method forms (`s.slice(...)`) and
// the string module (`string.slice(s, ...)`).
//
// Each existed as three hand-written copies -- VM callBuiltinMethod, two
// tree-walker method paths in call_dispatch.cpp, and the module -- and the
// copies disagreed. Measured on "hello" / "a-b-c":
//
//   s.replace("-", "+")   VM "a+b-c" (first only)   tree-walker "a+b+c"
//   s.substring(3, 1)     VM ""                     tree-walker "lo"
//   s.slice(-3)           VM "llo"                  tree-walker "hello"
//
// The module functions agreed with each other in every case, so they are the
// reference: replace replaces every occurrence, substring clamps and yields ""
// for an empty or reversed range, slice follows JavaScript (negative indices
// count from the end, reversed range -> ""). Positions are byte offsets, as
// they always were.

#pragma once

#include <string>

namespace naab {
namespace strops {

// substring(start[, end]): start < 0 -> 0; end clamped to the length;
// end <= start (including a negative end) -> "".
inline std::string substring(const std::string& s, int start, bool has_end, int end) {
    int len = static_cast<int>(s.size());
    if (start < 0) start = 0;
    if (start >= len) return "";
    if (!has_end) return s.substr(static_cast<size_t>(start));
    if (end > len) end = len;
    if (end <= start) return "";
    return s.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
}

// slice(start[, end]), JavaScript semantics: a negative index counts from the
// end; out-of-range indices clamp; an empty or reversed range yields "".
inline std::string slice(const std::string& s, int start, bool has_end, int end) {
    int len = static_cast<int>(s.size());
    if (start < 0) start += len;
    if (start < 0) start = 0;
    if (start > len) start = len;
    if (!has_end) end = len;
    if (end < 0) end += len;
    if (end < 0) end = 0;
    if (end > len) end = len;
    if (start >= end) return "";
    return s.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
}

// replace(from, to): every occurrence. An empty `from` returns s unchanged
// (replacing "" everywhere would never terminate).
inline std::string replaceAll(const std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out = s;
    size_t pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
        out.replace(pos, from.size(), to);
        pos += to.size();
    }
    return out;
}

} // namespace strops
} // namespace naab
