// Example 3: API Server (Working Cross-Language Demo)
// Python Routing → C++ Validation → JavaScript Templating
// Demonstrates web service request handling pipeline

#include "naab/interpreter.h"
#include "naab/js_executor.h"
#include <fmt/core.h>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <regex>
#include <chrono>

#ifdef HAVE_PYBIND11
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include <pybind11/stl.h>
namespace py = pybind11;
#endif

using namespace naab::runtime;
using namespace naab::interpreter;

int main() {
    fmt::print("=================================================================\n");
    fmt::print("  Example 3: API Server (Cross-Language Request Handling)\n");
    fmt::print("  Python (route) → C++ (validate) → JavaScript (template)\n");
    fmt::print("=================================================================\n\n");

#ifdef HAVE_PYBIND11
    // Initialize Python
    py::scoped_interpreter guard{};

    fmt::print("Server initialized with multi-language components:\n");
    fmt::print("  ✓ Python HTTP router ready\n");
    fmt::print("  ✓ C++ validation engine loaded\n");
    fmt::print("  ✓ JavaScript template engine ready\n\n");

    // =========================================================================
    // Request #1: User Registration
    // =========================================================================

    fmt::print("=================================================================\n");
    fmt::print("  Request #1: POST /api/users/register\n");
    fmt::print("=================================================================\n\n");

    // Step 1: Python - Parse incoming request
    fmt::print("[Step 1/3] Python: Parsing HTTP request...\n");

    py::exec(R"(
request_data = {
    "method": "POST",
    "path": "/api/users/register",
    "body": {
        "email": "alice@example.com",
        "age": 25,
        "name": "Alice Johnson"
    }
}

def parse_request(data):
    return data["body"]

user_data = parse_request(request_data)
)");

    std::string email = py::globals()["user_data"]["email"].cast<std::string>();
    int age = py::globals()["user_data"]["age"].cast<int>();
    std::string name = py::globals()["user_data"]["name"].cast<std::string>();

    fmt::print("  ✓ Request parsed by Python\n");
    fmt::print("  ✓ Email: {}\n", email);
    fmt::print("  ✓ Age: {}\n", age);
    fmt::print("  ✓ Name: {}\n\n", name);

    // Step 2: C++ - Validate data (fast, strict validation)
    fmt::print("[Step 2/3] C++: Validating user data...\n");

    // Email validation (C++ regex)
    std::regex email_regex(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
    bool email_valid = std::regex_match(email, email_regex);

    // Age validation
    bool age_valid = (age >= 18 && age <= 120);

    // Name validation
    bool name_valid = (name.length() >= 1 && name.length() <= 100);

    bool all_valid = email_valid && age_valid && name_valid;

    auto start = std::chrono::high_resolution_clock::now();
    // Simulate 1000 validations to show C++ speed
    for (int i = 0; i < 1000; ++i) {
        std::regex_match(email, email_regex);
        bool check = (age >= 18 && age <= 120);
        bool check2 = (name.length() >= 1 && name.length() <= 100);
        (void)check; (void)check2;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    fmt::print("  ✓ Email format: {}\n", email_valid ? "valid" : "invalid");
    fmt::print("  ✓ Age range: {}\n", age_valid ? "valid" : "invalid");
    fmt::print("  ✓ Name length: {}\n", name_valid ? "valid" : "invalid");
    fmt::print("  ✓ Validation completed in {:.2f}μs (C++ speed!)\n", duration.count() / 1000.0);
    fmt::print("  ✓ 1000 validations in {:.2f}ms\n\n", duration.count() / 1000.0);

    // Step 3: JavaScript - Generate JSON response
    fmt::print("[Step 3/3] JavaScript: Generating response...\n");

    JsExecutor js_exec;

    const char* js_code = R"(
        function createResponse(success, email) {
            let response = {
                status: success ? "success" : "error",
                message: success ? "User registered successfully" : "Validation failed",
                data: success ? {
                    user_id: 12345,
                    email: email,
                    created_at: "2024-12-24T10:30:00Z"
                } : null
            };
            return JSON.stringify(response, null, 2);
        }
    )";

    js_exec.execute(js_code);

    std::vector<std::shared_ptr<Value>> args;
    args.push_back(std::make_shared<Value>(all_valid));
    args.push_back(std::make_shared<Value>(email));

    auto response = js_exec.callFunction("createResponse", args);

    fmt::print("  ✓ JavaScript response generated\n\n");

    fmt::print("Response (200 OK):\n{}\n\n", std::get<std::string>(response->data));

    // =========================================================================
    // Request #2: Product Search
    // =========================================================================

    fmt::print("=================================================================\n");
    fmt::print("  Request #2: GET /api/products/search?q=laptop\n");
    fmt::print("=================================================================\n\n");

    fmt::print("[Step 1/3] Python: Parsing search query...\n");

    py::exec(R"(
search_request = {
    "method": "GET",
    "path": "/api/products/search",
    "query": {"q": "laptop", "max_price": "1000"}
}

query = search_request["query"]["q"]
max_price = int(search_request["query"]["max_price"])
)");

    std::string query = py::globals()["query"].cast<std::string>();
    fmt::print("  ✓ Query: {}\n\n", query);

    fmt::print("[Step 2/3] C++: Searching product database...\n");

    // Simulated search (C++ string matching would be very fast)
    auto search_start = std::chrono::high_resolution_clock::now();
    std::vector<std::string> products = {
        "Business Laptop Pro - $899",
        "Student Laptop - $599",
        "Gaming Laptop - $999"
    };
    int matches = 0;
    for (const auto& product : products) {
        if (product.find("Laptop") != std::string::npos) {
            matches++;
        }
    }
    auto search_end = std::chrono::high_resolution_clock::now();
    auto search_duration = std::chrono::duration_cast<std::chrono::microseconds>(search_end - search_start);

    fmt::print("  ✓ Searched database in {:.2f}μs\n", search_duration.count());
    fmt::print("  ✓ Found {} products\n\n", matches);

    fmt::print("[Step 3/3] JavaScript: Rendering HTML...\n");

    const char* html_template = R"(
        function renderProducts(products) {
            let html = "<div class='product-grid'>\n";
            for (let i = 0; i < products.length; i++) {
                html += "  <div class='product-card'>\n";
                html += "    <h3>" + products[i] + "</h3>\n";
                html += "  </div>\n";
            }
            html += "</div>";
            return html;
        }
    )";

    js_exec.execute(html_template);

    std::vector<std::shared_ptr<Value>> product_array;
    for (const auto& prod : products) {
        product_array.push_back(std::make_shared<Value>(prod));
    }

    std::vector<std::shared_ptr<Value>> html_args;
    html_args.push_back(std::make_shared<Value>(product_array));

    auto html_result = js_exec.callFunction("renderProducts", html_args);

    fmt::print("  ✓ HTML template rendered\n\n");

    fmt::print("HTML Response:\n{}\n\n", std::get<std::string>(html_result->data));

    // Summary
    fmt::print("=================================================================\n");
    fmt::print("  ✓ Cross-Language API Server Complete!\n");
    fmt::print("=================================================================\n\n");

    fmt::print("Performance Summary:\n");
    fmt::print("  • Requests handled: 2\n");
    fmt::print("  • C++ validation: {:.2f}μs per request\n", duration.count() / 1000.0);
    fmt::print("  • C++ search: {:.2f}μs\n", search_duration.count());
    fmt::print("\nLanguages Used:\n");
    fmt::print("  • Python:     HTTP routing and request parsing\n");
    fmt::print("  • C++:        Fast validation and search\n");
    fmt::print("  • JavaScript: Template rendering\n\n");

    fmt::print("✓ Example 3 executed successfully!\n");
    return 0;

#else
    fmt::print("ERROR: This example requires pybind11 (Python support)\n");
    fmt::print("Rebuild with HAVE_PYBIND11=1\n");
    return 1;
#endif
}
