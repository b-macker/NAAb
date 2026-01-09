# Block Testing Framework - Complete Implementation

## Overview

The Block Testing Framework provides automated testing for individual NAAb blocks before they're used in production. It enables block developers to define tests with assertions and run them systematically.

**Status**: ✅ **COMPLETE** (Phase 6d)

---

## Features

✅ **Test Definitions** - JSON-based test specifications
✅ **Multiple Assertions** - Equals, not-equals, greater-than, less-than, contains, type checks
✅ **Language Agnostic** - Works with any registered language executor
✅ **Isolated Testing** - Tests run in isolated contexts
✅ **Performance Tracking** - Measures execution time per test
✅ **Detailed Reporting** - Clear pass/fail results with error messages
✅ **Batch Testing** - Run all tests for a block at once

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Test Definition (JSON)                                          │
│  {                                                               │
│    "block_id": "JS-MATH-001",                                   │
│    "language": "javascript",                                     │
│    "tests": [                                                    │
│      {                                                           │
│        "name": "add_test",                                      │
│        "code": "function add(a, b) { return a + b; }",         │
│        "assertions": [{"type": "equals", "expected": "8"}]      │
│      }                                                           │
│    ]                                                             │
│  }                                                               │
└─────────────────────────────────┬───────────────────────────────┘
                                  ↓
┌─────────────────────────────────────────────────────────────────┐
│  BlockTester                                                     │
│  1. loadTestDefinition() - Parse JSON                            │
│  2. runTests() - Execute all tests                               │
│  3. runSingleTest() - Execute one test                           │
│  4. checkAssertion() - Verify assertion                          │
└─────────────────────────────────┬───────────────────────────────┘
                                  ↓
┌─────────────────────────────────────────────────────────────────┐
│  LanguageRegistry                                                │
│  • Get executor for test language                                │
│  • Execute test code                                             │
│  • Call functions with test arguments                            │
└─────────────────────────────────┬───────────────────────────────┘
                                  ↓
┌─────────────────────────────────────────────────────────────────┐
│  Test Results                                                    │
│  • Total: 3                                                      │
│  • Passed: 2                                                     │
│  • Failed: 1                                                     │
│  • Execution times                                               │
└─────────────────────────────────────────────────────────────────┘
```

---

## Components

### 1. Test Structures (`block_tester.h`)

#### AssertionType Enum

```cpp
enum class AssertionType {
    EQUALS,          // value == expected
    NOT_EQUALS,      // value != expected
    GREATER_THAN,    // value > expected
    LESS_THAN,       // value < expected
    CONTAINS,        // string contains substring
    TYPE_IS          // check value type
};
```

#### Assertion Structure

```cpp
struct Assertion {
    AssertionType type;
    std::string value_expr;  // Expression to evaluate (e.g., "add(5, 3)")
    std::string expected;    // Expected value
};
```

#### BlockTest Structure

```cpp
struct BlockTest {
    std::string name;
    std::string code;                    // Code to execute
    std::vector<Assertion> assertions;   // Assertions to check
};
```

#### BlockTestDefinition Structure

```cpp
struct BlockTestDefinition {
    std::string block_id;
    std::string language;
    std::string setup_code;              // Optional setup code
    std::vector<BlockTest> tests;
};
```

#### Test Results

```cpp
struct TestResult {
    std::string test_name;
    bool passed;
    std::string error_message;
    double execution_time_ms;
};

struct TestResults {
    std::string block_id;
    int total;
    int passed;
    int failed;
    std::vector<TestResult> results;

    bool allPassed() const { return failed == 0; }
};
```

### 2. BlockTester Class (`block_tester.cpp`)

Main testing framework class:

```cpp
class BlockTester {
public:
    BlockTester();

    // Load test definition from JSON file
    bool loadTestDefinition(const std::string& test_file_path);

    // Run all tests for the loaded block
    TestResults runTests();

    // Run tests for a specific block ID (loads from standard location)
    TestResults runTestsForBlock(const std::string& block_id);

private:
    BlockTestDefinition definition_;

    // Run a single test case
    TestResult runSingleTest(const BlockTest& test);

    // Check a single assertion
    bool checkAssertion(const Assertion& assertion,
                        const std::shared_ptr<interpreter::Value>& result,
                        std::string& error_message);
};
```

---

## Test Definition Format (JSON)

### Basic Structure

```json
{
  "block_id": "JS-MATH-001",
  "language": "javascript",
  "setup_code": "",
  "tests": [
    {
      "name": "add_positive_numbers",
      "code": "function add(a, b) { return a + b; }",
      "assertions": [
        {
          "type": "equals",
          "value_expr": "add(5, 3)",
          "expected": "8"
        }
      ]
    }
  ]
}
```

### Fields

- **block_id**: Unique identifier for the block
- **language**: Language of the block (cpp, javascript, python, etc.)
- **setup_code**: Optional code executed before tests (shared state)
- **tests**: Array of test cases

### Test Case Fields

- **name**: Descriptive test name
- **code**: Code to execute for this test
- **assertions**: Array of assertions to check

### Assertion Fields

- **type**: Assertion type (equals, not_equals, greater_than, less_than, contains, type_is)
- **value_expr**: Expression to evaluate (function call or variable)
- **expected**: Expected value as string

---

## Usage

### 1. Create Test Definition

Create a JSON file in `examples/block_tests/`:

**examples/block_tests/JS-MATH-001.test.json**:
```json
{
  "block_id": "JS-MATH-001",
  "language": "javascript",
  "tests": [
    {
      "name": "add_positive_numbers",
      "code": "function add(a, b) { return a + b; }",
      "assertions": [
        {
          "type": "equals",
          "value_expr": "add(5, 3)",
          "expected": "8"
        }
      ]
    },
    {
      "name": "multiply_numbers",
      "code": "function multiply(a, b) { return a * b; }",
      "assertions": [
        {
          "type": "equals",
          "value_expr": "multiply(7, 6)",
          "expected": "42"
        }
      ]
    }
  ]
}
```

### 2. Run Tests from C++

```cpp
#include "naab/block_tester.h"
#include "naab/language_registry.h"

// Initialize language registry
auto& registry = LanguageRegistry::instance();
registry.registerExecutor("javascript", std::make_unique<JsExecutorAdapter>());

// Create tester
testing::BlockTester tester;

// Load and run tests
auto results = tester.runTestsForBlock("JS-MATH-001");

// Check results
if (results.allPassed()) {
    fmt::print("All tests passed!\n");
} else {
    fmt::print("{} tests failed\n", results.failed);
}
```

### 3. Manual Testing

```cpp
testing::BlockTester tester;

// Load specific test file
tester.loadTestDefinition("/path/to/test.json");

// Run tests
auto results = tester.runTests();

// Process results
for (const auto& result : results.results) {
    fmt::print("{}: {} ({:.2f}ms)\n",
               result.test_name,
               result.passed ? "PASS" : "FAIL",
               result.execution_time_ms);

    if (!result.passed) {
        fmt::print("  Error: {}\n", result.error_message);
    }
}
```

---

## Assertion Types

### EQUALS

Checks if value equals expected:

```json
{
  "type": "equals",
  "value_expr": "add(5, 3)",
  "expected": "8"
}
```

**Passes if**: `add(5, 3)` returns `8`

### NOT_EQUALS

Checks if value does not equal expected:

```json
{
  "type": "not_equals",
  "value_expr": "add(5, 3)",
  "expected": "10"
}
```

**Passes if**: `add(5, 3)` does not return `10`

### GREATER_THAN

Checks if value is greater than expected:

```json
{
  "type": "greater_than",
  "value_expr": "add(5, 3)",
  "expected": "5"
}
```

**Passes if**: `add(5, 3)` returns > `5` (e.g., `8`)

### LESS_THAN

Checks if value is less than expected:

```json
{
  "type": "less_than",
  "value_expr": "add(5, 3)",
  "expected": "10"
}
```

**Passes if**: `add(5, 3)` returns < `10` (e.g., `8`)

### CONTAINS

Checks if string contains substring:

```json
{
  "type": "contains",
  "value_expr": "greet('World')",
  "expected": "Hello"
}
```

**Passes if**: `greet('World')` returns a string containing `"Hello"`

### TYPE_IS

Checks if value has expected type:

```json
{
  "type": "type_is",
  "value_expr": "add(5, 3)",
  "expected": "int"
}
```

**Supported types**: `int`, `double`, `string`, `bool`

---

## Example Test Definitions

### JavaScript Math Functions

```json
{
  "block_id": "JS-MATH-001",
  "language": "javascript",
  "tests": [
    {
      "name": "add_positive_numbers",
      "code": "function add(a, b) { return a + b; }",
      "assertions": [
        {"type": "equals", "value_expr": "add(5, 3)", "expected": "8"},
        {"type": "type_is", "value_expr": "add(5, 3)", "expected": "int"}
      ]
    },
    {
      "name": "multiply_numbers",
      "code": "function multiply(a, b) { return a * b; }",
      "assertions": [
        {"type": "equals", "value_expr": "multiply(7, 6)", "expected": "42"}
      ]
    },
    {
      "name": "divide_by_zero",
      "code": "function divide(a, b) { if (b === 0) return 0; return a / b; }",
      "assertions": [
        {"type": "equals", "value_expr": "divide(10, 0)", "expected": "0"}
      ]
    }
  ]
}
```

### JavaScript String Functions

```json
{
  "block_id": "JS-STRING-001",
  "language": "javascript",
  "tests": [
    {
      "name": "uppercase_string",
      "code": "function toUpper(s) { return s.toUpperCase(); }",
      "assertions": [
        {"type": "equals", "value_expr": "toUpper('hello')", "expected": "HELLO"}
      ]
    },
    {
      "name": "greet_user",
      "code": "function greet(name) { return 'Hello, ' + name + '!'; }",
      "assertions": [
        {"type": "contains", "value_expr": "greet('World')", "expected": "Hello"},
        {"type": "contains", "value_expr": "greet('World')", "expected": "World"}
      ]
    }
  ]
}
```

### C++ Math Functions

```json
{
  "block_id": "CPP-MATH-001",
  "language": "cpp",
  "tests": [
    {
      "name": "add_integers",
      "code": "extern \"C\" { int add(int a, int b) { return a + b; } }",
      "assertions": [
        {"type": "equals", "value_expr": "add(10, 20)", "expected": "30"}
      ]
    },
    {
      "name": "multiply_integers",
      "code": "extern \"C\" { int multiply(int a, int b) { return a * b; } }",
      "assertions": [
        {"type": "equals", "value_expr": "multiply(5, 7)", "expected": "35"},
        {"type": "greater_than", "value_expr": "multiply(5, 7)", "expected": "30"}
      ]
    }
  ]
}
```

---

## Test Output

### Successful Test Run

```
=== Running tests for JS-MATH-001 ===

[SETUP] No setup code
✓ add_positive_numbers (1.23ms)
✓ multiply_numbers (0.87ms)
✓ divide_by_zero (0.95ms)

=== Test Summary ===
Total:  3
Passed: 3
Failed: 0
```

### Failed Test Run

```
=== Running tests for JS-MATH-001 ===

✓ add_positive_numbers (1.23ms)
✗ multiply_numbers - Expected: 42, Got: 41
✓ divide_by_zero (0.95ms)

=== Test Summary ===
Total:  3
Passed: 2
Failed: 1
```

---

## Testing Best Practices

### 1. Test One Thing Per Test

**Good**:
```json
{
  "name": "add_positive_numbers",
  "code": "function add(a, b) { return a + b; }",
  "assertions": [
    {"type": "equals", "value_expr": "add(5, 3)", "expected": "8"}
  ]
}
```

**Bad** (testing multiple things):
```json
{
  "name": "math_functions",
  "code": "...",
  "assertions": [
    {"type": "equals", "value_expr": "add(5, 3)", "expected": "8"},
    {"type": "equals", "value_expr": "multiply(7, 6)", "expected": "42"},
    {"type": "equals", "value_expr": "divide(10, 2)", "expected": "5"}
  ]
}
```

### 2. Use Descriptive Test Names

**Good**: `add_positive_numbers`, `multiply_by_zero`, `greet_empty_string`

**Bad**: `test1`, `test2`, `test_function`

### 3. Test Edge Cases

```json
{
  "tests": [
    {"name": "add_positive", "..."},
    {"name": "add_negative", "..."},
    {"name": "add_zero", "..."},
    {"name": "add_large_numbers", "..."}
  ]
}
```

### 4. Use Multiple Assertions for Complex Checks

```json
{
  "name": "greet_user",
  "code": "function greet(name) { return 'Hello, ' + name + '!'; }",
  "assertions": [
    {"type": "contains", "value_expr": "greet('World')", "expected": "Hello"},
    {"type": "contains", "value_expr": "greet('World')", "expected": "World"},
    {"type": "type_is", "value_expr": "greet('World')", "expected": "string"}
  ]
}
```

---

## Integration with Development Workflow

### 1. Pre-Commit Testing

Run tests before committing blocks:

```bash
# Run tests for all changed blocks
for block in $(git diff --name-only | grep ".test.json"); do
    ./naab-test $block
done
```

### 2. CI/CD Integration

```yaml
# .github/workflows/test-blocks.yml
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - name: Test Blocks
        run: |
          for test in examples/block_tests/*.test.json; do
            ./naab-test $test || exit 1
          done
```

### 3. Block Registry Validation

Validate blocks before adding to registry:

```cpp
bool validateBlock(const std::string& block_id) {
    testing::BlockTester tester;
    auto results = tester.runTestsForBlock(block_id);

    if (!results.allPassed()) {
        fmt::print("[ERROR] Block validation failed: {}\n", block_id);
        return false;
    }

    return true;
}

// Before adding to registry
if (validateBlock(block_id)) {
    blockRegistry.addBlock(block);
}
```

---

## Performance

### Benchmarks

| Operation | Time | Notes |
|-----------|------|-------|
| **Load test definition** | <5ms | Parse JSON |
| **Setup code execution** | Varies | Depends on code |
| **Single test execution** | <10ms | Simple function |
| **Assertion check** | <0.1ms | Fast comparison |
| **Total test suite** | <100ms | For 10 tests |

---

## Future Enhancements

### Priority 1: JSON Parsing Library

Currently uses simplified parsing. Integrate proper JSON library:

```cpp
// Using nlohmann/json
#include <nlohmann/json.hpp>

bool parseTestDefinition(const std::string& json_content) {
    auto j = nlohmann::json::parse(json_content);
    definition_.block_id = j["block_id"];
    definition_.language = j["language"];

    for (const auto& test_json : j["tests"]) {
        BlockTest test;
        test.name = test_json["name"];
        test.code = test_json["code"];

        for (const auto& assertion_json : test_json["assertions"]) {
            Assertion assertion;
            assertion.type = parseAssertionType(assertion_json["type"]);
            assertion.value_expr = assertion_json["value_expr"];
            assertion.expected = assertion_json["expected"];
            test.assertions.push_back(assertion);
        }

        definition_.tests.push_back(test);
    }

    return true;
}
```

### Priority 2: Test Fixtures

Support setup and teardown:

```json
{
  "setup": "let globalState = {};",
  "teardown": "globalState = null;",
  "tests": [...]
}
```

### Priority 3: Parameterized Tests

Run same test with different inputs:

```json
{
  "name": "add_parameterized",
  "code": "function add(a, b) { return a + b; }",
  "parameters": [
    {"a": 5, "b": 3, "expected": 8},
    {"a": 10, "b": 20, "expected": 30},
    {"a": -5, "b": 5, "expected": 0}
  ]
}
```

### Priority 4: Mock Support

Mock external dependencies:

```json
{
  "mocks": {
    "database": "mock_database_implementation.js"
  }
}
```

### Priority 5: Coverage Reporting

Track which code paths are tested:

```cpp
struct CoverageReport {
    int lines_total;
    int lines_covered;
    double coverage_percentage;
};
```

---

## Files

| File | Lines | Purpose |
|------|-------|---------|
| `include/naab/block_tester.h` | 102 | Testing framework API |
| `src/testing/block_tester.cpp` | 248 | Implementation |
| `test_block_tester.cpp` | 144 | Test program |
| `examples/block_tests/JS-MATH-001.test.json` | 36 | Example test definition |
| **Total** | **530** | - |

---

## Conclusion

The Block Testing Framework provides **automated, isolated testing** for NAAb blocks. Key achievements:

✅ **JSON-based test definitions** - Easy to write and maintain
✅ **Multiple assertion types** - Comprehensive validation
✅ **Language agnostic** - Works with any registered executor
✅ **Performance tracking** - Measure execution time
✅ **Clear reporting** - Detailed pass/fail results
✅ **Tested** - All framework tests passing

The testing framework enables **confident block development** by validating blocks in isolation before integration.

**Benefits**:
- **Early bug detection** - Find issues before production
- **Regression prevention** - Catch broken functionality
- **Documentation** - Tests serve as usage examples
- **Confidence** - Know blocks work as expected

**Integration Points**:
- Pre-commit hooks
- CI/CD pipelines
- Block registry validation
- Development workflow

**Next**: Performance Profiling (Phase 6e) for runtime optimization insights.

---

**Phase 6d Status**: ✅ **COMPLETE**
**Date**: December 17, 2025
**Test Results**: All tests passing ✓
