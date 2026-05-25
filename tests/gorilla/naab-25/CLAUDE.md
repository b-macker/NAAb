# NAAb Language Reference for LLMs

This is the universal NAAb language reference. Copy this into your project's CLAUDE.md
and add project-specific sections (governance rules, module specs, data paths) below.

## About NAAb
NAAb is a polyglot programming language. You embed other languages (Python, JavaScript,
Shell, Go, Nim, Rust, C++, C#, Ruby, PHP, Zig, Julia) inside `<< >>` blocks within
.naab files. A govern.json file enforces rules at execution time.
DO NOT write standalone .py/.js/.go files — all code goes in .naab files.

## Running
```bash
naab src/main.naab [args]           # Run a .naab file
naab src/main.naab test             # Run with "test" argument
naab run --timeout 60 src/main.naab # With timeout override
```
The binary is `naab` (on PATH). Do NOT use `naab-lang`, `naab-final`, or other old names.

## Critical Syntax Rules

### File Structure
- Top level can ONLY contain: use, import, export, struct, enum, function/fn, main
- NO top-level `let` or `const` — variables MUST be inside main {} or functions
- Every executable .naab file needs a `main {}` block
- Imports: `use array`, `use json`, `use math`, etc.
- Do NOT write `use array as array` — the `as` alias is only for renaming (e.g. `use json as j`)

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
- `for item in collection { }`
- `for i in 0..10 { }` — range (exclusive), `0..=10` for inclusive
- `while condition { }`
- `match value { pattern => { } default => { } }`
- `try { } catch (e) { }` — parens around `e` are REQUIRED
- `break` and `continue` work in loops
- `return value` — explicit return from functions

### Match Expression
```naab
let result = match value {
    "attack" => "do attack",
    "move"   => "do move",
    _        => "default"
}
```
- Arms use `=>` (fat arrow), NOT `->` or `:`
- **Each arm body MUST be an expression** — NOT a block with statements
- `_` is the default/wildcard pattern
- Commas between arms are optional but recommended
- match IS an expression — `let x = match ...` works

### If Expressions
- `let x = if condition { value_a } else { value_b }` — returns a value
- `let x = if a { 1 } else if b { 2 } else { 3 }` — else-if chains work

### Null Coalesce Operator (??)
- `let y = x ?? "default"` — returns `x` if non-null, otherwise `"default"`
- Chainable: `a ?? b ?? c` — returns first non-null value
- `false ?? "x"` returns `false` (only null triggers the fallback, NOT falsy values)
- NOTE: `||` returns boolean, `??` returns the actual value

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

// Field names can be identifiers OR quoted strings:
let p = new Point { "x": 1, "y": 2 }     // Also valid

// Works inside function calls:
arr.push(new Point { x: 1, y: 2 })

// Cross-module struct instantiation:
import "models.naab" as models
let p = new models.Point { x: 1, y: 2 }

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

### Module Main Blocks
When a .naab file is imported via `import "file.naab" as alias`, its `main {}` block
is NOT executed. Only struct/enum/function definitions and exports are processed.
The `main {}` block only runs when the file is executed directly.

### Imports (file-based)
```naab
import "module.naab" as mod         // Relative to THIS file's directory (NOT CWD)
import "./sibling.naab" as sib      // Explicit relative (same directory)

// If THIS file is src/main.naab and you're importing src/models.naab:
//   WRONG: import "src/models.naab" as models   // looks for src/src/models.naab
//   RIGHT: import "models.naab" as models        // same directory = just the filename

// Functions in imported files MUST be exported:
//   export fn my_function() { ... }
// Non-exported functions are private to their file
```

### Exporting Structs, Enums, and Functions
Structs, enums, and functions are exported individually by prefixing `export`:
```naab
// Each struct/enum/fn gets its own export keyword:
export struct Point {
    x: int
    y: int
}

export enum Color {
    Red,
    Green,
    Blue
}

export fn create_point(x, y) {
    return new Point { x: x, y: y }
}
```

**WRONG** — NAAb does NOT support batch export syntax:
```naab
// WRONG — this does not work:
export { Point, Color, create_point }
```
Each export must be declared individually with `export struct`, `export enum`, or `export fn`.

## Polyglot Rules

### Block Syntax
Polyglot blocks MUST be multiline. The `>>` closer MUST be on its own line.
```naab
let result = <<python
x = 42
x * 2
>>
```

### WRONG (will error):
```naab
let result = <<python x * 2 >>     // Single-line NOT allowed
```

### Polyglot content indentation (auto-dedent)
Polyglot content and `>>` can be indented to match surrounding NAAb code — the runtime
auto-dedents by stripping the common leading whitespace prefix:
```naab
fn compute() {
    let result = <<python
    x = 42
    x * 2
    >>
    return result
}
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
For structured return data from polyglot blocks, use `-> JSON`:
```naab
let data = <<python[input_data] -> JSON
import json
json.dumps({"key": "value", "count": len(input_data)})
>>
```
**Python double-encoding trap**: do NOT call `json.dumps()` inside `<<python -> JSON`
when you want a NAAb dict — the `-> JSON` pipe already parses the output. But you DO
need `json.dumps()` to produce valid JSON for the pipe to parse. The result is a NAAb dict,
not a string.

## Taint Tracking

When govern.json has `taint_tracking` enabled, the runtime tracks data flow from
**sources** (polyglot output, env.get) to **sinks** (file.write, file.append).
Tainted data must pass through a **sanitizer** (functions starting with `sanitize_` or
`validate_`, or type casts like `int()`, `float()`, `string()`) before reaching a sink.

### Common pattern: polyglot output -> file.write
```naab
// WRONG — taint violation (polyglot output flows directly to file.write):
let result = <<python[]
import json
json.dumps({"key": "value"})
>>
file.write("output.json", result)

// RIGHT — sanitize before writing:
let result = <<python[]
import json
json.dumps({"key": "value"})
>>
let clean = sanitize_string(result)
file.write("output.json", clean)
```

### Where to sanitize
Sanitize at the **sink** (just before file.write/file.append), not at the source.

### sanitize_string design
Your `sanitize_string` function must NOT strip characters that are valid in the
output format. If writing JSON to a file, do NOT strip double quotes `"` — that breaks
JSON structure. A safe sanitizer for file output:
```naab
fn sanitize_string(input) {
    if input == null { return "" }
    let s = string.trim(input)
    s = string.replace(s, "<", "")
    s = string.replace(s, ">", "")
    return s
}
```
Do NOT strip `"`, `{`, `}`, `[`, `]` — these are structural characters in JSON/data formats.

### Taint pattern for multi-file projects
When your sanitizer is in a separate module (e.g., `validators.naab`), you must:
1. Import the validators module: `import "validators.naab" as validators`
2. Call the sanitizer before EVERY `file.write`/`file.append`:
```naab
import "validators.naab" as validators

fn write_report(data, path) {
    let content = json.stringify(data)
    let clean = validators.sanitize_string(content)
    file.write(path, clean)
}
```

## Stdlib Reference (ALL functions)

### array (dot-notation works: arr.push(4))
push, pop, shift, unshift, length, contains, find, first, last,
join, reverse, sort, slice_arr, map, filter, reduce, map_fn, filter_fn, reduce_fn

Both dot-notation aliases and module syntax work:
```naab
array.map(arr, fn(x) { return x * 2 })
array.filter(arr, fn(x) { return x > 5 })
array.reduce(arr, fn(acc, x) { return acc + x }, 0)
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

### string (dot-notation works: str.upper())
upper, lower, trim, split, replace, contains, starts_with, ends_with,
length, char_at, index_of, substring, reverse, repeat, pad_left, pad_right

### math
abs, floor, ceil, round, min, max, pow, sqrt, random, sin, cos, PI, E
- `math.random()` — float in [0.0, 1.0)
- `math.random(max)` — int in [0, max)
- `math.random(min, max)` — int in [min, max)
Both `math.PI` and `math.pi` work (case-insensitive). Same for `math.E`/`math.e`.

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
search (partial match, returns match or null), matches (full string match, true/false),
find (returns matched string), find_all (all matches as array),
replace, replace_first, split, groups, find_groups, escape, is_valid
**GOTCHA**: `regex.match()` and `regex.test()` do NOT exist — use `regex.search()` or `regex.matches()`

### env
get, get_args, set_var (NOT set — the function is set_var), list

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

### log (auto-leveled logging)
debug, warn, log, set_level, get_level, set_format, set_output

### uuid
v4, v5, nil, is_valid
- `uuid.v4()` — random UUID
- `uuid.v5(namespace, name)` — deterministic UUID
- `uuid.is_valid(str)` — boolean check

### validate
email, url, ip, ipv6, int_range, not_empty, length, matches, is_int, is_float, is_string
- `validate.email("user@example.com")` — returns true/false
- `validate.url("https://...")` — validates URL format
- `validate.int_range(val, min, max)` — range check
- `validate.ip("192.168.1.1")` — validates IPv4 address

### process
run, exit, kill, getpid

### path
join, dirname, basename, extension, resolve, is_absolute, normalize, exists

### http
get, post, put, delete, head, patch, call

### agent (requires `use agent`)
create, send, run, messages, usage, batch, fan_out, pipeline, check
- `agent.create(config_name)` — create agent handle from govern.json `agents` config
- `agent.send(handle, message)` — send message, returns {content, stop_reason, usage}
- `agent.batch(handles, messages)` — parallel: send messages[i] to handles[i], returns array
- `agent.fan_out(handles, message)` — parallel: same message to all handles
- `agent.pipeline(handles, initial_message)` — sequential chain
- `agent.check(config_name)` — pre-flight: returns {valid, error, provider, model, api_key_env}
- `agent.usage(handle)` — cumulative {input_tokens, output_tokens, total_tokens, turns}
- `agent.batch()` is resilient: failed calls return `{success: false, error: "..."}` not crash

## Functions That Do NOT Exist (use alternatives)
- `array.merge(a, b)` — use `a + b` (array concatenation with +)
- `array.concat(a, b)` — use `a + b`
- `string.match()` — use `regex.search()` or `regex.matches()` with `use regex`
- `regex.match()` — use `regex.search()` (partial match)
- `regex.test()` — use `regex.matches()` (full string match)
- `time.format()` — use `time.format_timestamp()`
- `env.set()` — use `env.set_var(name, value)`
- `dict.update()` — use `dict.merge(other)` or `dict.put(key, val)`

## Import Rules

**Every module except `debug` requires `use <module>` before calling its functions.**
debug is auto-imported (prelude) — do NOT write `use debug`.

**Only import what you actually call.** The scanner flags both unused and missing imports.

**IMPORTANT: Write imports LAST, not first.** Write your function bodies first,
then add only the `use` statements for modules you actually called.

| Module | Common functions | Notes |
|--------|-----------------|-------|
| array | map, filter, reduce, sort, contains | Dot-notation works: `arr.push(x)` |
| string | upper, lower, trim, split, replace | Dot-notation works: `s.upper()` |
| math | abs, floor, ceil, round, min, max | `math.PI`, `math.E` |
| json | parse, stringify | |
| file | read, write, exists, append | Taint sink: sanitize before write |
| time | now, now_millis, sleep, format_timestamp | NOT `time.format` |
| csv | parse, stringify | |
| regex | search, matches, find, find_all, replace | NOT `regex.match` or `regex.test` |
| env | get, get_args, set_var | NOT `env.set` |
| io | write, read_line, write_error | |
| crypto | sha256, sha512, hash, random_bytes | |
| uuid | v4, v5, is_valid | |
| validate | email, url, ip, int_range, not_empty | |
| agent | create, send, batch, fan_out, check | `use agent` required |
| dict | get, has, put, keys, values, merge | Built-in, no `use` needed |
| debug | type, inspect, keys, values | Auto-imported, do NOT `use debug` |

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
`|>` does NOT support a `_` placeholder. Create wrapper functions instead.

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
2. Float-to-string uses trimmed format: `3.14` stays `"3.14"` (no trailing zeros)
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
13. `array.find` takes a PREDICATE function, not a value
14. `||` ALWAYS returns boolean (true/false), NEVER the operand value.
    For null coalesce, use the `??` operator: `x ?? default_val`
15. `dict["key"]` THROWS on missing key — use `dict.get("key")` for safe access
16. Struct instantiation requires `new`: `let p = new Point { x: 1, y: 2 }`
17. Python polyglot: Do NOT use `return` — causes `SyntaxError: 'return' outside function`
18. Value semantics: modifying a nested dict/array requires re-assigning to parent
19. The `..` range operator can collide with `".."` string literals — use intermediate variables
20. `try` works as both a statement and an expression.
    `let x = try { compute() } catch (e) { default_val }` — single expression per branch.
    For multi-statement catch, use statement form: `try { ... } catch (e) { ...; ... }`
21. `and`/`or`/`not` are NOT boolean operators in NAAb — use `&&`/`||`/`!`
22. `config` is a reserved keyword — do NOT use it as a variable name
23. Enum values from imported modules use 3-level dot access: `module_alias.EnumName.Variant`
24. `0..len(arr)` is correct for iterating all elements. `..` is exclusive (like Python range).
    Do NOT write `0..len(arr) - 1` — that skips the last element.
25. `use` imports are file-scoped — they do NOT propagate across `import`.
26. Import paths are relative to the importing file, NOT the project root.
    If both `main.naab` and `models.naab` are in `src/`, write `import "models.naab" as models`
27. Python polyglot: `try/except` swallows the return value. Put result variable LAST:
    ```
    result = "{}"
    try:
        result = json.dumps(data)
    except:
        pass
    result
    ```
28. Function names starting with `sanitize_`, `validate_`, `check_`, `verify_` trigger
    governance inspection for real validation logic. Only use these prefixes for genuine validators.
29. `new` is ONLY for struct instantiation. Plain dicts do NOT use `new`.
    WRONG: `new { "key": val }`   RIGHT: `{ "key": val }`
30. `match` arm bodies are parsed as dict literals — `1 => { var = expr }` fails.
    Use expression arms only, or extract helper functions for complex logic.
31. `json.stringify()` on structs may produce non-standard output. Convert to dict first
    for reliable JSON serialization.
32. Python polyglot returning a dict/list MUST use `-> JSON` or `json.dumps()`.
    Without it, Python's repr output is returned as a string, not a NAAb dict.
33. Do NOT sanitize content that has already been hashed. Hash raw content, sanitize at file.write.
34. `math.min()`/`math.max()` return float when either argument is float.
35. Dot-notation array/string methods work WITHOUT `use array`/`use string`.
    Only need `use array` for module-form calls like `array.map(arr, fn)`.

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

Do NOT pad functions with dummy loops. Add real logic — input validation, edge cases, try/catch.

## Contract Patterns

When govern.json defines function contracts, your return value MUST match.
Every listed return key must be present in the returned dict.

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

## Project: DevOps Incident Management Platform

An incident response platform that ingests alerts, correlates them into incidents,
assigns responders, tracks resolution timelines, computes SLA compliance, and
generates post-mortem reports.

**This is a strict governance project.** Tests verify exact computed values. Governance
blocks sloppy code at hard level. All 15 function contracts are enforced.

## Governance Summary (govern.json)
- **Allowed languages:** python, shell
- **Blocked:** javascript, rust, cpp, csharp, go, nim, zig, julia, ruby, php
- **Variable binding: HARD** — every polyglot block must have `[var1, var2]` or `[]`
- **Network:** enabled (for agent API calls)
- **Filesystem:** read_write (for report output)
- **Shell:** enabled (blocked: rm, curl, wget, chmod, sudo)
- **Taint tracking:** enabled — polyglot_output and env.get are sources, file.write/append are sinks
- **Security:** no_secrets (hard), no_placeholders (hard), no_simulation_markers (hard)
- **15 function contracts** with return_keys enforcement
- **4-tier complexity floor** (test_/calculate_/classify_/create_ tiers)
- **Scanner HARD checks:** empty_catch, dead_conditional, value_semantics_bug, missing_null_check, dict_bracket_access, python_return_in_block, top_level_let
- **Agent config:** 3 Gemini agents (rca_agent, rca_agent_b1, rca_agent_b2) with GK1-GK3 keys
- **Cumulative scoring:** yellow at 15, red (block) at 30

## File Structure

All source files are in `src/`. Import paths use just the filename (same directory):
```
src/
├── main.naab         # Test orchestrator (90 tests, 9 suites)
├── models.naab       # Structs + enums + factory functions
├── alerts.naab       # Alert ingestion + parsing + classification
├── incidents.naab    # Incident lifecycle + correlation
├── responders.naab   # On-call management + assignment
├── escalation.naab   # Escalation engine + dependency traversal
├── sla.naab          # SLA computation + compliance tracking
├── reports.naab      # Report generation + file output (taint sink)
├── validators.naab   # Sanitizer + validation functions (taint clearing)
└── rca.naab          # Agent-based root cause analysis
```

**Import example (from main.naab):**
```naab
import "models.naab" as models
import "alerts.naab" as alerts
import "validators.naab" as validators
```

**Dependency graph:**
- models.naab: no imports (defines structs/enums only)
- validators.naab: imports models (for Severity enum)
- alerts.naab: imports models
- incidents.naab: imports models
- responders.naab: imports models
- escalation.naab: imports models
- sla.naab: imports models
- reports.naab: imports models, validators (for sanitize_string)
- rca.naab: imports models
- main.naab: imports ALL modules

## Enum Definitions

```naab
export enum Severity { Critical, High, Medium, Low }
export enum IncidentStatus { Open, Acknowledged, Investigating, Resolved, Closed }
export enum AlertSource { Prometheus, Grafana, CloudWatch, PagerDuty, Manual }
export enum EscalationLevel { L1, L2, L3, Executive }
export enum ResponderRole { Engineer, SRE, Manager, Executive }
```

**Usage from other modules:**
```naab
import "models.naab" as models
if sev == models.Severity.Critical { ... }
```

## Struct Definitions

```naab
export struct Alert {
    id: string
    source: int       // AlertSource enum value
    message: string
    severity: int     // Severity enum value
    timestamp: int    // unix seconds
    tags: array
    raw_log: string
}

export struct Incident {
    id: string         // uuid
    title: string
    severity: int      // Severity enum value
    status: int        // IncidentStatus enum value
    alerts: array      // array of Alert structs
    responder_id: string
    created_at: int    // unix seconds
    updated_at: int
    resolved_at: int   // 0 if not resolved
    sla_deadline: int  // unix seconds
    correlation_id: string  // uuid
}

export struct Responder {
    id: string
    name: string
    email: string
    role: int          // ResponderRole enum value
    on_call: bool
    current_load: int
    max_load: int
    skills: array      // array of strings
}

export struct Escalation {
    id: string
    incident_id: string
    from_level: int    // EscalationLevel enum value
    to_level: int
    reason: string
    escalated_at: int
}

export struct PostMortem {
    id: string
    incident_id: string
    root_cause: string
    timeline: array
    action_items: array
    created_by: string
}

export struct SLAPolicy {
    id: string
    name: string
    severity: int      // Severity enum value
    response_minutes: int
    resolution_minutes: int
}
```

## Module Specifications

### models.naab — Domain Models

**Exported Functions:**
- `create_alert(id, source, message, severity, timestamp, tags, raw_log)` -> new Alert struct
- `create_incident(id, title, severity, alerts, sla_deadline)` -> new Incident struct
  - Sets status=IncidentStatus.Open, created_at=time.now(), updated_at=time.now(), resolved_at=0, responder_id="", correlation_id=""
- `create_responder(id, name, email, role, on_call, max_load, skills)` -> new Responder struct
  - Sets current_load=0
- `create_sla_policy(id, name, severity, response_min, resolution_min)` -> new SLAPolicy struct
- `severity_name(sev)` -> string ("Critical", "High", "Medium", "Low")
- `status_name(status)` -> string ("Open", "Acknowledged", "Investigating", "Resolved", "Closed")
- `source_name(source)` -> string ("Prometheus", "Grafana", "CloudWatch", "PagerDuty", "Manual")
- `validate_alert(alert)` -> dict: {valid, errors}
  - Checks: id non-empty, message non-empty, tags is array
- `validate_responder(responder)` -> dict: {valid, errors}
  - Checks: id non-empty, name non-empty, max_load > 0

### alerts.naab — Alert Ingestion & Classification
Requires: `use regex`

**Exported Functions:**
- `parse_log_line(raw_line)` -> dict: {timestamp, level, service, message}
  - Regex pattern: `(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}) \[(\w+)\] (\w+): (.+)`
  - If no match: returns {timestamp: null, level: null, service: null, message: raw_line}
- `classify_alert(alert)` -> dict: {category, confidence, patterns_matched}
  - Category rules (check message with regex.search or string.contains):
    - "OOM" or "out of memory" or "memory" -> category="memory", confidence=90
    - "5xx" or "500" or "502" or "503" -> category="http_error", confidence=85
    - "timeout" or "timed out" -> category="timeout", confidence=80
    - "disk" or "storage" or "space" -> category="disk", confidence=85
    - "cpu" or "load" -> category="cpu", confidence=75
    - default -> category="unknown", confidence=50
  - patterns_matched = number of pattern keywords found in message
- `correlate_alerts(alerts, window_seconds)` -> array of arrays
  - Groups alerts where timestamp difference <= window_seconds AND same service tag
  - Sort alerts by timestamp first. Walk through, group consecutive alerts within window.
- `deduplicate_alerts(alerts)` -> array
  - Remove duplicates where message AND source are identical AND timestamps within 60 seconds
  - Keep the first occurrence
- `build_alert_pipeline(raw_lines)` -> array of dicts
  - Pipeline: for each raw_line, parse_log_line |> create alert dict |> classify
  - Returns array of {alert, classification} dicts
- `score_alert_urgency(alert, active_incidents)` -> int 0-100
  - Python polyglot with `-> JSON`:
  - Base score by severity: Critical=80, High=60, Medium=40, Low=20
  - +10 if active_incidents has same-service incident
  - +10 if category is "memory" or "http_error"
  - Capped at 100

### incidents.naab — Incident Lifecycle
Requires: `use uuid`, `use time`

**Exported Functions:**
- `open_incident(title, severity, alerts, sla_policies)` -> dict: {incident, sla_deadline_minutes}
  - Generate uuid.v4() for id and correlation_id
  - Find matching SLA policy by severity. sla_deadline = now + response_minutes * 60
  - If no matching policy: sla_deadline_minutes = 0
  - Returns {incident: new Incident struct, sla_deadline_minutes: policy.response_minutes or 0}
- `acknowledge_incident(incident, responder_id)` -> dict (incident fields as dict)
  - Only if status == Open. Sets status=Acknowledged, responder_id, updated_at=now
  - If not Open: returns incident unchanged
- `start_investigation(incident)` -> dict
  - Only if status == Acknowledged. Sets status=Investigating, updated_at=now
- `resolve_incident(incident, resolution_notes)` -> dict
  - Only if status == Investigating. Sets status=Resolved, resolved_at=now, updated_at=now
- `close_incident(incident)` -> dict
  - Only if status == Resolved. Sets status=Closed, updated_at=now
- `merge_incidents(primary, secondary)` -> dict: {merged, absorbed_count}
  - Combines alert arrays from both incidents. absorbed_count = len(secondary.alerts)
- `get_incidents_by_status(incidents, status)` -> filtered array
- `calculate_mttr(incidents)` -> float (hours)
  - Python polyglot: for resolved incidents, MTTR = avg(resolved_at - created_at) / 3600
  - If no resolved incidents: returns 0.0

### responders.naab — On-Call Management
Requires: `use validate`

**Exported Functions:**
- `register_responder(roster, responder)` -> dict: {roster, success}
  - Validates email with validate.email(). If invalid: {roster: roster, success: false, error: "invalid email"}
  - Checks for duplicate id. If duplicate: {roster: roster, success: false, error: "duplicate id"}
  - On success: pushes responder to roster, returns {roster, success: true}
- `find_best_responder(roster, severity, required_skills)` -> dict: {responder, score, reason}
  - Scoring: on_call=+30, low load (load < 50% of max)=+20, each matching skill=+10
  - If severity is Critical: on_call weight doubles to +60
  - Returns highest-scoring responder. If none found (all off-call or overloaded): {responder: null, score: 0, reason: "no available responder"}
- `assign_responder(roster, responder_id, incident_id)` -> dict: {roster, responder, success}
  - Finds responder by id. Increments current_load. Re-assigns in roster array (value semantics!)
  - If not found: {roster, responder: null, success: false}
- `release_responder(roster, responder_id)` -> dict: {roster, success}
  - Decrements current_load (min 0). Re-assigns in roster array.
- `get_on_call(roster)` -> array of responders where on_call == true
- `calculate_responder_load(responder)` -> dict: {load_percent, available_capacity, overloaded}
  - load_percent = (current_load / max_load) * 100
  - available_capacity = max_load - current_load
  - overloaded = current_load >= max_load
- `validate_contact_info(responder)` -> dict: {valid, errors}
  - Checks validate.email(responder.email)
  - errors is array of strings describing failures

### escalation.naab — Escalation Engine

**Exported Functions:**
- `check_escalation_needed(incident, sla_policies, current_time)` -> dict: {needed, reason, target_level}
  - Find SLA policy matching incident severity
  - If time elapsed > response_minutes * 60 and status is Open: needed=true, target_level=EscalationLevel.L2
  - If time elapsed > resolution_minutes * 60 and status not Resolved/Closed: needed=true, target_level=EscalationLevel.L3
  - Otherwise: needed=false
- `escalate_incident(incident, escalations, from_level, to_level, reason)` -> dict: {incident, escalation, escalations}
  - Creates new Escalation struct with uuid.v4() id. Pushes to escalations array.
  - Updates incident severity if escalating to L3 or Executive (bump to Critical)
- `get_escalation_chain(incident_id, escalations)` -> array
  - Filters escalations by incident_id, returns matching ones
- `traverse_dependencies(service_graph, root_service, visited)` -> array
  - RECURSIVE function. service_graph is dict: {"svc_a": ["svc_b", "svc_c"], ...}
  - Base case: root_service not in graph OR already in visited -> return []
  - Mark root_service as visited. For each dependency, recurse. Return all found services.
- `calculate_blast_radius(service_graph, failed_service)` -> dict: {affected_services, depth, critical_path}
  - Calls traverse_dependencies with empty visited array
  - depth = max depth reached during traversal
  - critical_path = the longest chain
- `auto_escalate_batch(incidents, sla_policies, escalations, current_time)` -> dict: {escalated, escalations, summary}
  - For each incident, check_escalation_needed. If needed, escalate.
  - escalated = count of escalated incidents
  - summary = array of {incident_id, target_level} for each escalated

### sla.naab — SLA Computation
Requires: `use math`, `use time`

**Exported Functions:**
- `check_sla_compliance(incident, sla_policy, current_time)` -> dict: {compliant, response_compliant, resolution_compliant, time_remaining_minutes, breach_severity}
  - response_compliant: (acknowledged_at or current_time) - created_at <= response_minutes * 60
  - resolution_compliant: if resolved, resolved_at - created_at <= resolution_minutes * 60; else time elapsed <= resolution_minutes * 60
  - time_remaining_minutes: (created_at + resolution_minutes * 60 - current_time) / 60
  - compliant = response_compliant AND resolution_compliant
  - breach_severity: if not compliant, match incident severity name; else "none"
- `calculate_sla_metrics(incidents, sla_policies)` -> dict: {total, compliant_count, breach_count, compliance_rate, avg_response_minutes, avg_resolution_minutes}
  - Python polyglot `-> JSON`: compute across all incidents
  - compliance_rate = compliant_count / total (0.0 if no incidents)
- `predict_breach_risk(incident, sla_policy, current_time)` -> dict: {risk_score, risk_level, estimated_resolution_minutes}
  - Python polyglot `-> JSON`:
  - elapsed = current_time - created_at
  - total_allowed = resolution_minutes * 60
  - ratio = elapsed / total_allowed
  - risk_score = int(ratio * 100), capped at 100
  - risk_level: "critical" if >= 80, "high" if >= 60, "medium" if >= 40, "low" otherwise
  - estimated_resolution_minutes = (total_allowed - elapsed) / 60
- `generate_sla_summary(incidents, sla_policies)` -> dict: {by_severity, overall_compliance, worst_breach}
  - Pipeline: group incidents by severity |> compute compliance per group |> find worst breach
- `get_breach_timeline(incidents, sla_policies)` -> array of dicts
  - For breached incidents: {incident_id, severity, breach_type, breached_at_minutes}
  - Sorted by breached_at_minutes ascending

### reports.naab — Report Generation (Taint Sink)
Requires: `use json`, `use csv`, `use crypto`, `use file`
Imports: validators.naab

**Exported Functions:**
- `generate_incident_report(incident, escalations, responder)` -> dict: {report_text, hash, sections}
  - report_text: formatted text with incident details, escalation history, responder info
  - hash: crypto.sha256(report_text) — 64-char hex string
  - sections: ["summary", "timeline", "escalations", "responder"]
- `generate_postmortem(incident, root_cause, timeline_events, action_items)` -> dict: {postmortem, report_text}
  - postmortem: dict with all PostMortem fields
  - report_text: formatted text
- `export_incidents_csv(incidents, headers)` -> string
  - Uses csv.stringify. headers = ["id", "title", "severity", "status", "created_at"]
- `generate_dashboard_data(incidents, responders, sla_policies)` -> dict: {open_count, mttr, sla_compliance, by_severity, by_status}
  - open_count: count of Open/Acknowledged/Investigating incidents
  - mttr: calculated from resolved incidents
  - sla_compliance: fraction of compliant incidents
  - by_severity: dict of severity_name -> count
  - by_status: dict of status_name -> count
- `build_audit_trail(events)` -> dict: {trail, hash_chain}
  - events is array of strings. Each event gets: {event, timestamp, hash}
  - hash = crypto.sha256(event + previous_hash). First hash uses "genesis" as previous.
  - trail = array of event dicts. hash_chain = array of hashes.
- `write_report_to_file(report_text, filepath)` -> dict: {success, path, bytes_written}
  - MUST sanitize report_text with validators.sanitize_string() before file.write()
  - This is the taint sink — taint tracking requires the sanitize call

### validators.naab — Sanitization & Validation
Requires: `use regex`, `use validate`
Imports: models.naab

**Exported Functions:**
- `sanitize_string(input)` -> string
  - If null: return ""
  - Strip `<` and `>` (injection vectors). Preserve `"`, `{`, `}`, `[`, `]`.
  - Trim whitespace.
- `sanitize_log_line(line)` -> string
  - Strip ANSI escape codes: regex.replace(line, `\x1b\[[0-9;]*m`, "")
  - Then call sanitize_string on the result
- `validate_incident_data(data)` -> dict: {valid, errors}
  - Required fields: "title" (non-empty string), "severity" (valid enum)
  - errors = array of error message strings
- `validate_email_address(email)` -> bool
  - Uses validate.email(email)
- `validate_ip_address(ip)` -> bool
  - Uses validate.ip(ip)
- `validate_severity(sev)` -> bool
  - Checks sev is one of Severity.Critical, .High, .Medium, .Low

### rca.naab — Agent-Based Root Cause Analysis
Requires: `use agent`, `use json`
Imports: models.naab

**Exported Functions:**
- `check_agent_available(config_name)` -> dict: {available, reason}
  - Uses agent.check(config_name). Returns {available: result.valid, reason: result.error or "ready"}
- `generate_rca_prompt(incident, logs)` -> string
  - Builds structured text: "Incident: [title]\nSeverity: [severity_name]\nLogs:\n[each log line]"
- `analyze_root_cause(incident, logs, service_graph)` -> dict: {root_cause, confidence, reasoning, suggestions}
  - try/catch wrapper around agent calls
  - Create agent with agent.create("rca_agent")
  - Turn 1: send incident context via generate_rca_prompt
  - Turn 2: send service graph for dependency analysis
  - Parse response for root_cause, confidence, reasoning, suggestions
  - On failure (API error): return {root_cause: "unavailable", confidence: 0, reasoning: "agent error", suggestions: []}
- `batch_analyze(incidents, logs)` -> array of dicts
  - Creates 3 agent handles (rca_agent, rca_agent_b1, rca_agent_b2)
  - Uses agent.batch for parallel analysis
  - Handles partial failures gracefully

## Test Orchestrator (main.naab) — 90 Tests

### test_models() — 10 tests
1. Create Alert: `models.create_alert("A1", models.AlertSource.Prometheus, "OOM killed", models.Severity.Critical, 1000, ["api", "prod"], "2024-01-01T00:00:00 [ERROR] api: OOM killed")` — verify alert.id == "A1"
2. Create Incident: `models.create_incident("I1", "API Down", models.Severity.Critical, [], 2000)` — verify status == models.IncidentStatus.Open
3. Create Responder: `models.create_responder("R1", "Alice", "alice@example.com", models.ResponderRole.SRE, true, 5, ["kubernetes", "networking"])` — verify name == "Alice"
4. Create SLAPolicy: `models.create_sla_policy("SLA1", "Critical SLA", models.Severity.Critical, 15, 60)` — verify response_minutes == 15
5. severity_name: `models.severity_name(models.Severity.Critical)` == "Critical"
6. status_name: `models.status_name(models.IncidentStatus.Resolved)` == "Resolved"
7. source_name: `models.source_name(models.AlertSource.Prometheus)` == "Prometheus"
8. validate_alert valid: create valid alert, `models.validate_alert(alert)` -> valid == true, len(errors) == 0
9. validate_responder invalid: create responder with empty name "", `models.validate_responder(r)` -> valid == false
10. Enum inequality: `models.Severity.Critical != models.Severity.Low` == true

### test_alerts() — 10 tests
1. parse_log_line valid: `alerts.parse_log_line("2024-01-15T10:30:00 [ERROR] api: OOM killed process")` -> timestamp == "2024-01-15T10:30:00", level == "ERROR", service == "api"
2. parse_log_line malformed: `alerts.parse_log_line("garbage data")` -> timestamp == null
3. classify OOM: create alert with message "OOM killed process", `alerts.classify_alert(alert)` -> category == "memory"
4. classify 5xx: create alert with message "HTTP 503 service unavailable", classify -> category == "http_error"
5. correlate within window: 3 alerts at timestamps 100, 130, 150 (same service "api") with window=60 -> 1 group of 3
6. correlate separate groups: 2 alerts at timestamps 100 and 300 (same service) with window=60 -> 2 groups
7. deduplicate: 2 alerts same message "OOM" same source, timestamps 100 and 110 -> 1 result
8. pipeline: 3 raw log lines -> `alerts.build_alert_pipeline(lines)` returns array of length 3
9. urgency Critical + active: score_alert_urgency with Critical alert + 1 active incident -> score >= 80
10. urgency Low + none: score_alert_urgency with Low alert + no incidents -> score <= 30

### test_incidents() — 10 tests
1. open_incident: creates incident with uuid ID, status Open
2. uuid valid: `uuid.is_valid(result.get("incident").get("id"))` == true
3. acknowledge: Open -> Acknowledged transition works
4. investigate: Acknowledged -> Investigating works
5. resolve: Investigating -> Resolved, resolved_at > 0
6. close: Resolved -> Closed
7. invalid transition: try to acknowledge a Closed incident -> status stays Closed
8. merge: primary has 2 alerts, secondary has 1 alert -> merged has 3 alerts, absorbed_count == 1
9. filter by status: 3 incidents (2 Open, 1 Resolved) -> get_incidents_by_status(Open) returns 2
10. MTTR: 2 resolved incidents with known created_at/resolved_at -> exact MTTR value

### test_responders() — 10 tests
1. register valid: valid email "alice@example.com" -> success == true
2. register invalid email: "not-an-email" -> success == false
3. find_best: 2 responders, one on-call with matching skills -> returns on-call one
4. find_best none: all off-call -> responder == null
5. assign: increments current_load from 0 to 1
6. release: decrements current_load from 2 to 1
7. get_on_call: 3 responders, 2 on-call -> returns array of length 2
8. load 60%: current_load=3, max_load=5 -> load_percent == 60, overloaded == false
9. load 100%: current_load=5, max_load=5 -> load_percent == 100, overloaded == true
10. validate_contact: valid email -> {valid: true, errors: []}

### test_escalation() — 10 tests
1. within SLA: incident created 5 min ago, SLA response=15 min -> needed == false
2. past SLA: incident created 20 min ago, SLA response=15 min -> needed == true, target_level L2
3. escalate: creates Escalation struct, incident updated
4. escalation chain: 2 escalations for same incident -> chain length 2
5. traverse linear: graph {a: [b], b: [c], c: []} -> traverse from "a" returns ["b", "c"]
6. traverse cycle: graph {a: [b], b: [a]} -> no infinite loop, returns ["b"]
7. traverse tree: graph {a: [b, c], b: [d], c: [e], d: [], e: []} -> returns ["b", "c", "d", "e"] (4 services)
8. blast radius: 5-service graph -> affected_services count == 4, depth >= 2
9. auto_escalate batch: 3 incidents, 2 past SLA -> escalated == 2
10. level ordering: EscalationLevel.L1 value < EscalationLevel.L3 value

### test_sla() — 10 tests
1. compliant: incident created 5 min ago, response SLA=15, resolution SLA=60 -> compliant == true
2. response breach: incident created 20 min ago, not acknowledged, response SLA=15 -> response_compliant == false
3. resolution breach: incident created 90 min ago, resolution SLA=60 -> resolution_compliant == false
4. metrics: 4 incidents, 3 compliant -> compliance_rate == 0.75
5. high breach risk: 80% of SLA time elapsed -> risk_level == "critical"
6. low breach risk: 20% of SLA time elapsed -> risk_level == "low"
7. summary pipeline: by_severity has correct counts per severity level
8. breach timeline: 2 breached incidents sorted by breach time
9. SLA values: Critical=15min response, High=30, Medium=60, Low=120
10. time remaining: created 10 min ago, resolution SLA=60 -> time_remaining_minutes == 50

### test_reports() — 10 tests
1. report contains title: generate_incident_report -> report_text contains incident title string
2. hash length: report hash is 64 characters (SHA-256 hex)
3. postmortem: generate_postmortem -> report_text contains root_cause string
4. CSV export: export_incidents_csv with 3 incidents -> CSV has 4 lines (header + 3 rows)
5. dashboard open_count: 2 open, 1 resolved -> open_count == 2
6. dashboard by_severity: correct count per severity
7. audit trail length: 3 events -> hash_chain length == 3
8. audit chain integrity: hash_chain[1] != hash_chain[0] (each depends on previous)
9. write sanitized: report with "<script>" -> sanitized output has no "<" or ">"
10. taint passes: write_report_to_file succeeds without taint violation

### test_validators() — 10 tests
1. sanitize strips angle brackets: `sanitize_string("hello <script>")` -> "hello script"
2. sanitize preserves JSON chars: `sanitize_string('{"key": "val"}')` -> `{"key": "val"}`
3. sanitize_log_line strips ANSI: `sanitize_log_line("\x1b[31mERROR\x1b[0m")` -> "ERROR"
4. validate_incident valid: {title: "Down", severity: Severity.Critical} -> valid == true
5. validate_incident missing title: {title: "", severity: Severity.Critical} -> valid == false
6. validate email valid: `validate_email_address("user@example.com")` == true
7. validate email invalid: `validate_email_address("not-an-email")` == false
8. validate IP valid: `validate_ip_address("192.168.1.1")` == true
9. validate IP invalid: `validate_ip_address("999.999.999.999")` == false
10. validate severity: `validate_severity(models.Severity.Critical)` == true

### test_rca() — 10 tests
1. agent available: `check_agent_available("rca_agent")` with valid config -> check available field exists
2. agent unavailable: `check_agent_available("nonexistent_agent")` -> available == false
3. prompt contains title: generate_rca_prompt with incident -> result contains incident title
4. prompt contains logs: generate_rca_prompt with 2 log lines -> result contains both lines
5. RCA returns contract keys: analyze_root_cause -> result has "root_cause", "confidence", "reasoning", "suggestions"
6. RCA confidence type: result.confidence is number or string (agent response varies)
7. RCA handles failure: if no API key, returns {root_cause: "unavailable", ...} without crashing
8. batch returns array: batch_analyze with 2 incidents -> result is array of length 2
9. batch partial failure: one agent has bad key -> at least one result still succeeds
10. usage tracking: after analyze_root_cause, agent.usage shows turns > 0

**Note on test_rca:** Tests 5-10 require Gemini API keys (GK1-GK3 env vars). If keys are
missing, these tests should gracefully fail (count as 0 passed, not crash). The test suite
should still complete and report scores for all other suites.

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
- Do NOT modify govern.json — it is signed and modifications will be blocked
- The governance engine detects 200+ stub/evasion patterns and will BLOCK execution

## Struct Usage Examples

### Creating structs in the same module:
```naab
export fn create_alert(id, source, message, severity, timestamp, tags, raw_log) {
    return new Alert {
        id: id,
        source: source,
        message: message,
        severity: severity,
        timestamp: timestamp,
        tags: tags,
        raw_log: raw_log
    }
}
```

### Cross-module struct instantiation:
```naab
import "models.naab" as models

fn make_test_alert() {
    return models.create_alert(
        "A1",
        models.AlertSource.Prometheus,
        "OOM killed",
        models.Severity.Critical,
        1000,
        ["api"],
        "log line"
    )
}
```

### Accessing struct fields:
Struct fields are accessed like dict fields:
```naab
let alert = models.create_alert(...)
let id = alert.get("id")         // Safe access
let msg = alert.get("message")   // Safe access
```

## Pipeline Usage Examples

```naab
// Single-arg pipeline:
fn parse_line(line) { return parse_log_line(line) }
fn classify(parsed) { return classify_alert(parsed) }
let result = raw_line |> parse_line |> classify

// With wrapper for multi-arg functions:
fn filter_critical(incidents) {
    return array.filter(incidents, fn(i) {
        return i.get("severity") == models.Severity.Critical
    })
}
let critical = all_incidents |> filter_critical
```

## Agent Usage Examples

```naab
use agent

fn safe_agent_call(config_name, prompt_text) {
    let pre = agent.check(config_name)
    if pre.get("valid") != true {
        return {root_cause: "unavailable", confidence: 0, reasoning: "agent not configured", suggestions: []}
    }
    let handle = agent.create(config_name)
    try {
        let resp = agent.send(handle, prompt_text)
        let content = resp.get("content") ?? ""
        return {root_cause: content, confidence: 1, reasoning: "agent analysis", suggestions: []}
    } catch (e) {
        return {root_cause: "unavailable", confidence: 0, reasoning: string(e), suggestions: []}
    }
}
```

## Recursive Function Example

```naab
fn traverse_dependencies(graph, root, visited) {
    if !graph.has(root) {
        return []
    }
    if array.contains(visited, root) {
        return []
    }
    visited.push(root)
    let deps = graph.get(root)
    let all_affected = []
    for dep in deps {
        if !array.contains(visited, dep) {
            all_affected.push(dep)
            let sub = traverse_dependencies(graph, dep, visited)
            for s in sub {
                if !array.contains(all_affected, s) {
                    all_affected.push(s)
                }
            }
        }
    }
    return all_affected
}
```
