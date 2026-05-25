# NAAb Language Reference for LLMs

This is the universal NAAb language reference. Copy this into your project's CLAUDE.md
and add project-specific sections (governance rules, module specs, data paths) below.

## Scope
Do NOT read or reference files outside this project directory. Do NOT look at other
tests/gorilla/ directories for reference implementations — build your solution independently.

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

### Dict Literals
```naab
let d = {"name": "Alice", "age": 30}   // Quoted keys (always valid)
let d = {name: "Alice", age: 30}       // Bare identifier keys (also valid)
let d = {method: "GET", class: "A"}    // Keywords work as keys too
```
Both forms produce identical dicts. Keys are always stored as strings.
For computed keys, use parenthesized expressions: `let k = "x"; let d = {(k): val}`.

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
Auto-dedent preserves relative indentation (critical for Python nested blocks).
Column 0 also works and is the traditional style:
```naab
fn compute() {
    let result = <<python
x = 42
x * 2
>>
    return result
}
```
**Do NOT mix tabs and spaces in polyglot indentation** — auto-dedent compares exact
whitespace characters, not column widths. Use spaces consistently.

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

### Taint pattern for multi-file projects
When your sanitizer is in a separate module (e.g., `validators.naab`), you must:
1. Import the validators module: `import "validators.naab" as validators`
2. Call the sanitizer before EVERY `file.write`/`file.append`:
```naab
import "validators.naab" as validators

fn write_report(data, path) {
    let content = json.stringify(data)
    let clean = validators.sanitize_string(content)
    file.write(path, clean)   // Taint cleared by sanitize_string
}
```
Every path from polyglot output -> file.write MUST pass through a sanitizer.
String concatenation with tainted data re-taints the result.

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
replace, replace_first, split, groups, escape, is_valid
find_groups — returns **2D array**: `[[full_match, group1, group2, ...], ...]`
  Each inner array is one match. First capture group: `result[0][1]` (NOT `result[1]`).
  No match: returns `[]`. Guard with: `if len(groups) > 0 { ... }`
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

## Import Rules

**Every module except `debug` requires `use <module>` before calling its functions.**
debug is auto-imported (prelude) — do NOT write `use debug`.

**Only import what you actually call.** The scanner flags both unused and missing imports.
Before writing `use X`, verify your file calls `X.something()`. Before removing `use X`,
verify no function in the file calls `X.something()`.

**IMPORTANT: Write imports LAST, not first.** Write your function bodies first,
then add only the `use` statements for modules you actually called. The scanner
will flag unused imports as violations. Common over-imports to avoid:
- `use time` — only needed if you call `time.now()`, `time.sleep()`, etc.
- `use array` — NOT needed for `.push()`, `.length()`, `len()` (these are built-in dot-notation)
- `use regex` — only needed if you call `regex.search()`, `regex.find_all()`, etc.
- `import "models.naab" as models` — only needed if you use `new models.StructName`

| Module | Common functions | Notes |
|--------|-----------------|-------|
| array | push, pop, length, map, filter, reduce, sort, contains | Dot-notation works: `arr.push(x)` |
| string | upper, lower, trim, split, replace, contains, length | Dot-notation works: `s.upper()` |
| math | abs, floor, ceil, round, min, max, pow, sqrt, random | `math.PI`, `math.E` |
| json | parse, stringify | |
| file | read, write, exists, list_dir, append | Taint sink: sanitize before write |
| time | now, now_millis, sleep, format_timestamp | NOT `time.format` |
| csv | parse, stringify | |
| regex | search, matches, find, find_all, replace, split | NOT `regex.match` or `regex.test` |
| env | get, get_args, set_var, list | NOT `env.set` |
| io | write, read_line, write_error | |
| crypto | sha256, sha512, hash, random_bytes, base64_encode | |
| log | debug, warn, log, set_level | |
| uuid | v4, v5, is_valid | |
| validate | email, url, ip, int_range, not_empty | |
| process | run, exit, kill, getpid | Blocked when shell disabled |
| path | join, dirname, basename, extension | |
| http | get, post, put, delete, call | Blocked when network disabled |
| agent | create, send, run, batch, fan_out, pipeline | `use agent` required |
| dict | get, has, put, keys, values, merge | Built-in, no `use` needed |
| debug | type, inspect, keys, values, log | Auto-imported, do NOT `use debug` |

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
9. Top-level `const`/`let` causes a parse error — variables and constants MUST be declared
   inside `main {}` or a function, never at file scope.
10. Polyglot blocks inside functions + return: works, but the function must have return AFTER the block
11. Dict iteration: `for key in my_dict { }` works
12. Null comparison: use `x == null` not `x === null` (no triple equals)
13. String concatenation with `+` is permissive (auto-converts numbers)
14. Arithmetic with strings is STRICT — `"5" + 3` works (string concat), `"5" * 3` errors
15. `array.find` takes a PREDICATE function, not a value
16. `||` ALWAYS returns boolean (true/false), NEVER the operand value.
    `null || "fallback"` returns `true` (NOT "fallback"!).
    NAAb's `||` is NOT like JavaScript's `||` operator.
    For null coalesce, use the `??` operator: `x ?? default_val`
17. `dict["key"]` THROWS on missing key — use `dict.get("key")` or `dict.get("key", default)` for safe access
18. Struct instantiation requires `new`: `let p = new Point { x: 1, y: 2 }` — without `new` you get a parse error.
    Plain dicts do NOT use `new`: `{name: "Alice"}` or `{"name": "Alice"}` — both work.
    WRONG: `new { "key": val }`   RIGHT: `{ "key": val }`
    `new` is ONLY for struct instantiation: `new StructName { field: value }`.
19. Python polyglot: Do NOT use `return` — causes `SyntaxError: 'return' outside function`
20. Value semantics: modifying a nested dict/array requires re-assigning to parent (see Value Semantics section)
21. The `..` range operator can collide with `".."` string literals — use intermediate variables: `let dots = ".."; path.contains(dots)`
22. `try` works as both a statement and an expression.
    `let x = try { compute() } catch (e) { default_val }` — returns the value from
    the successful branch or the catch branch. Both branches must be SINGLE EXPRESSIONS.
    WRONG (multi-statement catch in expression form):
      `let x = try { compute() } catch (e) { print(e); 0 }`  // ERROR: two statements
    RIGHT (expression form — single expression per branch):
      `let x = try { compute() } catch (e) { 0 }`
    RIGHT (statement form — use when you need multiple statements):
      `let result = 0`
      `try { result = compute() } catch (e) { print(e); result = fallback() }`
    The statement form has no restriction on branch body complexity.
23. `throw` works as an expression — it CAN appear in match arms and `let` assignments.
    `_ => throw "invalid value"` in a match arm is valid.
    Throw expressions diverge (never return), so the type system treats them as compatible
    with any branch type.
24. `string.match()` does NOT exist — use `regex.search(text, pattern)` for partial match,
    `regex.matches(text, pattern)` for full match, `regex.find_all(text, pattern)` for all matches.
    Requires `use regex`.
25. `-> JSON` behaves differently per language:
    **Python**: bare expressions (e.g. `json.dumps(data)`) as the last line are auto-wrapped
    in `print()`. Both `json.dumps(data)` and `print(json.dumps(data))` work.
    **JavaScript**: `-> JSON` is fragile with JS. Prefer dropping `-> JSON` and using
    `json.parse()` on the raw JS result instead (see JSON Sovereign Pipe section above).
    If using `-> JSON` with JS, put `JSON.stringify(result)` as the last expression — no `console.log`.
    **Python double-encoding trap**: do NOT call `json.dumps()` inside `<<python -> JSON` —
    the `-> JSON` pipe already converts the output to a NAAb dict. `json.dumps()` inside
    `-> JSON` causes double-encoding (the output becomes a JSON-encoded JSON string).
26. `and`/`or`/`not` are NOT boolean operators in NAAb — use `&&`/`||`/`!`
    `if x > 0 and y > 0` -> ERROR. Use: `if x > 0 && y > 0`
    `if not done` -> ERROR. Use: `if !done`
    Do NOT use `== false` as a workaround — `!expr` is the correct negation
27. `config` is a reserved keyword — do NOT use it as a variable name, import alias, or parameter name
28. Enum values from imported modules use 3-level dot access: `module_alias.EnumName.Variant`
    Example: `import "types.naab" as types` then `let c = types.Color.Red`
29. VM is the default execution engine; use `--tree-walk` for the legacy AST interpreter
30. `--agent-id <name>` CLI flag enables multi-agent governance role enforcement via `agents` (or legacy `agent_roles`) in govern.json
31. `--governance-dashboard` outputs a governance summary to stderr (checks passed/warned/blocked)
32. `agents` in govern.json defines per-agent permissions AND LLM configuration (model, max_tokens, system_prompt, tools, max_turns, max_total_tokens, provider). Supports `"provider": "anthropic"` (default) and `"provider": "gemini"`. Legacy `agent_roles` key is still accepted (permissions only, no LLM config).
33. `telemetry` in govern.json enables JSONL telemetry output for agent execution tracking (`"enabled": true, "output_file": "telemetry.jsonl"`)
34. `use agent` stdlib — governed LLM conversations. Responses are scanned by `checkSecrets()` (HARD) and `checkPii()` (configurable). `tool_use` responses are blocked. Turn/token limits use server-side tracking (immune to handle mutation).
35. `scoring` in govern.json enables cumulative risk scoring — advisory findings accumulate weighted scores. Cross `red_threshold` = block (exit 2 via quality gate). Cross `yellow_threshold` = warn once. `default_weight` sets the per-finding weight; `rule_weights` overrides per rule. Deterministic, monotonic, bounded (saturates at 100K), thread-safe (reads and writes protected by results_mutex_), with integrity verification at report time. Pass 2 findings (post-execution audit) are excluded from scoring — they use direct check_results_ push, not enforce(). In audit mode, scoring gate logs "WOULD block" but exits 0. If `cumulative_risk_score` is used as a quality gate condition AND `scoring.red_threshold` is set, the quality gate condition takes precedence (fires first).
36. Cross-module struct instantiation: `new module.StructName { field: value }`.
    Structs defined in imported files are accessible via the module alias.
    Example: `import "types.naab" as types` then `let p = new types.Point { x: 1, y: 2 }`
37. Match arm block bodies are parsed as **dict literals** — `1 => { var = expr }` fails with
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
    NOTE: Single-expression arms always work fine — the dict-literal issue only
    affects multi-statement bodies with `{ ... }`. Simple value mapping is ideal for match:
      `let name = match sev { Severity.Critical => "Critical", Severity.High => "High", _ => "Unknown" }`
38. `json.stringify()` on NAAb structs may produce non-standard output (unquoted keys).
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
39. Struct field names in `new` expressions accept both identifiers (`x: 1`) and
    quoted strings (`"x": 1`). Both work. If a field name collides with a keyword
    (like `method`, `class`, `type`), use the quoted form: `new Request { "method": "GET" }`.
40. **Polyglot blocks support auto-dedent.** Content and `>>` can be indented to match
    surrounding NAAb code. The runtime strips the common leading whitespace prefix.
    Both styles work:
    Style A (indented): content and `>>` indented to match function body
    Style B (column 0): content and `>>` at column 0 (traditional)
    **Rule:** Do NOT mix tabs and spaces within a polyglot block's indentation.
    Use spaces consistently. The `>>` closer must still be on its OWN line.
41. **JavaScript polyglot uses QuickJS (embedded), NOT Node.js.** Multi-line JS code is
    wrapped in an IIFE `(function() { ...; return (lastExpr); })()` for return capture.
    This means: (a) keep the last line a simple expression or variable, not an arrow function
    or complex statement — multi-line expressions like `JSON.stringify({...})` spanning several
    lines must be assigned to a variable first (see gotcha #45);
    (b) `console.log` is captured but not returned as the value;
    (c) no Node.js APIs (require, process, Buffer, etc.); (d) ES2020 syntax only;
    (e) `for...of` works on arrays but may fail on other iterables or when combined with
    destructuring. For maximum reliability, use C-style loops:
    `for (let i = 0; i < arr.length; i++) { const item = arr[i]; ... }`
    **When a project uses both Python and JavaScript polyglot, prefer Python for any block
    that needs `-> JSON` or complex data transformation.** Use JS for simpler operations
    like JSON manipulation, array sorting, or string processing where the last expression
    is a straightforward value.
42. **Always read data files before writing code that parses them.** JSON files often
    have wrapper objects (e.g., `{"alerts": [...]}` not `[{...}, ...]`). CSV files may
    have headers that need DictReader. Read the first few lines of each data file
    before writing loader/parser code. Do NOT assume the structure from the filename.
43. **`0..len(arr)` is correct for iterating all elements.** The `..` operator is exclusive
    (like Python's `range()`), so `0..len(arr)` visits indices 0 through len-1. Do NOT write
    `0..len(arr) - 1` — that skips the last element. Common mistake from Python habits where
    `range(len(x)-1)` is used for "all but last". In NAAb, `0..len(arr) - 1` IS "all but last".
44. **`use` imports are file-scoped — they do NOT propagate across `import`.** If module A
    has `use agent` and module B imports A via `import "a.naab" as a`, module B does NOT need
    `use agent` unless B directly calls agent functions. Only add `use X` for modules your
    file directly uses. Importing a module that uses X does not require you to also `use X`.
45. **Import paths are relative to the importing file, NOT the project root.** If both
    `main.naab` and `models.naab` are in `src/`, write `import "models.naab" as models` —
    NOT `import "src/models.naab"`. The `src/` prefix would look for `src/src/models.naab`.
46. **JavaScript polyglot: if the last expression is multi-line, assign it to a variable.**
    The JS executor wraps multi-line code in an IIFE and returns the last expression.
    When the last expression spans multiple lines (e.g. JSON.stringify with a multi-line
    object literal), the return capture can fail.
    WRONG — last expression spans multiple lines:
      `JSON.stringify({\n    winner: x,\n    margin: 0\n});`
    RIGHT — assign to variable, bare name on last line:
      `const result = JSON.stringify({ winner: x, margin: 0 });\nresult`
47. **No ternary operators.** Neither C-style `cond ? a : b` nor Python-style `x if cond else y`
    is valid NAAb syntax. NAAb uses `if` as an expression (prefix form):
    WRONG: `let result = score > 0 ? score : 0.0`           // C/JS ternary
    WRONG: `let result = score if score > 0 else 0.0`       // Python ternary
    RIGHT: `let result = if score > 0 { score } else { 0.0 }`
    The `if cond { a } else { b }` form IS an expression — assign it directly.
48. **JavaScript polyglot: do NOT redeclare bound variable names inside the block.**
    Variables listed in `<<javascript[x, y, z]` are injected as `const` into the IIFE.
    Redeclaring them with `const` or `let` causes "invalid redefinition of lexical identifier".
    WRONG:
      `<<javascript[margin]`
      `const margin = margin * 2;`
      `margin`
      `>>`
    RIGHT:
      `<<javascript[margin]`
      `const m = margin * 2;`
      `m`
      `>>`
    Use different names for any local variables that transform the bound inputs.
49. **`let` declarations cannot appear inside if-expressions.**
    If-expressions are values — their bodies must be pure expressions, not statements.
    WRONG: `let x = if cond { let tmp = compute(); tmp * 2 } else { 0 }`
    RIGHT: extract the intermediate computation before the if:
      let tmp = if cond { compute() } else { 0 }
      let x = tmp * 2
    Or restructure as an if-statement with a mutable variable:
      let x = 0
      if cond { x = compute() * 2 }
50. **Do not sanitize content that has already been hashed, or hash content before sanitizing.**
    Sanitizers change content (e.g. replacing `<>` with HTML entities). If you hash raw content
    then sanitize the same content, the stored hash will not match the sanitized version.
    Pattern for audit trails and hash chains:
      1. Assemble the raw content string
      2. Compute the hash on raw content: `let h = crypto.sha256(raw_content)`
      3. Sanitize only at the file.write() sink: `file.write(path, sanitize_string(raw_content))`
    Do NOT sanitize inside functions that build hash chains — sanitize is a presentation concern,
    not a data integrity concern.
51. **After editing files with polyglot blocks, verify indentation consistency.**
    Edit tools may re-indent polyglot content. Auto-dedent handles uniform indentation,
    but if edits produce MIXED indentation (some lines with 4 spaces, others with 8),
    the relative indentation changes and Python code may break. After partial edits,
    verify that all polyglot content lines use the same indentation base.
    Do NOT mix tabs and spaces — auto-dedent compares exact characters, not column widths.
52. **Python polyglot: `try/except` swallows the return value.** The last expression of
    the block must be AFTER the try/except, not inside it.
    WRONG (returns null — try/except is a statement, not an expression):
      `<<python[data]`
      `try:`
      `    result = json.dumps(data)`
      `except:`
      `    result = "{}"`
      `>>`
    RIGHT (bare variable as last line, AFTER try/except):
      `<<python[data]`
      `result = "{}"`
      `try:`
      `    result = json.dumps(data)`
      `except:`
      `    pass`
      `result`
      `>>`
    The same applies to `if/else` — put the result variable on the LAST LINE of the block,
    not inside a branch. Initialize it before the control flow, update inside, reference after.
53. **Function names starting with `sanitize_`, `validate_`, `check_`, or `verify_` trigger
    governance inspection.** The runtime verifies these functions contain real validation logic
    (regex, rejection path, type check, bounds check, or allowlist). If your function doesn't
    actually validate/sanitize, use a different name prefix:
    WRONG: `fn check_command(cmd)` — triggers cosmetic sanitizer detection if it just routes
    RIGHT: `fn handle_command(cmd)` or `fn route_command(cmd)` or `fn run_command(cmd)`
    Only use `check_*/validate_*/sanitize_*/verify_*` for functions that genuinely validate input.
    NOTE: These prefixes also get a LOW complexity threshold (score >= 3), so they don't need
    complex logic to pass the complexity floor — but they DO need real validation logic to
    pass the cosmetic sanitizer check.
54. **`math.min()`/`math.max()` return float when either argument is float.**
    When both arguments are integers, they return int. When either is float, the
    result is float. If you need an integer result from mixed-type args, wrap in
    `int()`: `int(math.min(float_val, 10.0))`
55. **Python polyglot returning a dict/list MUST use `-> JSON` or `json.dumps()`.**
    Without `-> JSON`, Python's repr output (e.g., `{'key': 'value'}`) is returned
    as a string, not a NAAb dict. This causes contract violations when the function
    is expected to return a dict.
    WRONG (returns string, not dict):
      `let result = <<python[data]`
      `{"key": "value", "count": len(data)}`
      `>>`
    RIGHT (-> JSON parses into NAAb dict):
      `let result = <<python[data] -> JSON`
      `import json`
      `json.dumps({"key": "value", "count": len(data)})`
      `>>`
    RIGHT (manual parse):
      `let raw = <<python[data]`
      `import json`
      `json.dumps({"key": "value", "count": len(data)})`
      `>>`
      `let result = json.parse(raw)`
56. **Dot-notation array/string methods work WITHOUT `use array`/`use string`.**
    `arr.push(x)`, `arr.length()`, `s.upper()`, `s.split(" ")` — all work without imports.
    You only need `use array` for module-form calls: `array.map(arr, fn)`, `array.filter_fn(...)`.
    Similarly, `use string` is only needed for `string.contains(s, sub)` module-form calls.
    **Do NOT add `use array` just because you use `.push()` or `len()` — it wastes an import.**
57. **`regex.find_groups()` returns a 2D array, not a flat array.**
    Each inner array is one match: `[full_match, group1, group2, ...]`.
    WRONG: `let ts = regex.find_groups(line, pat)[1]`  — this is the second match, not first group
    RIGHT:  `let ts = regex.find_groups(line, pat)[0][1]`  — first match, first capture group
    Empty outer array `[]` means no match. Always guard: `if len(groups) > 0 { ... }`
58. **On Termux, `/tmp` does not exist. Use relative paths or `env.get("TMPDIR")`.**
    WRONG: `file.write("/tmp/report.txt", content)`
    RIGHT:  `file.write("report.txt", content)`  — writes to current directory
    RIGHT:  `file.write(env.get("TMPDIR") + "/report.txt", content)`
59. **Integer division truncates.** `int / int` produces `int` in NAAb, NOT float.
    `12 / 8` evaluates to `1`, not `1.5`. For float division, cast one operand:
    WRONG: `let days = total_hours / 8`        // 12/8 = 1 (truncated)
    RIGHT: `let days = float(total_hours) / 8`  // 12.0/8 = 1.5
    RIGHT: `let days = total_hours / 8.0`       // 12/8.0 = 1.5
    This is the same as Python 2, C, Java, and Go integer division. If either operand
    is float, the result is float.

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

**return_keys_non_empty: true:**
When set in a contract, ALL `return_keys` values must be substantive —
not `""`, `[]`, `{}`, or `null`. Use this for functions where empty values
indicate a stub implementation rather than a legitimate empty result.

**must_contain:**
Literal string patterns that must appear in the function body.
Use for enforcing language construct requirements:
- `"|>"` — function must use pipeline operator
- `"match "` — function must use match expression (trailing space avoids matching `regex.matches`)
- `"try {"` — function must use try/catch

Unlike `must_call` (which matches function calls via `\bname\s*\(`), `must_contain`
does plain string search. Use `must_call` for function calls, `must_contain` for operators
and syntax patterns.

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

## Project: Compliance Rule Engine

A regulatory compliance engine that parses rules with complex condition trees,
evaluates entities against those rules using recursive evaluation, resolves rule
dependencies via topological sort, detects circular dependencies, computes risk
scores, generates compliance reports, and uses LLM agents for rule interpretation.

**This is a strict governance project.** Tests verify exact computed values. Governance
blocks sloppy code at hard level. All 18 function contracts are enforced.

## Governance Summary (govern.json)
- **Allowed languages:** python, shell
- **Blocked:** javascript, rust, cpp, csharp, go, nim, zig, julia, ruby, php
- **Variable binding: HARD** — every polyglot block must have `[var1, var2]` or `[]`
- **Network:** enabled (for agent API calls)
- **Filesystem:** read_write (for report output)
- **Shell:** enabled (blocked: rm, curl, wget, chmod, sudo)
- **Taint tracking:** enabled — polyglot_output and env.get are sources, file.write/append are sinks
- **Security:** no_secrets (hard), no_placeholders (hard), no_simulation_markers (hard)
- **18 function contracts** with return_keys enforcement
- **4-tier complexity floor** (test_/calculate_/evaluate_/create_ tiers)
- **Scanner HARD checks:** empty_catch, dead_conditional, value_semantics_bug, missing_null_check, dict_bracket_access, python_return_in_block, top_level_let
- **Agent config:** 3 Gemini agents (rule_interpreter, rule_interpreter_b1, rule_interpreter_b2) with GK1-GK3 keys
- **Cumulative scoring:** yellow at 15, red (block) at 30

## File Structure

All source files are in `src/`. Import paths use just the filename (same directory):
```
src/
├── main.naab          # Test orchestrator (120 tests, 12 suites)
├── models.naab        # Structs + enums + factory functions + name mappers
├── rules.naab         # Rule parsing + validation + condition trees
├── conditions.naab    # Condition evaluation (recursive tree walker)
├── entities.naab      # Entity registry + attribute management
├── evaluator.naab     # Rule evaluation engine + batch processing
├── dependencies.naab  # Dependency resolution + cycle detection
├── risk.naab          # Risk scoring + portfolio analysis
├── reports.naab       # Report generation + file output (taint sink)
├── remediation.naab   # Remediation planning + effort estimation
├── validators.naab    # Sanitizer + validation functions (taint clearing)
└── interpreter.naab   # Agent-based rule interpretation
```

**Import example (from main.naab):**
```naab
import "models.naab" as models
import "rules.naab" as rules
import "conditions.naab" as conditions
import "validators.naab" as validators
```

**Dependency graph:**
- models.naab: no imports
- validators.naab: imports models
- conditions.naab: imports models
- rules.naab: imports models
- entities.naab: imports models
- evaluator.naab: imports models, conditions, rules
- dependencies.naab: imports models
- risk.naab: imports models
- reports.naab: imports models, validators
- remediation.naab: imports models
- interpreter.naab: imports models
- main.naab: imports ALL modules

## Enum Definitions

```naab
export enum Severity { Critical, High, Medium, Low, Info }
export enum RuleStatus { Active, Draft, Deprecated, Suspended }
export enum ComplianceResult { Pass, Fail, Skip, Error }
export enum ConditionOp {
    Equals, NotEquals, GreaterThan, LessThan, GreaterOrEqual, LessOrEqual,
    Contains, Matches, And, Or, Not
}
export enum EntityType { Server, Database, Application, Network, User, Policy }
export enum RemediationPriority { Immediate, High, Medium, Low, Deferred }
```

**Usage from other modules:**
```naab
import "models.naab" as models
if sev == models.Severity.Critical { ... }
let op = models.ConditionOp.And
```

## Struct Definitions

```naab
export struct Rule {
    id: string
    name: string
    description: string
    severity: int
    status: int
    category: string
    conditions: array
    dependencies: array
    tags: array
    version: int
}

export struct Condition {
    op: int
    field: string
    value: string
    children: array
}

export struct Entity {
    id: string
    name: string
    entity_type: int
    attributes: dict
    tags: array
    owner: string
    created_at: int
}

export struct Finding {
    rule_id: string
    entity_id: string
    result: int
    severity: int
    message: string
    evidence: string
    timestamp: int
}

export struct RemediationStep {
    id: string
    finding_id: string
    action: string
    priority: int
    estimated_hours: int
    blocked_by: array
    completed: bool
}
```

## Module Specifications

### models.naab — Domain Models

**Exported Functions:**
- `create_rule(id, name, description, severity, category, conditions, dependencies, tags)` -> new Rule struct (status=Active, version=1)
- `create_condition(op, field, value, children)` -> dict (NOT struct — conditions are nested dict trees)
- `create_entity(id, name, entity_type, attributes, tags, owner)` -> new Entity struct (created_at=time.now())
- `create_finding(rule_id, entity_id, result, severity, message, evidence)` -> new Finding struct (timestamp=time.now())
- `severity_name(sev)` -> string — **MUST use match expression**
- `status_name(status)` -> string — **MUST use match expression**
- `result_name(result)` -> string — **MUST use match expression**
- `op_name(op)` -> string — **MUST use match expression** (11 operators)
- `entity_type_name(et)` -> string — **MUST use match expression**
- `priority_name(p)` -> string — **MUST use match expression**
- `validate_rule(rule)` -> dict: {valid, errors}
- `validate_entity(entity)` -> dict: {valid, errors}

### validators.naab — Sanitization & Validation
Requires: `use regex`, `use validate`
Imports: models.naab

- `sanitize_string(input)` -> string (strips `<` and `>`, preserves JSON chars, trims)
- `sanitize_log_line(line)` -> string (strips ANSI, then sanitize_string)
- `validate_rule_syntax(definition)` -> dict: {valid, errors}
- `validate_email_address(email)` -> bool
- `validate_condition_tree(conditions)` -> dict: {valid, errors, depth} — **RECURSIVE**

### conditions.naab — Condition Evaluation (RECURSIVE)
Requires: `use regex`
Imports: models.naab

- `evaluate_condition(condition, entity_attributes)` -> dict: {passed, evidence} — **RECURSIVE, uses match for op dispatch**
- `count_conditions(condition)` -> int — **RECURSIVE**
- `flatten_conditions(condition)` -> array — **RECURSIVE**
- `max_depth(condition)` -> int — **RECURSIVE**

### rules.naab — Rule Parsing
Requires: `use json`, `use regex`
Imports: models.naab

- `parse_rule_definition(text)` -> dict: {rule, valid, errors} — **uses try/catch expression**
- `parse_condition_tree(raw_conditions)` -> array — **RECURSIVE, uses match for op mapping**
- `filter_rules_by_category(rules, category)` -> array
- `filter_rules_by_severity(rules, min_severity)` -> array
- `filter_active_rules(rules)` -> array
- `get_rule_by_id(rules, id)` -> Rule or null
- `update_rule_status(rule, new_status)` -> dict
- `merge_rule_tags(rule, new_tags)` -> dict — **uses pipeline**

### entities.naab — Entity Registry
Requires: `use uuid`
Imports: models.naab

- `register_entity(registry, entity)` -> dict: {registry, success}
- `get_entity_by_id(registry, id)` -> Entity or null
- `get_entities_by_type(registry, entity_type)` -> array
- `update_entity_attribute(registry, entity_id, key, value)` -> dict: {registry, success} — **value semantics critical**
- `get_entity_attributes(entity)` -> dict
- `filter_entities_by_tag(registry, tag)` -> array
- `count_entities_by_type(registry)` -> dict — **uses pipeline**

### evaluator.naab — Rule Evaluation Engine
Requires: `use time`
Imports: models.naab, conditions.naab, rules.naab

- `evaluate_rule(rule, entity)` -> dict: {passed, rule_id, severity, message, evidence} — **uses match for result classification**
- `evaluate_entity(rules, entity)` -> dict: {entity_id, total_rules, passed, failed, findings} — **uses pipeline**
- `batch_evaluate(rules, entities)` -> dict: {results, total, passed_count, failed_count} — **Python polyglot -> JSON**
- `aggregate_findings(findings, group_by)` -> dict — **uses match for grouping key**
- `get_critical_findings(findings)` -> array

### dependencies.naab — Dependency Resolution
Imports: models.naab

- `build_dependency_graph(rules)` -> dict
- `resolve_rule_dependencies(rules)` -> dict: {order, cycles, unresolvable} — **Python polyglot -> JSON (Kahn's algorithm)**
- `detect_circular_dependencies(graph)` -> dict: {has_cycles, cycles, affected_rules} — **RECURSIVE DFS**
- `traverse_dependency_tree(graph, root_id, visited)` -> array — **RECURSIVE**
- `calculate_dependency_depth(graph, rule_id, memo)` -> int — **RECURSIVE with memoization**

### risk.naab — Risk Scoring
Requires: `use math`
Imports: models.naab

- `calculate_risk_score(entity_findings)` -> dict: {score, risk_level, contributing_factors, breakdown} — **Python polyglot -> JSON**
- `calculate_portfolio_risk(entity_results)` -> dict: {total_entities, avg_risk_score, high_risk_count, by_category} — **Python polyglot -> JSON**
- `generate_risk_matrix(entities, findings)` -> dict: {matrix, high_risk_cells, coverage} — **uses pipeline**
- `calculate_compliance_metrics(findings, rules)` -> dict: {total_rules, compliance_rate, by_severity, by_category, trend} — **Python polyglot -> JSON**
- `rank_entities_by_risk(entity_results)` -> array — **uses pipeline**

### reports.naab — Report Generation (Taint Sink)
Requires: `use json`, `use crypto`, `use file`
Imports: models.naab, validators.naab

- `generate_compliance_report(findings, rules, entities)` -> dict: {report_text, hash, sections}
- `generate_audit_report(events)` -> dict: {trail, hash_chain, summary}
- `export_findings_csv(findings)` -> string
- `format_finding(finding)` -> string — **uses match + pipeline**
- `write_report_to_file(report_text, filepath)` -> dict — **MUST sanitize before file.write**

### remediation.naab — Remediation Planning
Requires: `use uuid`
Imports: models.naab

- `find_best_remediation(finding, available_actions)` -> dict: {action, priority, estimated_effort, dependencies} — **uses match for priority**
- `build_remediation_plan(findings, available_actions)` -> dict: {steps, total_effort, priority_order, blocked_by}
- `estimate_total_effort(steps)` -> dict: {hours, days, cost_estimate}
- `get_actionable_steps(steps)` -> array
- `mark_step_completed(steps, step_id)` -> array — **value semantics critical**

### interpreter.naab — Agent-Based Rule Interpretation
Requires: `use agent`, `use json`
Imports: models.naab

- `check_agent_available(config_name)` -> dict: {available, reason}
- `generate_interpretation_prompt(rule, entity)` -> string
- `interpret_rule(rule, entity, context)` -> dict: {interpretation, confidence, reasoning, alternatives}
- `batch_interpret(rules, entity)` -> array of dicts
- `analyze_rule_conflicts(rules)` -> dict: {conflicts, count, resolutions} — **Python polyglot -> JSON**

## Required Language Feature Usage

These features MUST appear. Tests verify their correct operation.

### Match Expressions (minimum 9)
- `severity_name()` — 5+default
- `status_name()` — 4+default
- `result_name()` — 4+default
- `op_name()` — 11+default
- `entity_type_name()` — 6+default
- `priority_name()` — 5+default
- `evaluate_rule()` — result message classification
- `format_finding()` — severity prefix
- `aggregate_findings()` — grouping key selection

### Recursive Functions (minimum 8)
- `evaluate_condition()` — condition tree evaluation
- `count_conditions()` — tree counting
- `flatten_conditions()` — tree flattening
- `max_depth()` — tree depth
- `validate_condition_tree()` — recursive validation
- `traverse_dependency_tree()` — graph traversal
- `calculate_dependency_depth()` — depth with memoization
- `detect_circular_dependencies()` — DFS cycle detection

### Pipeline Operator (minimum 5)
- `evaluate_entity()` — rules |> filter |> evaluate |> collect
- `count_entities_by_type()` — registry |> group |> count
- `merge_rule_tags()` — tags |> combine |> deduplicate
- `rank_entities_by_risk()` — results |> score |> sort
- `format_finding()` — finding |> extract |> format

### Try/Catch Expressions (minimum 3)
- `parse_rule_definition()` — `let parsed = try { json.parse(text) } catch (e) { null }`
- `interpret_rule()` — try/catch around agent calls
- Integration tests — try/catch for error handling

### Value Semantics Correctness
- `update_entity_attribute()` — must re-assign entity in registry array
- `mark_step_completed()` — must re-assign step in steps array
- `register_entity()` — push and return modified registry
- `validate_condition_tree()` — mutable state via container pattern
- `evaluate_entity()` — collecting findings in loop

## Recursive Evaluation — Key Pattern

The condition evaluator is a tree walker. Here's the canonical pattern:

```naab
export fn evaluate_condition(condition, entity_attributes) {
    let op = condition.get("op")

    // Leaf operators — compare field to value
    if op == models.ConditionOp.Equals || op == models.ConditionOp.NotEquals ||
       op == models.ConditionOp.GreaterThan || op == models.ConditionOp.LessThan ||
       op == models.ConditionOp.GreaterOrEqual || op == models.ConditionOp.LessOrEqual ||
       op == models.ConditionOp.Contains || op == models.ConditionOp.Matches {

        let field = condition.get("field")
        let expected = condition.get("value")
        let actual = entity_attributes.get(field)

        if actual == null {
            return {"passed": false, "evidence": "missing field: " + field}
        }

        let passed = match op {
            models.ConditionOp.Equals => string(actual) == string(expected),
            models.ConditionOp.NotEquals => string(actual) != string(expected),
            models.ConditionOp.GreaterThan => int(actual) > int(expected),
            models.ConditionOp.LessThan => int(actual) < int(expected),
            models.ConditionOp.GreaterOrEqual => int(actual) >= int(expected),
            models.ConditionOp.LessOrEqual => int(actual) <= int(expected),
            models.ConditionOp.Contains => string.contains(string(actual), expected),
            _ => regex.matches(string(actual), expected)
        }

        let evidence = field + " " + models.op_name(op) + " " + expected + " -> " + string(passed)
        return {"passed": passed, "evidence": evidence}
    }

    // AND — all children must pass
    if op == models.ConditionOp.And {
        let children = condition.get("children") ?? []
        let all_passed = true
        let evidences = []
        for child in children {
            let result = evaluate_condition(child, entity_attributes)  // RECURSE
            evidences.push(result.get("evidence"))
            if result.get("passed") != true {
                all_passed = false
            }
        }
        return {"passed": all_passed, "evidence": evidences.join(" AND ")}
    }

    // OR — any child must pass
    if op == models.ConditionOp.Or {
        let children = condition.get("children") ?? []
        let any_passed = false
        let evidences = []
        for child in children {
            let result = evaluate_condition(child, entity_attributes)  // RECURSE
            evidences.push(result.get("evidence"))
            if result.get("passed") == true {
                any_passed = true
            }
        }
        return {"passed": any_passed, "evidence": evidences.join(" OR ")}
    }

    // NOT — invert single child
    if op == models.ConditionOp.Not {
        let children = condition.get("children") ?? []
        if len(children) == 0 {
            return {"passed": false, "evidence": "NOT: no child condition"}
        }
        let result = evaluate_condition(children[0], entity_attributes)  // RECURSE
        return {"passed": result.get("passed") != true, "evidence": "NOT(" + result.get("evidence") + ")"}
    }

    return {"passed": false, "evidence": "unknown operator"}
}
```

## Value Semantics Container Pattern

For recursive functions that need to accumulate state across recursive calls,
use a mutable container (array or dict) instead of a plain variable:

```naab
fn validate_condition_tree(conditions) {
    // Container pattern — state survives recursive calls
    let state = {"max_depth": 0, "errors": []}

    fn validate_node(node, depth, st) {
        if depth > st.get("max_depth") {
            st["max_depth"] = depth
        }
        // ... validation logic ...
        let children = node.get("children") ?? []
        for child in children {
            validate_node(child, depth + 1, st)
        }
        return st
    }

    for cond in conditions {
        state = validate_node(cond, 0, state)
    }
    return {"valid": len(state.get("errors")) == 0, "errors": state.get("errors"), "depth": state.get("max_depth")}
}
```

## Dependency Resolution — Topological Sort

```naab
fn resolve_rule_dependencies(rules) {
    // Build adjacency list
    let graph = {}
    for rule in rules {
        if rule.get("status") == models.RuleStatus.Active {
            graph[rule.get("id")] = rule.get("dependencies") ?? []
        }
    }

    // Kahn's algorithm via Python polyglot
    let result = <<python[graph] -> JSON
    import json
    from collections import deque

    in_degree = {n: 0 for n in graph}
    for node in graph:
        for dep in graph[node]:
            if dep in in_degree:
                in_degree[dep] = in_degree.get(dep, 0) + 1

    queue = deque([n for n, d in in_degree.items() if d == 0])
    order = []
    while queue:
        n = queue.popleft()
        order.append(n)
        for node in graph:
            if n in graph[node]:
                graph[node].remove(n)
                in_degree[node] -= 1
                if in_degree[node] == 0:
                    queue.append(node)

    cycles = []
    unresolvable = [n for n in in_degree if n not in order]
    json.dumps({"order": order, "cycles": cycles, "unresolvable": unresolvable})
    >>
    return result
}
```

## Pipeline Usage Examples

```naab
// Single-arg pipeline:
fn filter_active(rules) {
    return array.filter(rules, fn(r) {
        return r.get("status") == models.RuleStatus.Active
    })
}
let active = all_rules |> filter_active

// Multi-step pipeline:
fn extract_ids(entities) { return array.map(entities, fn(e) { return e.get("id") }) }
fn sort_ids(ids) { return array.sort(ids) }
let sorted_ids = entity_list |> extract_ids |> sort_ids
```

## Agent Usage Examples

```naab
use agent

fn safe_interpret(config_name, prompt_text) {
    let pre = agent.check(config_name)
    if pre.get("valid") != true {
        return {interpretation: "unavailable", confidence: 0, reasoning: "agent not configured", alternatives: []}
    }
    let handle = agent.create(config_name)
    try {
        let resp = agent.send(handle, prompt_text)
        let content = resp.get("content") ?? ""
        return {interpretation: content, confidence: 1, reasoning: "agent analysis", alternatives: []}
    } catch (e) {
        return {interpretation: "unavailable", confidence: 0, reasoning: string(e), alternatives: []}
    }
}
```

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
- Do NOT forget variable binding on polyglot blocks — HARD blocked
- Do NOT forget value semantics re-assignment when mutating dicts/arrays in loops
- Do NOT use if/else chains where match expressions are specified
- Do NOT modify govern.json — it is signed and modifications will be blocked
- The governance engine detects 200+ stub/evasion patterns and will BLOCK execution
