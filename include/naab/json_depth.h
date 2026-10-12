#pragma once
// Nesting-depth guard for untrusted JSON-RPC input (V-LSP-006).
//
// nlohmann::json::parse has no depth limit: a deeply nested payload exhausts
// the C++ call stack and crashes the process. Both stdio servers that parse
// messages from another process -- the language server and `naab-lang mcp` --
// check depth with this before parsing. One copy, so a fix lands in both.
#include <string>

namespace naab {
namespace util {

inline bool jsonDepthWithin(const std::string& s, int max_depth = 128) {
    int depth = 0;
    bool in_string = false;
    bool escape_next = false;
    for (unsigned char c : s) {
        if (escape_next) { escape_next = false; continue; }
        if (c == '\\' && in_string) { escape_next = true; continue; }
        if (c == '"') { in_string = !in_string; continue; }
        if (!in_string) {
            if (c == '{' || c == '[') { if (++depth > max_depth) return false; }
            else if (c == '}' || c == ']') { if (depth > 0) --depth; }
        }
    }
    return true;
}

}  // namespace util
}  // namespace naab
