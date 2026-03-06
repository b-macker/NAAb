# NAAb Language

[![CI](https://github.com/b-macker/NAAb/actions/workflows/ci.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/ci.yml)
[![Sanitizers](https://github.com/b-macker/NAAb/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/sanitizers.yml)
[![CodeQL](https://github.com/b-macker/NAAb/actions/workflows/codeql.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)
[![Discussions](https://img.shields.io/badge/Discussions-enabled-blue.svg)](https://github.com/b-macker/NAAb/discussions)

**A polyglot programming language** that seamlessly integrates Python, JavaScript, Rust, C++, Go, Nim, and more through an innovative block system. Use the best language for each task — in a single file.

```naab
main {
    // Use Python's ML libraries directly
    let prediction = <<python
import numpy as np
model = np.array([1.2, 3.4, 5.6])
float(model.mean())
>>

    // Process with Rust for performance
    let formatted = <<rust
fn main() {
    let val = std::env::args().last().unwrap();
    println!("Result: {:.2}", val.parse::<f64>().unwrap());
}
>>

    print(prediction)  // 3.4
}
```

---

## Why NAAb?

- **Use any language where it shines** — Python for data science, Rust for performance, JavaScript for web, Go for concurrency
- **No FFI boilerplate** — Variables flow between languages automatically
- **One project, many languages** — No microservices needed for polyglot architectures
- **Modern language features** — Pattern matching, async/await, lambdas, closures
- **Built-in LLM governance** — Detect oversimplified stubs, hallucinated APIs, and incomplete logic in AI-generated code
- **325 tests passing** across 12 stdlib modules

---

## Quick Start

```bash
# Clone and build
git clone --recursive https://github.com/b-macker/NAAb.git
cd NAAb
mkdir build && cd build
cmake .. -G Ninja
ninja naab-lang -j$(nproc)

# Run a file
./naab-lang hello.naab
```

### Hello World

```naab
main {
    let name = "World"
    print("Hello, " + name + "!")
}
```

### Pattern Matching

```naab
main {
    let status = 404

    let message = match status {
        200 => "OK"
        404 => "Not Found"
        500 => "Server Error"
        _ => "Unknown"
    }

    print(message)  // "Not Found"
}
```

### Async/Await

```naab
main {
    async fn fetch_data() {
        return "data loaded"
    }

    async fn process() {
        return "processed"
    }

    let data = await fetch_data()
    let result = await process()
    print(data + " -> " + result)
}
```

### Polyglot Blocks

```naab
main {
    let numbers = [10, 20, 30, 40, 50]

    // Python: statistical analysis
    let stats = <<python[numbers]
import statistics
result = {
    "mean": statistics.mean(numbers),
    "stdev": statistics.stdev(numbers)
}
str(result)
>>

    // JavaScript: format as HTML
    let html = <<javascript
const data = "Stats: mean=30, stdev=15.81";
`<div class="result">${data}</div>`;
>>

    print(stats)
    print(html)
}
```

### Standard Library

```naab
main {
    // JSON
    let data = json.parse('{"name": "NAAb", "version": "0.2.0"}')
    print(data["name"])

    // HTTP
    let response = http.get("https://api.github.com/repos/b-macker/NAAb")
    print(json.parse(response)["stargazers_count"])

    // File I/O
    file.write("output.txt", "Hello from NAAb!")

    // Math
    print(math.sqrt(144))  // 12

    // String operations
    let words = string.split("hello world", " ")
    print(string.upper(words[0]))  // "HELLO"
}
```

---

## Features

### Language
- Variables, constants, functions, closures, lambdas
- Pattern matching with `match` expressions
- Async/await for concurrent execution
- If expressions (`let x = if cond { a } else { b }`)
- Structs, enums, classes
- Module system with imports
- Error handling with try/catch/throw

### Polyglot Execution
- **12 languages:** Python, JavaScript, Rust, C++, Go, C#, Nim, Zig, Julia, Ruby, PHP, Shell
- Variable binding between languages
- Parallel polyglot execution with dependency analysis
- Persistent sub-runtime contexts
- JSON sovereign pipe for structured data return
- Automatic error mapping with source locations

### Standard Library (12 modules)
`array` `string` `math` `time` `env` `file` `io` `json` `http` `csv` `regex` `crypto`

### Developer Tools
- Interactive REPL with tab completion and history
- "Did you mean?" suggestions for typos
- Detailed error messages with examples
- LLM-friendly syntax (keyword aliases, semicolons optional)

### Governance Engine
- **Policy-as-code** via `govern.json` — control languages, APIs, code quality
- **LLM anti-drift checks** — detect stubs, hallucinated APIs, incomplete error handling
- **Three enforcement levels** — HARD (block), SOFT (overridable), ADVISORY (warn)
- **50+ built-in checks** — secrets, SQL injection, path traversal, privilege escalation, and more
- **Project context awareness** — auto-reads CLAUDE.md, .editorconfig, package.json and more to supplement governance rules
- **Custom rules** — define your own regex-based governance rules
- **CI/CD integration** — SARIF, JUnit XML, and JSON report output

```json
// govern.json — detect common LLM failures
{
  "version": "3.0",
  "mode": "enforce",
  "code_quality": {
    "no_oversimplification": { "level": "hard" },
    "no_incomplete_logic": { "level": "hard" },
    "no_hallucinated_apis": { "level": "soft" }
  }
}
```

See [Chapter 21: Governance](docs/book/chapter21.md) for the full reference.

---

## NAAb Ecosystem

Three powerful tools built with NAAb — code governance, performance optimization, and data security:

| Project | Purpose | Key Features |
|---------|---------|--------------|
| **[NAAb BOLO](https://github.com/b-macker/naab-bolo)** | Code governance & validation | 50+ checks, SARIF reports, AI drift detection |
| **[NAAb Pivot](https://github.com/b-macker/naab-pivot)** | Code evolution & optimization | 3-60x speedups, proven correctness, 8 languages |
| **[NAAb Passage](https://github.com/b-macker/naab-passage)** | Data gateway & PII protection | Zero leakage, sovereign architecture, HIPAA/GDPR |

### 🔍 NAAb BOLO — Code Governance & Validation

**[NAAb BOLO](https://github.com/b-macker/naab-bolo)** ("Be On the Lookout") catches oversimplified stubs, hallucinated APIs, and incomplete logic in AI-generated code.

```bash
# Scan for governance violations
naab-lang bolo.naab scan ./src --profile enterprise

# Generate SARIF report for CI
naab-lang bolo.naab report ./src --format sarif --output results.sarif

# AI governance validation
naab-lang bolo.naab ai-check ./ml-models
```

**50+ checks · 4 languages · 64 regression tests** → [Get started](https://github.com/b-macker/naab-bolo)

### ⚡ NAAb Pivot — Code Evolution & Optimization

**[NAAb Pivot](https://github.com/b-macker/naab-pivot)** automatically rewrites slow code in faster languages with mathematical proof of correctness.

```bash
# Analyze hotspots (Python → Rust candidates)
naab-lang pivot.naab analyze app.py

# Rewrite with proof
naab-lang pivot.naab rewrite app.py:expensive_loop --target rust --prove

# Result: 45x faster, semantically identical
```

**3-60x speedups · 8 source languages · Proven correct** → [Get started](https://github.com/b-macker/naab-pivot)

### 🔒 NAAb Passage — Data Gateway & PII Protection

**[NAAb Passage](https://github.com/b-macker/naab-passage)** ensures zero PII leakage to LLMs, APIs, or untrusted systems with sovereign architecture.

```bash
# Start secure gateway
naab-lang main.naab

# All requests validated, PII blocked
curl -X POST http://localhost:8091/ -d '{"prompt": "SSN: 123-45-6789"}'
# → {"error": "POLICY_VIOLATION"}
```

**Zero leakage · Self-synthesizing · HIPAA/GDPR compliant** → [Get started](https://github.com/b-macker/naab-passage)

---

## Architecture

```
Source Code (.naab)
    |
  Lexer ──> Tokens
    |
  Parser ──> AST (recursive descent)
    |
  Interpreter (visitor pattern)
    |
  ├── Native execution (NAAb code)
  ├── Python executor (C API)
  ├── JavaScript executor (QuickJS)
  ├── Go/Rust/C++/C#/Nim/Zig/Julia executors (compile & run)
  ├── Ruby/PHP executors (interpreted)
  └── Shell executor (subprocess)
```

- **15,000+** lines of C++17
- **185** test files, **325** mono test assertions
- **12** standard library modules
- Built with Abseil, fmt, spdlog, nlohmann/json, QuickJS

---

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions and guidelines.

### Areas for Contribution
- Performance optimizations
- New standard library modules
- Documentation and tutorials
- IDE integrations (Vim, Emacs, IntelliJ)
- Package manager implementation

---

## License

MIT License - see [LICENSE](LICENSE) for details.

**Brandon Mackert** - [@b-macker](https://github.com/b-macker)

---

_NAAb — Polyglot without the trip._
