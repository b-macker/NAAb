// Bounds what the task analyzers' regexes are asked to scan.
//
// libstdc++ runs std::regex by recursion (_Executor::_M_dfs), roughly one stack
// frame per character an unbounded repetition (\w+, .*, \s+) consumes. The
// analyzers are full of those, so a single ~40 KB line in a polyglot block
// exhausted the stack: SIGSEGV on both engines under a plain enforce config,
// with no verdict rendered. Bounding each line caps the recursion depth.
//
// Truncating is safe for every caller. The task detector only suggests a
// better language; the complexity floor can only see LESS structure in a
// truncated line, which lowers the measured complexity -- stricter, never a
// way past a floor that demands a minimum.

#pragma once

#include <string>

namespace naab {
namespace analyzer {

constexpr size_t kMaxAnalyzedLineLength = 1024;

// `code` with every line cut to kMaxAnalyzedLineLength characters.
inline std::string boundLineLengths(const std::string& code) {
    std::string out;
    out.reserve(code.size());
    size_t col = 0;
    for (char c : code) {
        if (c == '\n') { out.push_back(c); col = 0; continue; }
        if (col++ < kMaxAnalyzedLineLength) out.push_back(c);
    }
    return out;
}

} // namespace analyzer
} // namespace naab
