# Chapter 14: Testing and Quality Assurance

Ensuring the quality of your NAAb code, especially when coordinating multiple languages, is essential. NAAb provides several built-in tools for testing, governance validation, and static analysis.

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
    print("test_addition passed")
}

main {
    try {
        test_addition()
    } catch (e) {
        print("Test Failed:", e)
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
    print("test_python_integration passed")
}
```

## 14.3 The [passed, total] Pattern

The NAAb test suite uses a consistent pattern where each test function returns `[passed, total]` and the main block aggregates results:

```naab
fn test_array_ops() {
    let passed = 0
    let total = 0

    // Test 1: push
    total = total + 1
    let arr = [1, 2, 3]
    arr.push(4)
    if arr.length() == 4 { passed = passed + 1 }

    // Test 2: pop
    total = total + 1
    let last = arr.pop()
    if last == 4 { passed = passed + 1 }

    return [passed, total]
}

fn test_string_ops() {
    let passed = 0
    let total = 0

    total = total + 1
    if string.upper("hello") == "HELLO" { passed = passed + 1 }

    return [passed, total]
}

main {
    let total_passed = 0
    let total_tests = 0

    let r1 = test_array_ops()
    total_passed = total_passed + r1[0]
    total_tests = total_tests + r1[1]
    print(f"test_array_ops: {r1[0]}/{r1[1]}")

    let r2 = test_string_ops()
    total_passed = total_passed + r2[0]
    total_tests = total_tests + r2[1]
    print(f"test_string_ops: {r2[0]}/{r2[1]}")

    print(f"\nTotal: {total_passed}/{total_tests}")
}
```

## 14.4 The Test Runner

For larger projects, create a shell test runner that executes multiple test files and aggregates results:

```bash
#!/bin/bash
# run_tests.sh
PASS=0; FAIL=0
for test_file in tests/*.naab; do
    if ./naab-lang "$test_file" > /dev/null 2>&1; then
        echo "PASS: $test_file"
        ((PASS++))
    else
        echo "FAIL: $test_file"
        ((FAIL++))
    fi
done
echo "Results: $PASS passed, $FAIL failed"
```

This integrates well with CI/CD systems — a non-zero exit code signals test failure.

## 14.5 Governance Testing with --lint-only

The `--lint-only` flag runs parsing and governance checks without executing the code. This is useful for validating that your code passes all governance rules before running it:

```bash
# Check governance without running
naab --lint-only app.naab

# Check with full governance dashboard
naab --lint-only --governance-dashboard app.naab

# Generate SARIF report for CI
naab --lint-only --governance-sarif results.sarif app.naab
```

`--lint-only` catches issues like:
- Hallucinated API usage in polyglot blocks
- Stub functions and incomplete logic
- Secret detection and taint violations
- Code quality issues (TODO/FIXME, dead code)

## 14.6 Contract Testing

When `govern.json` defines function contracts, NAAb validates them at runtime:

```json
{
  "contracts": {
    "calculate_damage": {
      "params": { "attacker": "dict", "defender": "dict" },
      "return_type": "int",
      "return_min": 0
    },
    "get_action": {
      "return_one_of": ["attack", "defend", "heal", "flee"]
    }
  }
}
```

Contract violations produce clear error messages showing which constraint was violated, making it easy to write tests that verify contract compliance.

## 14.7 Static Analysis with --scan

The `--scan` flag runs NAAb's built-in static code scanner on external source files:

```bash
# Scan a Python file
naab --scan src/app.py --language python

# Scan with SARIF output for GitHub Code Scanning
naab --scan src/ --governance-sarif results.sarif
```

The scanner checks for 139+ patterns across security, code quality, and correctness categories.

## 14.8 Security Testing

NAAb's test suite includes a dedicated security leak check that scans all error messages for accidentally leaked bypass information:

```bash
# Run security leak check (112 checks)
bash tests/security/test_error_msg_leaks.sh
```

This test verifies that error messages never reveal:
- Bypass flags like `--no-governance` or `--governance-override`
- Internal sanitizer function names
- Config key paths or governance internals

When modifying any error text, always run this test afterward.
