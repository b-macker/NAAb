# Contributing to NAAb

Thank you for your interest in contributing to NAAb! This guide will help you get started.

## Getting Started

### Prerequisites

- C++17 compiler (GCC 9+ or Clang 10+)
- CMake 3.16+
- Ninja (recommended) or Make
- Python 3.8+ (for polyglot blocks)
- SQLite3, OpenSSL, libffi, libcurl development headers

### Building from Source

```bash
git clone --recursive https://github.com/b-macker/NAAb.git
cd NAAb
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -G Ninja
ninja naab-lang -j$(nproc)
```

### Running Tests

```bash
# From the project root
bash run-all-tests.sh
```

## How to Contribute

### Reporting Bugs

1. Check [existing issues](https://github.com/b-macker/NAAb/issues) to avoid duplicates
2. Open a new issue using the **Bug Report** template
3. Include: steps to reproduce, expected behavior, actual behavior, and your environment

### Suggesting Features

1. Open a new issue using the **Feature Request** template
2. Describe the use case and why it would be valuable

### Submitting Code

1. Fork the repository
2. Create a feature branch from `master`: `git checkout -b feature/my-feature`
3. Make your changes
4. Run the test suite: `bash run-all-tests.sh`
5. Commit with a clear message describing the change
6. Push to your fork and open a Pull Request

### Code Style

- Follow existing patterns in the codebase
- Use descriptive variable and function names
- Add comments only where the logic isn't self-evident
- Error messages should follow the project's helper error pattern (see `MEMORY.md` notes)

### Project Structure

```
src/
  lexer/          # Tokenizer
  parser/         # Recursive descent parser
  interpreter/    # AST visitor-pattern interpreter
  stdlib/         # Standard library modules
  runtime/        # Polyglot executors (Python, JS, Rust, C++, Go, etc.)
  cli/            # Command-line interface
  repl/           # Interactive REPL
include/naab/     # Header files
tests/            # Test files (.naab)
external/         # Third-party dependencies (git submodules)
```

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
