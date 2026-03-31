# NAAb Language Reference for LLMs

This is the universal NAAb language reference. Copy this into your project's CLAUDE.md
and add project-specific sections (governance rules, module specs, data paths) below.

## About NAAb
NAAb is a polyglot programming language. You embed other languages (Python, JavaScript,
Shell, Go, Nim, Rust, C++, C#, Ruby, PHP, Zig, Julia) inside `<< >>` blocks within
.naab files. A govern.json file enforces rules at execution time.
DO NOT write standalone .py/.js/.go files — all code goes in .naab files.

## Critical Syntax Rules

### File Structure
- Top level can ONLY contain: use, import, export, struct, enum, function/fn, main
- NO top-level `let` or `const` — variables MUST be inside main {} or functions
- Every executable .naab file needs a `main {}` block
- Imports: `use array`, `use json`, `use math`, etc.

### Variable Declaration
- `let x = 5` — mutable variable
- `const MAX = 100` — constant (UPPER_SNAKE_CASE)
- NO `var` keyword — NAAb does not have `var`

### Functions
- Use `fn` keyword: `fn my_function(param1, param2) { }`
- `function`, `func`, `def` also work but `fn` is preferred
- Lambda: `fn(x) { return x * 2 }` — NOT `(x) => x * 2`
- Arrow syntax `=>` is NOT supported for lambdas

### Control Flow
- `if condition { } else if condition { } else { }`
- NO `elif` — use `else if`
- `and`, `or`, `not` work as aliases for `&&`, `||`, `!` (Python-style)
- `for item in collection { }`
- `for i in 0..10 { }` — range (exclusive), `0..=10` for inclusive
- `while condition { }`
- `match value { pattern => { } default => { } }`
- `try { } catch (e) { }` — parens around `e` are REQUIRED
- `break` and `continue` work in loops
- `return value` — explicit return from functions

### If Expressions
- `let x = if condition { value_a } else { value_b }` — returns a value
- `let x = if a { 1 } else if b { 2 } else { 3 }` — else-if chains work

### Null Coalesce Operator (??)
- `let y = x ?? "default"` — returns `x` if non-null, otherwise `"default"`
- Chainable: `a ?? b ?? c` — returns first non-null value
- `false ?? "x"` returns `false` (only null triggers the fallback, NOT falsy values)
- NOTE: `||` (and `or`) returns boolean, `??` returns the actual value

### Structs and Enums (top-level only)
```naab
struct Point {
    x: int
    y: int
}

enum Color {
    Red,
    Green,
    Blue
}

// Instantiation REQUIRES `new` keyword:
let p = new Point { x: 1, y: 2 }

// Works inside function calls:
arr.push(new Point { x: 1, y: 2 })

// WRONG (will error):
// let p = Point { x: 1, y: 2 }     // Missing `new`
```

### Type Casts (builtins)
- `int(value)` — convert to integer
- `float(value)` — convert to float
- `string(value)` — convert to string
- `bool(value)` — convert to boolean
- `len(collection)` — get length
- `type(value)` — get type name as string
- `typeof(value)` — same as type()
- `range(start, end)` — generate range
- `print(value)` — print to stdout

### Imports (file-based)
```naab
import "src/module.naab" as mod     // Relative to THIS file's directory (NOT CWD)
import "./sibling.naab" as sib      // Explicit relative

// Functions in imported files MUST be exported:
//   export fn my_function() { ... }
// Non-exported functions are private to their file
```

## Polyglot Rules

### Block Syntax
Polyglot blocks MUST be multiline. The `>>` closer MUST be on its own line at column 0.
```naab
let result = <<python[my_var]
x = my_var * 2
x
>>
```

### WRONG (will error):
```naab
let result = <<python x * 2 >>     // Single-line NOT allowed
```

### Variable Binding (HARD ENFORCED)
**This project enforces variable binding at HARD level.** Every polyglot block MUST
list all NAAb variables it uses. If a block uses no NAAb variables, use empty brackets:

```naab
// Block that uses NAAb variables:
let data = [1, 2, 3]
let multiplier = 10
let result = <<python[data, multiplier]
sum(d * multiplier for d in data)
>>

// Block that uses NO NAAb variables (empty brackets required):
let timestamp = <<shell[]
date '+%Y-%m-%d %H:%M:%S'
>>
```

**WRONG — will be BLOCKED by governance:**
```naab
let result = <<python        // Missing variable binding!
sum(data)
>>
```

### Language-Specific Rules
- **Python**: Start at column 0 (no leading indent). Use separate lines, not semicolons.
  Last expression is the return value. `import` works.
  **Do NOT use `return`** — it causes `SyntaxError: 'return' outside function`.
  The last expression's value is automatically captured.
  WRONG: `return json.dumps(data)`
  RIGHT: `json.dumps(data)`
- **Shell**: stdout is the return value.

### JSON Sovereign Pipe
For structured return data:
```naab
let data = <<python[input_data] -> JSON
import json
json.dumps({"key": "value", "count": len(input_data)})
>>
```

## Stdlib Reference (ALL functions)

### array (dot-notation works: arr.push(4))
push, pop, shift, unshift, length, contains, find, first, last,
join, reverse, sort, slice_arr, map_fn, filter_fn, reduce_fn

**GOTCHA**: map_fn, filter_fn, reduce_fn do NOT work with dot notation!
```naab
// WRONG: arr.filter_fn(fn(x) { return x > 5 })
// RIGHT:
array.filter_fn(arr, fn(x) { return x > 5 })
array.map_fn(arr, fn(x) { return x * 2 })
array.reduce_fn(arr, fn(acc, x) { return acc + x }, 0)
```

### dict (built-in, dot-notation)
- `dict.get(key)` — returns value or null (NO error on missing key)
- `dict.get(key, default)` — returns value or default
- `dict.has(key)` / `dict.contains(key)` / `dict.containsKey(key)` — boolean check
- `dict.size()` / `dict.length()` — element count
- `dict.isEmpty()` — boolean
- `dict.put(key, val)` / `dict.set(key, val)` — add/update entry
- `dict.remove(key)` / `dict.delete(key)` — remove entry
- `dict.keys()` — array of keys
- `dict.values()` — array of values
- `dict.entries()` — array of [key, value] pairs
- `dict.merge(other)` — merge another dict

**CRITICAL**: `dict["key"]` THROWS on missing key! Use `dict.get("key")` for safe access.
**CRITICAL**: Chained `.get()` is HARD blocked by scanner! `a.get("x").get("y")` will fail.
Instead: `let x = a.get("x")` then check null before second `.get()`.

### string (dot-notation works: str.upper())
upper, lower, trim, split, replace, contains, starts_with, ends_with,
length, char_at, index_of, substring, reverse, repeat, pad_left, pad_right

### math
abs, floor, ceil, round, min, max, pow, sqrt, random, sin, cos, PI, E
**GOTCHA**: Use `math.PI` and `math.E` (uppercase), NOT `math.pi`/`math.e`

### json
parse, stringify

### file
read, write, exists, list_dir, delete, append

### time
now, now_millis, sleep (takes SECONDS not ms — use 0.01 for 10ms),
format_timestamp (NOT format!), parse_datetime,
year, month, day, hour, minute, second, weekday

### csv
parse, stringify

### regex
match, test, replace, split

### env
get, set_var (NOT set — the function is set_var), list

### io
write, read_line, write_error

### debug
type, inspect, keys, values, log, trace, timer, compare, diff,
snapshot, stack, env, watch
**NOTE**: debug is auto-imported (prelude). Do NOT `use debug` — it causes a file search error.

### crypto
sha256, sha512, md5, sha1, hash (dispatcher: `hash("sha256", data)`),
random_bytes, random_string, random_int,
base64_encode, base64_decode, hex_encode, hex_decode,
compare_digest, generate_token, hash_password

## Pipeline Operator
```naab
let result = data |> transform |> analyze |> format
```
`|>` passes the left value as the FIRST argument to the right function.
`data |> fn_name` becomes `fn_name(data)`.
For multi-arg stdlib functions, create a wrapper:
```naab
fn my_filter(arr) { return array.filter_fn(arr, fn(x) { return x > 5 }) }
let result = data |> my_filter
```

## Value Semantics
NAAb dictionaries and arrays use **value semantics** (copy-on-assignment).
Modifying a nested value requires re-assignment to the parent:
```naab
let stats = depts[d]
stats["count"] = int(stats["count"]) + 1
depts[d] = stats    // MANDATORY — without this, the change is lost
```

### Value Semantics in Loops (CRITICAL)
```naab
// WRONG — changes lost:
for i in 0..len(entities) {
    let e = entities[i]
    e["hp"] = e.get("hp") - 10
    // Missing: entities[i] = e
}

// RIGHT — re-assign both inner and outer:
for i in 0..len(entities) {
    let e = entities[i]
    e["hp"] = e.get("hp") - 10
    entities[i] = e        // MUST re-assign element
}
state["entities"] = entities  // MUST re-assign container
```

## Testing Pattern
Each test function returns [passed, total]. Main aggregates:
```naab
fn test_something() {
    let passed = 0
    let total = 0

    // Test 1
    total = total + 1
    if some_condition {
        passed = passed + 1
    }

    return [passed, total]
}

main {
    let results = test_something()
    print("test_something: " + string(results[0]) + "/" + string(results[1]))
}
```

## Known Gotchas (avoid these mistakes)
1. `array.push/pop/shift/unshift` MUTATE the original array
2. Float-to-string adds 6 decimals: `3.14` becomes `"3.140000"` — use `int(round(x))` for clean numbers
3. `True`/`False`/`None` are NOT NAAb keywords — use `true`/`false`/`null`
4. `elif` does NOT exist — use `else if`
5. `.len()` does NOT exist — use `.length()` or `len(x)`
6. `env.set()` does NOT exist — use `env.set_var()`
7. `catch e { }` will error — must be `catch (e) { }`
8. `use debug` causes file search error — debug is auto-imported (prelude). Don't import it.
9. Polyglot blocks inside functions + return: works, but the function must have return AFTER the block
10. Dict iteration: `for key in my_dict { }` works
11. Null comparison: use `x == null` not `x === null` (no triple equals)
12. String concatenation with `+` is permissive (auto-converts numbers)
13. Arithmetic with strings is STRICT — `"5" + 3` works (string concat), `"5" * 3` errors
14. `array.find` takes a PREDICATE function, not a value
15. `||` (and its alias `or`) ALWAYS returns boolean (true/false), NEVER the operand value.
    `null || "fallback"` returns `true` (NOT "fallback"!).
    For null coalesce, use the `??` operator: `x ?? default_val`
16. `dict["key"]` THROWS on missing key — use `dict.get("key")` or `dict.get("key", default)`
17. Struct instantiation requires `new`: `let p = new Point { x: 1, y: 2 }`
18. Python polyglot: Do NOT use `return` — causes `SyntaxError: 'return' outside function`
19. Value semantics: modifying a nested dict/array requires re-assigning to parent
20. The `..` range operator can collide with `".."` string literals — use intermediate variables

## Enum Best Practices

When your module defines enums, use them consistently:
```naab
// GOOD — uses the enum type:
if tx.get("type") == TransactionType.Sale { ... }
if order.get("status") == OrderStatus.Delivered { ... }

// BAD — magic values (governance may flag these):
if tx.get("type") == 1 { ... }           // What is 1?
if order.get("status") == "delivered" { ... }  // Fragile string literal
```

When passing enum values to other modules, the receiving module should import the enum:
```naab
import "./models.naab" as models
// Then use: models.TransactionType.Sale, models.OrderStatus.Pending, etc.
```

## Null Safety Pattern

When accessing nested dict values, NEVER chain `.get()` calls:
```naab
// WRONG — scanner BLOCKS this (hard):
let val = outer.get("key1").get("key2")

// RIGHT — split into steps with null check:
let inner = outer.get("key1")
if inner != null {
    let val = inner.get("key2")
}

// Or use ?? for safe fallback:
let inner = outer.get("key1") ?? {}
let val = inner.get("key2", 0)
```

## Complexity Scoring (for governance)

If govern.json has `complexity_floor` enabled, functions must reach a minimum
complexity score. Here's what contributes:

| Pattern | Score | Example |
|---------|-------|---------|
| Real loop (for/while) | +5 each | `for item in data { ... }` |
| Nested loops | +15 | `for row in grid { for col in row { } }` |
| try/catch | +5 | `try { risky() } catch (e) { handle(e) }` |
| Array operations | +5 | `array.filter_fn(arr, predicate)` |
| Pipeline operator | +5 | `data \|> transform \|> format` |
| Function definition | +3 each | `fn helper(x) { ... }` |
| External function call | +1 each | `math.sqrt(x)` |
| Recursion | +10 | Function calls itself |

Functions named `get_*`, `set_*`, `is_*`, `has_*`, `to_*`, `make_*`, `apply_*`,
`move_*`, `check_*`, `find_*`, `create_*`, `update_*`, `remove_*`, `delete_*`,
`add_*`, `reset_*`, `init_*`, `validate_*`, `convert_*` have LOW threshold (score >= 3).

Functions named `forecast_*`, `optimize_*`, `calculate_*`, `process_*`, `analyze_*`
need substantial logic (score >= 20, must have loops or conditionals).

Functions shorter than `min_lines_for_check` (6) lines skip the floor entirely.

Do NOT pad functions with `for i in 0..1 { }` or `for i in 0..2 { }` loops to pass
complexity checks. Instead: add real logic — input validation, edge case handling,
error recovery with try/catch.

## Contract Patterns

When govern.json defines function contracts, your return value MUST match.
This project has **20 function contracts**. Every listed return key must be present.

```naab
// Contract: return_keys: ["id", "type", "hp", "x", "y"]
return {
    "id": computed_id,
    "type": entity_type,
    "hp": calculated_hp,
    "x": position_x,
    "y": position_y
}
```

---

## Project: Agent Governance Framework

A governance framework for AI coding agents, built as a real NAAb module. It provides
telemetry, analytics, adaptive rule learning, role-based access control, and intelligent
language assignment — all enforced at the NAAb runtime level where it cannot be bypassed.

**This is a strict governance project.** The module governs itself via its own govern.json,
eating its own dogfood. Tests verify exact computed values, not just types or ranges.

### Why Runtime Governance Matters
Wrapper-based governance (pre/post hooks, linters, CI gates) can be bypassed — agents can
call APIs directly, skip hooks, or restructure code to avoid static checks. NAAb embeds
governance INTO the execution engine. When an agent writes code inside a NAAb polyglot
block, governance rules fire at parse time, compile time, AND runtime. There is no
alternative execution path that skips enforcement.

### The 5 Features
1. **Telemetry** — Event collection and JSON logging for all governance actions
2. **Dashboard** — Analytics and reporting over telemetry data
3. **Adaptive Rules** — Pattern analysis that proposes new governance rules from violations
4. **Agent Roles** — Multi-agent role-based access control (paths, languages, actions)
5. **Language Scoring** — Task-to-language assignment using polyglot scoring

### Execution Order
```
Agent writes code
  → Role check (is this agent allowed to touch this file/language?)
  → Language assignment (which language is best for this task?)
  → Governance gate (does the code pass all rules?)
  → Helper errors (guide the agent to fix issues)
  → Execution
  → Telemetry event logged
  → Pattern analysis (learn from this interaction)
```

## Governance Summary (govern.json)
- Allowed languages: python, shell
- Blocked: javascript, rust, cpp, csharp, go, nim, zig, julia, ruby, php
- **Variable binding: HARD** — every polyglot block must have `[var1, var2]` or `[]`
- Network: DISABLED
- Filesystem: READ_WRITE (telemetry needs to write log files)
- Security: no_secrets (hard), no_placeholders (hard), no_simulation_markers (hard)
- **20 function contracts** with return_keys enforcement
- 4-tier complexity floor (test_/analyze_/generate_/create_ tiers)
- **Scanner HARD checks:** empty_catch, dead_conditional, value_semantics_bug, missing_null_check, dict_bracket_access, python_return_in_block, top_level_let

## Module Specifications

### models.naab — Shared Data Structures

**Enums:**
- `EventType { GovernanceCheck, HelperTriggered, ExecutionComplete, RuleViolation, LanguageAssignment }`
- `Severity { Low, Medium, High, Critical }`
- `Enforcement { Hard, Soft, Advisory }`
- `AgentRole { Frontend, Backend, DataScience, DevOps, Security, Admin }`
- `CheckResult { Pass, Block, Warn }`

**Exported Functions (10):**
- `create_event(agent_id, event_type, language, rule_name, result, message, timestamp)` → dict with all keys + session_id
- `create_agent(id, name, role, allowed_paths, blocked_paths, allowed_languages)` → dict with all 6 keys
- `create_rule_proposal(pattern, proposed_rule, confidence, evidence_count, source_events)` → dict with all 5 keys
- `create_score_result(language, score, task_type, reasons)` → dict with all 4 keys
- `create_session(id, agent_id, start_time)` → dict with events=[], status="active"
- `validate_event(event)` → `{valid, errors}`
- `validate_agent(agent)` → `{valid, errors}`
- `event_type_name(event_type)` → string
- `severity_name(severity)` → string
- `role_name(role)` → string

### telemetry.naab — Event Collection & Logging

**Exported Functions (8):**
- `init_telemetry(output_dir)` → `{collector, success}`
- `record_event(collector, event)` → updated collector with event appended
- `record_governance_check(collector, agent_id, language, rule_name, passed, message)` → updated collector
- `record_helper_triggered(collector, agent_id, helper_name, language, original_error)` → updated collector
- `flush_events(collector, filepath)` → `{success, events_written}`
- `load_events(filepath)` → array of events
- `get_session_events(collector, session_id)` → filtered events
- `get_events_by_agent(collector, agent_id)` → filtered events

### dashboard.naab — Reporting & Analytics

**Exported Functions (8):**
- `generate_summary(events)` → `{total_events, by_type, by_agent, by_language, by_result, time_range}`
- `top_violations(events, limit)` → array of `{rule_name, count, agents_affected}` sorted desc
- `agent_scorecard(events, agent_id)` → `{agent_id, total_checks, pass_rate, violations, most_common_violation, helper_effectiveness}`
- `helper_effectiveness_report(events)` → dict: helper_name → `{triggered, led_to_fix, effectiveness_rate}`
- `language_usage_report(events)` → dict: language → `{blocks_executed, violations, pass_rate}`
- `generate_text_report(events)` → formatted string with tables
- `export_report_csv(events, headers)` → CSV string
- `trend_analysis(events, window_hours)` → `{periods, violation_trend, pass_rate_trend}`

### adaptive.naab — Bidirectional Rule Learning

**Exported Functions (8):**
- `analyze_patterns(events, min_occurrences)` → array of detected patterns
- `propose_rule(pattern, existing_rules)` → `{proposed_rule, confidence, rationale, evidence_count}`
- `evaluate_proposal(proposal, events)` → `{would_catch, false_positive_rate, recommendation}`
- `generate_helper_text(violation_pattern)` → string
- `merge_proposals(proposals)` → deduplicated + ranked proposals
- `format_govern_json_patch(proposals)` → JSON string
- `get_repeat_offenders(events, threshold)` → array of `{agent_id, violation_count, top_rules}`
- `calculate_confidence(evidence_count, consistency, time_span)` → float 0.0-1.0

### roles.naab — Multi-Agent Role Enforcement

**Exported Functions (8):**
- `load_roles(config_path)` → array of agent role configs
- `create_role(name, allowed_paths, blocked_paths, allowed_languages, max_complexity)` → dict
- `check_access(agent, file_path)` → `{allowed, reason}`
- `check_language_permission(agent, language)` → `{allowed, reason}`
- `enforce_role(agent, action_type, target)` → `{allowed, reason, enforcement}`
- `get_agent_permissions(agent)` → `{allowed_paths, blocked_paths, allowed_languages, restrictions}`
- `validate_role_config(roles)` → `{valid, errors, warnings}`
- `role_summary(agents)` → formatted text table

### language_scorer.naab — Task-to-Language Assignment

**Exported Functions (8):**
- `score_language(task_description, language)` → `{language, score, task_type, reasons}`
- `rank_languages(task_description, available_languages)` → sorted array of score results
- `assign_language(task_description, agent)` → `{language, score, alternative, reason}`
- `get_language_strengths()` → dict: language → `{strengths, weaknesses, best_for}`
- `detect_task_type(description)` → string ("numerical", "text_processing", "system_ops", etc.)
- `validate_assignment(language, task_type)` → `{suitable, score, warnings}`
- `compare_languages(lang_a, lang_b, task_description)` → `{winner, margin, reasoning}`
- `format_recommendation(assignment)` → formatted string

### config.naab — Configuration Loader

**Exported Functions (6):**
- `load_config(config_path)` → full config dict
- `get_agent_roles(config)` → array of agent role dicts
- `get_telemetry_config(config)` → `{enabled, output_dir, format, retention_days}`
- `get_adaptive_config(config)` → `{enabled, auto_propose, auto_apply, min_confidence}`
- `get_language_config(config)` → `{assignment_enabled, scoring_enabled, override_level}`
- `validate_config(config)` → `{valid, errors, warnings}`

### main.naab — Test Orchestrator (80 tests)

Imports all 7 modules. Contains 8 test functions (10 tests each):
- `test_models()` — creation, validation, enum names
- `test_telemetry()` — init, record, flush, load, filter
- `test_dashboard()` — summary, violations, scorecards, reports
- `test_adaptive()` — patterns, proposals, evaluation, confidence
- `test_roles()` — access control, language permissions, enforcement
- `test_language_scorer()` — scoring, ranking, assignment, detection
- `test_config()` — loading, validation, defaults
- `test_integration()` — end-to-end pipelines across modules

**Tests verify exact computed values, not just types or ranges.**

## What NOT to Do (project-specific)
- Do NOT write standalone .py, .js, .go files
- Do NOT hardcode results, use placeholders, or stub functions
- Do NOT leave TODO/FIXME/STUB comments — governance BLOCKS these patterns
- Do NOT use hedging comments: "simplified", "basic", "for now", "in a real system"
- Do NOT write empty/trivial functions (pass-only, return True, return [])
- Do NOT swallow errors silently (empty catch blocks — HARD blocked)
- Do NOT pad functions with dummy loops to pass complexity checks
- Do NOT use `return` inside Python polyglot blocks — SyntaxError
- Do NOT use `dict["key"]` — HARD blocked. Use `dict.get("key")`
- Do NOT chain `.get()` calls — HARD blocked. Split into separate let + null check
- Do NOT forget variable binding on polyglot blocks — HARD blocked
- Do NOT forget value semantics re-assignment when mutating dicts/arrays in loops
- Do NOT use magic numbers/strings when enums are available
- The governance engine detects 200+ stub/evasion patterns and will BLOCK execution
