# Phase 4.7: Testing Framework (naab-test) - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** MEDIUM - Test runner with assertions and reporting
**Estimated Effort:** 3 weeks implementation
**Priority:** HIGH - Essential for code quality

This document outlines the design for `naab-test`, NAAb's built-in testing framework for writing and running tests with assertions, test discovery, and reporting.

---

## Current Problem

**No Testing Framework:**
- No standard way to write tests
- Must manually run programs and check output
- No assertions or test structure
- No test discovery or organization
- Difficult to ensure code quality

**Impact:** Low code quality, bugs in production, difficult refactoring.

---

## Goals

### Primary Goals

1. **Test Syntax** - Simple `test "name" { ... }` blocks
2. **Assertions** - assert, assert_eq, assert_ne, assert_throws
3. **Test Discovery** - Automatically find tests
4. **Test Runner** - Run tests and report results
5. **Clean Output** - Pass/fail with helpful messages

### Secondary Goals

6. **Test Organization** - Group related tests
7. **Setup/Teardown** - Before/after each test
8. **Code Coverage** - Track which code is tested (future)
9. **Parallel Execution** - Run tests concurrently (future)

---

## Test Syntax

### Basic Test

```naab
test "addition works" {
    assert(1 + 1 == 2)
}

test "subtraction works" {
    let result = 5 - 3
    assert_eq(result, 2)
}
```

### Test with Setup

```naab
function setUp() {
    // Run before each test
    print("Setting up...")
}

function tearDown() {
    // Run after each test
    print("Tearing down...")
}

test "string concatenation" {
    let greeting = "Hello" + " " + "World"
    assert_eq(greeting, "Hello World")
}
```

### Grouped Tests

```naab
group "Math operations" {
    test "addition" {
        assert_eq(2 + 2, 4)
    }

    test "multiplication" {
        assert_eq(3 * 4, 12)
    }

    test "division" {
        assert_eq(10 / 2, 5)
    }
}

group "String operations" {
    test "length" {
        assert_eq("hello".length, 5)
    }

    test "uppercase" {
        assert_eq(string.upper("hello"), "HELLO")
    }
}
```

---

## Assertions

### assert(condition)

**Pass if condition is true:**

```naab
test "basic assertion" {
    let x = 42
    assert(x > 0)
    assert(x == 42)
}
```

**Failure Output:**
```
✗ basic assertion
  Assertion failed: x == 43
  Expected: true
  Got: false
  at test.naab:4
```

### assert_eq(actual, expected)

**Pass if values are equal:**

```naab
test "equality assertion" {
    let result = add(2, 3)
    assert_eq(result, 5)
}
```

**Failure Output:**
```
✗ equality assertion
  Values not equal
  Expected: 5
  Got: 6
  at test.naab:3
```

### assert_ne(actual, expected)

**Pass if values are NOT equal:**

```naab
test "inequality assertion" {
    let x = 42
    assert_ne(x, 0)
}
```

### assert_throws(function)

**Pass if function throws exception:**

```naab
test "division by zero throws" {
    assert_throws(function() {
        let result = 10 / 0
    })
}
```

**Failure Output:**
```
✗ division by zero throws
  Expected exception but none was thrown
  at test.naab:2
```

### assert_null(value)

```naab
test "null check" {
    let x: int? = null
    assert_null(x)
}
```

### assert_not_null(value)

```naab
test "not null check" {
    let x: int? = 42
    assert_not_null(x)
}
```

---

## Test Discovery

### File Patterns

**Automatically discover test files:**

```bash
# Run all tests in tests/ directory
naab-test tests/

# Run specific test file
naab-test tests/math_test.naab

# Run tests matching pattern
naab-test tests/**/*_test.naab
```

**Naming Conventions:**
- Test files: `*_test.naab` or `test_*.naab`
- Test directories: `tests/` or `__tests__/`

### Test Collection

```cpp
class TestDiscovery {
public:
    std::vector<std::string> discoverTests(const std::string& path) {
        std::vector<std::string> test_files;

        if (fs::is_directory(path)) {
            // Find all *_test.naab files recursively
            for (const auto& entry : fs::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename();
                    if (filename.ends_with("_test.naab") || filename.starts_with("test_")) {
                        test_files.push_back(entry.path());
                    }
                }
            }
        } else {
            // Single file
            test_files.push_back(path);
        }

        return test_files;
    }
};
```

---

## Test Runner

### Architecture

```cpp
struct Test {
    std::string name;
    std::string file;
    int line;
    std::function<void()> body;
};

struct TestGroup {
    std::string name;
    std::vector<Test> tests;
    std::function<void()> setUp;
    std::function<void()> tearDown;
};

class TestRunner {
public:
    void addTest(const Test& test);
    void addGroup(const TestGroup& group);

    TestResults runAll();
    TestResults runFile(const std::string& file);

private:
    std::vector<Test> tests_;
    std::vector<TestGroup> groups_;

    TestResult runTest(const Test& test);
    void runSetUp(const TestGroup& group);
    void runTearDown(const TestGroup& group);
};

struct TestResult {
    std::string name;
    bool passed;
    std::string error_message;
    std::string file;
    int line;
    double duration_ms;
};

struct TestResults {
    int total;
    int passed;
    int failed;
    std::vector<TestResult> results;
    double total_duration_ms;
};
```

### Running Tests

```cpp
TestResults TestRunner::runAll() {
    TestResults results;
    results.total = tests_.size();

    auto start = std::chrono::high_resolution_clock::now();

    for (const auto& test : tests_) {
        auto result = runTest(test);
        results.results.push_back(result);

        if (result.passed) {
            results.passed++;
        } else {
            results.failed++;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    results.total_duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return results;
}

TestResult TestRunner::runTest(const Test& test) {
    TestResult result;
    result.name = test.name;
    result.file = test.file;
    result.line = test.line;

    auto start = std::chrono::high_resolution_clock::now();

    try {
        test.body();
        result.passed = true;
    } catch (const AssertionFailure& e) {
        result.passed = false;
        result.error_message = e.what();
    } catch (const std::exception& e) {
        result.passed = false;
        result.error_message = "Unexpected exception: " + std::string(e.what());
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

    return result;
}
```

---

## Assertion Implementation

### AssertionFailure Exception

```cpp
class AssertionFailure : public std::exception {
public:
    AssertionFailure(const std::string& message, const SourceLocation& location)
        : message_(message), location_(location) {}

    const char* what() const noexcept override {
        return message_.c_str();
    }

    const SourceLocation& getLocation() const {
        return location_;
    }

private:
    std::string message_;
    SourceLocation location_;
};
```

### Built-in Assertion Functions

```cpp
// assert(condition)
void assert(bool condition, const SourceLocation& loc) {
    if (!condition) {
        throw AssertionFailure("Assertion failed", loc);
    }
}

// assert_eq(actual, expected)
template<typename T>
void assert_eq(const T& actual, const T& expected, const SourceLocation& loc) {
    if (!(actual == expected)) {
        std::ostringstream msg;
        msg << "Values not equal\n";
        msg << "  Expected: " << expected << "\n";
        msg << "  Got: " << actual;
        throw AssertionFailure(msg.str(), loc);
    }
}

// assert_ne(actual, expected)
template<typename T>
void assert_ne(const T& actual, const T& expected, const SourceLocation& loc) {
    if (actual == expected) {
        std::ostringstream msg;
        msg << "Values should not be equal\n";
        msg << "  Both are: " << actual;
        throw AssertionFailure(msg.str(), loc);
    }
}

// assert_throws(function)
void assert_throws(std::function<void()> fn, const SourceLocation& loc) {
    try {
        fn();
        throw AssertionFailure("Expected exception but none was thrown", loc);
    } catch (const AssertionFailure&) {
        throw;  // Re-throw our own assertion failures
    } catch (...) {
        // Good, expected exception
    }
}
```

---

## Output Formatting

### Success Output

```
$ naab-test tests/

Running tests...

  ✓ addition works (0.1ms)
  ✓ subtraction works (0.1ms)
  ✓ string concatenation (0.2ms)

Math operations
  ✓ addition (0.1ms)
  ✓ multiplication (0.1ms)
  ✓ division (0.1ms)

String operations
  ✓ length (0.1ms)
  ✓ uppercase (0.3ms)

8 tests, 8 passed, 0 failed (1.2ms)
```

### Failure Output

```
$ naab-test tests/

Running tests...

  ✓ addition works (0.1ms)
  ✗ subtraction works (0.1ms)

    Values not equal
    Expected: 2
    Got: 3
    at tests/math_test.naab:8

  ✓ string concatenation (0.2ms)

3 tests, 2 passed, 1 failed (0.4ms)
```

### Verbose Output

```bash
naab-test --verbose tests/
```

```
$ naab-test --verbose tests/

Running tests...

[PASS] addition works (0.1ms)
  File: tests/math_test.naab:3
  Assertions: 1

[FAIL] subtraction works (0.1ms)
  File: tests/math_test.naab:8
  Assertions: 1
  Error: Values not equal
    Expected: 2
    Got: 3
    Stack trace:
      at assert_eq (builtin)
      at test "subtraction works" (tests/math_test.naab:8)

[PASS] string concatenation (0.2ms)
  File: tests/string_test.naab:3
  Assertions: 1

3 tests, 2 passed, 1 failed (0.4ms)
```

---

## CLI Interface

### Commands

```bash
# Run all tests
naab-test

# Run tests in directory
naab-test tests/

# Run specific file
naab-test tests/math_test.naab

# Run with verbose output
naab-test --verbose

# Run and watch for changes
naab-test --watch

# Generate coverage report (future)
naab-test --coverage

# Run tests matching pattern
naab-test --filter "addition*"

# Fail fast (stop on first failure)
naab-test --fail-fast

# Output as JSON
naab-test --json
```

### Exit Codes

- **0**: All tests passed
- **1**: Some tests failed
- **2**: Error running tests (parse error, etc.)

---

## Integration with Build System

### Run Tests After Build

**naab.build.json:**

```json
{
  "scripts": {
    "test": "naab-test tests/",
    "build": "naab-build",
    "test:watch": "naab-test --watch"
  }
}
```

**Run:**

```bash
naab-build && naab-test
```

---

## Parser Integration

### Test Syntax in AST

```cpp
// AST nodes for testing
class TestDecl : public Decl {
public:
    TestDecl(const std::string& name, std::unique_ptr<Stmt> body)
        : name_(name), body_(std::move(body)) {}

    const std::string& getName() const { return name_; }
    const Stmt* getBody() const { return body_.get(); }

private:
    std::string name_;
    std::unique_ptr<Stmt> body_;
};

class GroupDecl : public Decl {
public:
    GroupDecl(const std::string& name, std::vector<std::unique_ptr<Decl>> tests)
        : name_(name), tests_(std::move(tests)) {}

    const std::string& getName() const { return name_; }
    const std::vector<std::unique_ptr<Decl>>& getTests() const { return tests_; }

private:
    std::string name_;
    std::vector<std::unique_ptr<Decl>> tests_;
};
```

### Parser Updates

```cpp
// In Parser::parseDeclaration()
if (match(TokenType::TEST)) {
    return parseTestDecl();
}

if (match(TokenType::GROUP)) {
    return parseGroupDecl();
}

std::unique_ptr<Decl> Parser::parseTestDecl() {
    // test "name" { body }
    expect(TokenType::STRING, "Expected test name");
    std::string name = previous().value;

    expect(TokenType::LBRACE, "Expected '{' after test name");
    auto body = parseCompoundStmt();

    return std::make_unique<TestDecl>(name, std::move(body));
}

std::unique_ptr<Decl> Parser::parseGroupDecl() {
    // group "name" { tests... }
    expect(TokenType::STRING, "Expected group name");
    std::string name = previous().value;

    expect(TokenType::LBRACE, "Expected '{' after group name");

    std::vector<std::unique_ptr<Decl>> tests;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match(TokenType::TEST)) {
            tests.push_back(parseTestDecl());
        } else {
            throw ParseError("Expected test declaration in group");
        }
    }

    expect(TokenType::RBRACE, "Expected '}' after group");

    return std::make_unique<GroupDecl>(name, std::move(tests));
}
```

---

## Example Test File

```naab
// tests/calculator_test.naab

import "../src/calculator"

group "Calculator tests" {
    test "addition" {
        let result = calculator.add(2, 3)
        assert_eq(result, 5)
    }

    test "subtraction" {
        let result = calculator.subtract(10, 3)
        assert_eq(result, 7)
    }

    test "multiplication" {
        let result = calculator.multiply(4, 5)
        assert_eq(result, 20)
    }

    test "division" {
        let result = calculator.divide(10, 2)
        assert_eq(result, 5)
    }

    test "division by zero throws" {
        assert_throws(function() {
            calculator.divide(10, 0)
        })
    }
}

group "Edge cases" {
    test "large numbers" {
        let result = calculator.add(1000000, 2000000)
        assert_eq(result, 3000000)
    }

    test "negative numbers" {
        let result = calculator.add(-5, 3)
        assert_eq(result, -2)
    }

    test "zero" {
        let result = calculator.multiply(42, 0)
        assert_eq(result, 0)
    }
}
```

---

## Implementation Plan

### Week 1: Core Framework (5 days)

- [ ] Implement Test/Group AST nodes
- [ ] Update parser for test syntax
- [ ] Implement TestRunner
- [ ] Test: Basic tests run

### Week 2: Assertions (5 days)

- [ ] Implement assert functions
- [ ] Implement AssertionFailure
- [ ] Error messages with locations
- [ ] Test: All assertion types work

### Week 3: CLI & Reporting (5 days)

- [ ] Implement CLI (test discovery, filtering)
- [ ] Output formatting (pass/fail reporting)
- [ ] Integration with build system
- [ ] Documentation

**Total: 3 weeks**

---

## Testing Strategy

### Testing the Test Framework

**Meta-tests:**

```cpp
TEST(TestFrameworkTest, AssertPasses) {
    EXPECT_NO_THROW(assert(true));
}

TEST(TestFrameworkTest, AssertFails) {
    EXPECT_THROW(assert(false), AssertionFailure);
}

TEST(TestFrameworkTest, AssertEqPasses) {
    EXPECT_NO_THROW(assert_eq(42, 42));
}

TEST(TestFrameworkTest, AssertEqFails) {
    EXPECT_THROW(assert_eq(42, 43), AssertionFailure);
}
```

---

## Success Metrics

### Phase 4.7 Complete When:

- [x] Test syntax working (test, group)
- [x] All assertions implemented
- [x] Test discovery working
- [x] Test runner working
- [x] Pass/fail reporting clear
- [x] CLI interface complete
- [x] Integration with build system
- [x] Documentation complete

---

## Conclusion

**Phase 4.7 Status: DESIGN COMPLETE**

A testing framework will:
- Enable test-driven development
- Improve code quality
- Catch bugs early
- Make refactoring safe

**Implementation Effort:** 3 weeks

**Priority:** HIGH (essential for quality)

**Dependencies:** Parser, interpreter

Once implemented, NAAb will have testing capabilities for writing reliable code.
