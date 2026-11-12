// Bounded reading of JSON-lines evidence (Bundle 14 PR 2).
//
// Telemetry and transcript files are untrusted input: they carry model-written
// text, and anyone who can write the file can write a line of any length or
// nesting. A reader that judges such a file reads each line through these two
// functions, so one line can cost at most `cap` bytes of memory and `max_depth`
// levels of structure, whatever the file holds.
#pragma once

#include <cstddef>
#include <istream>
#include <string>

namespace naab::evidence {

constexpr size_t kMaxTelemetryLineBytes = 1u << 20;    // 1 MiB
constexpr size_t kMaxTranscriptLineBytes = 16u << 20;  // 16 MiB: holds whole prompts
constexpr int kMaxJsonDepth = 64;

// Reads one line (without its terminator; a trailing CR is dropped). Returns
// false at end of input with nothing read. With cap > 0, a line longer than
// `cap` bytes is consumed to its end but not kept: `oversized` is set and
// `out` is empty. cap == 0 means no bound.
inline bool readBoundedLine(std::istream& in, std::string& out, size_t cap,
                            bool& oversized) {
    out.clear();
    oversized = false;
    std::streambuf* sb = in.rdbuf();
    if (!sb) return false;
    bool any = false;
    for (;;) {
        int c = sb->sbumpc();
        if (c == std::char_traits<char>::eof()) {
            if (!any) { in.setstate(std::ios::eofbit | std::ios::failbit); return false; }
            break;
        }
        any = true;
        if (c == '\n') break;
        if (oversized) continue;
        if (cap > 0 && out.size() >= cap) {
            oversized = true;
            out.clear();
            out.shrink_to_fit();
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    if (!oversized && !out.empty() && out.back() == '\r') out.pop_back();
    return true;
}

// True when the brackets of `line` (outside JSON strings) never nest deeper
// than max_depth. A line that fails is not handed to a JSON parser or
// serializer, whose recursion follows the nesting.
inline bool jsonNestingWithin(const std::string& line, int max_depth) {
    int depth = 0;
    bool in_string = false, escaped = false;
    for (char ch : line) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') in_string = true;
        else if (ch == '{' || ch == '[') { if (++depth > max_depth) return false; }
        else if (ch == '}' || ch == ']') { if (depth > 0) --depth; }
    }
    return true;
}

}  // namespace naab::evidence
