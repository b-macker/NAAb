# NAAb Language Tour

A tour of the language itself: syntax, polyglot blocks and the standard library. The [README](../README.md) covers what NAAb is for: the governance engine, the quickstart and configuration.

**Every program on this page runs in CI.** `tests/self-audit/test_readme_claims.sh` runs each one, and checks every value its `// ...` comments promise. To run one yourself, save it in a project made with `naab-lang init` (NAAb needs a `govern.json`). For a polyglot example, add its languages to `languages.allowed`; for the HTTP example, set `capabilities.network.enabled`.

## Contents

- [The language](#the-language)
- [Polyglot blocks](#polyglot-blocks)
- [Standard library](#standard-library)
- [Error messages and LLM-friendly syntax](#error-messages-and-llm-friendly-syntax)

---

## The language

### Pattern Matching

```naab
main {
    let status = 404

    let message = match status {
        200 => "OK"
        404 => "Not Found"
        n if n >= 500 => f"Server Error ({n})"  // guard clause
        _ => "Unknown"
    }

    print(message)  // "Not Found"

    // Array destructuring in match
    let point = [3, 4]
    let label = match point {
        [0, 0] => "origin"
        [x, 0] => f"on x-axis at {x}"
        [0, y] => f"on y-axis at {y}"
        [x, y] => f"point ({x}, {y})"
    }
    print(label)  // "point (3, 4)"
}
```

### Async/Await

```naab
main {
    async fn fetch_data() {
        return "data loaded"
    }

    async fn process() {
        return "processed"
    }

    let data = await fetch_data()
    let result = await process()
    print(data + " -> " + result)
}
```

### Lambdas & Closures

```naab
main {
    let multiplier = 3
    let scale = fn(x) { return x * multiplier }

    print(scale(10))  // 30

    // Closures capture their environment
    function make_counter() {
        let count = 0
        return fn() {
            count = count + 1
            return count
        }
    }

    let counter = make_counter()
    print(counter())  // 1
    print(counter())  // 2
}
```

### Pipeline Operator

```naab
fn twice(s) { return s + " " + s }
fn shout(s) { return s + "!" }

main {
    let result = "hello" |> twice |> shout
    print(result)  // "hello hello!"
}
```

The right-hand side is a function of your own; piping into a standard-library function (`|> string.upper()`) does not work yet.

### If Expressions

```naab
main {
    let score = 85
    let grade = if score >= 90 { "A" } else if score >= 80 { "B" } else { "C" }
    print(grade)  // "B"
}
```

### Error Handling

```naab
main {
    try {
        let result = <<python
raise ValueError("something broke")
>>
    } catch (e) {
        print("Caught from Python: " + e)
    }
}
```

### Generators

```naab
fn fibonacci(limit) {
    let a = 0
    let b = 1
    while a < limit {
        yield a
        let temp = a + b
        a = b
        b = temp
    }
}

main {
    for n in fibonacci(100) {
        print(n)  // 0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89
    }
}
```

### Interfaces

```naab
interface Printable {
    fn to_string() -> string
}

struct Point implements Printable {
    x: int
    y: int
}

fn Point.to_string(p) -> string {
    return f"({p.x}, {p.y})"
}

main {
    let p = new Point { x: 3, y: 4 }
    print(Point.to_string(p))  // "(3, 4)"
}
```

### F-Strings

```naab
main {
    let name = "World"
    let count = 42
    print(f"Hello {name}, you have {count} items")
    print(f"Total: {count * 2}")  // expressions work too
}
```

### More Language Features

- Optional chaining (`user?.name`, `obj?.method()`), null coalescing (`x ?? "default"`, `x ??= fallback`)
- Destructuring (`let [a, b] = arr`, `let {x, y} = dict`)
- `in` / `not in` operators, array slicing (`arr[1:3]`)
- Structs, enums, module system with imports/exports/URL imports
- For loops with destructuring, while loops, break/continue
- Dictionaries and arrays with dot-notation methods

---

## Polyglot blocks

A block runs code in another language. `<<python[x, y]` passes NAAb variables in; the last expression, or a value passed to `naab_return()`, comes back as a NAAb value. A language runs only if `govern.json` allows it (`languages.allowed`) and its compiler or interpreter is installed.

**Supported languages:** Python · JavaScript · TypeScript · Rust · C++ · Go · C# · Ruby · PHP · Shell · Nim · Zig · Julia · SQL

> Zig and Julia executors ship with the runtime but are disabled by default in `naab.toml` — flip `zig = true` / `julia = true` under `[polyglot]` to enable them.

### Passing values in and out

```naab
main {
    // A persistent runtime keeps its state between calls
    runtime py = python.start()
    py.exec("total = 0")
    py.exec("total = total + 40")
    print(py.exec("total + 2"))  // 42

    // naab_return() hands a structured value back to NAAb
    let info = <<python
naab_return({"lang": "python", "ok": True})
>>
    print(info["lang"])  // "python"

    // -> JSON: the block prints JSON; other output lines are skipped
    let parsed = <<python -> JSON
import json
print("working...")
print(json.dumps({"answer": 42}))
>>
    print(parsed["answer"])  // 42
}
```

### SQL

SQL blocks run in an in-process SQLite database with no filesystem access. Bound variables become SQL parameters (`:min_year`), never text, so a value cannot inject SQL. The rows of the last query come back as a list of dicts.

```naab
main {
    let min_year = 2000
    let rows = <<sql[min_year]
CREATE TABLE langs(name TEXT, year INT);
INSERT INTO langs VALUES ('python', 1991), ('go', 2009), ('rust', 2010);
SELECT name, year FROM langs WHERE year > :min_year ORDER BY year;
>>
    for row in rows {
        print(row["name"] + " " + string(row["year"]))
    }
    print(rows.length())  // 2
}
```

### Also

- **Errors** from a block map back to the NAAb source line and can be caught with `try`/`catch` (see [Error Handling](#error-handling)).
- **Block headers:** a Go block gets `package main` and a PHP block gets `<?php` added, so they can be written as bare code.
- **Parallel execution:** on the tree-walker (`--tree-walk`), independent blocks run concurrently after a dependency analysis.

---

## Standard library

There are 25 modules. A misspelled function gets a "Did you mean?" suggestion.

```naab
main {
    // JSON
    let data = json.parse('{"name": "NAAb", "type": "language"}')
    print(data["name"])

    // HTTP (needs "capabilities": {"network": {"enabled": true}})
    let response = http.get("https://api.github.com/repos/b-macker/NAAb")
    if response["status"] == 200 {
        print(json.parse(response["body"])["stargazers_count"])
    }

    // File I/O
    file.write("output.txt", "Hello from NAAb!")

    // Math
    print(math.sqrt(144))  // 12

    // String operations
    let words = string.split("hello world", " ")
    print(string.upper(words[0]))  // "HELLO"
}
```

| Module | Functions |
|---|---|
| `array` | push, pop, shift, unshift, map_fn, filter_fn, reduce_fn, sort, sorted, slice_arr, find, reverse, length, contains, first, last, join |
| `string` | split, join, upper, lower, trim, replace, reverse, contains, starts_with, ends_with, length, char_at, index_of, substring, slice, repeat, pad_left, pad_right, format |
| `math` | sqrt, pow, abs, floor, ceil, round, min, max, sin, cos, tan, random, PI, E |
| `json` | parse, stringify, pretty, is_valid |
| `http` | get, post, put, delete, head, patch (with headers, body, timeout) |
| `file` | read, write, append, exists, delete, list_dir, create_dir, is_file, is_dir, read_lines, write_lines, copy, move, size, basename, dirname, extension |
| `path` | join, dirname, basename, extension, resolve, is_absolute, normalize, exists |
| `time` | now, now_millis, sleep, format_timestamp, parse_datetime, year, month, day, hour, minute, second, weekday |
| `debug` | inspect, type, trace, watch, snapshot, diff, keys, values, log, timer, compare, stack, env |
| `env` | get, has, get_int, get_bool, set_var, list, load_dotenv |
| `csv` | parse, stringify, read, write, read_dict, write_dict |
| `regex` | search, matches, find, find_all, replace, replace_first, split, groups, find_groups, escape, is_valid |
| `crypto` | hash, sha256, sha512, md5, sha1, hmac, random_bytes, random_string, random_int, base64_encode, base64_decode, hex_encode, hex_decode, compare_digest, generate_token, hash_password |
| `dict` | get, get_or, has_key, keys, values, entries, merge, size |
| `collections` | Set, set_add, set_contains, set_remove, set_size |
| `io` | print, println, input, read_line, read_file, write_file, write, write_error, exists, list_dir |
| `log` | debug, info, warn, error, set_level, get_level, set_format, set_output |
| `process` | run, exit, getpid, kill |
| `uuid` | v4, v5, nil, is_valid |
| `validate` | email, url, ip, ipv6, is_int, is_float, is_string, int_range, length, matches, not_empty |
| `agent` | create, send, run, propose, commit, record_validation, extract_code, register_tool, batch, fan_out, pipeline, check, usage, reset, key_health, dispatch_status, environment |
| `codegen` | run, run_with_args, run_strict, supported_languages, is_enabled |
| `orchestra` | sequential_refinement, consensus_vote, enforce_convergence, select_admissible |
| `governance` | health, guidance, findings, evaluate, score, calibrate |
| `bolo` | scan, summary, violations, check_count, profiles, load_profile, load_config, reset |

---

## Error messages and LLM-friendly syntax

### Error messages that say how to fix it

NAAb doesn't just tell you what's wrong — it tells you how to fix it:

```
Error: Unknown array function: append

  Did you mean: array.push()?
  Example: array.push(my_array, new_item)
  Note: push() mutates the original array.
```

### Common Mistake Detection

NAAb recognises idioms from other languages and names the NAAb equivalent:

- `array.append()` → "Did you mean: array.push()?"
- `console.log()` → "NAAb does not have a 'Console' object", followed by the `print(...)` forms to use
- `array.pussh()` → "Did you mean: push?", followed by the module's function list

### LLM-Friendly Syntax

NAAb accepts multiple keyword styles so AI-generated code works without manual edits:

- `function` / `func` / `fn` / `def` — all valid
- `let` / `const` — both declare a variable (`const` does not yet prevent reassignment)
- Semicolons — optional (accepted but not required)
- `return` — optional in single-expression functions
