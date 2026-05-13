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
Polyglot blocks MUST be multiline. The `>>` closer MUST be on its own line at column 0.
**The `>>` closer must have NO leading spaces/tabs — not indented to match the function body.**
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

### WRONG — indented `>>` (most common polyglot mistake):
```naab
fn compute() {
    let result = <<python
    x = 42
    x * 2
    >>              // WRONG — >> is indented to match the function body
    return result
}
```

### RIGHT — `>>` at column 0 even inside functions:
```naab
fn compute() {
    let result = <<python
x = 42
x * 2
>>
    return result
}
```
The `>>` closer and Python code inside the block start at column 0, regardless of
how deeply nested the surrounding NAAb code is. This looks odd but is required.

### Variable Binding
Pass NAAb variables to polyglot blocks explicitly.
If your govern.json requires variable binding, ALL variables must be listed:
```naab
let data = [1, 2, 3]
let multiplier = 10
let result = <<python[data, multiplier]
sum(d * multiplier for d in data)
>>
```

### Language-Specific Rules
- **Python**: Start at column 0 (no leading indent). Use separate lines, not semicolons.
  Last expression is the return value. `import` works.
  **Do NOT use `return`** — it causes `SyntaxError: 'return' outside function`.
  The last expression's value is automatically captured.
  WRONG: `return json.dumps(data)`
  RIGHT: `json.dumps(data)`
- **JavaScript**: Uses embedded QuickJS (NOT Node.js). Last expression is the return value.
  Use `const`/`let`, NOT `var`. Multi-line JS is wrapped in an IIFE for return capture.
  **Keep JS blocks simple**: declare variables, compute, then put the result expression last.
  Arrow functions work but avoid them on the last line — prefer a named result variable.
  WRONG (last line is arrow function):
  ```
  const items = data.filter(x => x > 0);
  items.map(x => x * 2)    // arrow as last expr — fragile
  ```
  RIGHT (last line is a simple variable):
  ```
  const items = data.filter(x => x > 0);
  const result = items.map(x => x * 2);
  result
  ```
  No `console.log` — use NAAb `io.write` for output.
- **Shell**: stdout is the return value.
- **Go**: Needs `package main`, `import`, `func main()`. `fmt.Println` for output.
- **Nim**: Compiled language. Use `echo` for output. No `execCmd`.
- **Rust**: Needs `fn main()`. `println!` for output. Compiled via `rustc`.
- **C++**: Needs `#include` and `int main()`. Compiled via `g++`.
- **Ruby**: Use `puts` for output (bare expressions return empty string).
- **PHP**: Needs `<?php` on first line. Use `echo` for output.
- **Zig**: Compiled via `zig build-exe`. Use `std.debug.print` for output.
- **Julia**: JIT-compiled. Use `println` for output.

### JSON Sovereign Pipe
For structured return data from polyglot blocks, use `-> JSON`:
```naab
let data = <<python -> JSON
import json
print(json.dumps({"key": "value", "count": 42}))
>>
```
Bare `json.dumps(data)` as the last expression also works (auto-wrapped in `print()`).

**`-> JSON` with JavaScript** works differently from Python. JS uses eval-based return (not stdout).
Use `JSON.stringify()` as the last expression — do NOT use `console.log`:
```naab
let data = <<javascript -> JSON
const result = {key: "value", count: 42};
JSON.stringify(result)
>>
```
**If `-> JSON` fails with JS, drop the `-> JSON` and use `json.parse()` instead:**
```naab
let raw = <<javascript[input]
const result = {key: "value", count: input.length};
JSON.stringify(result)
>>
let data = json.parse(raw)
```
This two-step approach is more reliable than `-> JSON` for JavaScript blocks.

## Taint Tracking

When govern.json has `taint_tracking` enabled, the runtime tracks data flow from
**sources** (polyglot output, file.read, io.read_line) to **sinks** (file.write, file.append).
Tainted data must pass through a **sanitizer** (functions starting with `validate_` or
`sanitize_`, or type casts like `int()`, `float()`, `string()`) before reaching a sink.

### Common pattern: polyglot output → file.write
```naab
// WRONG — taint violation (polyglot output flows directly to file.write):
let result = <<python
json.dumps({"key": "value"})
>>
file.write("data/output.json", result)

// RIGHT — sanitize before writing:
let result = <<python
json.dumps({"key": "value"})
>>
let clean = sanitize_string(result)
file.write("data/output.json", clean)
```

### Where to sanitize
Sanitize at the **sink** (just before file.write/file.append), not at the source.
The taint engine traces data flow through string concatenation, interpolation, function
returns, loop iterators, and dict/array access. Sanitizing early may not clear taint
if the value is later concatenated with other tainted data.

### sanitize_string design
Your `sanitize_string` function must NOT strip characters that are valid in the
output format. If writing JSON to a file, do NOT strip double quotes `"` — that breaks
JSON structure. A safe sanitizer for file output:
```naab
fn sanitize_string(input) {
    if input == null { return "" }
    let s = string.trim(input)
    s = string.replace(s, "<", "")   // Strip HTML/XML injection vectors
    s = string.replace(s, ">", "")
    return s
}
```
Do NOT strip `"`, `{`, `}`, `[`, `]` — these are structural characters in JSON/data formats.

## Stdlib Reference (ALL functions)

### array (dot-notation works: arr.push(4))
push, pop, shift, unshift, length, contains, find, first, last,
join, reverse, sort, slice_arr, map, filter, reduce, map_fn, filter_fn, reduce_fn

Both dot-notation aliases and module syntax work:
```naab
// Dot-notation aliases (preferred):
array.map(arr, fn(x) { return x * 2 })
array.filter(arr, fn(x) { return x > 5 })
array.reduce(arr, fn(acc, x) { return acc + x }, 0)

// Original _fn variants also work:
array.map_fn(arr, fn(x) { return x * 2 })
array.filter_fn(arr, fn(x) { return x > 5 })
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
- `env.get_args()` — returns array of CLI arguments passed after the script name
- Always check `len(args) > N` before indexing `args[N]` — out-of-bounds throws; the scanner flags unguarded access
- Do NOT use `env.get("NAAB_ARGS")` or Python `sys.argv` — use `env.get_args()` for CLI args

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
- `log.debug("msg")`, `log.warn("msg")`, `log.log("msg")`
- `log.set_level("debug"|"info"|"warn"|"error")`

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

### process
run, exit, kill, getpid
- `process.run("command")` — execute shell command, returns {stdout, stderr, exit_code}
- `process.exit(code)` — exit with code (**requires `use process`**; BLOCKED when
  `capabilities.shell.enabled: false` in govern.json — use `return` to exit main instead)
- `process.getpid()` — current process ID

### path
join, dirname, basename, extension, resolve, is_absolute, normalize, exists
- `path.join("dir", "file.txt")` — OS-aware path joining
- `path.dirname("/a/b/c")` → `"/a/b"`
- `path.extension("file.txt")` → `".txt"`

### http
get, post, put, delete, head, patch, call
- `http.get(url)` — returns response dict {status, body, headers}
- `http.post(url, body)` — POST request
- `http.call(method, url, options)` — generic request

### agent (requires `use agent`)
create, send, run, messages, usage, batch, fan_out, pipeline
- `agent.create(config_name)` — create agent handle from govern.json `agents` config, returns handle dict
- `agent.send(handle, message)` — send message to agent, returns response dict {content, stop_reason, usage}
- `agent.run(config_name, prompt)` — one-shot: create + send + return content string
- `agent.messages(handle)` — return conversation history array
- `agent.usage(handle)` — return cumulative {input_tokens, output_tokens, total_tokens, turns}
- `agent.batch(handles, messages)` — parallel: send messages[i] to handles[i], returns array of response dicts
- `agent.fan_out(handles, message)` — parallel: send same message to all handles, returns array of response dicts
- `agent.pipeline(handles, initial_message)` — sequential chain: output of agent N becomes input to agent N+1, returns final response dict
- `agent.check(config_name)` — pre-flight validation: returns `{valid: bool, error: string, provider, model, api_key_env}`. Checks config exists and API key env var is set. Does NOT make an API call.
- `agent.batch()` is resilient: if individual calls fail, returns `{success: false, error: "...", content: ""}` for that slot instead of crashing. Callers should check `resp.get("success") == false`.
- `agent.batch()` note: handle dicts are copied by value for thread safety — caller's handle objects are NOT updated with turn counts/message history after batch. Use `agent.usage(handle)` for authoritative state.
- `agent.pipeline()` throws if any non-final stage returns empty content — prevents silent message reuse across stages.
- Providers: `"anthropic"` (default, uses `ANTHROPIC_API_KEY`) or `"gemini"`/`"google"` (uses `GEMINI_API_KEY` or custom `api_key_env`)
- Governance enforcement on responses: `checkSecrets()` (HARD blocks leaked API keys/tokens), `checkPii()` (respects configured level)
- `tool_use`/`FUNCTION_CALL` responses are HARD blocked (agent tool execution not yet supported)
- Turn/token limits enforced server-side — handle dict mutation does not bypass governance
- Per-agent `allowed_paths`/`shell_allowed` logged as advisory once per config name (enforced when tool execution loop lands)
- Parallel dispatch config (optional in govern.json):
  ```json
  "agent_dispatch": { "max_concurrent": 6, "pool_size": 6, "pool_queue_max": 50 }
  ```
- Agent review dispatch mode (in govern.json `agent_review` section):
  `"dispatch_mode": "parallel"` — run detection agents concurrently (default: `"sequential"`)
  `"max_parallel": 4` — limit concurrent detection agents (0 = unlimited)
  `"fail_strategy": "fail_fast"` — abort on first error (`"continue"` = collect all results)
- Output tokens estimated (~content.size()/4) when Gemini API omits `candidatesTokenCount` (common with Gemma models)

## Functions That Do NOT Exist (use alternatives)
- `array.merge(a, b)` — use `a + b` (array concatenation with +)
- `array.concat(a, b)` — use `a + b`
- `array.flat()` — not available, manually iterate
- `string.match()` — use `regex.search()` or `regex.matches()` with `use regex`
- `regex.match()` — use `regex.search()` (partial match, returns match or null)
- `regex.test()` — use `regex.matches()` (full string match, returns bool)
- `time.format()` — use `time.format_timestamp()` (the function name is `format_timestamp`, NOT `format`)
- `env.set()` — use `env.set_var(name, value)` (the function name is `set_var`, NOT `set`)
- `dict.update()` — use `dict.merge(other)` or `dict.put(key, val)` individually
- `dict.clear()` — not available, reassign to empty dict: `my_dict = {}`
- `process.exit()` when shell is disabled in govern.json — use `return` at end of main{} instead

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
`|>` does NOT support a `_` placeholder for non-first-argument positions.
`data |> string.split(_, " ")` is INVALID — create a wrapper function:
```naab
fn split_spaces(s) { return string.split(s, " ") }
let words = data |> split_spaces
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
13. Arithmetic with strings is STRICT — `"5" + 3` works (string concat), `"5" * 3` errors
14. `array.find` takes a PREDICATE function, not a value
15. `||` ALWAYS returns boolean (true/false), NEVER the operand value.
    `null || "fallback"` returns `true` (NOT "fallback"!).
    NAAb's `||` is NOT like JavaScript's `||` operator.
    For null coalesce, use the `??` operator: `x ?? default_val`
16. `dict["key"]` THROWS on missing key — use `dict.get("key")` or `dict.get("key", default)` for safe access
17. Struct instantiation requires `new`: `let p = new Point { x: 1, y: 2 }` — without `new` you get a parse error.
    Conversely, `new { }` WITHOUT a struct name is also a parse error — plain dicts do NOT use `new`.
    WRONG: `new { "key": val }`   RIGHT: `{ "key": val }`
    `new` is ONLY for struct instantiation: `new StructName { field: value }`.
18. Python polyglot: Do NOT use `return` — causes `SyntaxError: 'return' outside function`
19. Value semantics: modifying a nested dict/array requires re-assigning to parent (see Value Semantics section)
20. The `..` range operator can collide with `".."` string literals — use intermediate variables: `let dots = ".."; path.contains(dots)`
21. `try` works as both a statement and an expression.
    `let x = try { compute() } catch (e) { default_val }` — returns the value from
    the successful branch or the catch branch. Both branches must be expressions.
    Use expression form for inline fallback patterns; use statement form for side effects.
22. `throw` works as an expression — it CAN appear in match arms and `let` assignments.
    `_ => throw "invalid value"` in a match arm is valid.
    Throw expressions diverge (never return), so the type system treats them as compatible
    with any branch type.
23. `string.match()` does NOT exist — use `regex.search(text, pattern)` for partial match,
    `regex.matches(text, pattern)` for full match, `regex.find_all(text, pattern)` for all matches.
    Requires `use regex`.
24. `-> JSON` behaves differently per language:
    **Python**: bare expressions (e.g. `json.dumps(data)`) as the last line are auto-wrapped
    in `print()`. Both `json.dumps(data)` and `print(json.dumps(data))` work.
    **JavaScript**: `-> JSON` is fragile with JS. Prefer dropping `-> JSON` and using
    `json.parse()` on the raw JS result instead (see JSON Sovereign Pipe section above).
    If using `-> JSON` with JS, put `JSON.stringify(result)` as the last expression — no `console.log`.
    **Python double-encoding trap**: do NOT call `json.dumps()` inside `<<python -> JSON` —
    the `-> JSON` pipe already converts the output to a NAAb dict. `json.dumps()` inside
    `-> JSON` causes double-encoding (the output becomes a JSON-encoded JSON string).
25. `and`/`or`/`not` are NOT boolean operators in NAAb — use `&&`/`||`/`!`
    `if x > 0 and y > 0` -> ERROR. Use: `if x > 0 && y > 0`
    `if not done` -> ERROR. Use: `if !done`
    Do NOT use `== false` as a workaround — `!expr` is the correct negation
26. `config` is a reserved keyword — do NOT use it as a variable name, import alias, or parameter name
27. Enum values from imported modules use 3-level dot access: `module_alias.EnumName.Variant`
    Example: `import "types.naab" as types` then `let c = types.Color.Red`
28. VM is the default execution engine; use `--tree-walk` for the legacy AST interpreter
29. `--agent-id <name>` CLI flag enables multi-agent governance role enforcement via `agents` (or legacy `agent_roles`) in govern.json
30. `--governance-dashboard` outputs a governance summary to stderr (checks passed/warned/blocked)
31. `agents` in govern.json defines per-agent permissions AND LLM configuration (model, max_tokens, system_prompt, tools, max_turns, max_total_tokens, provider). Supports `"provider": "anthropic"` (default) and `"provider": "gemini"`. Legacy `agent_roles` key is still accepted (permissions only, no LLM config).
32. `telemetry` in govern.json enables JSONL telemetry output for agent execution tracking (`"enabled": true, "output_file": "telemetry.jsonl"`)
33. `use agent` stdlib — governed LLM conversations. Responses are scanned by `checkSecrets()` (HARD) and `checkPii()` (configurable). `tool_use` responses are blocked. Turn/token limits use server-side tracking (immune to handle mutation).
34. `scoring` in govern.json enables cumulative risk scoring — advisory findings accumulate weighted scores. Cross `red_threshold` = block (exit 2 via quality gate). Cross `yellow_threshold` = warn once. `default_weight` sets the per-finding weight; `rule_weights` overrides per rule. Deterministic, monotonic, bounded (saturates at 100K), thread-safe (reads and writes protected by results_mutex_), with integrity verification at report time. Pass 2 findings (post-execution audit) are excluded from scoring — they use direct check_results_ push, not enforce(). In audit mode, scoring gate logs "WOULD block" but exits 0. If `cumulative_risk_score` is used as a quality gate condition AND `scoring.red_threshold` is set, the quality gate condition takes precedence (fires first).
35. Cross-module struct instantiation: `new module.StructName { field: value }`.
    Structs defined in imported files are accessible via the module alias.
    Example: `import "types.naab" as types` then `let p = new types.Point { x: 1, y: 2 }`
36. Match arm block bodies are parsed as **dict literals** — `1 => { var = expr }` fails with
    "Expected ':' after dict key" because `{` opens a dict, not a block.
    Use expression arms only — each arm must be a single expression, not a statement block.
    For multi-step logic in a match arm, extract a helper function or restructure as if/else:
    WRONG:
      `match status { "ok" => { let x = compute(); x * 2 } }`  // ERROR: { parsed as dict
    RIGHT (helper function):
      `fn handle_ok() { let x = compute(); return x * 2 }`
      `match status { "ok" => handle_ok() }`
    RIGHT (if/else instead of match):
      `if status == "ok" { let x = compute(); x * 2 } else { 0 }`
37. `json.stringify()` on NAAb structs may produce non-standard output (unquoted keys).
    For reliable JSON serialization of structs, convert to a dict first or use Python polyglot:
    ```naab
    // Option A: manual dict conversion
    let d = { "id": entry.id, "name": entry.name }
    let s = json.stringify(d)

    // Option B: Python polyglot
    let data = { "id": entry.id, "name": entry.name }
    let s = <<python[data]
    import json
    json.dumps(data)
    >>
    ```
    When using `-> JSON` with Python: `json.dumps()` as the last expression auto-wraps
    in `print()`. But if you then pass the result to `json.parse()`, you get double-parsing.
    Use `-> JSON` only when you want the result parsed into a NAAb dict, not when you need
    a JSON string for file output.
38. Struct field names in `new` expressions accept both identifiers (`x: 1`) and
    quoted strings (`"x": 1`). Both work. If a field name collides with a keyword
    (like `method`, `class`, `type`), use the quoted form: `new Request { "method": "GET" }`.
39. **Polyglot `>>` closer must be at column 0** — no leading whitespace, ever.
    When writing polyglot blocks inside functions, do NOT indent `>>` to match the function body.
    WRONG: `    >>` (indented)  RIGHT: `>>` (column 0).
    The Python/JS code inside the block also starts at column 0.
    This is the #1 most common polyglot mistake — causes parse errors or silent failures.
40. **JavaScript polyglot uses QuickJS (embedded), NOT Node.js.** Multi-line JS code is
    wrapped in an IIFE `(function() { ...; return (lastExpr); })()` for return capture.
    This means: (a) keep the last line a simple expression or variable, not an arrow function
    or complex statement — multi-line expressions like `JSON.stringify({...})` spanning several
    lines must be assigned to a variable first (see gotcha #45);
    (b) `console.log` is captured but not returned as the value;
    (c) no Node.js APIs (require, process, Buffer, etc.); (d) ES2020 syntax only.
    **When a project uses both Python and JavaScript polyglot, prefer Python for any block
    that needs `-> JSON` or complex data transformation.** Use JS for simpler operations
    like JSON manipulation, array sorting, or string processing where the last expression
    is a straightforward value.
41. **Always read data files before writing code that parses them.** JSON files often
    have wrapper objects (e.g., `{"alerts": [...]}` not `[{...}, ...]`). CSV files may
    have headers that need DictReader. Read the first few lines of each data file
    before writing loader/parser code. Do NOT assume the structure from the filename.
42. **`0..len(arr)` is correct for iterating all elements.** The `..` operator is exclusive
    (like Python's `range()`), so `0..len(arr)` visits indices 0 through len-1. Do NOT write
    `0..len(arr) - 1` — that skips the last element. Common mistake from Python habits where
    `range(len(x)-1)` is used for "all but last". In NAAb, `0..len(arr) - 1` IS "all but last".
43. **`use` imports are file-scoped — they do NOT propagate across `import`.** If module A
    has `use agent` and module B imports A via `import "a.naab" as a`, module B does NOT need
    `use agent` unless B directly calls agent functions. Only add `use X` for modules your
    file directly uses. Importing a module that uses X does not require you to also `use X`.
44. **Import paths are relative to the importing file, NOT the project root.** If both
    `main.naab` and `models.naab` are in `src/`, write `import "models.naab" as models` —
    NOT `import "src/models.naab"`. The `src/` prefix would look for `src/src/models.naab`.
45. **JavaScript polyglot: if the last expression is multi-line, assign it to a variable.**
    The JS executor wraps multi-line code in an IIFE and returns the last expression.
    When the last expression spans multiple lines (e.g. JSON.stringify with a multi-line
    object literal), the return capture can fail.
    WRONG — last expression spans multiple lines:
      `JSON.stringify({\n    winner: x,\n    margin: 0\n});`
    RIGHT — assign to variable, bare name on last line:
      `const result = JSON.stringify({ winner: x, margin: 0 });\nresult`
46. **No Python/Ruby-style ternary.** `x if cond else y` is NOT valid NAAb syntax.
    NAAb uses `if` as an expression (prefix form):
    WRONG: `let result = score if score > 0 else 0.0`
    RIGHT: `let result = if score > 0 { score } else { 0.0 }`
    The `if cond { a } else { b }` form IS an expression — assign it directly.
47. **JavaScript polyglot: do NOT redeclare bound variable names inside the block.**
    Variables listed in `<<javascript[x, y, z]` are injected as `const` into the IIFE.
    Redeclaring them with `const` or `let` causes "invalid redefinition of lexical identifier".
    WRONG: `<<javascript[margin]  const margin = margin * 2; margin >>`
    RIGHT: `<<javascript[margin]  const m = margin * 2; m >>`
    Use different names for any local variables that transform the bound inputs.
48. **`let` declarations cannot appear inside if-expressions.**
    If-expressions are values — their bodies must be pure expressions, not statements.
    WRONG: `let x = if cond { let tmp = compute(); tmp * 2 } else { 0 }`
    RIGHT: extract the intermediate computation before the if:
      let tmp = if cond { compute() } else { 0 }
      let x = tmp * 2
    Or restructure as an if-statement with a mutable variable:
      let x = 0
      if cond { x = compute() * 2 }
49. **Do not sanitize content that has already been hashed, or hash content before sanitizing.**
    Sanitizers change content (e.g. replacing `<>` with HTML entities). If you hash raw content
    then sanitize the same content, the stored hash will not match the sanitized version.
    Pattern for audit trails and hash chains:
      1. Assemble the raw content string
      2. Compute the hash on raw content: `let h = crypto.sha256(raw_content)`
      3. Sanitize only at the file.write() sink: `file.write(path, sanitize_string(raw_content))`
    Do NOT sanitize inside functions that build hash chains — sanitize is a presentation concern,
    not a data integrity concern.

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
`add_*`, `reset_*`, `init_*`, `validate_*`, `convert_*`, `clamp`, `distance`,
`manhattan`, `in_bounds`, `direction` have LOW threshold (score >= 3).

Functions named `simulate_*`, `compute_*`, `calculate_*`, `process_*`, `analyze_*`
need substantial logic (score >= 20-25, must have loops or conditionals).

Functions shorter than `min_lines_for_check` lines skip the floor entirely.

Do NOT pad functions with `for i in 0..1 { }` or `for i in 0..2 { }` loops to pass
complexity checks. Instead: add real logic — input validation, edge case handling,
error recovery with try/catch.

**Anti-gaming rules (the scanner will detect these):**
- Do NOT use `let zero = 0` or `let one = 1` to avoid magic number warnings.
  Magic number checks automatically skip test functions — use literal numbers freely in tests.
- Do NOT add comments mentioning "complexity", "score", "increases", or "adds" to justify
  artificial code padding. Write natural code — if a function needs complexity, add real logic
  (input validation, edge case handling, error recovery).
- Do NOT add try/catch blocks around code that cannot throw. Only wrap genuinely risky
  operations (file I/O, parsing, external calls).
- Do NOT import modules you don't use. Only `use array`, `use json`, etc. if you actually
  call functions from that module. The scanner detects unused imports.
- The complexity floor ONLY applies to functions with these prefixes:
  `simulate_*`, `compute_*`, `calculate_*`, `process_*`, `analyze_*`.
  Regular functions (`tokenize`, `parse_factor`, `format_report`) have NO complexity floor.
  Do NOT artificially inflate complexity of non-prefixed functions.

## Contract Patterns

When govern.json defines function contracts, your return value MUST match.
Common patterns:

**return_type: dict + return_keys:**
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

**Parameter types — arrays vs dicts:**
`return_keys` describes what the FUNCTION RETURNS, not what it takes.
For input parameters, follow the module spec:
- Parameters named `history`, `events`, `findings`, `signals`, `items` → pass an **array** `[...]`
- Parameters named `summary`, `context`, `config`, `options`, `metadata` → pass a **dict** `{...}`
- When a parameter accepts a list of homogeneous items, it expects an array even if items are dicts.
  WRONG: `score_signal(signal, {"entries": history_list})`
  RIGHT: `score_signal(signal, history_list)` — pass the array directly

**return_one_of:**
```naab
// Contract: return_one_of: ["attack", "move", "flee", "idle"]
// Use match or if/else to ensure only valid values returned
if hp_ratio < threshold { return "flee" }
else if enemy_in_range { return "attack" }
else if enemy_nearby { return "move" }
return "idle"
```

**return_min: 0 (non-negative):**
```naab
// Contract: return_min: 0
let result = base_damage - defense
return int(math.max(0, result))  // Clamp to 0
```

---

## Code Quality Scanner

NAAb has a built-in code quality scanner (`naab --scan`) that checks 139 patterns
across 6 categories and 6 language-specific modules.

### Auto-Run (Runtime)
When your govern.json has a `"scanner"` section, the scanner runs automatically after
every `naab` execution — no flags needed. It reports issues to stderr:
```
[scanner] 4 issues (1 hard, 2 soft, 1 advisory) in my_file.naab
[scanner] HARD violations:
  X Line 8: security.hardcoded_credentials — Hardcoded credential detected
    Fix: Use environment variables or secrets manager
```

### CLI Mode
```bash
naab --scan <path> [language|auto]    # Standalone scan
naab --scan src/ auto                 # Scan directory, auto-detect languages
naab --scan app.py python             # Scan single file
```

### Check Categories (127 checks total)
| Category | Checks | Key Rules |
|----------|--------|-----------|
| redundancy | 16 | obvious_comments, over_abstraction, apologetic_comments, placeholder_code, missing_imports |
| code_quality | 15 | empty_catch, magic_numbers, dead_code_after_return, god_functions, deep_nesting |
| complexity | 8 | cyclomatic_complexity, cognitive_complexity, file_length |
| style | 10 | inconsistent_naming, debug_leftovers, commented_out_code, long_lines |
| security | 10 | hardcoded_credentials, sql_string_concat, shell_injection, path_traversal |
| lang_naab | 14 | value_semantics_bug, top_level_let, arrow_lambda, python_return_in_block, json_double_encode |
| lang_python | 14 | bare_except, mutable_default_arg, star_import, open_without_with |
| lang_javascript | 12 | loose_equality, var_declaration, eval_usage, prototype_pollution |
| lang_cpp | 12 | raw_new_delete, using_namespace_std, c_style_cast, goto_usage |
| lang_go | 9 | ignored_error, panic_usage, empty_interface, error_capitalization |
| lang_rust | 10 | unsafe_block, todo_macro, string_push_in_loop, complex_lifetime |

### Enforcement Levels
- **hard** — Critical issues (security, dead code). Severity: critical
- **soft** — Important quality issues. Severity: high/medium
- **advisory** — Suggestions for improvement. Severity: low

### Configuration (govern.json)
```json
{
  "scanner": {
    "version": "1.0",
    "mode": "enforce",
    "code_quality": {
      "empty_catch":       { "enabled": true, "level": "hard" },
      "god_functions":     { "enabled": true, "level": "soft", "max_lines": 80 },
      "deep_nesting":      { "enabled": true, "level": "soft", "max_depth": 4 }
    },
    "security": {
      "hardcoded_credentials": { "enabled": true, "level": "hard" }
    }
  }
}
```
See `govern-template.json` for all 127 checks with their options.

### Output
- Text report: `quality-report.txt` (saved automatically)
- JSON report: `quality-report.json` (machine-readable)
- SARIF report: optional (`"save_sarif": true`)

---

## Project-Specific Template
Copy everything above into your project's CLAUDE.md, then add sections like these below:

```markdown
## Project: [Your Project Name]
[Brief description of what this project does]

## Governance (govern.json)
- Allowed languages: [list which languages your govern.json permits]
- Blocked languages: [list blocked languages]
- Variable binding: [REQUIRED/optional]
- Security restrictions: [banned functions, blocked paths, network policy]
- Polyglot block limit: [number, if set]

## Module Specifications
[Describe each .naab file, its exports, and expected behavior]

## Data Files
[Describe input/output files and their formats]

## What NOT to Do (project-specific)
- Do NOT write standalone .py, .js, .go files
- Do NOT hardcode results, use placeholders, or stub functions
- Do NOT leave TODO/FIXME/STUB comments — governance BLOCKS these patterns
- Do NOT use comments that admit code is incomplete: "simplified version", "for demonstration",
  "would normally do X", "in a real system", "basic implementation", "mock data", "placeholder"
- Do NOT write empty/trivial functions (pass-only, return True, return [])
- Do NOT swallow errors silently (empty catch blocks, except: pass)
- Do NOT pad functions with `for i in 0..1 { }` or `for i in 0..2 { }` loops to pass
  complexity checks. Instead: add real logic — input validation, edge case handling,
  error recovery with try/catch. The governance engine rewards: loops over real data,
  conditionals, try/catch, function calls, array operations (map_fn, filter_fn, reduce_fn).
- Do NOT add hedging comments like "for now", "simplified", "basic implementation",
  "in a real system". If the code works, it IS the real implementation. Delete the
  qualifying comment — governance blocks these patterns even when the code is correct.
- The governance engine detects 200+ stub/evasion patterns and will BLOCK execution
- Do NOT modify govern.json — it is signed and modifications will be blocked.
  If you need different settings, ask the user to update and re-sign governance.
- [Add project-specific restrictions]
```
