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
let result = <<python
x = 42
x * 2
>>
```

### WRONG (will error):
```naab
let result = <<python x * 2 >>     // Single-line NOT allowed
```

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
- **JavaScript**: Last expression is the return value. Use `const`/`let`, NOT `var`.
  No `console.log` — use NAAb `io.write`.
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
For structured return data:
```naab
let data = <<python -> JSON
import json
json.dumps({"key": "value", "count": 42})
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
15. `||` ALWAYS returns boolean (true/false), NEVER the operand value.
    `null || "fallback"` returns `true` (NOT "fallback"!).
    NAAb's `||` is NOT like JavaScript's `||` operator.
    For null coalesce, use the `??` operator: `x ?? default_val`
16. `dict["key"]` THROWS on missing key — use `dict.get("key")` or `dict.get("key", default)` for safe access
17. Struct instantiation requires `new`: `let p = new Point { x: 1, y: 2 }` — without `new` you get a parse error
18. Python polyglot: Do NOT use `return` — causes `SyntaxError: 'return' outside function`
19. Value semantics: modifying a nested dict/array requires re-assigning to parent (see Value Semantics section)
20. The `..` range operator can collide with `".."` string literals — use intermediate variables: `let dots = ".."; path.contains(dots)`

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
- Do NOT leave TODO/FIXME/STUB comments
- [Add project-specific restrictions]
```
