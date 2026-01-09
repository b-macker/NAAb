// Debug test for JSON module
#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <memory>
#include <vector>

int main() {
    using namespace naab;

    fmt::print("=== JSON Module Debug Test ===\n\n");

    stdlib::JSONModule json_module;

    // Test: Parse number
    fmt::print("Test: Parse number 42\n");
    auto num_str = std::make_shared<interpreter::Value>(std::string("42"));
    fmt::print("  Input type: {}\n", num_str->data.index());
    fmt::print("  Input value: {}\n", num_str->toString());

    auto num = json_module.call("parse", {num_str});
    fmt::print("  Result type index: {}\n", num->data.index());
    fmt::print("  Result toString: {}\n", num->toString());

    // Check if it's actually an int
    if (std::holds_alternative<int>(num->data)) {
        fmt::print("  ✓ Correct! It's an int: {}\n", std::get<int>(num->data));
    } else if (std::holds_alternative<bool>(num->data)) {
        fmt::print("  ✗ Wrong! It's a bool: {}\n", std::get<bool>(num->data));
    }

    fmt::print("\nTest: Parse string\n");
    auto str_str = std::make_shared<interpreter::Value>(std::string("\"hello\""));
    auto str = json_module.call("parse", {str_str});
    fmt::print("  Result type index: {}\n", str->data.index());
    fmt::print("  Result toString: {}\n", str->toString());

    if (std::holds_alternative<std::string>(str->data)) {
        fmt::print("  ✓ Correct! It's a string: {}\n", std::get<std::string>(str->data));
    } else if (std::holds_alternative<bool>(str->data)) {
        fmt::print("  ✗ Wrong! It's a bool: {}\n", std::get<bool>(str->data));
    }

    return 0;
}
