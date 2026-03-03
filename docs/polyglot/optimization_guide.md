# Polyglot Optimization Guide

Complete guide to using NAAb's polyglot optimization system for maximum performance and code quality.

## Table of Contents

1. [Quick Start](#quick-start)
2. [How It Works](#how-it-works)
3. [Configuration](#configuration)
4. [Interpreting Suggestions](#interpreting-suggestions)
5. [Enforcement Levels](#enforcement-levels)
6. [Tuning for Your Project](#tuning-for-your-project)
7. [Best Practices](#best-practices)
8. [Troubleshooting](#troubleshooting)

---

## Quick Start

### Enable Polyglot Optimization

Create `govern.json` in your project root:

```json
{
  "version": "3.0",
  "polyglot_optimization": {
    "enabled": true,
    "enforcement_level": "advisory"
  }
}
```

### Run Your Script

```bash
cd ~/.naab/language
./build/naab-lang your_script.naab
```

### See Suggestions

If suboptimal language choices are detected:

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
```

---

## How It Works

The optimization system uses **7 detection layers**:

### Layer 1: Lexical Analysis
- Counts 205 token patterns across 10 categories
- Examples: `matrix`, `mean`, `regex`, `fetch`, `malloc`
- Output: Token frequency map

```
numerical_computation: 15
string_manipulation: 3
file_operations: 8
```

### Layer 2: Syntactic Analysis
- Analyzes code structure: loops, functions, nesting
- Detects: nested loops, recursion, array operations, pipelines
- Output: Complexity score (0-100), structure metrics

```
loop_count: 3
has_nested_loops: true
complexity_score: 65
```

### Layer 3: Semantic Analysis
- Infers programmer's intent from patterns
- Maps to 26 TaskIntent types
- Output: Primary + secondary intents, confidence

```
primary_intent: NUMERICAL_COMPUTATION
confidence: 0.85
```

### Layer 4: Language Mismatch Detection
- Detects idioms from wrong language
- Examples: Python list comprehensions in JavaScript
- Output: List of mismatches with confidence

```
mismatch: "Python with statement" in javascript
confidence: 90
```

### Layer 5: Language Scoring
- Scores current language for detected task (0-100)
- Looks up optimal language from matrix
- Output: Current score, optimal score, improvement %

```
current: python=70, optimal: julia=100
improvement: +43%
```

### Layer 6: Composite Detection
- Integrates all layers
- Generates comprehensive recommendation
- Output: Full DetectionResult

### Layer 7: Governance Enforcement
- Applies enforcement level rules
- Generates helper errors
- Blocks or warns based on configuration

---

## Configuration

### Basic Configuration

```json
{
  "polyglot_optimization": {
    "enabled": true,
    "enforcement_level": "soft",

    "pattern_detection": {
      "enabled": true
    },

    "language_diversity": {
      "enabled": true,
      "min_languages": 2,
      "max_single_language_percent": 70
    },

    "helper_errors": {
      "enabled": true,
      "show_alternative_language": true,
      "show_example_code": true
    }
  }
}
```

### Custom Task→Language Scores

Override built-in scores:

```json
{
  "polyglot_optimization": {
    "task_language_matrix": {
      "numerical_operations": {
        "julia": {"score": 100, "reason": "Best for our use case"},
        "python": {"score": 50, "reason": "Too slow for production"}
      },
      "string_processing": {
        "python": {"score": 100},
        "rust": {"score": 30, "reason": "Overkill for our needs"}
      }
    }
  }
}
```

### Custom Pattern Detection

Add your own task inference patterns:

```json
{
  "polyglot_optimization": {
    "pattern_detection": {
      "task_inference": {
        "cryptography": {
          "patterns": ["\\bcrypto\\b", "\\bhash\\b", "\\bencrypt\\b"],
          "optimal_languages": ["rust", "zig"],
          "suboptimal_languages": ["python", "javascript"],
          "message": "Cryptography requires memory-safe compiled language"
        }
      }
    }
  }
}
```

---

## Interpreting Suggestions

### Understanding the Output

When you see a suggestion:

```
💡 Hint: Language optimization opportunity detected.

  Current language: python (for numerical_operations task)  ← [1]
  Optimal language: julia                                   ← [2]
  Potential improvement: +80%                               ← [3]

  Reasons:                                                   ← [4]
    • Julia provides 10-100x speedup for numerical operations
    • Built-in BLAS/LAPACK integration
    • JIT compilation optimized for math

  Example refactoring:                                       ← [5]
    ✗ Current: <<python  [numpy code] >>
    ✓ Better:  <<julia   [LinearAlgebra code] >>
```

**[1] Task Detection:** System detected the operation type
**[2] Recommendation:** Based on scoring matrix
**[3] Improvement:** Calculated from score difference
**[4] Reasons:** Why the suggestion makes sense
**[5] Example:** Quick syntax reference

### When to Follow Suggestions

**✅ Follow when:**
- Improvement > 30% and operation is performance-critical
- Code runs frequently (hot path)
- Operation is isolated (easy to refactor)
- You're familiar with suggested language

**⚠️ Consider carefully when:**
- Improvement 10-30% (weigh against complexity)
- Code runs infrequently
- Suggested language unfamiliar
- Existing code works fine

**❌ Ignore when:**
- Improvement < 10%
- Prototyping/debugging
- Readability more important than performance
- Team doesn't know suggested language

### Score Interpretation

| Score Range | Interpretation | Action |
|-------------|----------------|--------|
| 90-100 | Excellent fit | Use this language |
| 75-89 | Very good | Strong choice |
| 60-74 | Acceptable | Workable |
| 40-59 | Suboptimal | Consider alternatives |
| 0-39 | Poor fit | Avoid for this task |

---

## Enforcement Levels

Configure how strictly the system enforces recommendations:

### 1. `none` - Disabled

No checking performed. Optimization system inactive.

```json
{"enforcement_level": "none"}
```

**Use when:** You don't want any suggestions

### 2. `advisory` - Suggestions Only

Shows suggestions but never blocks execution.

```json
{"enforcement_level": "advisory"}
```

**Output:**
```
Advisory: Consider using julia instead of python for +80% improvement
```

**Use when:**
- Learning about optimization opportunities
- Gathering data before enforcing
- Suggestions welcome but not mandatory

### 3. `soft` - Block with Override

Blocks execution but allows `--governance-override` flag.

```json
{"enforcement_level": "soft"}
```

**Output:**
```
SOFT violation: Suboptimal language choice
  Current: python (score: 70/100)
  Optimal: julia (score: 100/100)
  Improvement: +43%

  Override with --governance-override if needed.
```

**Override:**
```bash
./build/naab-lang --governance-override script.naab
```

**Use when:**
- Want to enforce best practices
- Allow exceptions with explicit flag
- Team alignment on optimization goals

### 4. `hard` - Strict Enforcement

Blocks execution with no override possible.

```json
{"enforcement_level": "hard"}
```

**Output:**
```
HARD violation: Suboptimal language choice
  This code MUST use a more appropriate language.
```

**Use when:**
- Production code with strict performance requirements
- Critical paths that must be optimized
- Final enforcement after team training

---

## Tuning for Your Project

### Step 1: Start with Advisory

Begin with `advisory` level to gather data:

```json
{"enforcement_level": "advisory"}
```

Run your test suite and collect suggestions:

```bash
./build/naab-lang tests/*.naab 2>&1 | grep "Advisory" > optimization_report.txt
```

### Step 2: Analyze Suggestions

Review the report:
- How many suggestions?
- What tasks most commonly flagged?
- What languages suggested?
- Are suggestions reasonable?

### Step 3: Customize Matrix

Adjust scores based on your needs:

```json
{
  "task_language_matrix": {
    "numerical_operations": {
      "julia": {"score": 100},    // We use Julia extensively
      "python": {"score": 80},    // Python OK for small datasets
      "javascript": {"score": 20} // Never use JS for math
    }
  }
}
```

### Step 4: Set Thresholds

Adjust when suggestions trigger:

```json
{
  "language_diversity": {
    "min_languages": 3,              // Require 3+ languages
    "max_single_language_percent": 60 // No single lang > 60%
  }
}
```

### Step 5: Gradual Enforcement

Move from `advisory` → `soft` → `hard` over time:

**Month 1:** Advisory (learn)
**Month 2:** Soft (enforce with overrides)
**Month 3+:** Hard (strict enforcement for critical paths)

### Step 6: Per-Scope Configuration

Use scopes for different rules in different parts:

```json
{
  "scopes": [
    {
      "name": "performance_critical",
      "paths": ["src/compute/**"],
      "overrides": {
        "polyglot_optimization": {
          "enforcement_level": "hard"
        }
      }
    },
    {
      "name": "prototypes",
      "paths": ["experiments/**"],
      "overrides": {
        "polyglot_optimization": {
          "enforcement_level": "advisory"
        }
      }
    }
  ]
}
```

---

## Best Practices

### 1. Profile Before Optimizing

Use NAAb's profiler to find hot paths:

```naab
import profiling

profiling.start()
main {
    // your code
}
profiling.stop()
profiling.report()
```

Focus optimization on the 20% of code that takes 80% of time.

### 2. Measure Actual Impact

Don't just trust scores - measure:

```naab
import time

let start = time.now()
let result_python = <<python [operation] >>
let time_python = time.since(start)

let start2 = time.now()
let result_julia = <<julia [operation] >>
let time_julia = time.since(start2)

println("Python: " + time_python + "ms")
println("Julia: " + time_julia + "ms")
println("Speedup: " + (time_python / time_julia) + "x")
```

### 3. Consider the Full Pipeline

Optimize for end-to-end performance, not individual operations:

```naab
// May not be optimal to use fastest language for each step
// if data transfer overhead dominates

// GOOD: Minimize language transitions
let result = <<python
# Do steps 1, 2, 3 in Python
# Slight overhead, but no serialization cost
>>

// vs

// Possibly worse: Optimal per step but transfer overhead
let step1 = <<python step1() >>
let step2 = <<julia step2(step1) >>  // Serialization cost
let step3 = <<nim step3(step2) >>    // Another serialization
```

### 4. Balance Team Skills

Consider your team's expertise:

```json
{
  "task_language_matrix": {
    "numerical_operations": {
      "julia": {"score": 90},   // Reduced from 100 - team learning
      "python": {"score": 85}   // Increased from 70 - team knows well
    }
  }
}
```

### 5. Document Exceptions

When you override, document why:

```naab
// Override: Using Python despite Julia recommendation
// Reason: Integration with existing pandas pipeline
// Performance: Acceptable for our dataset size
let result = <<python
import pandas as pd
df = pd.read_csv('data.csv')
stats = df.describe()
>>
```

### 6. Regular Review

Schedule quarterly reviews:
- Are enforcements still appropriate?
- Has team skill level changed?
- Are there new languages to consider?
- Update `govern.json` accordingly

---

## Troubleshooting

### Problem: Too Many Suggestions

**Symptom:** Every script gets 5+ suggestions

**Solutions:**
1. Increase score tolerance:
   ```json
   {"enforcement_level": "advisory"}  // Less strict
   ```

2. Adjust custom scores to match your reality:
   ```json
   {
     "task_language_matrix": {
       "numerical_operations": {
         "python": {"score": 85}  // Higher if Python is OK for you
       }
     }
   }
   ```

3. Focus on specific areas:
   ```json
   {
     "scopes": [{
       "paths": ["src/critical/**"],
       "overrides": {"polyglot_optimization": {"enabled": true}}
     }]
   }
   ```

### Problem: False Positives

**Symptom:** Suggestions don't make sense for your use case

**Solutions:**
1. Check task detection:
   - Is it correctly identifying the operation type?
   - Review lexical/syntactic patterns

2. Override specific task scores:
   ```json
   {
     "numerical_operations": {
       "python": {"score": 95, "reason": "Small datasets, Python fine"}
     }
   }
   ```

3. Disable specific checks:
   ```json
   {
     "pattern_detection": {"enabled": false}
   }
   ```

### Problem: Missing Suggestions

**Symptom:** Expected suggestions not appearing

**Solutions:**
1. Check if optimization is enabled:
   ```json
   {"polyglot_optimization": {"enabled": true}}
   ```

2. Verify enforcement level isn't `none`:
   ```json
   {"enforcement_level": "advisory"}  // or soft/hard
   ```

3. Check if code patterns are detected:
   - Add debug logging
   - Review pattern detection rules

### Problem: Performance Overhead

**Symptom:** Optimization checks slow down execution

**Solutions:**
1. Disable for development:
   ```bash
   # Use environment variable
   export NAAB_SKIP_OPTIMIZATION=1
   ```

2. Cache analysis results:
   - Results are cached per file
   - Only re-analyzed on file change

3. Disable complex checks:
   ```json
   {
     "pattern_detection": {"enabled": false}
   }
   ```

---

## Advanced Configuration

### Custom Detection Patterns

Add domain-specific task types:

```json
{
  "polyglot_optimization": {
    "pattern_detection": {
      "task_inference": {
        "image_processing": {
          "patterns": [
            "\\bimage\\b", "\\bpixel\\b", "\\bconvolution\\b",
            "\\bfilter\\b.*\\bimage\\b"
          ],
          "optimal_languages": ["cpp", "rust", "julia"],
          "suboptimal_languages": ["python", "javascript"],
          "message": "Image processing benefits from compiled languages"
        },
        "blockchain": {
          "patterns": ["\\bhash\\b", "\\bmerkle\\b", "\\bcryptography\\b"],
          "optimal_languages": ["rust", "go"],
          "message": "Security-critical code needs memory-safe languages"
        }
      }
    }
  }
}
```

### Conditional Enforcement

Different rules for different contexts:

```json
{
  "scopes": [
    {
      "name": "production",
      "paths": ["src/prod/**"],
      "overrides": {
        "polyglot_optimization": {
          "enforcement_level": "hard",
          "language_diversity": {"min_languages": 3}
        }
      }
    },
    {
      "name": "tests",
      "paths": ["tests/**"],
      "overrides": {
        "polyglot_optimization": {
          "enforcement_level": "advisory"
        }
      }
    }
  ]
}
```

---

## References

- [Task→Language Matrix](task_language_matrix.md) - Complete scoring reference
- [Composition Patterns](composition_patterns.md) - Multi-language design patterns
- [Pattern Detection Internals](pattern_detection_internals.md) - How detection works
- [Enforcement Levels](enforcement_levels.md) - Detailed enforcement guide
