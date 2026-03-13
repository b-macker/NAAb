# NAAb — The Programming Language with Built-in AI Governance

[![CI](https://github.com/b-macker/NAAb/actions/workflows/ci.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/ci.yml)
[![Sanitizers](https://github.com/b-macker/NAAb/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/sanitizers.yml)
[![CodeQL](https://github.com/b-macker/NAAb/actions/workflows/codeql.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)
[![Discussions](https://img.shields.io/badge/Discussions-enabled-blue.svg)](https://github.com/b-macker/NAAb/discussions)

**Govern AI-generated code at the language level.** NAAb is a polyglot programming language that enforces code quality, security, and correctness as runtime constraints — not suggestions. A single `govern.json` controls what executes across 12 languages in one file.

```json
// govern.json — catches AI hallucinations before they execute
{
  "version": "3.0",
  "mode": "enforce",
  "code_quality": {
    "no_hallucinated_apis": { "level": "hard" },
    "no_oversimplification": { "level": "hard" },
    "no_incomplete_logic": { "level": "soft" }
  }
}
```

```
$ naab-lang app.naab

Error: Hallucinated API in python block:
  ".push()" is not valid Python                        [HARD]

  Rule: code_quality.no_hallucinated_apis

  Help:
  .push() is JavaScript — in Python, use .append()

  ✗ Wrong: my_list.push(item)
  ✓ Right: my_list.append(item)
```

---

## The Problem: AI Code Drift

AI models generate code that looks right but isn't. Every session starts fresh — no memory of your security rules, naming conventions, or architecture decisions:

- **Hallucinated APIs** — `.push()` in Python, `print()` in JavaScript, `json.stringify()` instead of `json.dumps()`
- **Stubs shipped as "complete"** — `def validate(): return True`, functions with only `pass` or `NotImplementedError`
- **Security patterns bypassed** — hardcoded secrets, SQL injection, `except: pass` swallowing errors
- **Language misuse** — Python for heavy computation, JavaScript for file operations, Shell for complex logic

Prompts are suggestions. **`govern.json` is enforcement.** NAAb checks every polyglot block against your policies before execution — where it can't be bypassed.

---

## At a Glance

| Capability | Details |
|---|---|
| **Governance Engine** | 50+ checks, 3-tier enforcement (hard / soft / advisory), `govern.json` config |
| **Polyglot Execution** | 12 languages in one file — Python, JavaScript, Rust, C++, Go, C#, Ruby, PHP, Shell, Nim, Zig, Julia |
| **Smart Error Messages** | "Did you mean?" suggestions via Levenshtein distance, detailed fixes with examples |
| **Standard Library** | 13 modules — array, string, math, json, http, file, time, debug, env, csv, regex, crypto, bolo |
| **Language Features** | Pattern matching, async/await, lambdas, closures, pipeline operator, if-expressions |
| **CI/CD Integration** | SARIF (GitHub Code Scanning), JUnit XML (Jenkins/GitLab), JSON reports |
| **Project Context** | Auto-reads CLAUDE.md, .editorconfig, .eslintrc, package.json to supplement governance |
| **Developer Tools** | REPL, LLM-friendly syntax (keyword aliases, optional semicolons), 204 error messages |

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

---

## Governance Engine

NAAb's governance engine is what sets it apart. Drop a `govern.json` in your project root and every polyglot block is checked before execution.

### What It Catches

| Category | Examples | Patterns |
|---|---|---|
| **Hallucinated APIs** | `.push()` in Python, `print()` in JS, `len()` in JS | 86+ cross-language patterns |
| **Oversimplification** | `def validate(): return True`, `pass`-only bodies, identity functions | 35+ stub patterns |
| **Incomplete Logic** | `except: pass`, bare raises, `"something went wrong"` messages | 40+ patterns |
| **Security** | SQL injection, path traversal, shell injection, hardcoded secrets | Entropy-based detection |
| **Code Quality** | TODO/FIXME, debug artifacts, mock data, hardcoded URLs/IPs | Dead code detection |
| **PII Exposure** | SSN patterns, credit card numbers, API keys in strings | Regex + entropy |

### Three Enforcement Levels

- **HARD** — Block execution. Code does not run. No override.
- **SOFT** — Block execution. Code does not run unless `--governance-override` is passed.
- **ADVISORY** — Warn and continue. Logged in audit trail.

### govern.json Example

```json
{
  "version": "3.0",
  "mode": "enforce",

  "languages": {
    "allowed": ["python", "javascript", "go"],
    "blocked": ["php"]
  },

  "code_quality": {
    "no_secrets": { "level": "hard" },
    "no_sql_injection": { "level": "hard" },
    "no_oversimplification": { "level": "hard" },
    "no_incomplete_logic": { "level": "soft" },
    "no_hallucinated_apis": { "level": "soft" }
  },

  "restrictions": {
    "no_eval": { "level": "hard" },
    "no_shell_injection": { "level": "hard" }
  },

  "limits": {
    "max_lines_per_block": 200,
    "timeout_seconds": 30
  }
}
```

### Project Context Awareness

NAAb can read your existing project configuration files and supplement governance rules — without overriding `govern.json`:

- **Layer 1:** LLM instruction files — `CLAUDE.md`, `.cursorrules`, `AGENTS.md`
- **Layer 2:** Linter configs — `.eslintrc`, `.flake8`, `pyproject.toml`
- **Layer 3:** Project manifests — `package.json`, `Cargo.toml`, `go.mod`

Each layer is opt-in and toggleable.

### Custom Rules

Define your own regex-based governance rules:

```json
{
  "custom_rules": [
    {
      "name": "no_print_debugging",
      "pattern": "console\\.log|print\\(.*debug",
      "message": "Remove debug print statements before committing",
      "level": "soft"
    }
  ]
}
```

### CI/CD Integration

```bash
# GitHub Code Scanning (SARIF)
naab-lang app.naab --governance-report sarif > results.sarif

# Jenkins / GitLab CI (JUnit XML)
naab-lang app.naab --governance-report junit > results.xml

# Custom tooling (JSON)
naab-lang app.naab --governance-report json > results.json
```

> **[Build your govern.json interactively](https://b-macker.github.io/NAAb/governance.html)** | [Full governance reference (Chapter 21)](docs/book/chapter21.md)

---

## Polyglot Execution

Use each language where it shines — Python for data science, Rust for performance, JavaScript for web, Go for concurrency, Shell for file ops. Variables flow between languages automatically. No FFI, no serialization, no microservices.

**Supported languages:** Python · JavaScript · Rust · C++ · Go · C# · Ruby · PHP · Shell · Nim · Zig · Julia

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

### Key Polyglot Features

- **Variable binding** — Pass NAAb variables into polyglot blocks with `<<python[x, y]`
- **Parallel execution** — Independent blocks run concurrently with automatic dependency analysis
- **Persistent runtimes** — Keep interpreter state across multiple calls with the `runtime` keyword
- **JSON sovereign pipe** — Return structured data from any language with `naab_return()` or `-> JSON` header
- **Error mapping** — Polyglot errors map back to NAAb source locations and flow into try/catch
- **Block-header awareness** — Go gets `package main`, PHP gets `<?php` automatically

---

## Language Features

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

### Lambdas & Closures

```naab
main {
    let multiplier = 3
    let scale = fn(x) { return x * multiplier }

    print(scale(10))  // 30

    // Closures capture their environment
    function make_counter() {
        let count = 0
        return fn() {
            count = count + 1
            return count
        }
    }

    let counter = make_counter()
    print(counter())  // 1
    print(counter())  // 2
}
```

### Pipeline Operator

```naab
main {
    let result = "hello world"
        |> string.upper()
        |> string.split(" ")

    print(result)  // ["HELLO", "WORLD"]
}
```

### If Expressions

```naab
main {
    let score = 85
    let grade = if score >= 90 { "A" } else if score >= 80 { "B" } else { "C" }
    print(grade)  // "B"
}
```

### Error Handling

```naab
main {
    try {
        let result = <<python
raise ValueError("something broke")
>>
    } catch (e) {
        print("Caught from Python: " + e)
    }
}
```

### More Language Features

- Variables, constants, functions
- Structs, enums, classes
- Module system with imports/exports
- For loops, while loops, break/continue
- Dictionaries and arrays with dot-notation methods

---

## Standard Library

13 modules with 204 error messages, "Did you mean?" suggestions, and detailed documentation.

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

| Module | Functions |
|---|---|
| `array` | push, pop, shift, unshift, map_fn, filter_fn, reduce_fn, sort, slice_arr, find, reverse, length, contains, first, last, join |
| `string` | split, join, upper, lower, trim, replace, reverse, contains, starts_with, ends_with, length, char_at, index_of, substring, repeat, pad_left, pad_right |
| `math` | sqrt, pow, abs, floor, ceil, round, min, max, sin, cos, random, PI, E |
| `json` | parse, stringify |
| `http` | get, post, put, delete (with headers and body) |
| `file` | read, write, append, exists, delete, list_dir |
| `time` | now, now_millis, sleep, format_timestamp, parse_datetime, year, month, day, hour, minute, second, weekday |
| `debug` | inspect, type, trace, watch, snapshot, diff, keys, values, log, timer, compare, stack, env |
| `env` | get, set_var, list |
| `csv` | parse, stringify |
| `regex` | search, matches, find, find_all, replace, replace_first, split, groups, find_groups, escape, is_valid |
| `crypto` | hash, sha256, sha512, md5, sha1, random_bytes, random_string, random_int, base64_encode, base64_decode, hex_encode, hex_decode, compare_digest, generate_token, hash_password |
| `bolo` | scan, report (governance integration) |

---

## Developer Experience

### Smart Error Messages

NAAb doesn't just tell you what's wrong — it tells you how to fix it:

```
Error: Unknown function "array.pussh"

  Did you mean: array.push ?

  Help:
  - array.push(arr, value) adds an element to the end of an array

  Example:
    ✗ Wrong: array.pussh(my_list, 42)
    ✓ Right: array.push(my_list, 42)
```

### Common Mistake Detection

NAAb detects ~35 patterns where developers (and AI) use the wrong language's idioms:

- `array.append()` → "That's Python. In NAAb, use `array.push()`"
- `console.log()` → "That's JavaScript. In NAAb, use `print()`"
- `str.upper()` → "Use `string.upper(str)` or `str.upper()` (dot-notation)"

### LLM-Friendly Syntax

NAAb accepts multiple keyword styles so AI-generated code works without manual edits:

- `function` / `func` / `fn` / `def` — all valid
- `let` / `const` — mutable and immutable bindings
- Semicolons — optional (accepted but not required)
- `return` — optional in single-expression functions

---

## NAAb Ecosystem

Three tools built with NAAb — code governance, performance optimization, and data security:

| Project | Purpose | Key Features |
|---------|---------|--------------|
| **[NAAb BOLO](https://github.com/b-macker/naab-bolo)** | Code governance & validation | 50+ checks, SARIF reports, AI drift detection |
| **[NAAb Pivot](https://github.com/b-macker/naab-pivot)** | Code evolution & optimization | 3-60x speedups, proven correctness, 8 languages |
| **[NAAb Passage](https://github.com/b-macker/naab-passage)** | Data gateway & PII protection | Zero leakage, sovereign architecture, HIPAA/GDPR |

### NAAb BOLO — Code Governance & Validation

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

### NAAb Pivot — Code Evolution & Optimization

**[NAAb Pivot](https://github.com/b-macker/naab-pivot)** rewrites slow code in faster languages with mathematical proof of correctness.

```bash
# Analyze hotspots (Python → Rust candidates)
naab-lang pivot.naab analyze app.py

# Rewrite with proof
naab-lang pivot.naab rewrite app.py:expensive_loop --target rust --prove

# Result: 45x faster, semantically identical
```

**3-60x speedups · 8 source languages · Proven correct** → [Get started](https://github.com/b-macker/naab-pivot)

### NAAb Passage — Data Gateway & PII Protection

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
  Governance Engine ──> Policy checks (govern.json)
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

- **73,000+** lines of C++17
- **208** test files, **325** mono test assertions
- **13** standard library modules with **204** error messages
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
