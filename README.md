# NAAb Language

A polyglot programming language that seamlessly integrates Python, JavaScript, Rust, C++, and other languages through an innovative block system. Write once, execute in any language.

---

## Features

### Core Language
- **Type System** - Union types, type inference, type annotations
- **Modern Syntax** - Clean, expressive syntax with automatic semicolon insertion
- **Structs & Enums** - First-class data structures
- **Module System** - Clean imports with alias support

### Polyglot Blocks
- **Multi-Language Execution** - Seamlessly call Python, JavaScript, Rust, C++, Go, Ruby, Shell, and more
- **Block Library** - Pre-built block registry for common operations across languages
- **Type-Safe Bridges** - Automatic marshalling between language boundaries

### Developer Experience
- **LSP Server** - Full IDE support with autocomplete, hover, go-to-definition, diagnostics
- **VS Code Extension** - Syntax highlighting, IntelliSense, debugging support
- **Auto-Formatter** - `naab-lang fmt` for consistent code style
- **Linter** - Static analysis and code quality checks
- **Debugger** - Built-in debugging with breakpoints and variable inspection

### Standard Library
- **File I/O** - Read, write, list directories
- **HTTP Client** - RESTful API calls with request/response handling
- **JSON** - Parse and stringify with full type conversion
- **String Utilities** - Split, join, trim, replace, regex support
- **Math** - Comprehensive math functions and constants
- **Time** - High-precision timing, timestamps, sleep functions
- **Collections** - Advanced array and dictionary operations
- **CSV** - Parse and generate CSV files
- **Crypto** - Hashing (MD5, SHA-256), HMAC, Base64
- **Regex** - Full regular expression support
- **Environment** - Access environment variables and process info

---

## Quick Start

### Installation

```bash
# Clone the repository
git clone https://github.com/b-macker/NAAb.git
cd NAAb

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Install (optional)
sudo make install
```

### Hello World

```naab
fn greet(name: string) -> string {
    return "Hello, " + name + "!"
}

main {
    let message = greet("World")
    print(message)
}
```

### Polyglot Example

```naab
main {
    let data = [1, 2, 3, 4, 5]

    // Execute Python code with variable binding
    let result = <<python[data]
import statistics
statistics.mean(data)
>>

    print("Average: " + result)
}
```

### More Examples

See the [examples/](examples/) directory for:
- Multi-language analytics
- API server
- Web scraper
- Data pipeline
- Enterprise applications

---

## Documentation

- **[The NAAb Book](docs/book/)** - Comprehensive language reference (21 chapters)
- **[Quick Start](docs/book/QUICK_START.md)** - Get up and running fast
- **[User Guide](USER_GUIDE.md)** - Complete language guide
- **[Contributing](docs/CONTRIBUTING.md)** - Contributor guidelines

---

## Architecture

- **Parser** - Hand-written recursive descent parser with full AST
- **Type Checker** - Bidirectional type inference
- **Interpreter** - Tree-walking interpreter with optimized execution
- **Memory Model** - Automatic memory management with RAII and smart pointers
- **Cross-Language Bridge** - FFI layer with type-safe marshalling
- **LSP Server** - JSON-RPC protocol with caching and debouncing

### Statistics
- **10,000+** lines of C++ code
- **308/350** tests passing (308 pass + 42 expected failures = 100%)
- **325** mono test assertions passing (0 failures)
- **7** LSP capabilities implemented
- **12** standard library modules

---

## Development

### Build Requirements
- CMake 3.15+
- C++17 compiler (GCC 7+, Clang 5+, MSVC 2017+)
- Python 3.7+ (for polyglot blocks)
- Node.js 14+ (for JavaScript blocks)
- Rust 1.50+ (optional, for Rust blocks)

### Running Tests

```bash
cd build
ctest --output-on-failure

# Or run specific test suites
./naab_unit_tests
./lsp_integration_test
```

### Code Formatting

```bash
# Format all NAAb code
naab-lang fmt src/**/*.naab

# Format specific file
naab-lang fmt examples/hello_world.naab
```

### LSP Server

```bash
# Build LSP server
cmake --build build --target naab-lsp

# Use with VS Code
# Install the vscode-naab extension
```

---

## Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](docs/CONTRIBUTING.md) for guidelines.

### How to Contribute
1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Areas for Contribution
- Standard library modules
- Language features (pattern matching, async/await)
- Performance optimizations
- Documentation and examples
- IDE integrations (Vim, Emacs, IntelliJ)
- Package manager implementation

---

## Project Status

**Phase 1:**  Syntax & Parser - 100% Complete
**Phase 2:**  Type System - 100% Complete
**Phase 3:**  Memory Management - 100% Complete
**Phase 4:**  Tooling (LSP, Formatter, Linter, Debugger) - 80% Complete
**Phase 5:**  Standard Library - 100% Complete (12 modules)

See [docs/](docs/) for detailed documentation.

---

## License

MIT License - see [LICENSE](LICENSE) for details.

Copyright © 2026 Brandon Mackert

---

## Author

**Brandon Mackert**

- GitHub: [@b-macker](https://github.com/b-macker)
- Repository: [NAAb](https://github.com/b-macker/NAAb)
- Created: 2026

---

## Acknowledgments

- **Implementation Assistance:** Claude (Anthropic)
- **Inspiration:** Rust, Python, TypeScript, Go
- **Dependencies:**
  - nlohmann/json - JSON parsing
  - fmtlib - String formatting
  - Google Test - Testing framework

---

## Links

- **Documentation:** [docs/](docs/)
- **Examples:** [examples/](examples/)
- **Issue Tracker:** GitHub Issues
- **Discussions:** GitHub Discussions

---

## Star History

If you find NAAb useful, please consider giving it a star on GitHub!

---

**NAAb** - _Because polyglot programming should be seamless._
