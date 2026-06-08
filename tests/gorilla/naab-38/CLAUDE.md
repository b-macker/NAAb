# NAAb Language Reference for LLMs

This is the universal NAAb language reference. Copy this into your project's CLAUDE.md
and add project-specific sections (governance rules, module specs, data paths) below.

## About NAAb
NAAb is a polyglot programming language. You embed other languages (Python, JavaScript,
Shell) inside `<< >>` blocks within .naab files. A govern.json file enforces rules at execution time.
DO NOT write standalone .py/.js/.go files — all code goes in .naab files.

## Running
```bash
naab-lang src/main.naab                    # Run a .naab file
naab-lang --governance-dashboard src/main.naab  # Run with governance summary
```

## Critical Syntax Rules

### File Structure
- Top level can ONLY contain: use, import, export, struct, enum, function/fn, main
- NO top-level `let` or `const` — variables MUST be inside main {} or functions
- Every executable .naab file needs a `main {}` block

### Variable Declaration
- `let x = 5` — mutable variable
- `const MAX = 100` — constant

### Functions
- Use `fn` keyword: `fn my_function(param1, param2) { }`
- Lambda: `fn(x) { return x * 2 }`

### Control Flow
- `if condition { } else if condition { } else { }`
- `for item in collection { }` / `for i in 0..10 { }`
- `while condition { }`
- `match value { pattern => expr, _ => default_expr }`
- `try { } catch (e) { }` — parens around `e` are REQUIRED

### Polyglot Blocks
```naab
let result = <<python
x = 42
x * 2
>>
```
- MUST be multiline. `>>` must be on its own line.
- Python: last expression is return value. Do NOT use `return`.
- JavaScript: uses embedded QuickJS. Last expression is return value.
- Shell: stdout is return value.
- For JSON data: use `-> JSON` or `json.dumps()` + `json.parse()`

### Variable Binding
```naab
let data = [1, 2, 3]
let result = <<python[data]
sum(data)
>>
```

### Imports
- `use array`, `use json`, `use math`, etc.
- `import "file.naab" as mod` — relative to THIS file's directory
- `export fn my_func() { }` — export for other modules

### Structs
```naab
struct Point { x: int, y: int }
let p = new Point { x: 1, y: 2 }
```

### Value Semantics
Dicts and arrays are copied on assignment. Modify + re-assign:
```naab
let e = entities[i]
e["hp"] = e.get("hp") - 10
entities[i] = e  // MUST re-assign
```

## Stdlib Reference
- **array**: push, pop, map, filter, reduce, sort, sorted, contains, join, length
- **string**: upper, lower, trim, split, replace, contains, length
- **math**: abs, floor, ceil, round, min, max, pow, sqrt, random, PI
- **json**: parse, stringify
- **file**: read, write, exists, list_dir, delete, append
- **time**: now, now_millis, sleep, format_timestamp
- **csv**: parse, stringify
- **regex**: search (bool), find (string), find_all, replace, split, find_groups
- **env**: get, get_args, set_var, list
- **io**: write, read_line, write_error
- **crypto**: sha256, sha512, hash, random_bytes, base64_encode
- **codegen**: run_with_args(language, code, bindings_dict)
- **path**: join, dirname, basename, extension
- **log**: debug, warn, log, set_level

## Key Gotchas
1. `dict["key"]` THROWS on missing key — use `dict.get("key")` or `dict.get("key", default)`
2. `regex.search()` returns bool, NOT matched text — use `regex.find()` for text
3. `time.format()` doesn't exist — use `time.format_timestamp()`
4. `env.set()` doesn't exist — use `env.set_var()`
5. `||` returns boolean, NOT the value — use `??` for null coalescing
6. Python: do NOT use `return` in polyglot blocks
7. `use debug` causes error — debug is auto-imported
8. `and`/`or`/`not` don't work — use `&&`/`||`/`!`
9. No ternary — use `if cond { a } else { b }` expression
10. Integer division truncates: `12 / 8 = 1` — use `float()` cast

---

## Project: Multi-Stage Data Analysis Pipeline

### What to Build

A comprehensive data analysis pipeline that processes security telemetry data through
multiple stages. This project is designed to exercise the governance engine's depth
features through sustained polyglot execution, diverse language usage, and structured
data flow.

### Architecture (4 modules)

**src/models.naab** — Data structures
- `make_event(id, timestamp, source, event_type, severity, message)` — returns dict
- `make_analysis(event_id, category, risk_score, indicators, recommendation)` — returns dict
- `make_summary(total, by_category, by_severity, top_risks, chain_hash)` — returns dict

**src/validators.naab** — Sanitization (REQUIRED for taint tracking)
- `sanitize_string(s)` — strip dangerous chars, return safe string
- `validate_event(event)` — check required keys, return bool
- `sanitize_output(text)` — prepare for safe output
- `validate_analysis(analysis)` — check analysis dict completeness

**src/engine.naab** — Analysis engine (the core logic)
- `classify_event(event)` — categorize by event_type using match expressions
- `score_severity(event, history)` — compute risk score (0-100) using Python polyglot
  for statistical analysis (mean, stddev of historical scores)
- `detect_patterns(events)` — use JavaScript polyglot to find event patterns
  (repeated sources, escalation sequences, time clustering)
- `analyze_batch(events)` — process array of events, return array of analyses
- `analyze_batch_async(events)` — async version for concurrent processing
- `build_audit_chain(analyses)` — SHA-256 hash chain of all analysis results

**src/main.naab** — Entry point (the orchestrator)
- Generate 30 security events (auth_failure, port_scan, malware_detect, sql_injection,
  brute_force, data_exfil, privilege_escalation, config_change, anomalous_login, ddos_attempt)
- Run classification on all events
- Run statistical severity scoring using Python codegen
- Run pattern detection using JavaScript
- Run batch analysis (sync first 10, async remaining 20)
- Build audit hash chain
- Generate shell metrics (uptime, pid)
- Produce final summary with category breakdown
- Sanitize ALL outputs before printing or file writes
- Write report to report.json

### Governance Rules
- Languages: python, javascript, shell only
- Taint tracking: DETECT level — polyglot output must be sanitized before file.write
- Codegen: enabled for python, shell, javascript
- Network: disabled
- Shell: enabled but rm/curl/wget blocked
- Polyglot block limit: 500 per execution
- Advisory escalation: enabled (4th occurrence hardens)
- BSD: behavioral sequence detection active
- Scoring: cumulative risk scoring enabled

### Requirements
- Use `codegen.run_with_args()` for at least 5 parameterized computations
- Use all 3 allowed languages (Python, JavaScript, Shell) in polyglot blocks
- Use `match` expressions for event classification
- Use `async` functions for concurrent batch processing
- Sanitize ALL polyglot output with sanitize_string() before any file.write
- Build SHA-256 hash chain with `crypto.sha256()` for audit trail
- Handle errors with try/catch for runtime errors
- Use pipeline operator `|>` for at least one data transformation chain

### What NOT to Do
- Do NOT write standalone .py/.js files — all code in .naab files
- Do NOT modify govern.json — it is signed and modifications will be blocked
- Do NOT use `import os` or `subprocess` in Python blocks
- Do NOT hardcode results or use placeholder values
- Do NOT leave TODO/FIXME/STUB comments
- Do NOT use blocked languages (Go, Rust, C++, etc.)
- Do NOT write to files without sanitizing first
