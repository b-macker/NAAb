# NAAb Language Syntax Reference

## About NAAb
NAAb is a polyglot programming language. You embed other languages (Python, JavaScript,
Shell, Go, Nim, Rust, C++, C#, Ruby, PHP) inside `<< >>` blocks within .naab files.

## File Structure
- Top level can ONLY contain: use, import, export, struct, enum, function/fn, main
- NO top-level `let` — variables MUST be inside main {} or functions
- Every executable .naab file needs a `main {}` block

## Variable Declaration
- `let x = 5` — mutable variable
- `const MAX = 100` — constant

## Functions
- `fn name(param1, param2) { }` — function declaration
- `function`, `func`, `def` also work
- Lambda: `fn(x) { return x * 2 }`

## Control Flow
- `if condition { } else if condition { } else { }`
- `for item in collection { }`
- `for i in 0..10 { }` — range (exclusive), `0..=10` for inclusive
- `while condition { }`
- `match value { pattern => { } default => { } }`
- `try { } catch (e) { }` — parens around `e` required
- `break`, `continue`, `return value`

## If Expressions
- `let x = if condition { value_a } else { value_b }`

## Null Coalesce
- `let y = x ?? "default"` — returns first non-null value
- `||` returns boolean, `??` returns the actual value

## Structs and Enums
```naab
struct Point { x: int, y: int }
enum Color { Red, Green, Blue }
let p = new Point { x: 1, y: 2 }   // `new` keyword required
```

## Type Casts
- `int()`, `float()`, `string()`, `bool()`, `len()`, `type()`, `print()`

## Imports
```naab
import "src/module.naab" as mod   // Relative to THIS file's directory
// Imported functions MUST use `export fn name() { }`
```

## Polyglot Blocks
Multiline only. `>>` closer MUST be on its own line at column 0:
```naab
let result = <<python
x = 42
x * 2
>>
```

### Variable Binding
```naab
let data = [1, 2, 3]
let result = <<python[data]
sum(data)
>>
```

### Language Notes
- **Python**: Start at column 0. Last expression is return value. Do NOT use `return`.
- **JavaScript**: Last expression is return value.
- **Shell**: stdout is return value.

### JSON Sovereign Pipe
```naab
let data = <<python -> JSON
import json
json.dumps({"key": "value"})
>>
```

## Stdlib
- **array**: push, pop, shift, unshift, length, contains, find, join, reverse, sort, slice_arr, map_fn, filter_fn, reduce_fn
  - `array.filter_fn(arr, fn)` — NOT `arr.filter_fn(fn)`
- **dict**: get, has, size, put, remove, keys, values, entries, merge
  - `dict.get("key")` safe, `dict["key"]` throws on missing
- **string**: upper, lower, trim, split, replace, contains, starts_with, ends_with, length, char_at, substring, reverse
- **math**: abs, floor, ceil, round, min, max, pow, sqrt, random, PI, E
- **json**: parse, stringify
- **file**: read, write, exists, list_dir, delete, append
- **time**: now, sleep (SECONDS not ms), format_timestamp, parse_datetime
- **regex**: search, matches, find, find_all, replace, split
- **env**: get, set_var
- **io**: write, read_line
- **crypto**: sha256, sha512, md5, random_bytes, random_string, base64_encode, base64_decode

## Pipeline Operator
```naab
let result = data |> transform |> analyze |> format
```

## Value Semantics
Dicts/arrays use copy-on-assignment. Re-assign after modifying nested values:
```naab
let e = entities[i]
e["hp"] = new_hp
entities[i] = e    // MUST re-assign
```

## Common Gotchas
1. NO `elif` — use `else if`
2. NO `.len()` — use `.length()` or `len()`
3. Use `true`/`false`/`null` (NOT `True`/`False`/`None`)
4. `catch (e)` needs parens
5. `env.set_var()` NOT `env.set()`
6. `config` is a reserved keyword
7. `try` is a statement, not an expression
8. `string.match()` doesn't exist — use `regex.search()`
