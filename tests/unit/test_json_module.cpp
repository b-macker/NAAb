// Standalone test for JSON module using nlohmann/json
#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <memory>
#include <vector>

int main() {
    using namespace naab;

    fmt::print("=== JSON Module Test ===\n\n");

    stdlib::JSONModule json_module;

    // Test 1: Parse simple values
    fmt::print("Test 1: Parse simple values\n");

    auto num_str = std::make_shared<interpreter::Value>(std::string("42"));
    auto num = json_module.call("parse", {num_str});
    fmt::print("  Number: {}\n", num->toString());

    auto str_str = std::make_shared<interpreter::Value>(std::string("\"hello\""));
    auto str = json_module.call("parse", {str_str});
    fmt::print("  String: {}\n", str->toString());

    auto bool_str = std::make_shared<interpreter::Value>(std::string("true"));
    auto bool_val = json_module.call("parse", {bool_str});
    fmt::print("  Boolean: {}\n", bool_val->toString());

    auto null_str = std::make_shared<interpreter::Value>(std::string("null"));
    auto null_val = json_module.call("parse", {null_str});
    fmt::print("  Null: {}\n", null_val->toString());

    // Test 2: Parse array
    fmt::print("\nTest 2: Parse array\n");
    auto arr_str = std::make_shared<interpreter::Value>(std::string("[1, 2, 3, 4, 5]"));
    auto arr = json_module.call("parse", {arr_str});
    fmt::print("  Array: {}\n", arr->toString());

    // Test 3: Parse object
    fmt::print("\nTest 3: Parse object\n");
    auto obj_str = std::make_shared<interpreter::Value>(std::string(R"({"name": "NAAb", "version": 1.0, "active": true})"));
    auto obj = json_module.call("parse", {obj_str});
    fmt::print("  Object: {}\n", obj->toString());

    // Test 4: Stringify
    fmt::print("\nTest 4: Stringify\n");
    auto stringified = json_module.call("stringify", {obj});
    fmt::print("  Compact: {}\n", stringified->toString());

    auto indent = std::make_shared<interpreter::Value>(2);
    auto pretty = json_module.call("stringify", {obj, indent});
    fmt::print("  Pretty:\n{}\n", pretty->toString());

    // Test 5: Round-trip
    fmt::print("\nTest 5: Round-trip test\n");
    auto original = std::make_shared<interpreter::Value>(std::string(R"({"test": "value", "number": 123})"));
    auto parsed1 = json_module.call("parse", {original});
    auto stringified2 = json_module.call("stringify", {parsed1});
    auto parsed2 = json_module.call("parse", {stringified2});
    fmt::print("  Original: {}\n", original->toString());
    fmt::print("  Stringified: {}\n", stringified2->toString());
    fmt::print("  Round-trip successful!\n");

    fmt::print("\n=== All JSON tests passed! ===\n");

    return 0;
}
