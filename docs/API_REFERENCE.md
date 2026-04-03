# NAAb Standard Library API Reference

Version: 0.8.1 | 20 modules

---

## Quick Reference

| Module | Key Functions | Sandbox-Restricted |
|--------|--------------|-------------------|
| `io` | println, print, read, write, error | No |
| `json` | parse, stringify, pretty, is_valid | No |
| `http` | get, post, put, delete, head, patch | Yes — blocked under `restricted` and `standard` |
| `collections` | set_create, set_add, set_contains, set_remove | No |
| `string` | upper, lower, split, join, replace, contains | No |
| `array` | push, pop, map_fn, filter_fn, reduce_fn, sort | No |
| `math` | abs, sqrt, pow, floor, ceil, round, random | No |
| `time` | now, sleep, format_timestamp, parse_datetime | No |
| `env` | get, set_var, has, get_all, load_dotenv | Yes — blocked under `restricted`; blocked under `standard` (no SYS_ENV) |
| `csv` | read, write, parse, stringify | No |
| `regex` | matches, search, replace, groups, find_all | No |
| `crypto` | sha256, sha512, md5, base64_encode/decode, hash | No |
| `file` | read, write, exists, delete, list_dir, copy | Yes — blocked under `restricted` |
| `debug` | inspect, type, log, assert, trace, stack | No |
| `bolo` | scan, check_count, violations, summary | No |
| `path` | join, dirname, basename, extension, resolve | No |
| `dict` | keys, values, has_key | No |
| `log` | info, warn, error, debug, set_level, set_format | No |
| `uuid` | v4, v5, is_valid, nil | No |
| `validate` | email, url, ip, int_range, not_empty, length | No |

---

## Module: io

**Import:** `use io` (auto-imported; `io` is available without explicit use)

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `println` | `...args` | null | Print args joined by space, with newline |
| `print` | `...args` | null | Print args joined by space, no newline |
| `write` | `msg: string` | null | Write to stdout (pipe mode: stderr) |
| `error` | `msg: string` | null | Write to stderr |
| `read` | _(none)_ | string | Read a line from stdin |
| `output` | `msg: string` | null | Write to stdout regardless of pipe mode |

**Example:**
```naab
main {
    io.println("Hello,", "World")    // "Hello, World\n"
    io.print("no newline")
    let line = io.read()             // reads from stdin
}
```

---

## Module: json

**Import:** `use json`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `parse` | `s: string` | any | Parse JSON string into NAAb value |
| `stringify` | `val: any` | string | Serialize NAAb value to JSON string |
| `pretty` | `val: any` | string | Serialize with indentation (2 spaces) |
| `parse_object` | `s: string` | dict | Parse JSON object, assert it is a dict |
| `parse_array` | `s: string` | list | Parse JSON array, assert it is a list |
| `is_valid` | `s: string` | bool | Returns true if s is valid JSON |

**Example:**
```naab
use json
main {
    let data = json.parse('{"name": "alice", "age": 30}')
    io.println(data["name"])            // "alice"
    let s = json.stringify(data)
    io.println(json.pretty(data))
}
```

---

## Module: http

**Import:** `use http`

**Sandbox:** Blocked under `restricted` (no `NET_CONNECT`) and `standard` (network_enabled=false).
Requires `--sandbox-level elevated` or `unrestricted`.
`file://`, `gopher://`, and `dict://` URL schemes are rejected at all sandbox levels.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `get` | `url: string` | dict | HTTP GET. Returns `{status, body, headers}` |
| `post` | `url: string, body: string` | dict | HTTP POST with body |
| `put` | `url: string, body: string` | dict | HTTP PUT with body |
| `delete` | `url: string` | dict | HTTP DELETE |
| `head` | `url: string` | dict | HTTP HEAD (no body in response) |
| `patch` | `url: string, body: string` | dict | HTTP PATCH with body |

Response dict fields: `status` (int), `body` (string), `headers` (dict).

**Example:**
```naab
use http
main {
    let resp = http.get("https://api.example.com/data")
    if resp["status"] == 200 {
        let data = json.parse(resp["body"])
    }
}
```

---

## Module: collections

**Import:** `use collections`

Provides set data structure (unordered, no duplicates).

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `set_create` | _(none)_ | set | Create an empty set |
| `set_add` | `s: set, val: any` | null | Add value to set (mutates in place) |
| `set_contains` | `s: set, val: any` | bool | Check membership |
| `set_remove` | `s: set, val: any` | null | Remove value from set |
| `set_size` | `s: set` | int | Number of elements |

**Example:**
```naab
use collections
main {
    let s = collections.set_create()
    collections.set_add(s, "apple")
    collections.set_add(s, "banana")
    collections.set_add(s, "apple")   // duplicate ignored
    io.println(collections.set_size(s))          // 2
    io.println(collections.set_contains(s, "apple"))  // true
}
```

---

## Module: string

**Import:** `use string`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `length` | `s: string` | int | Byte length of string |
| `upper` | `s: string` | string | Convert to uppercase |
| `lower` | `s: string` | string | Convert to lowercase |
| `trim` | `s: string` | string | Remove leading/trailing whitespace |
| `split` | `s: string, delim: string` | list | Split by delimiter |
| `join` | `parts: list, sep: string` | string | Join list elements with separator |
| `replace` | `s: string, from: string, to: string` | string | Replace all occurrences |
| `contains` | `s: string, sub: string` | bool | Check if substring present |
| `starts_with` | `s: string, prefix: string` | bool | Check prefix |
| `ends_with` | `s: string, suffix: string` | bool | Check suffix |
| `index_of` | `s: string, sub: string` | int | First index of substring (-1 if not found) |
| `substring` | `s: string, start: int, end: int` | string | Extract [start, end) |
| `char_at` | `s: string, i: int` | string | Single character at index |
| `repeat` | `s: string, n: int` | string | Repeat string n times |
| `reverse` | `s: string` | string | Reverse characters |
| `pad_left` | `s: string, width: int, char: string` | string | Left-pad to width |
| `pad_right` | `s: string, width: int, char: string` | string | Right-pad to width |
| `format` | `template: string, ...args` | string | Printf-style format |
| `fmt` | `template: string, ...args` | string | Alias for `format` |
| `concat` | `a: string, b: string` | string | Concatenate two strings |

**Note:** Dot-notation also works: `s.upper()`, `s.contains("x")`, `s.split(",")`.

**Example:**
```naab
use string
main {
    let s = "Hello, World!"
    io.println(string.upper(s))               // "HELLO, WORLD!"
    io.println(string.split(s, ", "))         // ["Hello", "World!"]
    io.println(string.length(s))              // 13
    io.println(string.replace(s, "World", "NAAb"))  // "Hello, NAAb!"
}
```

---

## Module: array

**Import:** `use array`

**Note:** `push`, `pop`, `shift`, `unshift`, `reverse`, `sort` mutate the array in place.
Dot-notation works for most functions: `arr.push(x)`, `arr.length()`.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `length` | `arr: list` | int | Number of elements |
| `push` | `arr: list, val: any` | null | Append element (mutates) |
| `pop` | `arr: list` | any | Remove and return last element |
| `shift` | `arr: list` | any | Remove and return first element |
| `unshift` | `arr: list, val: any` | null | Prepend element (mutates) |
| `first` | `arr: list` | any | First element (no mutation) |
| `last` | `arr: list` | any | Last element (no mutation) |
| `contains` | `arr: list, val: any` | bool | Membership test |
| `index_of` | `arr: list, val: any` | int | First index of val (-1 if absent) |
| `join` | `arr: list, sep: string` | string | Concatenate elements with separator |
| `slice` | `arr: list, start: int, end: int` | list | Sub-array [start, end) |
| `reverse` | `arr: list` | null | Reverse in place |
| `sort` | `arr: list` | null | Sort in place (ascending) |
| `find` | `arr: list, fn: function` | any | First element where fn(x) is true |
| `map_fn` | `arr: list, fn: function` | list | Apply fn to each element, return new list |
| `filter_fn` | `arr: list, fn: function` | list | Keep elements where fn(x) is true |
| `reduce_fn` | `arr: list, fn: function, init: any` | any | Fold left with accumulator |

**Example:**
```naab
main {
    let nums = [3, 1, 4, 1, 5, 9]
    nums.push(2)
    io.println(array.length(nums))                    // 7
    let evens = array.filter_fn(nums, fn(x) { x % 2 == 0 })
    let doubled = array.map_fn(nums, fn(x) { x * 2 })
    let sum = array.reduce_fn(nums, fn(acc, x) { acc + x }, 0)
}
```

---

## Module: math

**Import:** `use math`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `PI` / `pi` | _(none)_ | float | π ≈ 3.14159265... |
| `E` / `e` | _(none)_ | float | e ≈ 2.71828... |
| `abs` | `x: number` | number | Absolute value |
| `sqrt` | `x: float` | float | Square root |
| `pow` | `base: float, exp: float` | float | base^exp |
| `floor` | `x: float` | int | Round down |
| `ceil` | `x: float` | int | Round up |
| `round` | `x: float` | int | Round to nearest integer |
| `min` | `a: number, b: number` | number | Minimum of two values |
| `max` | `a: number, b: number` | number | Maximum of two values |
| `sin` | `x: float` | float | Sine (radians) |
| `cos` | `x: float` | float | Cosine (radians) |
| `tan` | `x: float` | float | Tangent (radians) |
| `log` | `x: float` | float | Natural logarithm |
| `log2` | `x: float` | float | Base-2 logarithm |
| `log10` | `x: float` | float | Base-10 logarithm |
| `random` | _(none)_ | float | Random float in [0.0, 1.0) |

**Example:**
```naab
use math
main {
    io.println(math.PI)              // 3.141592653589793
    io.println(math.sqrt(16.0))      // 4.0
    io.println(math.pow(2.0, 10.0))  // 1024.0
    let r = math.random()            // e.g. 0.7341...
}
```

---

## Module: time

**Import:** `use time`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `now` | _(none)_ | float | Current time as Unix timestamp (seconds, fractional) |
| `now_millis` | _(none)_ | float | Current time in milliseconds |
| `sleep` | `seconds: float` | null | Pause execution. Use 0.01 for 10ms |
| `format_timestamp` | `ts: float, fmt: string` | string | Format Unix timestamp with strftime pattern |
| `parse_datetime` | `s: string, fmt: string` | float | Parse datetime string to Unix timestamp |
| `year` | `ts: float` | int | Year component of timestamp |
| `month` | `ts: float` | int | Month (1–12) |
| `day` | `ts: float` | int | Day of month (1–31) |
| `hour` | `ts: float` | int | Hour (0–23) |
| `minute` | `ts: float` | int | Minute (0–59) |
| `second` | `ts: float` | int | Second (0–59) |
| `weekday` | `ts: float` | int | Day of week (0=Sunday … 6=Saturday) |

**Example:**
```naab
use time
main {
    let ts = time.now()
    io.println(time.format_timestamp(ts, "%Y-%m-%d"))  // "2026-04-01"
    io.println(time.year(ts))                           // 2026
    time.sleep(0.01)                                    // sleep 10ms
}
```

---

## Module: env

**Import:** `use env`

**Sandbox:** All env functions are blocked under `restricted` (no `SYS_ENV`). Also blocked
under `standard` when using `createEnterpriseConfig()`. Requires `--sandbox-level elevated`
or `unrestricted` to access environment variables.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `get` | `name: string` | string | Get env var value (throws if missing) |
| `has` | `name: string` | bool | Check if env var exists |
| `set_var` | `name: string, val: string` | null | Set env var in process |
| `delete_var` | `name: string` | null | Delete env var |
| `get_all` | _(none)_ | dict | All env vars as dict |
| `list` | _(none)_ | list | All env var names |
| `get_int` | `name: string, default: int` | int | Get as integer, with default |
| `get_float` | `name: string, default: float` | float | Get as float, with default |
| `get_bool` | `name: string, default: bool` | bool | Get as bool ("true"/"1"/"yes"), with default |
| `get_args` | _(none)_ | list | Script command-line arguments |
| `load_dotenv` | `path: string` | null | Load .env file and set vars |
| `parse_env_file` | `path: string` | dict | Parse .env file, return as dict (no side effects) |

**Example:**
```naab
use env
main {
    let home = env.get("HOME")
    let port = env.get_int("PORT", 8080)
    if env.has("DEBUG") {
        io.println("Debug mode enabled")
    }
}
```

---

## Module: csv

**Import:** `use csv`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `read` | `path: string` | list | Read CSV file, return list of lists |
| `read_dict` | `path: string` | list | Read CSV with header row, return list of dicts |
| `parse` | `s: string` | list | Parse CSV string, return list of lists |
| `parse_dict` | `s: string` | list | Parse CSV string with header, return list of dicts |
| `write` | `path: string, rows: list` | null | Write list of lists to CSV file |
| `write_dict` | `path: string, rows: list` | null | Write list of dicts to CSV file (keys → header) |
| `format_row` | `row: list` | string | Format a single row as CSV string |
| `format_rows` | `rows: list` | string | Format multiple rows as CSV string |
| `stringify` | `rows: list` | string | Alias for `format_rows` |

**Example:**
```naab
use csv
main {
    let rows = csv.read("data.csv")           // [["name","age"],["alice","30"]]
    let records = csv.read_dict("data.csv")   // [{"name":"alice","age":"30"}]
    csv.write("out.csv", rows)
}
```

---

## Module: regex

**Import:** `use regex`

**Note:** `groups(text, pattern)` returns `[full_match, group1, group2, ...]` — full match is
at index 0. All patterns use ECMAScript/POSIX regex syntax.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `matches` | `text: string, pattern: string` | bool | True if full string matches pattern |
| `search` | `text: string, pattern: string` | bool | True if pattern found anywhere in text |
| `find` | `text: string, pattern: string` | string | First match, or empty string |
| `find_all` | `text: string, pattern: string` | list | All non-overlapping matches |
| `replace` | `text: string, pattern: string, repl: string` | string | Replace all matches |
| `replace_first` | `text: string, pattern: string, repl: string` | string | Replace first match only |
| `split` | `text: string, pattern: string` | list | Split by regex pattern |
| `groups` | `text: string, pattern: string` | list | Capture groups: `[full, g1, g2, ...]` |
| `find_groups` | `text: string, pattern: string` | list | All matches' groups: list of lists |
| `escape` | `s: string` | string | Escape regex metacharacters |
| `is_valid` | `pattern: string` | bool | Check if pattern compiles without error |
| `compile_pattern` | `pattern: string` | string | Validate and normalize pattern |

**Example:**
```naab
use regex
main {
    let m = regex.matches("2026-04-01", "\\d{4}-\\d{2}-\\d{2}")
    let g = regex.groups("2026-04-01", "(\\d{4})-(\\d{2})-(\\d{2})")
    // g == ["2026-04-01", "2026", "04", "01"]
    io.println(g[1])   // "2026"
    let parts = regex.find_all("one two three", "\\w+")
    // parts == ["one", "two", "three"]
}
```

---

## Module: crypto

**Import:** `use crypto`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `md5` | `s: string` | string | MD5 hex digest (not for security — use sha256) |
| `sha1` | `s: string` | string | SHA-1 hex digest |
| `sha256` | `s: string` | string | SHA-256 hex digest |
| `sha512` | `s: string` | string | SHA-512 hex digest |
| `hash` | `s: string, algo: string` | string | Hash with named algorithm |
| `base64_encode` | `s: string` | string | Base64-encode string |
| `base64_decode` | `s: string` | string | Base64-decode string |
| `hex_encode` | `s: string` | string | Hex-encode string |
| `hex_decode` | `s: string` | string | Hex-decode string |
| `random_bytes` | `n: int` | string | n random bytes as raw string |
| `random_string` | `n: int` | string | n-character random alphanumeric string |
| `random_int` | `min: int, max: int` | int | Random integer in [min, max] |
| `compare_digest` | `a: string, b: string` | bool | Constant-time string comparison |
| `generate_token` | `n: int` | string | URL-safe random token of n bytes |
| `hash_password` | `pw: string` | string | Hash password for storage |

**Note:** OpenSSL must be present for SHA-256/512 and hash_password. Falls back gracefully
if OpenSSL is unavailable.

**Example:**
```naab
use crypto
main {
    let h = crypto.sha256("hello world")
    let token = crypto.generate_token(32)
    let encoded = crypto.base64_encode("binary data")
    io.println(crypto.compare_digest(h, h))   // true
}
```

---

## Module: file

**Import:** `use file`

**Sandbox:** All file operations check sandbox capabilities. Under `restricted`, ALL file
operations are blocked. Under `standard`, only `allowed_read_paths` and `allowed_write_paths`
are accessible. Use `--sandbox-level elevated` for broad file access.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `read` | `path: string` | string | Read file contents as string |
| `write` | `path: string, content: string` | null | Write content to file (overwrites) |
| `append` | `path: string, content: string` | null | Append content to file |
| `exists` | `path: string` | bool | True if path exists |
| `delete` | `path: string` | null | Delete a file |
| `is_file` | `path: string` | bool | True if path is a regular file |
| `is_dir` | `path: string` | bool | True if path is a directory |
| `list_dir` | `path: string` | list | List directory contents (filenames) |
| `create_dir` | `path: string` | null | Create directory (and parents) |
| `read_lines` | `path: string` | list | Read file as list of lines |
| `write_lines` | `path: string, lines: list` | null | Write list of lines to file |
| `copy` | `src: string, dst: string` | null | Copy file |
| `move` | `src: string, dst: string` | null | Move/rename file |
| `size` | `path: string` | int | File size in bytes |
| `basename` | `path: string` | string | Filename without directory |
| `dirname` | `path: string` | string | Directory part of path |
| `extension` | `path: string` | string | File extension (e.g. ".txt") |

**Example:**
```naab
use file
main {
    file.write("output.txt", "Hello\n")
    let content = file.read("output.txt")
    let lines = file.read_lines("data.csv")
    let entries = file.list_dir(".")
    io.println(file.basename("/home/user/script.naab"))  // "script.naab"
}
```

---

## Module: debug

**Import:** `use debug`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `inspect` | `val: any` | string | Detailed string representation of any value |
| `type` | `val: any` | string | Type name of value |
| `log` | `label: string, val: any` | null | Print labeled value to stderr |
| `assert` | `cond: bool, msg: string` | null | Throw if cond is false |
| `trace` | `msg: string` | null | Print message with source location |
| `keys` | `val: any` | list | Keys of dict or struct |
| `values` | `val: any` | list | Values of dict or struct |
| `diff` | `a: any, b: any` | string | Human-readable diff of two values |
| `timer` | `fn: function` | float | Execute fn and return elapsed ms |
| `env` | _(none)_ | dict | Current interpreter environment snapshot |
| `stack` | _(none)_ | list | Current call stack frames |
| `watch` | `name: string, val: any` | null | Register value for watch output |
| `snapshot` | _(none)_ | dict | Full interpreter state snapshot |
| `compare` | `a: any, b: any` | bool | Deep equality comparison |

**Example:**
```naab
use debug
main {
    let x = {"name": "alice", "scores": [95, 87, 92]}
    io.println(debug.inspect(x))
    io.println(debug.type(x))          // "dict"
    debug.assert(debug.type(x) == "dict", "expected dict")
    let ms = debug.timer(fn() { let s = 0; for i in 0..1000 { s = s + i } })
}
```

---

## Module: bolo

**Import:** `use bolo`

The `bolo` module provides programmatic access to the governance/security scanner. It allows
NAAb scripts to run code quality checks on source files.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `scan` | `path: string` | list | Scan file for violations, return list of findings |
| `load_profile` | `path: string` | null | Load a governance profile from file |
| `load_config` | `path: string` | null | Load a bolo config file |
| `check_count` | _(none)_ | int | Total checks run since last reset |
| `profiles` | _(none)_ | list | List loaded profile names |
| `reset` | _(none)_ | null | Reset scan state and counters |
| `violations` | _(none)_ | list | All violations from last scan |
| `summary` | _(none)_ | dict | Summary: `{total, violations, passed}` |

**Example:**
```naab
use bolo
main {
    let findings = bolo.scan("./src/main.naab")
    let summary = bolo.summary()
    io.println("Violations: " + summary["violations"])
}
```

---

## Module: path

**Import:** `use path`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `join` | `...parts` | string | Join path components with OS separator |
| `dirname` | `p: string` | string | Parent directory of path |
| `basename` | `p: string` | string | Final component of path |
| `extension` | `p: string` | string | File extension including dot (e.g. `.txt`) |
| `resolve` | `p: string` | string | Resolve to absolute path |
| `is_absolute` | `p: string` | bool | True if path is absolute |
| `normalize` | `p: string` | string | Normalize `.` and `..` components |
| `exists` | `p: string` | bool | True if path exists on filesystem |

**Example:**
```naab
use path
main {
    let full = path.join("/home/user", "projects", "app.naab")
    io.println(path.basename(full))    // "app.naab"
    io.println(path.extension(full))   // ".naab"
    io.println(path.dirname(full))     // "/home/user/projects"
    io.println(path.is_absolute(full)) // true
}
```

---

## Module: dict

**Import:** `use dict`

**Note:** NAAb dicts support direct bracket access (`d["key"]`), dot notation for known keys,
and the `??` operator for safe access: `d.get("key") ?? "default"`.
Use `d.get(key)` instead of `d["key"]` to avoid throws on missing keys.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `keys` | `d: dict` | list | List of all keys |
| `values` | `d: dict` | list | List of all values |
| `has_key` | `d: dict, key: string` | bool | True if key exists |

**Example:**
```naab
use dict
main {
    let d = {"name": "alice", "age": 30}
    io.println(dict.keys(d))        // ["name", "age"]
    io.println(dict.has_key(d, "age"))  // true
    let val = d.get("missing") ?? "default"
}
```

---

## Module: log

**Import:** `use log`

Provides structured logging with levels, formats, and output targets.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `debug` | `msg: string` | null | Log at DEBUG level |
| `info` | `msg: string` | null | Log at INFO level |
| `warn` | `msg: string` | null | Log at WARN level |
| `error` | `msg: string` | null | Log at ERROR level |
| `log` | `level: string, msg: string` | null | Log at named level |
| `set_level` | `level: string` | null | Set minimum level: "debug"/"info"/"warn"/"error"/"none" |
| `get_level` | _(none)_ | string | Get current minimum level |
| `set_format` | `format: string` | null | Set output format: "text" (default) or "json" |
| `set_output` | `target: string` | null | Set output: "stderr" (default), "stdout", or file path |

Level ordering (lowest to highest): `debug` < `info` < `warn` < `error` < `none`.
Messages below the current level are silently discarded.

JSON format: `{"level":"info","msg":"...","ts":"2026-04-01T12:00:00Z"}`

**Example:**
```naab
use log
main {
    log.set_level("info")
    log.set_format("json")
    log.info("Server started")
    log.warn("High memory usage")
    log.debug("This is suppressed")   // filtered by level
    log.set_output("/var/log/app.log")
    log.error("Connection failed")
}
```

---

## Module: uuid

**Import:** `use uuid`

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `v4` | _(none)_ | string | Generate a random UUID v4 (uses /dev/urandom) |
| `v5` | `namespace: string, name: string` | string | Generate deterministic UUID v5 (SHA-1 based) |
| `is_valid` | `s: string` | bool | Validate UUID format (any version) |
| `nil` | _(none)_ | string | Return the nil UUID: "00000000-0000-0000-0000-000000000000" |

UUID format: `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` (lowercase hex with dashes).

**Example:**
```naab
use uuid
main {
    let id = uuid.v4()
    io.println(uuid.is_valid(id))        // true
    io.println(uuid.nil())               // "00000000-0000-0000-0000-000000000000"
    let stable_id = uuid.v5("dns", "example.com")
}
```

---

## Module: validate

**Import:** `use validate`

All functions return `bool`. Useful for input validation without polyglot.

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `email` | `s: string` | bool | Valid email address format |
| `url` | `s: string` | bool | Valid HTTP/HTTPS URL |
| `ip` | `s: string` | bool | Valid IPv4 address |
| `ipv6` | `s: string` | bool | Valid IPv6 address |
| `int_range` | `val: number, min: number, max: number` | bool | Value in [min, max] (inclusive) |
| `not_empty` | `s: string` | bool | Non-empty string (after trimming whitespace) |
| `length` | `s: string, min: int, max: int` | bool | String length in [min, max] |
| `matches` | `s: string, pattern: string` | bool | Full regex match |
| `is_int` | `val: any` | bool | Value is an integer |
| `is_float` | `val: any` | bool | Value is a float |
| `is_string` | `val: any` | bool | Value is a string |

**Example:**
```naab
use validate
main {
    let email = "user@example.com"
    if validate.email(email) {
        io.println("Valid email")
    }
    if !validate.not_empty(email) {
        io.println("Email is required")
    }
    let port = 8080
    if !validate.int_range(port, 1, 65535) {
        io.println("Invalid port")
    }
}
```
