// Standalone test for HTTP module using libcurl
#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <memory>
#include <vector>

int main() {
    using namespace naab;

    fmt::print("=== HTTP Module Test ===\n\n");

    stdlib::HTTPModule http_module;

    // Test 1: HTTP GET
    fmt::print("Test 1: HTTP GET request\n");
    try {
        auto url = std::make_shared<interpreter::Value>(std::string("https://httpbin.org/get"));
        auto response = http_module.call("get", {url});

        // Check if response is a dict
        if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(response->data)) {
            auto& resp_dict = std::get<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(response->data);

            fmt::print("  Status: {}\n", resp_dict["status"]->toString());
            fmt::print("  OK: {}\n", resp_dict["ok"]->toString());
            fmt::print("  Body length: {} bytes\n", resp_dict["body"]->toString().length());

            // Check if status is 200
            int status = resp_dict["status"]->toInt();
            if (status == 200) {
                fmt::print("  ✓ GET request successful\n");
            } else {
                fmt::print("  ✗ GET request failed with status {}\n", status);
            }
        }
    } catch (const std::exception& e) {
        fmt::print("  ✗ Error: {}\n", e.what());
    }

    // Test 2: HTTP POST
    fmt::print("\nTest 2: HTTP POST request\n");
    try {
        auto url = std::make_shared<interpreter::Value>(std::string("https://httpbin.org/post"));
        auto data = std::make_shared<interpreter::Value>(std::string(R"({"test": "data", "value": 123})"));
        auto response = http_module.call("post", {url, data});

        if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(response->data)) {
            auto& resp_dict = std::get<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(response->data);

            fmt::print("  Status: {}\n", resp_dict["status"]->toString());
            fmt::print("  Body length: {} bytes\n", resp_dict["body"]->toString().length());

            int status = resp_dict["status"]->toInt();
            if (status == 200) {
                fmt::print("  ✓ POST request successful\n");
            } else {
                fmt::print("  ✗ POST request failed with status {}\n", status);
            }
        }
    } catch (const std::exception& e) {
        fmt::print("  ✗ Error: {}\n", e.what());
    }

    // Test 3: HTTP PUT
    fmt::print("\nTest 3: HTTP PUT request\n");
    try {
        auto url = std::make_shared<interpreter::Value>(std::string("https://httpbin.org/put"));
        auto data = std::make_shared<interpreter::Value>(std::string(R"({"updated": true})"));
        auto response = http_module.call("put", {url, data});

        if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(response->data)) {
            auto& resp_dict = std::get<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(response->data);

            int status = resp_dict["status"]->toInt();
            if (status == 200) {
                fmt::print("  Status: {}\n", status);
                fmt::print("  ✓ PUT request successful\n");
            } else {
                fmt::print("  ✗ PUT request failed with status {}\n", status);
            }
        }
    } catch (const std::exception& e) {
        fmt::print("  ✗ Error: {}\n", e.what());
    }

    // Test 4: HTTP DELETE
    fmt::print("\nTest 4: HTTP DELETE request\n");
    try {
        auto url = std::make_shared<interpreter::Value>(std::string("https://httpbin.org/delete"));
        auto response = http_module.call("delete", {url});

        if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(response->data)) {
            auto& resp_dict = std::get<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(response->data);

            int status = resp_dict["status"]->toInt();
            if (status == 200) {
                fmt::print("  Status: {}\n", status);
                fmt::print("  ✓ DELETE request successful\n");
            } else {
                fmt::print("  ✗ DELETE request failed with status {}\n", status);
            }
        }
    } catch (const std::exception& e) {
        fmt::print("  ✗ Error: {}\n", e.what());
    }

    // Test 5: Error handling - invalid URL
    fmt::print("\nTest 5: Error handling (invalid URL)\n");
    try {
        auto url = std::make_shared<interpreter::Value>(std::string("not-a-valid-url"));
        auto response = http_module.call("get", {url});
        fmt::print("  ✗ Should have thrown an error\n");
    } catch (const std::exception& e) {
        fmt::print("  ✓ Correctly caught error: {}\n", e.what());
    }

    fmt::print("\n=== HTTP tests complete! ===\n");

    return 0;
}
