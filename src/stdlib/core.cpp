// NAAb Standard Library - Core functions
// print(), len(), type(), range(), etc.

#include <fmt/core.h>
#include <string>

namespace naab {
namespace stdlib {

// print(value)
void naab_print(const std::string& value) {
    fmt::print("{}\n", value);
}

// len(collection)
int naab_len(const std::string& str) {
    return static_cast<int>(str.length());
}

// type(value)
std::string naab_type(const std::string& value) {
    return "string";  // Stub
}

// TODO: Implement remaining core functions

} // namespace stdlib
} // namespace naab
