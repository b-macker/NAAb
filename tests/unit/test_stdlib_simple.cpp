// Simplified test to diagnose crash
#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <iostream>

using namespace naab;

int main() {
    fmt::print("Creating StdLib instance...\n");

    try {
        stdlib::StdLib stdlib_instance;
        fmt::print("StdLib created successfully!\n");

        fmt::print("Getting string module...\n");
        auto string_module = stdlib_instance.getModule("string");
        if (!string_module) {
            fmt::print("ERROR: string module is null!\n");
            return 1;
        }
        fmt::print("String module obtained: {}\n", (void*)string_module.get());

        fmt::print("Creating argument...\n");
        auto arg = std::make_shared<interpreter::Value>(std::string("hello"));
        fmt::print("Argument created\n");

        fmt::print("Calling string.length()...\n");
        auto result = string_module->call("length", {arg});
        fmt::print("Call completed\n");

        fmt::print("Getting result as int...\n");
        int value = result->toInt();
        fmt::print("Result: {}\n", value);

        fmt::print("SUCCESS!\n");
        return 0;

    } catch (const std::exception& e) {
        fmt::print("EXCEPTION: {}\n", e.what());
        return 1;
    }
}
