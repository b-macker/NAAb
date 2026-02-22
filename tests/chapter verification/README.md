# NAAb Book Verification Suite

This directory contains executable code examples used to verify the content of "The NAAb Programming Language" book.

## Philosophy

Every code snippet in the book should be verifiable. For each chapter, we maintain a corresponding `.naab` file (or set of files) that exercises the features discussed.

## Structure

*   `ch01_intro/`: Basic setup and Hello World.
*   `ch02_basics/`: Variables, types, structs, and mutability.
*   `ch03_control/`: Control flow (if, loops).
*   `ch04_functions/`: Functions, modules, and pipelines.
*   `ch16_tooling/`: Development tools (formatter, debugger, quality hints, error messages).
*   `...`: Future chapters.

## Running Tests

To run a verification test:

```bash
naab-lang run docs/book/verification/ch01_intro/hello.naab
```

### Special Testing Modes

**Formatter Tests:**
```bash
# Format code
naab-lang fmt docs/book/verification/ch16_tooling/formatter_test.naab

# Check if formatted (CI mode)
naab-lang fmt --check docs/book/verification/ch16_tooling/*.naab
```

**Debugger Tests:**
```bash
# Run with interactive debugger
naab-lang run --debug docs/book/verification/ch16_tooling/debugger_test.naab

# Run with watch expressions
naab-lang run --debug --watch="sum,count" docs/book/verification/ch16_tooling/debugger_test.naab
```

**Quality Hints:**
Quality hints are automatically shown during execution. The `quality_hints_test.naab` file contains intentional code quality issues to demonstrate the hint system.

**Enhanced Error Messages:**
The `error_hints_test.naab` file shows correct patterns. To see error hints, introduce deliberate errors (see file comments for examples).

## Issue Tracking

If a feature described in the plan does not work as expected, it is documented in `ISSUES.md` rather than breaking the build. This helps us distinguish between "bug in existing feature" and "feature not yet implemented".
