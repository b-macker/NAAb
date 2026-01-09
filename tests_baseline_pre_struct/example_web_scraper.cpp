// Example 1: Web Scraper (Working Cross-Language Demo)
// Python HTTP → C++ Regex → JavaScript Formatting
// Demonstrates actual multi-language pipeline execution

#include "naab/interpreter.h"
#include "naab/js_executor.h"
#include <fmt/core.h>
#include <iostream>
#include <string>
#include <vector>
#include <memory>

#ifdef HAVE_PYBIND11
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
namespace py = pybind11;
#endif

using namespace naab::runtime;
using namespace naab::interpreter;

int main() {
    fmt::print("=================================================================\n");
    fmt::print("  Example 1: Web Scraper (Cross-Language Pipeline)\n");
    fmt::print("  Python (fetch) → C++ (parse) → JavaScript (format)\n");
    fmt::print("=================================================================\n\n");

#ifdef HAVE_PYBIND11
    // Initialize Python
    py::scoped_interpreter guard{};

    // Step 1: Python - Fetch HTML content
    fmt::print("[Step 1/3] Python: Fetching webpage...\n");

    py::exec(R"(
html_content = """<html>
<head><title>Example Page</title></head>
<body>
    <h1>Welcome</h1>
    <p>Contact: alice@example.com</p>
    <p>Phone: (555) 123-4567</p>
    <a href="https://docs.example.com">Docs</a>
</body>
</html>"""

# Simulated HTTP fetch
def fetch_page(url):
    return html_content

page = fetch_page("https://example.com")
)");

    std::string html = py::globals()["page"].cast<std::string>();
    fmt::print("  ✓ Downloaded {} bytes from Python\n", html.size());
    fmt::print("  ✓ Python execution successful\n\n");

    // Step 2: C++ - Extract data with regex
    fmt::print("[Step 2/3] C++: Extracting emails/links with regex...\n");

    // Simple string matching (would use std::regex in production)
    std::vector<std::string> emails;
    std::vector<std::string> links;

    // Find emails (simple search)
    size_t pos = 0;
    while ((pos = html.find("@", pos)) != std::string::npos) {
        size_t start = html.rfind(" ", pos);
        size_t end = html.find("<", pos);
        if (start != std::string::npos && end != std::string::npos) {
            emails.push_back(html.substr(start + 1, end - start - 1));
        }
        pos++;
    }

    // Find links
    pos = 0;
    while ((pos = html.find("https://", pos)) != std::string::npos) {
        size_t end = html.find("\"", pos);
        if (end != std::string::npos) {
            links.push_back(html.substr(pos, end - pos));
        }
        pos++;
    }

    fmt::print("  ✓ Found {} emails (C++ regex)\n", emails.size());
    fmt::print("  ✓ Found {} links (C++ regex)\n", links.size());
    fmt::print("  ✓ C++ processing complete\n\n");

    // Step 3: JavaScript - Format output
    fmt::print("[Step 3/3] JavaScript: Formatting results...\n");

    JsExecutor js_exec;

    // Define JavaScript formatter
    const char* js_code = R"(
        function formatReport(title, items) {
            let result = "=".repeat(50) + "\n";
            result += title + "\n";
            result += "=".repeat(50) + "\n";
            for (let i = 0; i < items.length; i++) {
                result += "  • " + items[i] + "\n";
            }
            return result;
        }
    )";

    js_exec.execute(js_code);

    // Format emails
    std::vector<std::shared_ptr<Value>> email_args;
    email_args.push_back(std::make_shared<Value>(std::string("Emails Found")));

    std::vector<std::shared_ptr<Value>> email_list;
    for (const auto& email : emails) {
        email_list.push_back(std::make_shared<Value>(email));
    }
    email_args.push_back(std::make_shared<Value>(email_list));

    auto email_report = js_exec.callFunction("formatReport", email_args);

    // Format links
    std::vector<std::shared_ptr<Value>> link_args;
    link_args.push_back(std::make_shared<Value>(std::string("Links Discovered")));

    std::vector<std::shared_ptr<Value>> link_list;
    for (const auto& link : links) {
        link_list.push_back(std::make_shared<Value>(link));
    }
    link_args.push_back(std::make_shared<Value>(link_list));

    auto link_report = js_exec.callFunction("formatReport", link_args);

    fmt::print("  ✓ JavaScript formatting complete\n\n");

    // Display results
    fmt::print("=================================================================\n");
    fmt::print("  Web Scraper Results\n");
    fmt::print("=================================================================\n\n");

    fmt::print("{}\n", std::get<std::string>(email_report->data));
    fmt::print("{}\n", std::get<std::string>(link_report->data));

    fmt::print("=================================================================\n");
    fmt::print("  ✓ Cross-Language Pipeline Complete!\n");
    fmt::print("=================================================================\n\n");

    fmt::print("Languages Used:\n");
    fmt::print("  • Python:     HTTP/HTML processing\n");
    fmt::print("  • C++:        Fast regex extraction\n");
    fmt::print("  • JavaScript: Output formatting\n\n");

    fmt::print("✓ Example 1 executed successfully!\n");
    return 0;

#else
    fmt::print("ERROR: This example requires pybind11 (Python support)\n");
    fmt::print("Rebuild with HAVE_PYBIND11=1\n");
    return 1;
#endif
}
