// Cross-Language Integration Tests
// Tests Python ↔ C++ ↔ JavaScript type marshalling and function calls

#include "naab/js_executor.h"
#include <fmt/core.h>
#include <iostream>
#include <cassert>
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

#ifdef HAVE_PYBIND11
#include "naab/python_executor.h"
#include "naab/cross_language_bridge.h"
#endif

using namespace naab::runtime;

// Minimal Value type for testing (to avoid full interpreter dependency)
namespace test_interpreter {
    using ValueData = std::variant<
        std::monostate,
        int,
        double,
        bool,
        std::string,
        std::vector<std::shared_ptr<struct Value>>,
        std::unordered_map<std::string, std::shared_ptr<struct Value>>
    >;

    struct Value {
        ValueData data;

        Value() : data(std::monostate{}) {}
        explicit Value(int v) : data(v) {}
        explicit Value(double v) : data(v) {}
        explicit Value(bool v) : data(v) {}
        explicit Value(std::string v) : data(std::move(v)) {}
        explicit Value(std::vector<std::shared_ptr<Value>> v) : data(std::move(v)) {}
        explicit Value(std::unordered_map<std::string, std::shared_ptr<Value>> v) : data(std::move(v)) {}
    };
}

using Value = test_interpreter::Value;

// Test utilities
int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) \
    fmt::print("\n[TEST] {}\n", name); \
    try {

#define TEST_END \
        tests_passed++; \
        fmt::print("[PASS] Test passed\n"); \
    } catch (const std::exception& e) { \
        tests_failed++; \
        fmt::print("[FAIL] Test failed: {}\n", e.what()); \
    }

#define ASSERT(condition, message) \
    if (!(condition)) { \
        throw std::runtime_error(message); \
    }

// ============================================================================
// Test 1: C++ → JavaScript Function Call
// ============================================================================

void test_cpp_to_js_call() {
    TEST("C++ → JavaScript: Function Call")

    JsExecutor js_exec;

    // Define JavaScript function
    const char* js_code = R"(
        function add(a, b) {
            return a + b;
        }

        function greet(name) {
            return "Hello, " + name + "!";
        }

        function double_numbers(arr) {
            var result = [];
            for (var i = 0; i < arr.length; i++) {
                result.push(arr[i] * 2);
            }
            return result;
        }
    )";

    bool success = js_exec.execute(js_code);
    ASSERT(success, "Failed to execute JavaScript code");
    fmt::print("  ✓ JavaScript functions defined\n");

    // Test add(15, 27)
    std::vector<std::shared_ptr<Value>> args;
    args.push_back(std::make_shared<Value>(15));
    args.push_back(std::make_shared<Value>(27));

    auto result = js_exec.callFunction("add", args);
    ASSERT(std::get<int>(result->data) == 42, "add(15, 27) should return 42");
    fmt::print("  ✓ add(15, 27) = 42\n");

    // Test greet("NAAb")
    args.clear();
    args.push_back(std::make_shared<Value>(std::string("NAAb")));

    result = js_exec.callFunction("greet", args);
    ASSERT(std::get<std::string>(result->data) == "Hello, NAAb!",
           "greet('NAAb') failed");
    fmt::print("  ✓ greet('NAAb') = 'Hello, NAAb!'\n");

    TEST_END
}

// ============================================================================
// Test 2: JavaScript Expression Evaluation
// ============================================================================

void test_js_evaluation() {
    TEST("JavaScript: Expression Evaluation")

    JsExecutor js_exec;

    // Test number expression
    auto result = js_exec.evaluate("10 + 20 * 2");
    ASSERT(std::get<int>(result->data) == 50, "10 + 20 * 2 should be 50");
    fmt::print("  ✓ 10 + 20 * 2 = 50\n");

    // Test string expression
    result = js_exec.evaluate("'Hello' + ' ' + 'World'");
    ASSERT(std::get<std::string>(result->data) == "Hello World",
           "String concatenation failed");
    fmt::print("  ✓ 'Hello' + ' ' + 'World' = 'Hello World'\n");

    // Test boolean expression
    result = js_exec.evaluate("true && false");
    ASSERT(std::get<bool>(result->data) == false, "true && false should be false");
    fmt::print("  ✓ true && false = false\n");

    TEST_END
}

// ============================================================================
// Test 3: Python → C++ Type Conversions
// ============================================================================

#ifdef HAVE_PYBIND11
void test_python_cpp_types() {
    TEST("Python ↔ C++: Basic Types")

    CrossLanguageBridge bridge;

    // Test int
    py::int_ py_int(42);
    auto cpp_val = bridge.pythonToValue(py_int);
    ASSERT(std::get<int>(cpp_val->data) == 42, "Python int conversion failed");
    auto back_py = bridge.valueToPython(cpp_val);
    ASSERT(back_py.cast<int>() == 42, "C++ → Python int failed");
    fmt::print("  ✓ Int: 42 (Python ↔ C++)\n");

    // Test float
    py::float_ py_float(3.14159);
    cpp_val = bridge.pythonToValue(py_float);
    ASSERT(std::abs(std::get<double>(cpp_val->data) - 3.14159) < 0.0001,
           "Python float conversion failed");
    back_py = bridge.valueToPython(cpp_val);
    ASSERT(std::abs(back_py.cast<double>() - 3.14159) < 0.0001,
           "C++ → Python float failed");
    fmt::print("  ✓ Float: 3.14159 (Python ↔ C++)\n");

    // Test string
    py::str py_str("Hello from Python");
    cpp_val = bridge.pythonToValue(py_str);
    ASSERT(std::get<std::string>(cpp_val->data) == "Hello from Python",
           "Python string conversion failed");
    back_py = bridge.valueToPython(cpp_val);
    ASSERT(back_py.cast<std::string>() == "Hello from Python",
           "C++ → Python string failed");
    fmt::print("  ✓ String: 'Hello from Python' (Python ↔ C++)\n");

    // Test list
    py::list py_list;
    py_list.append(1);
    py_list.append(2);
    py_list.append(3);

    cpp_val = bridge.pythonToValue(py_list);
    auto& arr = std::get<std::vector<std::shared_ptr<Value>>>(cpp_val->data);
    ASSERT(arr.size() == 3, "Python list size wrong");
    ASSERT(std::get<int>(arr[0]->data) == 1, "List element 0 wrong");
    ASSERT(std::get<int>(arr[1]->data) == 2, "List element 1 wrong");
    ASSERT(std::get<int>(arr[2]->data) == 3, "List element 2 wrong");
    fmt::print("  ✓ List: [1, 2, 3] (Python → C++)\n");

    // Test dict
    py::dict py_dict;
    py_dict["name"] = "NAAb";
    py_dict["version"] = 1;

    cpp_val = bridge.pythonToValue(py_dict);
    auto& dict = std::get<std::unordered_map<std::string, std::shared_ptr<Value>>>(cpp_val->data);
    ASSERT(dict.size() == 2, "Python dict size wrong");
    ASSERT(std::get<std::string>(dict["name"]->data) == "NAAb", "Dict name wrong");
    ASSERT(std::get<int>(dict["version"]->data) == 1, "Dict version wrong");
    fmt::print("  ✓ Dict: {{'name': 'NAAb', 'version': 1}} (Python → C++)\n");

    TEST_END
}

// ============================================================================
// Test 4: Python → C++ Function Call
// ============================================================================

void test_python_to_cpp_call() {
    TEST("Python → C++: Function Call")

    PythonExecutor py_exec;

    // Define Python functions
    const char* py_code = R"(
def multiply(a, b):
    return a * b

def concat_strings(s1, s2):
    return s1 + " " + s2

def sum_list(numbers):
    return sum(numbers)

def create_dict():
    return {"status": "ok", "value": 100}
)";

    py_exec.execute(py_code);
    fmt::print("  ✓ Python functions defined\n");

    // Test multiply(6, 7)
    std::vector<std::shared_ptr<Value>> args;
    args.push_back(std::make_shared<Value>(6));
    args.push_back(std::make_shared<Value>(7));

    auto result = py_exec.callFunction("multiply", args);
    ASSERT(std::get<int>(result->data) == 42, "multiply(6, 7) should return 42");
    fmt::print("  ✓ multiply(6, 7) = 42\n");

    // Test concat_strings("Hello", "World")
    args.clear();
    args.push_back(std::make_shared<Value>(std::string("Hello")));
    args.push_back(std::make_shared<Value>(std::string("World")));

    result = py_exec.callFunction("concat_strings", args);
    ASSERT(std::get<std::string>(result->data) == "Hello World",
           "concat_strings failed");
    fmt::print("  ✓ concat_strings('Hello', 'World') = 'Hello World'\n");

    // Test sum_list([1, 2, 3, 4, 5])
    std::vector<std::shared_ptr<Value>> numbers;
    for (int i = 1; i <= 5; ++i) {
        numbers.push_back(std::make_shared<Value>(i));
    }

    args.clear();
    args.push_back(std::make_shared<Value>(numbers));

    result = py_exec.callFunction("sum_list", args);
    ASSERT(std::get<int>(result->data) == 15, "sum_list([1,2,3,4,5]) should return 15");
    fmt::print("  ✓ sum_list([1, 2, 3, 4, 5]) = 15\n");

    TEST_END
}

// ============================================================================
// Test 5: Simulated Multi-Language Pipeline
// ============================================================================

void test_multi_language_pipeline() {
    TEST("Multi-Language Pipeline: Python → C++ → JS")

    CrossLanguageBridge bridge;
    PythonExecutor py_exec;
    JsExecutor js_exec;

    // Step 1: Python creates data
    py_exec.execute("data_value = 10");
    py_exec.execute("multiplier = 3");
    fmt::print("  Step 1: Python created data_value=10, multiplier=3\n");

    // Step 2: Python computes result
    std::vector<std::shared_ptr<Value>> args;
    args.push_back(std::make_shared<Value>(10));
    args.push_back(std::make_shared<Value>(3));

    py_exec.execute("def compute(a, b): return a * b");
    auto py_result = py_exec.callFunction("compute", args);
    int computed = std::get<int>(py_result->data);
    fmt::print("  Step 2: Python computed: 10 * 3 = {}\n", computed);

    // Step 3: Pass to JavaScript for formatting
    js_exec.execute("function format_result(num) { return 'Result: ' + num; }");

    args.clear();
    args.push_back(std::make_shared<Value>(computed));
    auto js_result = js_exec.callFunction("format_result", args);
    std::string formatted = std::get<std::string>(js_result->data);

    ASSERT(formatted == "Result: 30", "Formatted result should be 'Result: 30'");
    fmt::print("  Step 3: JavaScript formatted: '{}'\n", formatted);

    fmt::print("  ✓ Complete pipeline successful\n");

    TEST_END
}

// ============================================================================
// Test 6: Type Information
// ============================================================================

void test_type_info() {
    TEST("CrossLanguageBridge: Type Information")

    CrossLanguageBridge bridge;

    auto val_int = std::make_shared<Value>(42);
    ASSERT(bridge.getTypeName(val_int) == "int", "Type name for int failed");
    ASSERT(bridge.isMarshallable(val_int), "Int should be marshallable");
    fmt::print("  ✓ Type: int (marshallable)\n");

    auto val_str = std::make_shared<Value>(std::string("test"));
    ASSERT(bridge.getTypeName(val_str) == "string", "Type name for string failed");
    ASSERT(bridge.isMarshallable(val_str), "String should be marshallable");
    fmt::print("  ✓ Type: string (marshallable)\n");

    std::vector<std::shared_ptr<Value>> arr;
    arr.push_back(val_int);
    auto val_arr = std::make_shared<Value>(arr);
    ASSERT(bridge.getTypeName(val_arr) == "array", "Type name for array failed");
    ASSERT(bridge.isMarshallable(val_arr), "Array should be marshallable");
    fmt::print("  ✓ Type: array (marshallable)\n");

    TEST_END
}
#endif // HAVE_PYBIND11

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    fmt::print("=================================================================\n");
    fmt::print("  NAAb Cross-Language Integration Tests\n");
    fmt::print("=================================================================\n");

    // Always run C++ ↔ JavaScript tests
    test_cpp_to_js_call();
    test_js_evaluation();

#ifdef HAVE_PYBIND11
    // Run Python tests only if pybind11 is available
    test_python_cpp_types();
    test_python_to_cpp_call();
    test_multi_language_pipeline();
    test_type_info();
#else
    fmt::print("\n[SKIP] Python tests (pybind11 not available)\n");
#endif

    // Summary
    fmt::print("\n=================================================================\n");
    fmt::print("  Test Summary\n");
    fmt::print("=================================================================\n");
    fmt::print("Passed: {}\n", tests_passed);
    fmt::print("Failed: {}\n", tests_failed);
    fmt::print("Total:  {}\n", tests_passed + tests_failed);

    if (tests_failed == 0) {
        fmt::print("\n✓ ALL TESTS PASSED\n");
        return 0;
    } else {
        fmt::print("\n✗ SOME TESTS FAILED\n");
        return 1;
    }
}
