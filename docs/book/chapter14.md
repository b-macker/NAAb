# Chapter 14: Testing and Quality Assurance

Ensuring the quality of your NAAb code, especially when coordinating multiple languages, is essential. While a dedicated testing framework is planned for future releases, NAAb currently supports a robust pattern for writing and running tests using assertions and exception handling.

## 14.1 Writing Unit Tests

The standard way to write a test in NAAb is to create a function that performs an action and asserts the result. If the assertion fails, the test should `throw` an error.

```naab
// test_math.naab

fn assert_eq(actual: int, expected: int, message: string) {
    if actual != expected {
        throw "Assertion failed: " + message + ". Expected " + expected + ", got " + actual
    }
}

fn test_addition() {
    let sum = 10 + 20
    assert_eq(sum, 30, "10 + 20 should be 30")
    print("✓ test_addition passed")
}

main {
    try {
        test_addition()
    } catch (e) {
        print("✗ Test Failed:", e)
        // exit(1) // Optional: Exit with error code
    }
}
```

## 14.2 Testing Polyglot Code

Testing polyglot blocks involves verifying that data is correctly passed to and returned from the foreign language.

```naab
fn test_python_integration() {
    let input = 10
    let result: int = <<python[input]
    input * 2
    >>
    
    if result != 20 {
        throw "Python doubling failed"
    }
    print("✓ test_python_integration passed")
}
```

## 14.3 The Test Runner Pattern

For larger projects, you can create a test runner script (e.g., in Bash or NAAb) that executes multiple test files and aggregates the results.

```bash
# run_tests.sh
for test_file in tests/*.naab; do
    ./naab-lang run "$test_file"
    if [ $? -eq 0 ]; then
        echo "PASS: $test_file"
    else
        echo "FAIL: $test_file"
    fi
done
```

This simple approach integrates well with CI/CD systems.
