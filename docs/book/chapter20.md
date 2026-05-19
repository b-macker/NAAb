# Chapter 20: Future Roadmap

NAAb is a living language, continuously evolving to meet the demands of modern polyglot development. This chapter outlines the current state of implemented features, what's planned for upcoming releases, and how you can contribute to the project.

## 20.1 Implemented Features

The following features are fully implemented and tested:

### 20.1.1 Asynchronous Programming (`async`/`await`)

NAAb supports `async`/`await` for concurrent function execution:

```naab
async fn fetch_data(url) {
    let result = <<python[url]
import urllib.request
urllib.request.urlopen(url).read().decode()
    >>
    return result
}

main {
    let data = await fetch_data("https://example.com")
    print(data)
}
```

Async functions run concurrently with independent taint state (snapshot-based isolation). See Chapter 8 for details.

### 20.1.2 Null Coalescing (`??`)

The null coalescing operator provides default values for `null` expressions:

```naab
let name = env.get("USER") ?? "anonymous"
let config = settings.get("timeout") ?? 30
```

**Important:** `??` only triggers on `null`, not on falsy values like `false`, `0`, or `""`. This differs from JavaScript's `||` operator.

### 20.1.3 Governance Engine v4.0

The governance engine provides comprehensive policy-as-code enforcement:

- **50+ built-in checks** across security, code quality, and LLM anti-drift categories
- **Taint tracking** — variable-level tracking of untrusted data from sources through sinks
- **VM taint lineage** — every tainted value carries a source-to-sink chain showing where it was tainted and why
- **Cross-module contracts** — parameter type validation at function call sites
- **Enhanced audit trail** — JSONL logging with tamper-evident hash chains
- **Built-in scanner** — 139 static analysis checks across 6 categories
- **Project context awareness** — reads CLAUDE.md, .eslintrc, package.json to supplement govern.json
- **12 polyglot languages** — Python, JavaScript, Shell, Go, Nim, Zig, Julia, Rust, C++, C#, Ruby, PHP
- **CI/CD integration** — SARIF, JUnit XML, and JSON report output
- **Cumulative risk scoring** — advisory findings accumulate weighted scores with green/yellow/red zones

See [Chapter 21: Governance and LLM Code Quality](chapter21.md) for the full reference.

### 20.1.4 Pattern Matching

```naab
match value {
    1 => { print("one") }
    2 => { print("two") }
    default => { print("other") }
}
```

### 20.1.5 Lambda Expressions and Closures

```naab
let adder = fn(x) { return fn(y) { return x + y } }
let add5 = adder(5)
print(add5(3))  // 8
```

### 20.1.6 Pipeline Operator

```naab
let result = data |> transform |> analyze |> format
```

### 20.1.7 String Comparison

String `<`, `<=`, `>`, `>=` operators use lexicographic comparison:

```naab
if "apple" < "banana" { print("correct") }
```

### 20.1.8 Optional Chaining (`?.`)

Safely access properties of potentially `null` objects without throwing:

```naab
let city = user?.address?.city ?? "unknown"

// Works with method calls too:
let len = maybe_array?.length()    // null if maybe_array is null
let val = maybe_dict?.get("key")   // null if maybe_dict is null
```

`?.` returns `null` immediately if the left side is `null`, instead of throwing an error. Chains propagate: `a?.b?.c` returns `null` if either `a` or `a.b` is `null`.

### 20.1.9 Destructuring Assignment

Extract values from arrays and dicts into named variables:

```naab
// Array destructuring
let [x, y, z] = [1, 2, 3]
let [first, second] = get_coordinates()

// Spread/rest operator
let [head, ...tail] = [1, 2, 3, 4, 5]  // head=1, tail=[2,3,4,5]
let [a, b, ...rest] = get_items()       // rest collects remaining

// Dict destructuring
let {name, age} = get_user()
let {city, zip} = {"city": "NYC"}  // zip is null (missing key)
```

Array destructuring is positional — extra elements are ignored (or collected with `...rest`). Dict destructuring matches by key name — missing keys produce `null`. Destructured values follow NAAb's value semantics (independent copies).

### 20.1.10 Ed25519 Governance Signing

Trust-anchored governance signing ensures govern.json integrity:

- `--keygen <path>` generates Ed25519 keypair
- `--sign-governance` signs govern.json with your private key
- `--trust-key <pubkey>` installs a public key to the trust store (`~/.naab/trusted-keys/`)
- `--list-keys` displays trusted key fingerprints
- One-way ratchet on mid-run reload — governance can only tighten, never loosen

### 20.1.11 Agent Module

The `agent` stdlib module provides governed LLM conversations:

- `agent.create(name, config)` — create an LLM-backed agent (Anthropic or Gemini)
- `agent.send(handle, message)` — single-turn interaction with governance enforcement
- `agent.batch(handle, prompts)` — parallel execution across multiple prompts
- `agent.fan_out(handle, fn, data)` — map operation over data
- `agent.pipeline(handle, stages)` — sequential processing pipeline
- Server-side turn/token enforcement immune to handle mutation
- Output filtering: secrets (18 patterns) and PII (5 patterns) blocked in all responses

### 20.1.12 Behavioral Sequence Detection (BSD)

Runtime detection of multi-step attack patterns:

- FSM-based pattern matching over an event ring buffer
- User-defined sequences in govern.json (e.g., read secrets → encode → exfiltrate)
- Detail globs for per-argument matching (`env.get:*SECRET*|*TOKEN*`)
- Pre-execution blocking — sequences blocked before the final dangerous step runs
- Decay/expiry — old events expire after N turns or seconds

### 20.1.13 Context Drift Detection (CDD)

Coherence monitoring for LLM agent conversations:

- Tracks declared intent vs observed actions across agent turns
- Drift signals: repeated failures, circular actions, scope creep, contradictions
- Per-signal configurable weights and coherence threshold
- Governance notices surfaced via `agent.send()` return dict

### 20.1.14 Tooling

The following development tools are implemented:

- **Debugger** — interactive debugging with `--debug` flag (breakpoints, step-through, variable inspection, call stack navigation)
- **Package Manager** — `naab install` for dependency management with SHA-256 integrity verification via naab.lock
- **Formatter** — code formatting via the formatter module
- **Linter / Scanner** — static analysis via `--scan` with SARIF output
- **Profiler** — performance profiling via `--profile` flag

## 20.2 Planned Language Features

### 20.2.1 Enhanced Generics and Type System

Future work will focus on expanding the capabilities of the type system:

*   **Type Constraints**: Allow generics to be constrained (e.g., `fn sort<T: Comparable>`).
*   **Variadic Generics**: Support functions that accept a variable number of type arguments.
*   **Advanced Type Inference**: Further reduce the need for explicit type annotations.

### 20.2.2 String Interpolation in Polyglot Blocks

Currently, NAAb variables must be explicitly bound to polyglot blocks. A future enhancement would allow inline interpolation:

```naab
// Planned:
let name = "world"
<<python
print(f"Hello {naab.name}")
>>
```

## 20.3 The Tooling Roadmap

### 20.3.1 Language Server Protocol (LSP) — In Progress

The NAAb Language Server is being developed to provide IDE-like features:

*   **Intelligent Autocompletion**: Keywords, variables, functions, stdlib methods.
*   **Real-time Diagnostics**: Syntax errors, governance violations.
*   **Hover Information**: Type signatures, documentation.
*   **Go-to-Definition and Find References**: Codebase navigation.

## 20.4 Test Suite Status

NAAb maintains a comprehensive test suite with multiple verification layers:

| Category | Tests | Description |
|----------|-------|-------------|
| Core regression | 391 | Full language + governance + polyglot |
| Security leak checks | 112 | Error message bypass flag scanning |
| Robustness | 313 | Exhaustive stdlib, operators, closures, control flow |
| Mutations | 45 | Wrong-answer detection (meta-test) |
| Sensitivity | 33 | Input-dependency verification (meta-test) |
| Static audit | 7 | Structural integrity of test files |
| Runtime manifest | 8 | Output verification against expected values |
| Stdlib coverage | 92/92 | 100% of all stdlib functions tested |

## 20.5 Contributing to NAAb

NAAb is an open-source project, and contributions are highly encouraged.

### 20.5.1 Reporting Bugs and Suggesting Features

Report issues on the project's GitHub repository. Clear, detailed bug reports with reproducible steps are invaluable.

### 20.5.2 Code Contributions

The NAAb core is primarily written in C++. Contributions could involve:

*   Implementing planned language features (generics, type constraints).
*   Improving the LSP server.
*   Adding new polyglot language executors.
*   Enhancing governance checks and scanner rules.
*   Expanding BSD/CDD detection patterns.

### 20.5.3 Documentation and Examples

High-quality documentation is vital for adoption. Contributions to the user guide, API reference, tutorials, and code examples are always welcome.

Join the NAAb community and help shape the future of polyglot programming!
