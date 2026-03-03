# Task→Language Scoring Matrix

Complete reference for the polyglot optimization system's language scoring database.

## Overview

The task→language matrix assigns scores (0-100) to each language for specific task types. Higher scores indicate better suitability. The system uses this matrix to recommend optimal languages during code analysis.

**Score Ranges:**
- **90-100:** Excellent choice, optimal for this task
- **75-89:** Very good, strong alternative
- **60-74:** Acceptable, workable choice
- **40-59:** Suboptimal, consider alternatives
- **0-39:** Poor choice, likely performance/ergonomics issues

## Complete Scoring Matrix

### 1. Numerical Operations

**Task Description:** Heavy mathematical computations, linear algebra, statistics, matrix operations, numerical analysis.

| Language | Score | Rationale |
|----------|-------|-----------|
| **Julia** | 100 | JIT-compiled (LLVM), built-in BLAS/LAPACK, multiple dispatch, designed for numerical computing |
| **Nim** | 95 | Compiles to C, fast numerical libraries, Python-like syntax with C performance |
| **Python** | 70 | NumPy/SciPy excellent but interpreter overhead, good for prototyping |
| **Rust** | 65 | Fast but verbose, good numerical libraries emerging |
| **Go** | 50 | No operator overloading, limited math libraries |
| **C++** | 85 | Raw performance with Eigen/Armadillo, but verbose |
| **JavaScript** | 30 | Limited precision (53-bit integers), slow for math |
| **Shell** | 10 | Not designed for computation, only basic calc |

**Performance Impact:** Julia vs Python = 10-100x faster for numerical code

**Benchmark Example:**
```
Matrix multiplication (10000×10000):
- Julia: 2.3s
- Nim: 2.8s
- Python (NumPy): 8.5s
- JavaScript: 180s
```

---

### 2. String Processing

**Task Description:** Text manipulation, regex matching, string parsing, formatting, encoding.

| Language | Score | Rationale |
|----------|-------|-----------|
| **Python** | 100 | Rich string API, regex built-in, extensive text libraries |
| **Ruby** | 95 | Excellent string handling, powerful regex, expressive syntax |
| **Nim** | 90 | Fast string operations, Python-like API |
| **JavaScript** | 85 | Good string methods, regex, template literals |
| **Go** | 70 | Decent but more verbose, good stdlib |
| **Rust** | 60 | Powerful but complex String/&str types |
| **Zig** | 40 | Low-level, manual string handling |
| **C++** | 35 | std::string verbose, no built-in regex until C++11 |

**Performance Impact:** Nim vs Python = 5-20x faster for string-heavy operations

**Feature Comparison:**
```
String slicing syntax:
- Python: s[1:5]      ✓ Simple
- Ruby: s[1..5]       ✓ Simple
- Nim: s[1..5]        ✓ Simple
- JavaScript: s.substring(1,5)  ~ Verbose
- Go: s[1:5]          ✓ Simple
- Rust: &s[1..5]      ~ Borrow checker
- C++: s.substr(1,4)  ~ Verbose
```

---

### 3. File Operations

**Task Description:** Filesystem traversal, file I/O, directory operations, file searching, text filtering.

| Language | Score | Rationale |
|----------|-------|-----------|
| **Shell** | 100 | Purpose-built for file ops, grep/find/sed/awk optimized |
| **Bash** | 100 | Same as shell, full Unix toolset |
| **Python** | 80 | Good pathlib/os modules, readable code |
| **Nim** | 75 | Fast filesystem access, good stdlib |
| **Go** | 70 | Good os/filepath packages |
| **Ruby** | 70 | File and Dir classes, clean API |
| **Rust** | 65 | Safe but verbose std::fs |
| **JavaScript** | 40 | Node.js fs module limited, async complexity |

**Performance Impact:** Shell vs Python = 2-5x faster for simple file operations

**Benchmark Example:**
```
Find all .txt files and count lines:
- Shell (find + wc): 0.3s
- Python (glob + read): 1.2s
- JavaScript (fs): 2.1s
```

**When Shell Excels:**
- Pattern matching: `find . -name "*.log"`
- Text filtering: `grep "ERROR" *.log | wc -l`
- Pipelines: `find . -type f | xargs wc -l | sort -n`

**When Python Better:**
- Complex logic (conditionals, loops)
- Cross-platform compatibility
- Error handling requirements

---

### 4. Systems Operations

**Task Description:** Memory management, pointers, low-level I/O, system calls, C interop.

| Language | Score | Rationale |
|----------|-------|-----------|
| **Zig** | 100 | C replacement, explicit memory control, comptime, no hidden control flow |
| **Rust** | 95 | Memory safety without GC, zero-cost abstractions, ownership system |
| **C++** | 85 | Full low-level control, mature tooling, but manual memory management |
| **Nim** | 80 | Can compile to C, access low-level features, ref counting option |
| **Go** | 60 | GC limits low-level control, but good for system tools |
| **C** | 90 | Direct hardware access, but manual everything |
| **Python** | 20 | Too high-level, GIL, slow |
| **JavaScript** | 10 | Not designed for systems programming |

**Performance Impact:** Zig/Rust vs Python = 50-200x faster for systems code

**Memory Safety Comparison:**
```
Use-after-free protection:
- Zig: Compile-time + runtime checks
- Rust: Compile-time (borrow checker)
- C++: None (manual tracking)
- Python: GC handles it (with overhead)
```

---

### 5. Web APIs

**Task Description:** HTTP requests, REST APIs, JSON handling, web scraping, async I/O.

| Language | Score | Rationale |
|----------|-------|-----------|
| **JavaScript** | 100 | Native fetch API, JSON.parse/stringify, async/await built-in |
| **Python** | 90 | requests library excellent, aiohttp for async |
| **Go** | 85 | net/http production-grade, clean concurrency |
| **Ruby** | 80 | Net::HTTP, RestClient, good HTTP libraries |
| **Nim** | 70 | httpclient module available, async support |
| **Rust** | 65 | reqwest/hyper powerful but verbose |
| **Zig** | 40 | Low-level HTTP, manual parsing |
| **C++** | 35 | libcurl complex, no standard HTTP library |

**Performance Impact:** Go vs Python = 3-10x faster for HTTP services

**Ease of Use Comparison:**
```javascript
// JavaScript (score: 100) - 3 lines
const response = await fetch(url);
const data = await response.json();
console.log(data);

// Python (score: 90) - 4 lines
import requests
response = requests.get(url)
data = response.json()
print(data)

// Rust (score: 65) - 8+ lines (verbose)
let response = reqwest::get(url).await?;
let data: Value = response.json().await?;
println!("{:?}", data);
```

---

### 6. Concurrency

**Task Description:** Parallel processing, multi-threading, async operations, message passing.

| Language | Score | Rationale |
|----------|-------|-----------|
| **Go** | 100 | Goroutines (lightweight threads), channels built-in, CSP model |
| **Rust** | 95 | Fearless concurrency, compile-time data race prevention |
| **Julia** | 85 | Tasks and channels, good parallel computing support |
| **Nim** | 75 | async/await, threading support |
| **JavaScript** | 70 | async/await good, but single-threaded (workers for parallelism) |
| **Python** | 50 | asyncio available, but GIL limits true parallelism |
| **Ruby** | 40 | Threads limited by GIL |
| **Shell** | 60 | & background processes, simple pipes |

**Performance Impact:** Go vs Python = 5-20x faster for parallel workloads

**Concurrency Model Comparison:**
```
1 million concurrent operations:
- Go (goroutines): 2-3s, ~200MB RAM
- Rust (tokio): 2-4s, ~150MB RAM
- Python (asyncio): 8-12s, ~400MB RAM
- Python (threads, GIL): 30-40s, ~600MB RAM
```

---

### 7. JSON Processing

**Task Description:** JSON parsing, serialization, manipulation, schema validation.

| Language | Score | Rationale |
|----------|-------|-----------|
| **JavaScript** | 100 | JSON.parse/stringify native, no external dependencies |
| **Python** | 95 | json module in stdlib, excellent performance |
| **Go** | 90 | encoding/json in stdlib, struct tags for mapping |
| **Nim** | 85 | json module fast, compile-time parsing available |
| **Ruby** | 80 | JSON class built-in, clean API |
| **Rust** | 75 | serde powerful but verbose, compile-time guarantees |
| **Julia** | 70 | JSON.jl available, good integration |
| **C++** | 50 | nlohmann/json good but not stdlib |
| **Shell** | 30 | jq external tool, complex for programmatic use |

**Performance Impact:** Nim vs Python = 3-10x faster for JSON parsing

**API Simplicity:**
```javascript
// JavaScript (score: 100)
const obj = JSON.parse(jsonString);
const json = JSON.stringify(obj);

// Python (score: 95)
import json
obj = json.loads(json_string)
json_str = json.dumps(obj)

// Rust (score: 75) - requires serde
let obj: MyStruct = serde_json::from_str(&json_string)?;
let json_str = serde_json::to_string(&obj)?;
```

---

### 8. CLI Tools

**Task Description:** Command-line applications, argument parsing, terminal output, scripting.

| Language | Score | Rationale |
|----------|-------|-----------|
| **Shell** | 100 | Purpose-built for CLI, composable with Unix tools |
| **Bash** | 100 | Same as shell, full scripting capabilities |
| **Python** | 85 | argparse/click excellent, readable scripts |
| **Go** | 80 | Fast single-binary, flag package good |
| **Rust** | 75 | clap crate powerful, but compilation time |
| **Nim** | 70 | Fast CLI tools, good stdlib |
| **Ruby** | 65 | OptionParser good, scripting-friendly |
| **JavaScript** | 50 | Node.js works but slow startup, npm overhead |

**Startup Time Comparison:**
```
Hello world CLI execution time:
- Shell: <0.001s
- Go (compiled): 0.003s
- Python: 0.025s
- JavaScript (Node): 0.080s
- Ruby: 0.045s
```

---

### 9. Data Parsing (CSV, XML, etc.)

**Task Description:** Structured data parsing, CSV/XML/YAML processing, data transformation.

| Language | Score | Rationale |
|----------|-------|-----------|
| **Python** | 100 | pandas, csv, lxml, PyYAML - rich ecosystem |
| **JavaScript** | 85 | cheerio (XML), csv-parser, good libraries |
| **Ruby** | 80 | Nokogiri (XML), CSV class, yaml built-in |
| **Go** | 75 | encoding/csv, encoding/xml in stdlib |
| **Nim** | 70 | Fast parsers available, compile-time options |
| **Rust** | 65 | csv/serde/quick-xml powerful but verbose |
| **Julia** | 60 | DataFrames.jl, CSV.jl available |
| **Shell** | 40 | awk/sed limited, complex parsing difficult |

---

### 10. Machine Learning / AI

| Language | Score | Rationale |
|----------|-------|-----------|
| **Python** | 100 | TensorFlow, PyTorch, scikit-learn - ecosystem dominates |
| **Julia** | 90 | Flux.jl, fast native code, growing ML ecosystem |
| **Rust** | 60 | ndarray, tch-rs available but immature |
| **C++** | 80 | TensorFlow C++ API, libtorch, production inference |
| **JavaScript** | 50 | TensorFlow.js for browser, limited server-side |

---

## Using the Matrix

### In govern.json

```json
{
  "polyglot_optimization": {
    "task_language_matrix": {
      "numerical_operations": {
        "julia": {"score": 100, "reason": "JIT-compiled math"},
        "nim": {"score": 95, "reason": "Compiled performance"},
        "python": {"score": 70, "reason": "NumPy good but slow"}
      },
      "string_processing": {
        "python": {"score": 100, "reason": "Rich string API"},
        "ruby": {"score": 95, "reason": "Excellent string handling"}
      }
    }
  }
}
```

### Programmatic Access

The analyzer loads this matrix and uses it for scoring:

```cpp
// In language_scorer.cpp
auto score = scoreLanguageForTask("julia", "numerical_operations");
// Returns: LanguageScore{language="julia", score=100, reason="..."}
```

### Override Defaults

You can override built-in scores in your project's `govern.json`:

```json
{
  "polyglot_optimization": {
    "task_language_matrix": {
      "numerical_operations": {
        "julia": {"score": 100},
        "my_custom_lang": {"score": 95, "reason": "Custom accelerator"}
      }
    }
  }
}
```

---

## Score Adjustment Factors

The base scores are adjusted based on:

1. **Computational Profile**
   - CPU-intensive: +10 for compiled languages
   - Memory-intensive: +5 for manual memory management
   - I/O-intensive: +5 for high-level languages

2. **Code Complexity**
   - High complexity (>70): +5 for readable languages (Python, Ruby, Nim)
   - High complexity: -3 for complex languages (Rust, C++)

3. **Latency Sensitivity**
   - Latency-sensitive: +8 for fast-startup compiled (Go, Rust, Zig)
   - Latency-sensitive: -5 for JIT-compiled (Julia)

4. **Throughput Focus**
   - Throughput-focused: Favor parallel-friendly (Go, Rust)

---

## Performance Benchmarks

### Real-World Impact Examples

**Example 1: Matrix Operations**
```
Task: SVD of 10000×10000 matrix
- Julia: 2.3s (baseline)
- Nim: 2.8s (22% slower)
- Python (NumPy): 8.5s (270% slower)
- JavaScript: timeout (>60s)

Switching Python→Julia = 73% improvement
```

**Example 2: String Processing**
```
Task: Parse 100MB log file, extract patterns
- Python (regex): 3.2s (baseline)
- Nim: 0.8s (75% faster)
- Ruby: 3.5s (similar to Python)
- Rust: 0.6s (81% faster, but verbose)

Switching Python→Nim = 75% improvement
```

**Example 3: File Operations**
```
Task: Find and count lines in 50k files
- Shell (find + wc): 1.2s (baseline)
- Python (glob + read): 5.1s (325% slower)
- Go: 2.8s (133% slower)

Using Shell = 76% improvement vs Python
```

---

## Ecosystem Maturity

Consider library availability when overriding scores:

| Domain | Leader | Alternatives |
|--------|--------|--------------|
| ML/AI | Python | Julia, C++ |
| Web | JavaScript, Python | Go, Ruby |
| Systems | C, C++, Rust | Zig, Nim |
| Data Science | Python, R | Julia |
| DevOps | Shell, Python | Go |

---

## References

- Benchmark methodology: `docs/benchmarks/`
- Language executor docs: `docs/polyglot/*.md`
- Governance config: `govern.json` examples
- Detection system: `docs/polyglot/pattern_detection_internals.md`
