# Contributing to NAAb

Thank you for your interest in contributing to NAAb! This document provides guidelines and instructions for contributing.

---

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Getting Started](#getting-started)
3. [Development Setup](#development-setup)
4. [Making Changes](#making-changes)
5. [Testing](#testing)
6. [Submitting Changes](#submitting-changes)
7. [Code Style](#code-style)
8. [Documentation](#documentation)

---

## Code of Conduct

We are committed to providing a welcoming and inclusive environment. Please:

- Be respectful and considerate
- Welcome newcomers and help them learn
- Focus on constructive feedback
- Respect different viewpoints and experiences

---

## Getting Started

### Prerequisites

- C++ compiler (clang++ 10+ or g++ 9+)
- CMake 3.15+
- SQLite3
- Python 3.8+ (optional)
- Git

### Areas for Contribution

We welcome contributions in these areas:

1. **Bug Fixes** 🐛
   - Check open issues
   - Fix and submit PR

2. **Features** ✨
   - Discuss in issues first
   - Follow design guidelines
   - Add tests

3. **Documentation** 📚
   - Fix typos
   - Improve examples
   - Add tutorials

4. **Testing** 🧪
   - Increase coverage
   - Add edge cases
   - Performance tests

5. **Standard Library** 📦
   - New modules
   - New functions
   - Optimizations

---

## Development Setup

### Clone Repository

```bash
git clone https://github.com/yourusername/naab.git
cd naab/naab_language
```

### Build

```bash
mkdir build && cd build
cmake ..
cmake --build . -j$(nproc)
```

### Run Tests

```bash
# C++ compilation test suite
./test_cpp_compilation_suite

# Library detection tests
./test_library_detection

# Integration tests (if available)
./test_cross_language
```

### Verify Installation

```bash
./naab-lang version
./naab-repl
```

---

## Making Changes

### Branching Strategy

```bash
# Create feature branch
git checkout -b feature/your-feature-name

# Or bug fix branch
git checkout -b fix/issue-number-description
```

### Commit Messages

Use clear, descriptive commit messages:

```
Add default parameter support to parser

- Implement parameter default value parsing
- Add interpreter evaluation logic
- Update tests
- Update documentation

Fixes #123
```

**Format:**
- First line: Brief summary (50 chars max)
- Blank line
- Detailed description if needed
- Reference issues: `Fixes #123` or `Relates to #456`

---

## Testing

### Writing Tests

All new features must include tests:

**Example test (C++):**

```cpp
// test_my_feature.cpp
#include "naab/my_feature.h"
#include <fmt/core.h>

int main() {
    MyFeature feature;

    // Test 1
    if (feature.test_case_1()) {
        fmt::print("✅ Test 1 passed\n");
    } else {
        fmt::print("❌ Test 1 failed\n");
        return 1;
    }

    // Test 2
    // ...

    fmt::print("All tests passed!\n");
    return 0;
}
```

**Add to CMakeLists.txt:**

```cmake
add_executable(test_my_feature
    test_my_feature.cpp
)
target_link_libraries(test_my_feature
    naab_my_feature
    fmt::fmt
)
```

### Running Tests

```bash
# Run specific test
./test_my_feature

# Run all tests
make test  # If CTest is configured
```

### Test Requirements

- ✅ All tests must pass
- ✅ New code must have tests
- ✅ Aim for 80%+ coverage
- ✅ Include edge cases

---

## Submitting Changes

### Pull Request Process

1. **Update your branch**
   ```bash
   git fetch origin
   git rebase origin/main
   ```

2. **Run tests**
   ```bash
   cmake --build build
   ./test_cpp_compilation_suite
   ```

3. **Push changes**
   ```bash
   git push origin feature/your-feature-name
   ```

4. **Create Pull Request**
   - Go to GitHub
   - Click "New Pull Request"
   - Fill in description
   - Link related issues

### PR Checklist

- [ ] Code follows style guidelines
- [ ] Tests added and passing
- [ ] Documentation updated
- [ ] Commit messages are clear
- [ ] No merge conflicts
- [ ] CI passes (if configured)

### PR Description Template

```markdown
## Description
Brief description of changes

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Documentation update
- [ ] Performance improvement
- [ ] Refactoring

## Related Issues
Fixes #123

## Testing
- Describe testing done
- List any manual testing steps

## Checklist
- [ ] Tests pass
- [ ] Documentation updated
- [ ] Code follows style guide
```

---

## Code Style

### C++ Style

**Formatting:**
- Indentation: 4 spaces (no tabs)
- Line length: 100 characters max
- Braces: Same line for functions, control flow

**Example:**
```cpp
// Good
void myFunction(int param) {
    if (condition) {
        doSomething();
    }
}

// Naming conventions
class MyClass {};           // PascalCase for classes
void myFunction() {};       // camelCase for functions
int my_variable = 0;        // snake_case for variables
const int MAX_SIZE = 100;   // UPPER_CASE for constants
```

**Comments:**
```cpp
// Single line comment

/* Multi-line comment
 * for longer explanations
 */

/// Doxygen-style comment for documentation
/// @param x The input value
/// @return The computed result
int compute(int x);
```

### NAAb Style

**Formatting:**
```naab
# Indentation: 4 spaces
# Snake_case for variables and functions
# PascalCase for types (planned)

fn my_function(param: int) -> int {
    let my_variable = 10
    return my_variable + param
}
```

---

## Documentation

### When to Update Docs

Update documentation when you:
- Add new features
- Change existing APIs
- Fix bugs that affect usage
- Add examples

### Documentation Files

- **README.md**: Project overview, quick start
- **docs/USER_GUIDE.md**: Usage instructions
- **docs/API_REFERENCE.md**: Complete API docs
- **docs/ARCHITECTURE.md**: System design
- **docs/tutorials/**: Step-by-step guides

### Writing Good Documentation

✅ **Do:**
- Use clear, simple language
- Include code examples
- Test all examples
- Keep it up-to-date

❌ **Don't:**
- Assume prior knowledge
- Use jargon without explanation
- Skip edge cases
- Leave outdated information

### Example Documentation

````markdown
## myFunction()

**Signature:** `int myFunction(int x, int y)`

**Description:** Computes the sum of two integers.

**Parameters:**
- `x` (int): First number
- `y` (int): Second number

**Returns:** Sum of x and y

**Example:**
```naab
let result = myFunction(10, 20)
print(result)  # 30
```

**Notes:**
- Both parameters must be integers
- Result may overflow for large values
````

---

## Review Process

### What We Look For

1. **Correctness**
   - Does it work?
   - Are there bugs?
   - Edge cases handled?

2. **Tests**
   - Good coverage?
   - Tests pass?
   - Clear test cases?

3. **Code Quality**
   - Follows style guide?
   - Well-structured?
   - Good names?

4. **Documentation**
   - Updated?
   - Clear?
   - Examples work?

### Review Timeline

- Initial review: 1-3 days
- Follow-up reviews: 1-2 days
- Merge decision: After approval

---

## Getting Help

### Questions?

- Open an issue with "Question:" prefix
- Check existing issues and discussions
- Ask in pull request comments

### Need Guidance?

- Read [ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Check [API_REFERENCE.md](docs/API_REFERENCE.md)
- Look at existing code

---

## Recognition

Contributors will be:
- Listed in CONTRIBUTORS.md
- Mentioned in release notes
- Credited in commit history

---

## License

By contributing, you agree that your contributions will be licensed under the same license as the project.

---

**Thank you for contributing to NAAb!** 🙏

Every contribution, no matter how small, helps make NAAb better for everyone.

---

**Questions?** Open an issue or discussion on GitHub.
