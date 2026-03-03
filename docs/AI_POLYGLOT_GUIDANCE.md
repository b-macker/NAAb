# Polyglot Script Design Guide for AI Assistants

## Core Principle

**Use the BEST language for EACH micro-task, not one language for everything.**

When writing NAAb scripts, resist the temptation to default to Python or JavaScript for all operations. NAAb's strength lies in seamlessly combining multiple languages, each used at the optimal point for maximum performance and expressiveness.

## Decision Framework

Before writing a polyglot block, ask these questions:

1. **What is the PRIMARY operation?** (computation, I/O, parsing, systems, web)
2. **Which language is OPTIMAL for this operation?** (consult table below)
3. **Can I break this into smaller tasks using different languages?**
4. **What's the performance impact?** (10x faster? 100x faster?)
5. **What's the trade-off?** (readability vs performance vs ecosystem)

##Task→Language Scoring Matrix

### Complete Reference Table

| Task | 1st Choice (Score) | 2nd Choice | 3rd Choice | Avoid |
|------|-------------------|------------|------------|-------|
| **Numerical/Math** | Julia (100) | Nim (95) | Python (70) | JavaScript (30), Shell (10) |
| **String Processing** | Python (100) | Ruby (95) | Nim (90) | Zig (40), C++ (35) |
| **File Operations** | Shell (100) | Python (80) | Nim (75) | JavaScript (40) |
| **Systems/Memory** | Zig (100) | Rust (95) | C++ (85) | Python (20), JS (10) |
| **Web APIs** | JavaScript (100) | Python (90) | Go (85) | Zig (40), C++ (35) |
| **Concurrency** | Go (100) | Rust (95) | Julia (85) | Python (50), Ruby (40) |
| **JSON Processing** | JavaScript (100) | Python (95) | Go (90) | C++ (50), Shell (30) |
| **CLI Tools** | Shell (100) | Python (85) | Go (80) | JavaScript (50) |
| **Data Parsing** | Python (100) | JavaScript (85) | Ruby (80) | Shell (40) |

### Performance Impact Guidelines

- **Julia vs Python** (numerical): 10-100x faster
- **Zig/Rust vs Python** (systems): 50-200x faster
- **Go vs Python** (concurrency): 5-20x faster
- **Shell vs Python** (file ops): 2-5x faster (for simple operations)
- **Nim vs Python** (general): 10-50x faster

## Language Strengths Quick Reference

### Julia
- **Best for:** Scientific computing, linear algebra, statistics, ML
- **Strengths:** JIT-compiled (LLVM), multiple dispatch, built-in BLAS/LAPACK
- **Weaknesses:** JIT warmup time, smaller ecosystem than Python
- **When to use:** Heavy numerical computation, matrix operations, statistical analysis

### Python
- **Best for:** Data processing, scripting, glue code, prototyping
- **Strengths:** Huge ecosystem (PyPI), readable syntax, rich standard library
- **Weaknesses:** Slow execution, GIL limits threading
- **When to use:** Data transformation, API integration, rapid prototyping

### JavaScript
- **Best for:** Web APIs, JSON manipulation, async operations
- **Strengths:** Native JSON, fetch API, async/await, NPM ecosystem
- **Weaknesses:** Type coercion surprises, limited numerical precision
- **When to use:** HTTP requests, web scraping, JSON processing

### Go
- **Best for:** Concurrency, network services, CLI tools
- **Strengths:** Goroutines (lightweight threads), fast compilation, single binary
- **Weaknesses:** Verbose error handling, no generics (pre-1.18)
- **When to use:** Parallel processing, concurrent operations, system tools

### Rust
- **Best for:** Systems programming, memory-safe low-level code
- **Strengths:** Memory safety without GC, zero-cost abstractions, fearless concurrency
- **Weaknesses:** Steep learning curve, slower compilation
- **When to use:** Performance-critical systems code, safe memory management

### Zig
- **Best for:** Systems programming, C replacement, explicit control
- **Strengths:** Explicit memory control, comptime metaprogramming, C interop
- **Weaknesses:** Immature ecosystem, still pre-1.0
- **When to use:** Low-level operations, memory management, C interop

### Nim
- **Best for:** Performance + readability, compiled speed with Python-like syntax
- **Strengths:** Python-like syntax, compiles to C, metaprogramming
- **Weaknesses:** Smaller community, fewer libraries
- **When to use:** Need Python readability with C performance

### Shell/Bash
- **Best for:** File operations, text processing, command composition
- **Strengths:** Purpose-built for files, pipeline composition, Unix tool integration
- **Weaknesses:** Poor error handling, hard to maintain complex scripts
- **When to use:** Simple file ops, text filtering, command chaining

### Ruby
- **Best for:** String handling, scripting, DSLs
- **Strengths:** Excellent string methods, expressive syntax, blocks
- **Weaknesses:** Slower execution, GIL limits threading
- **When to use:** Text processing, scripting, when expressiveness matters

## Examples of Good Polyglot Composition

### Example 1: Data Processing Pipeline

```naab
// GOOD: Multiple languages for optimal performance
main {
    // Shell for file listing (best at text processing)
    let csv_files = <<shell find /data -name "*.csv" -type f >>

    // Python for CSV parsing (rich libraries)
    let parsed_data = <<python
import csv
with open('data.csv') as f:
    data = list(csv.DictReader(f))
    >>

    // Julia for statistical analysis (fast math)
    let statistics = <<julia
using Statistics
values = [row["value"] for row in parsed_data]
(mean=mean(values), std=std(values))
    >>

    // Nim for JSON output (fast serialization)
    let json_output = <<nim
import json
echo %* {
    "mean": statistics.mean,
    "std": statistics.std,
    "count": parsed_data.len
}
    >>
}
```

**Why this is good:**
- Shell: 2-3x faster than Python for file operations
- Python: Rich CSV library ecosystem
- Julia: 10-100x faster than Python for statistics
- Nim: Fast JSON serialization with clean syntax

### Example 2: Web Scraper

```naab
// GOOD: Right language for each phase
main {
    // JavaScript for web fetching (built-in fetch API)
    let html = <<javascript
const response = await fetch('https://example.com');
const text = await response.text();
text;
    >>

    // Python for HTML parsing (BeautifulSoup)
    let links = <<python
from bs4 import BeautifulSoup
soup = BeautifulSoup(html, 'html.parser')
[link.get('href') for link in soup.find_all('a')]
    >>

    // Shell for URL validation
    let valid_urls = <<bash
echo "$links" | tr ' ' '\n' | grep -E '^https://'
    >>

    // Go for concurrent downloading
    let downloads = <<go
package main
import "sync"
var wg sync.WaitGroup
for _, url := range valid_urls {
    wg.Add(1)
    go download(url, &wg)
}
wg.Wait()
    >>
}
```

### Example 3: Systems Tool

```naab
// GOOD: Specialized languages for different tasks
main {
    // Shell for process listing
    let processes = <<bash ps aux | grep "python" >>

    // Zig for memory inspection (safe systems code)
    let memory_info = <<zig
const std = @import("std");
const allocator = std.heap.page_allocator;
// Read /proc/meminfo
    >>

    // Python for data aggregation
    let summary = <<python
import json
json.dumps({
    "processes": len(processes.split('\n')),
    "memory_mb": memory_info / 1024 / 1024
})
    >>
}
```

## Anti-Patterns to Avoid

### ❌ BAD: Python for Everything

```naab
// Suboptimal: Using Python for tasks better suited to other languages
main {
    let result = <<python
# File operations (shell is 2-3x faster)
import subprocess
files = subprocess.check_output(['find', '.', '-name', '*.txt'])

# Heavy math (Julia is 10-100x faster)
import numpy as np
matrix = np.random.randn(10000, 10000)
eigenvalues = np.linalg.eig(matrix)

# Systems calls (Zig/Rust safer and faster)
import os
os.system('chmod +x script.sh')

# JSON processing (JavaScript more natural)
import json
data = json.loads(response_text)
    >>
}
```

### ✅ GOOD: Polyglot Optimization

```naab
main {
    let files = <<shell find . -name "*.txt" >>
    let eigenvalues = <<julia using LinearAlgebra; eigvals(randn(10000,10000)) >>
    let perms = <<zig // safe chmod implementation >>
    let data = <<javascript JSON.parse(response_text) >>
}
```

**Performance Gain:** 20-100x overall speedup

### ❌ BAD: JavaScript for File Operations

```naab
// Suboptimal: JavaScript lacks file operation tools
main {
    let files = <<javascript
const fs = require('fs');
const files = fs.readdirSync('.')
    .filter(f => f.endsWith('.txt'));
    >>
}
```

### ✅ GOOD: Shell for File Operations

```naab
main {
    let files = <<shell find . -name "*.txt" -type f >>
}
```

### ❌ BAD: Shell for Complex Logic

```naab
// Suboptimal: Shell is hard to maintain for complex logic
main {
    let result = <<bash
if [ -f "data.txt" ]; then
    while IFS= read -r line; do
        if [[ "$line" =~ ^[0-9]+$ ]]; then
            sum=$((sum + line))
            count=$((count + 1))
        fi
    done < data.txt
    echo "scale=2; $sum / $count" | bc
fi
    >>
}
```

### ✅ GOOD: Python for Complex Logic

```naab
main {
    let result = <<python
if os.path.exists("data.txt"):
    numbers = [int(line) for line in open("data.txt") if line.strip().isdigit()]
    print(sum(numbers) / len(numbers) if numbers else 0)
    >>
}
```

## Complexity Threshold Guidance

### Simple Scripts (1-5 operations)
- **Language count:** 1-2 languages acceptable
- **Example:** Single computation, simple I/O
- **Optimization opportunity:** Low (< 10% improvement usually)

### Medium Scripts (6-15 operations)
- **Language count:** 3-4 languages recommended
- **Example:** Data pipeline, web scraper, CLI tool
- **Optimization opportunity:** Medium (20-50% improvement possible)

### Complex Scripts (16+ operations)
- **Language count:** 4+ languages for optimal performance
- **Example:** Multi-stage processing, distributed system, ML pipeline
- **Optimization opportunity:** High (50-200% improvement possible)

## Implementation Strategy for AI Assistants

1. **Identify discrete tasks** in the user's request
2. **Map each task** to optimal language using scoring matrix
3. **Compose polyglot blocks** with clear task boundaries
4. **Explain language choices** in comments
5. **Provide performance rationale** when using non-obvious language choice

### Example Workflow

**User request:** "Process CSV files, calculate statistics, generate JSON report"

**AI analysis:**
- Task 1: CSV parsing → Python (100) - rich libraries
- Task 2: Statistics → Julia (100) - 10-100x faster than Python
- Task 3: JSON output → JavaScript (100) OR Nim (85) - native JSON

**AI implementation:**
```naab
// Data processing pipeline with optimal language per task
main {
    // Python: Rich CSV library (pandas, csv module)
    let data = <<python
import csv
with open('data.csv') as f:
    list(csv.DictReader(f))
    >>

    // Julia: 10-100x faster for numerical operations
    let stats = <<julia
using Statistics
values = [parse(Float64, row["value"]) for row in data]
(mean=mean(values), median=median(values), std=std(values))
    >>

    // JavaScript: Native JSON support
    let report = <<javascript
JSON.stringify({
    timestamp: new Date().toISOString(),
    statistics: stats,
    count: data.length
}, null, 2)
    >>

    println(report)
}
```

## Detection and Enforcement

The NAAb governance system will detect suboptimal language choices and suggest alternatives based on:

1. **Pattern detection:** Code patterns indicate task type (numerical, string, I/O)
2. **Scoring:** Each language scored 0-100 for detected task
3. **Improvement calculation:** Optimal vs current language score
4. **Enforcement levels:**
   - `none`: No checking
   - `advisory`: Suggestions only
   - `soft`: Block with override option
   - `hard`: Block execution

### Example Detection

```naab
// Code written in Python
main {
    let result = <<python
import numpy as np
matrix = np.random.randn(10000, 10000)
eigenvalues, eigenvectors = np.linalg.eig(matrix)
    >>
}
```

**Detection output:**
```
💡 Hint: Language optimization opportunity detected.

  Current language: python (for numerical_operations task)
  Optimal language: julia
  Potential improvement: +80%

  Reasons:
    • Julia provides 10-100x speedup for numerical operations
    • Built-in BLAS/LAPACK integration
    • JIT compilation optimized for math

  Example refactoring:
    ✗ Current: <<python  [numpy code] >>
    ✓ Better:  <<julia   [LinearAlgebra code] >>

  For more: docs/polyglot/optimization_guide.md
```

## Summary Checklist

When writing NAAb scripts, ensure:

- [ ] Each polyglot block uses the optimal language for its operation
- [ ] No single language dominates (< 70% for complex scripts)
- [ ] Performance-critical sections use compiled languages (Julia, Nim, Zig, Rust, Go)
- [ ] String/text operations use high-level languages (Python, Ruby)
- [ ] File operations use shell tools when appropriate
- [ ] JSON processing uses JavaScript or languages with native JSON
- [ ] Concurrency uses Go or Rust
- [ ] Reasoning for language choices is clear
- [ ] Code patterns from one language aren't misused in another

## References

- **Polyglot Executor Docs:** `docs/polyglot/*.md` (12 languages)
- **Governance Config:** `govern.json` with `polyglot_optimization` section
- **Task→Language Matrix:** Built-in defaults + govern.json overrides
- **Detection System:** 7-layer analysis (lexical → syntactic → semantic → mismatch → scoring → composite)
