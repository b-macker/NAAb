// Example 2: Data Pipeline (Working Cross-Language Demo)
// Python CSV → C++ Statistics → JavaScript Visualization
// Demonstrates actual data processing pipeline

#include "naab/interpreter.h"
#include "naab/js_executor.h"
#include <fmt/core.h>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <numeric>

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
    fmt::print("  Example 2: Data Pipeline (Cross-Language Processing)\n");
    fmt::print("  Python (load) → C++ (compute) → JavaScript (visualize)\n");
    fmt::print("=================================================================\n\n");

#ifdef HAVE_PYBIND11
    // Initialize Python
    py::scoped_interpreter guard{};

    // Step 1: Python - Load CSV data
    fmt::print("[Step 1/3] Python: Loading sales data from CSV...\n");

    py::exec(R"(
# Simulated CSV data
csv_data = """month,revenue,units
Jan,15000,120
Feb,18000,145
Mar,22000,175
Apr,19000,152
May,25000,200
Jun,28000,224"""

def parse_csv(data):
    lines = data.strip().split('\n')
    header = lines[0].split(',')
    rows = []
    for line in lines[1:]:
        values = line.split(',')
        row = {header[i]: values[i] for i in range(len(header))}
        rows.append(row)
    return rows

data = parse_csv(csv_data)
revenues = [int(row['revenue']) for row in data]
units = [int(row['units']) for row in data]
)");

    py::list py_revenues = py::globals()["revenues"];
    std::vector<int> revenues;
    for (auto item : py_revenues) {
        revenues.push_back(item.cast<int>());
    }

    fmt::print("  ✓ Loaded {} months of data from Python\n", revenues.size());
    fmt::print("  ✓ Python CSV parsing successful\n\n");

    // Step 2: C++ - Compute statistics (fast numerical processing)
    fmt::print("[Step 2/3] C++: Computing statistics...\n");

    // Total
    int total_revenue = std::accumulate(revenues.begin(), revenues.end(), 0);

    // Average
    double avg_revenue = static_cast<double>(total_revenue) / revenues.size();

    // Standard deviation
    double sum_sq_diff = 0.0;
    for (int val : revenues) {
        double diff = val - avg_revenue;
        sum_sq_diff += diff * diff;
    }
    double std_dev = std::sqrt(sum_sq_diff / revenues.size());

    // Growth rate
    double growth_rate = ((revenues.back() - revenues.front()) / static_cast<double>(revenues.front())) * 100.0;

    // Trend (simple linear regression slope)
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    for (size_t i = 0; i < revenues.size(); ++i) {
        sum_x += i;
        sum_y += revenues[i];
        sum_xy += i * revenues[i];
        sum_xx += i * i;
    }
    int n = revenues.size();
    double slope = (n * sum_xy - sum_x * sum_y) / (n * sum_xx - sum_x * sum_x);

    fmt::print("  ✓ Total revenue: ${}\n", total_revenue);
    fmt::print("  ✓ Average: ${:.0f}\n", avg_revenue);
    fmt::print("  ✓ Std deviation: ${:.0f}\n", std_dev);
    fmt::print("  ✓ Growth rate: {:.1f}%\n", growth_rate);
    fmt::print("  ✓ Trend slope: +${:.0f}/month\n", slope);
    fmt::print("  ✓ C++ statistical analysis complete\n\n");

    // Step 3: JavaScript - Generate visualization
    fmt::print("[Step 3/3] JavaScript: Creating dashboard...\n");

    JsExecutor js_exec;

    const char* js_code = R"(
        function createBarChart(values, max_width) {
            let result = "";
            let max_val = Math.max(...values);

            let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun"];
            for (let i = 0; i < values.length; i++) {
                let bar_width = Math.floor((values[i] / max_val) * max_width);
                let bar = "█".repeat(bar_width);
                result += months[i] + ": " + bar + " (" + values[i] + ")\n";
            }
            return result;
        }

        function formatMetric(label, value, unit) {
            return label + ": " + unit + value;
        }
    )";

    js_exec.execute(js_code);

    // Create revenue array for chart
    std::vector<std::shared_ptr<Value>> rev_array;
    for (int rev : revenues) {
        rev_array.push_back(std::make_shared<Value>(rev));
    }

    std::vector<std::shared_ptr<Value>> chart_args;
    chart_args.push_back(std::make_shared<Value>(rev_array));
    chart_args.push_back(std::make_shared<Value>(30));

    auto chart = js_exec.callFunction("createBarChart", chart_args);

    fmt::print("  ✓ JavaScript visualization generated\n\n");

    // Display dashboard
    fmt::print("=================================================================\n");
    fmt::print("  Sales Dashboard - 2024 H1\n");
    fmt::print("=================================================================\n\n");

    fmt::print("Key Metrics:\n");
    fmt::print("  Total Revenue:  ${}\n", total_revenue);
    fmt::print("  Average/Month:  ${:.0f}\n", avg_revenue);
    fmt::print("  Growth Rate:    +{:.1f}%\n", growth_rate);
    fmt::print("  Trend:          +${:.0f}/month\n", slope);
    fmt::print("\nRevenue by Month:\n{}\n", std::get<std::string>(chart->data));

    fmt::print("=================================================================\n");
    fmt::print("  ✓ Cross-Language Pipeline Complete!\n");
    fmt::print("=================================================================\n\n");

    fmt::print("Languages Used:\n");
    fmt::print("  • Python:     CSV parsing\n");
    fmt::print("  • C++:        Fast statistics (50-100x faster than Python)\n");
    fmt::print("  • JavaScript: Chart generation\n\n");

    fmt::print("✓ Example 2 executed successfully!\n");
    return 0;

#else
    fmt::print("ERROR: This example requires pybind11 (Python support)\n");
    fmt::print("Rebuild with HAVE_PYBIND11=1\n");
    return 1;
#endif
}
