# NAAb User Guide

This guide takes you from an empty directory to a working NAAb project: writing and running programs, splitting them into modules, calling other languages, and working with the policy file every project has. The [README](README.md) says what NAAb is for. The [language tour](docs/language-tour.md) is the feature reference.

Every program here is a complete file. Save it in a project made with `naab-lang init`, run it, and it prints what the guide shows. Where a program needs a change to `govern.json`, the change is shown just before it.

## Contents

1. [Set up a project](#1-set-up-a-project)
2. [Program structure](#2-program-structure)
3. [Values](#3-values)
4. [Strings, arrays and dictionaries](#4-strings-arrays-and-dictionaries)
5. [Control flow](#5-control-flow)
6. [Functions](#6-functions)
7. [Structs and enums](#7-structs-and-enums)
8. [Errors](#8-errors)
9. [Modules](#9-modules)
10. [Files, environment and network](#10-files-environment-and-network)
11. [Other languages](#11-other-languages)
12. [Working with the policy](#12-working-with-the-policy)
13. [Tools](#13-tools)
14. [Not working yet](#14-not-working-yet)

---

## 1. Set up a project

### Build

```bash
git clone --recursive https://github.com/b-macker/NAAb.git
cd NAAb
mkdir build && cd build
cmake ..
make naab-lang naab-gov -j4
cd ..
export PATH="$PWD/build:$PATH"
```

`naab-lang` runs programs; `naab-gov` checks them without running them. [INSTALL.md](INSTALL.md) lists the dependencies, and the compilers each polyglot language needs.

### Create a project

```bash
mkdir hello && cd hello
naab-lang init
```

`init` writes two files:

- `naab.toml`: the project manifest (name, version, dependencies).
- `govern.json`: the project's policy: which languages may run, what the program may touch (files, network, environment, shell), and which code-quality checks apply at which level.

NAAb looks for a `govern.json` in the program's directory and then in each parent directory. It refuses to run a program without one, with exit code 4. `--no-governance` waives that requirement, but a `govern.json` that is found still applies.

### Your first program

Save this as `hello.naab` and run it with `naab-lang hello.naab`:

```naab
main {
    let name = "World"
    print("Hello, " + name + "!")
}
```

```text
Hello, World!
```

That is the program's output, on stdout. The governance engine reports on stderr:

```text
[governance] WARNING contradiction.CONTRA-013
Governance warning: capabilities.filesystem path rules are enforced inside NAAb's standard library and the embedded Python executor only. ...
[governance] Loaded: /home/you/hello/govern.json (mode: enforce)
[governance] Sandbox: upgraded unrestricted → standard (enforce mode; set security.sandbox_level in govern.json to override)

[governance] Summary (mode: enforce): 8 passed, 1 warning, 0 blocked
  ⚠ contradiction.CONTRA-013            [advisory]  WARN

-- Governance: PASS (9 static, 0 runtime, 0 violations) --
```

- **CONTRA-013** is an advisory warning: execution continues. The generated `govern.json` blocks some paths (`/etc/shadow`, `/etc/passwd`). NAAb enforces that inside its own standard library and embedded Python, but not inside a shell block, which runs as a separate process. The warning currently appears for every project with path rules unless the sandbox is `restricted`, even one that never runs shell.
- **Sandbox** is the runtime's own containment. With `mode: enforce` and no `security.sandbox_level`, it runs at `standard`. [Section 10](#10-files-environment-and-network) shows what that refuses and how to change it.
- **Summary** and **Governance: PASS** list the checks that ran and their results.

### The REPL

`naab-lang` with no arguments starts an interactive session. `.help` lists its commands and `.exit` leaves.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Runtime error (an uncaught error, a parse error) |
| 2 | Quality gate failed |
| 3 | Refused by the policy |
| 4 | Configuration error, including "no govern.json found" |

---

## 2. Program structure

A program's statements go in `main { }`. At the top level of a file, only these are allowed: `use`, `import`, `export`, `struct`, `enum`, functions and `main`. A `let` outside a function or `main` is a parse error.

```naab
// A line comment
/* A block comment */
use string

fn shout(text) {
    return string.upper(text) + "!"
}

main {
    print(shout("hello"))  // "HELLO!"
}
```

- `use MODULE` names a standard-library module the file uses. Write one for each module you call.
- Semicolons are optional.
- `fn`, `function`, `func` and `def` all declare a function.
- `print(a, b, c)` prints its arguments separated by spaces, then a newline. `io.write(text)` prints without a newline.

---

## 3. Values

```naab
main {
    let name = "Alice"     // string
    let age = 30           // int
    let score = 95.5       // float
    let active = true      // bool
    let nothing = null

    print(typeof(name), typeof(age), typeof(score), typeof(active), typeof(nothing))
    print(7 / 2)              // 3.5
    print(10 / 5)             // 2
    print(int(7 / 2))         // 3
    print(7 % 3)              // 1
    print("n=" + string(5))   // "n=5"
    print(int("42") + 1)      // 43
}
```

```text
string int float bool null
3.5
2
3
1
n=5
43
```

- Division always gives a float: `7 / 2` is `3.5`, and `10 / 5` is the float `2`. Use `int()` or `math.floor()` for a whole number. `//` starts a comment; there is no integer-division operator and no `**` (use `math.pow`).
- `+` joins strings, and converts a non-string on its right: `"x" + 5` is `"x5"`.
- `int()`, `float()` and `string()` convert between types.

### Interpolation

```naab
main {
    let name = "Ada"
    let items = 3
    print("Hello, ${name}!")                     // "Hello, Ada!"
    print(f"{name} has {items * 2} items")        // "Ada has 6 items"
    print("A literal \${name} stays as written")  // "A literal ${name} stays as written"
}
```

`${...}` works in every string; `f"..."` uses `{...}`. Both take any expression.

### Null

```naab
main {
    let settings = {"theme": "dark"}
    let size = settings.get("size") ?? 12
    print(size)  // 12
}
```

`a ?? b` is `a` unless `a` is null. `?.` stops at a null instead of failing (`user?.name`).

### Type annotations

```naab
fn add(x: int, y: int) -> int {
    return x + y
}

main {
    let count: int = 42
    print(add(count, 1))  // 43
}
```

`naab-lang check FILE` reports a value that does not match its annotation. A program still runs with one: the default engine does not check annotations at run time.

### Equality

```naab
main {
    print(5 == 5.0)                                   // "true"
    print("1" == 1)                                   // "false"
    print([1, 2] == [1, 2])                           // "true"
    print([1, 2] == [2, 1])                           // "false"
    print({"a": 1, "b": 2} == {"b": 2, "a": 1})       // "true"
}
```

`==` compares by value. Numbers compare numerically, and values of different types are never equal (no conversion). Arrays compare element by element in order, dictionaries key by key in any order, and structs by type and fields.

---

## 4. Strings, arrays and dictionaries

Most operations come in two forms: a method (`text.upper()`) and a module function (`string.upper(text)`).

### Strings

```naab
use string

main {
    let text = "Hello, World!"
    print(text.length())                       // 13
    print(text.upper())                        // "HELLO, WORLD!"
    print(text.replace("World", "NAAb"))       // "Hello, NAAb!"
    print(text.contains("World"))              // "true"
    print(text.starts_with("Hello"))           // "true"
    print(text.substring(0, 5))                // "Hello"
    print(text.slice(-6))                      // "World!"
    print(string.index_of(text, "World"))      // 7
    print("  padded  ".trim())                 // "padded"
    print(string.pad_left("7", 3, "0"))        // "007"
    print(string.repeat("ab", 3))              // "ababab"
    print(text[0])                             // "H"

    let parts = string.split("a,b,c", ",")
    print(parts.length())                      // 3
    print(parts.join(" + "))                   // "a + b + c"
}
```

### Arrays

```naab
use array

main {
    let nums = [3, 1, 2]
    nums.push(4)
    print(nums)                       // "[3, 1, 2, 4]"
    print(nums.length())              // 4
    print(nums[0], nums[-1])          // "3 4"
    print(nums[1:3])                  // "[1, 2]"
    print(nums.contains(2), 2 in nums, 9 not in nums)  // "true true true"

    print(array.sorted(nums))         // "[1, 2, 3, 4]"
    print(nums)                       // "[3, 1, 2, 4]"
    nums.sort()
    print(nums)                       // "[1, 2, 3, 4]"

    let last = nums.pop()
    print(last, nums)                 // "4 [1, 2, 3]"
}
```

`array.sorted()` returns a new sorted array; `.sort()` sorts in place.

To transform an array, pass a function to `array.map_fn`, `array.filter_fn` or `array.reduce_fn`. Arrays have no `.map()` or `.filter()` methods.

```naab
use array

main {
    let nums = [1, 2, 3, 4, 5]
    let doubled = array.map_fn(nums, fn(x) { return x * 2 })
    let evens = array.filter_fn(nums, fn(x) { return x % 2 == 0 })
    let total = array.reduce_fn(nums, fn(sum, x) { return sum + x }, 0)
    let first_big = nums.find(fn(x) { return x > 3 })

    print(doubled)    // "[2, 4, 6, 8, 10]"
    print(evens)      // "[2, 4]"
    print(total)      // 15
    print(first_big)  // 4
}
```

### Dictionaries

```naab
use dict

main {
    let person = {"name": "Alice", "age": 30}
    let point = {x: 1, y: 2}     // keys may be written without quotes

    print(person["name"])        // "Alice"
    print(point.x)               // 1

    person["age"] = 31
    person["city"] = "Boston"
    print(person["age"])         // 31
    print(person.size())         // 3

    print(person.has("city"))                // "true"
    print(person.get("zip"))                 // "null"
    print(person.get("zip", "unknown"))      // "unknown"

    for [key, value] in point.entries() {
        print(key, value)
    }
}
```

Reading a missing key with `d["key"]` is an error; use `.get()` when the key may be absent. Keys do not keep the order they were added in, so do not rely on the order of `keys()`, `values()` or `entries()`.

### Copies and shared values

```naab
fn add_item(list) {
    list.push(99)
}

main {
    let a = [1, 2]
    let b = a                    // a copy
    b.push(3)
    print(a, b)                  // "[1, 2] [1, 2, 3]"

    add_item(a)                  // a function works on the caller's array
    print(a)                     // "[1, 2, 99]"

    let players = [{"name": "ann", "hp": 10}]
    for p in players {
        p["hp"] = p["hp"] - 1    // the loop variable is the element itself
    }
    print(players[0]["hp"])      // 9

    let first = players[0]       // a copy again
    first["hp"] = 0
    print(players[0]["hp"])      // 9

    players[0]["hp"] = 50        // change a nested value in place
    print(players[0]["hp"])      // 50
}
```

`let b = a` copies an array or dictionary. Passing one to a function, or looping over it with `for`, does not.

---

## 5. Control flow

```naab
main {
    let score = 85
    if score >= 90 {
        print("A")
    } else if score >= 80 {
        print("B")
    } else {
        print("C")
    }

    let grade = if score >= 80 { "pass" } else { "fail" }
    print(grade)  // "pass"

    let count = 0
    while count < 3 {
        count = count + 1
    }
    print(count)  // 3

    for n in [1, 2, 3, 4, 5] {
        if n == 2 { continue }
        if n == 4 { break }
        print(n)
    }

    let total = 0
    for i in 0..4 {
        total += i
    }
    print(total)  // 6

    let status = 404
    let message = match status {
        200 => "OK"
        404 => "Not Found"
        n if n >= 500 => "Server Error"
        _ => "Unknown"
    }
    print(message)  // "Not Found"
}
```

```text
B
pass
3
1
3
6
Not Found
```

- Parentheses around a condition are optional.
- `0..4` counts 0, 1, 2, 3; `range(0, 4)` does the same.
- `match` takes literals, guards (`n if n >= 500`), array patterns and `_`; the [tour](docs/language-tour.md#pattern-matching) shows them all.
- A loop that runs more than `limits.execution.loop_iterations` times (1,000,000 in a new project) is stopped with exit code 3. The whole run is stopped after `limits.timeout.global` seconds (60 in a new project). `--timeout N` can raise that limit for one run, but not lower it.

---

## 6. Functions

```naab
fn factorial(n) {
    if n <= 1 {
        return 1
    }
    return n * factorial(n - 1)
}

fn apply(f, value) {
    return f(value)
}

fn divmod(a, b) {
    return [int(a / b), a % b]
}

fn make_counter() {
    let count = 0
    return fn() {
        count = count + 1
        return count
    }
}

fn greet(name = "World") {
    return "Hello, " + name + "!"
}

fn twice(s) { return s + " " + s }
fn exclaim(s) { return s + "!" }

main {
    print(factorial(5))                            // 120
    print(apply(fn(x) { return x * 10 }, 4))       // 40

    let [q, r] = divmod(17, 5)
    print(q, r)                                    // "3 2"

    let counter = make_counter()
    counter()
    print(counter())                               // 2

    print(greet())                                 // "Hello, World!"
    print(greet("Ada"))                            // "Hello, Ada!"

    print("hi" |> twice |> exclaim)                // "hi hi!"
}
```

- Functions are values: pass them, return them, store them.
- `fn(x) { ... }` is an anonymous function, and it keeps access to the variables around it (`make_counter`).
- `value |> f` is `f(value)`. The right-hand side must be one of your own functions; piping into a standard-library function does not work yet.
- A parameter with a default (`name = "World"`) may be left out of a call. The default is evaluated on every call and may use earlier parameters.

---

## 7. Structs and enums

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

fn describe(c) {
    return match c {
        Color.Red => "warm"
        Color.Green => "natural"
        _ => "cool"
    }
}

main {
    let p = new Point { x: 3, y: 4 }
    p.x = 10
    print(p.x, p.y)              // "10 4"
    print(describe(Color.Green)) // "natural"
    print(Color.Blue == Color.Blue) // "true"
}
```

Interfaces, and functions attached to a struct (`fn Point.to_string(p)`), are in the [tour](docs/language-tour.md#interfaces).

---

## 8. Errors

```naab
fn parse_age(text) {
    let n = int(text)
    if n < 0 {
        throw "age cannot be negative"
    }
    return n
}

main {
    try {
        parse_age("-3")
    } catch (e) {
        print("caught: " + e)   // "caught: age cannot be negative"
    } finally {
        print("done")           // "done"
    }

    try {
        throw {"code": 42, "reason": "custom"}
    } catch (e) {
        print(e["code"])        // 42
    }

    try {
        let d = {"a": 1}
        print(d["missing"])
    } catch (e) {
        print(typeof(e))        // "dict"
    }
}
```

- `throw` takes any value, and `catch` receives it unchanged.
- An error raised by NAAb itself (a missing key, a failed conversion) arrives as a dictionary whose `message` holds the full error text.
- An error raised inside a block of another language can be caught the same way ([tour](docs/language-tour.md#error-handling)).
- A policy refusal at HARD level cannot be caught: the program stops with exit code 3. See [Section 12](#12-working-with-the-policy).

---

## 9. Modules

A module is a `.naab` file whose shareable functions are marked `export`. Save this as `lib/text_utils.naab`:

```naab
// lib/text_utils.naab
export fn initials(name) {
    let parts = name.split(" ")
    let result = ""
    for part in parts {
        result = result + part[0]
    }
    return result
}

export fn shout(text) {
    return text.upper() + "!"
}
```

And this as `main.naab`, beside the `lib` directory:

```naab
import {initials, shout} from "./lib/text_utils.naab"
import * as text from "./lib/text_utils.naab"

main {
    print(initials("Grace Brewster Hopper"))   // "GBH"
    print(shout("ready"))                      // "READY!"
    print(text.initials("Ada Lovelace"))       // "AL"
}
```

- `import {a, b} from "PATH"` brings in named functions, and `import {a as b} from "PATH"` renames one. `import * as m from "PATH"` brings in the whole module as `m`.
- Only exported functions can be imported.
- An imported module gets the same content checks as the file that imports it (secrets, placeholders, incomplete logic).

---

## 10. Files, environment and network

What a program may touch is set in two places: `capabilities` in `govern.json`, and the runtime sandbox.

The sandbox has four levels: `restricted`, `standard`, `elevated` and `unrestricted`. A new project runs at `standard`, which allows reading and writing files in the project directory and the temp directory. A program that reads environment variables or runs shell blocks needs `elevated`, set at the top level of `govern.json`:

```json
"security": {
  "sandbox_level": "elevated"
}
```

`elevated` relaxes only the sandbox. Everything in `capabilities` still applies.

In a new project:

| Action | New project | To allow it |
|---|---|---|
| Read a file in the project | allowed | |
| Write a file in the project | refused | `capabilities.filesystem.mode: "write"` |
| Read an environment variable | refused by the sandbox | `elevated` |
| Read `AWS_SECRET_ACCESS_KEY` or `GITHUB_TOKEN` | refused | they are in `capabilities.env_vars.blocked_read` |
| Network access | refused | `capabilities.network.enabled: true` |
| Write `govern.json` | refused | never: the policy file is protected |

### Reading files

```naab
use file
use json

main {
    let manifest = file.read("naab.toml")
    print(manifest.contains("[package]"))   // "true"
    print(file.exists("naab.toml"))         // "true"
    print(file.exists("missing.txt"))       // "false"

    let data = json.parse("{\"items\": [1, 2, 3]}")
    print(data["items"].length())           // 3
    print(json.stringify([1, 2, 3]))        // "[1,2,3]"
}
```

### Writing files

Change `capabilities.filesystem.mode` in `govern.json` from `"read"` to `"write"`:

```json
"filesystem": {
  "mode": "write",
  ...
}
```

```naab
use file

main {
    file.write("notes.txt", "first line\n")
    file.append("notes.txt", "second line\n")
    let lines = file.read_lines("notes.txt")
    print(lines.length())  // 2
    print(lines[1])        // "second line"
}
```

Paths listed in `capabilities.filesystem.blocked_paths` stay refused, and so do `govern.json`, its signature file and the trusted-keys directory, whatever the mode.

### Environment variables

The `standard` sandbox refuses `env.get()`. With `elevated` set:

```naab
use env

main {
    let path = env.get("PATH")
    print(path != null)                         // "true"
    print(env.get("NO_SUCH_VARIABLE") ?? "unset")  // "unset"
}
```

`env.get()` returns null for a variable that is not set. A variable in `capabilities.env_vars.blocked_read` is refused even at `elevated`, and it is also withheld from every process the program starts.

### Network

A new project has `capabilities.network.enabled: false`, so `http.get()` stops the program with exit code 3. Set it to `true` to allow requests, and use `allowed_hosts` / `blocked_hosts` to say where. `http.get(url)` returns a dictionary with `status`, `body` and `headers`.

---

## 11. Other languages

A block written as `<<language ... >>` runs code in another language. The closing `>>` must start its own line. A language runs only if `languages.allowed` in `govern.json` lists it and its interpreter or compiler is installed. A new project allows `python` and `shell`.

```naab
main {
    let numbers = [3, 1, 4, 1, 5]
    let stats = <<python[numbers]
import statistics
{"mean": statistics.mean(numbers), "max": max(numbers)}
>>
    print(stats["mean"])  // 2.8
    print(stats["max"])   // 5

    let name = "ada"
    let upper = <<python[name]
name.upper()
>>
    print(upper)          // "ADA"
}
```

- `<<python[numbers]` passes NAAb variables into the block.
- The block's last expression comes back as a NAAb value: a number, string, list or dict.
- `naab_return()`, `-> JSON` blocks, persistent runtimes and SQL are in the [tour](docs/language-tour.md#polyglot-blocks).

### Another language

To use JavaScript, add it to `languages.allowed`:

```json
"languages": {
  "allowed": ["python", "shell", "javascript"],
  ...
}
```

```naab
main {
    let total = <<javascript
[1, 2, 3, 4].reduce((sum, x) => sum + x, 0)
>>
    print(total)  // 10
}
```

A language that is not listed is refused before the program starts:

```text
Error: Governance error: Language "ruby" is not allowed [HARD-MANDATORY]

  At: line 2: <<ruby
  Rule: languages.allowed = [shell, python]

  Help:
  - Only shell, python polyglot blocks are permitted
  - To allow ruby, add it to the "allowed" array in govern.json
```

### Shell blocks

A new project lists `shell` in `languages.allowed`, but the `standard` sandbox refuses to start a shell ("shell execution denied by sandbox"). Shell blocks need `security.sandbox_level: "elevated"`, as for environment variables above:

```naab
main {
    let greeting = <<shell
echo "hello from shell"
>>
    print(greeting)  // "hello from shell"
}
```

A shell block runs as a separate process. The filesystem path rules do not reach it: that is what the CONTRA-013 warning says. `capabilities.shell.enabled: false` refuses shell, and every other language that runs as a separate process, outright.

### What the per-language rules catch

Each language has its own rules under `languages.per_language`. In a new project, Python may not call `eval()` or `exec()`, and `import os` is refused as a code-injection risk: import only what you need (`from os import path`). A block that is not inside `try`/`catch` gets an advisory note ("Polyglot blocks without try/catch").

---

## 12. Working with the policy

### Reading a refusal

This program leaves a `TODO` in a function:

```naab
// Refused: exit 3
fn total(items) {
    // TODO: handle discounts
    let sum = 0
    for item in items {
        sum = sum + item
    }
    return sum
}

main {
    print(total([1, 2, 3]))
}
```

```text
Error: Governance error: Placeholder "TODO" found in code [SOFT-MANDATORY]

  At: line 2: // TODO: handle discounts
  Rule: code_quality.no_placeholders = <redacted>

  Help:
  - Code must be complete — no placeholder markers allowed.
  - Implement the actual functionality instead of deferring.
  - The most common fix: delete the comment and write real logic.
  ...
```

- The first line says what was found and the level, here SOFT.
- `At:` gives the line where the enclosing function starts, then the offending text.
- `Rule:` names the `govern.json` key that decided it.
- `Help:` and `Example:` say how to fix the code.

Nothing ran: the checks happen before the program starts.

### Levels

Every rule has a level:

| Level | What happens |
|---|---|
| HARD | Stops the program (exit 3). `try`/`catch` cannot catch it and `--governance-override` does not lift it. |
| SOFT | Stops the program (exit 3) unless it is run with `--governance-override`. |
| ADVISORY | Prints a warning and continues. |
| DETECT | Stops the operation, but `try`/`catch` can catch it. For test configurations. |

The `TODO` above is SOFT, so `naab-lang total.naab --governance-override` runs it and prints `6`. A Python block that swallows an exception is HARD in a new project:

```naab
// Refused: exit 3
main {
    let size = <<python
from os import path
try:
    n = path.getsize("naab.toml")
except Exception:
    pass
n
>>
    print(size)
}
```

```text
Error: Governance error: Incomplete logic detected: "except Exception:
    pass" [HARD-MANDATORY]

  Rule: code_quality.no_incomplete_logic
```

`--governance-override` changes nothing here: the program is refused with exit code 3 either way.

### Pre-flight notes

Before a program runs, the scanner may print `[scanner] Pre-flight notes`: advisory style findings, each with a line number and a suggested fix. They do not stop the program.

### Changing the policy

`govern.json` is an ordinary JSON file in the project, and the policy is whatever it says. A few ways to start:

- `naab-lang init --governance-preset relaxed|standard|strict|paranoid` writes a stricter or looser file. Every preset turns taint tracking and sequence detection off unless `--taint` is also given; plain `naab-lang init` turns both on.
- [`govern-template.json`](govern-template.json) lists every key, each with its default and a `_comment_` explaining it.
- `"mode"` is `"enforce"` (refusals stop the program), `"audit"` (they are reported and the program continues) or `"off"`.
- A signed `govern.json` cannot be edited without the change being detected. The [README](README.md#how-it-works) shows the signing commands.

### Reports

```bash
naab-lang app.naab --governance-verbose             # every check and its result
naab-lang app.naab --governance-report report.json  # JSON
naab-lang app.naab --governance-sarif results.sarif # SARIF, for GitHub code scanning
naab-lang app.naab --governance-junit results.xml   # JUnit XML
```

Options may go before or after the file name.

---

## 13. Tools

| Command | What it does |
|---|---|
| `naab-lang FILE` | Run a program (also `naab-lang run FILE`) |
| `naab-lang` | Start the REPL |
| `naab-lang check FILE` | Check type annotations without running |
| `naab-lang fmt FILE` | Format the file in place |
| `naab-lang parse FILE` | Parse and summarise (imports, functions, whether there is a `main`) |
| `naab-gov lint FILE` | Run the policy checks without running the program (exit 3 on a refusal) |
| `naab-lang init` | Create `naab.toml` and `govern.json` |
| `naab-lang FILE --timeout N` | Allow this run N seconds, when N is above `limits.timeout.global` |
| `naab-lang --help` | Every command and option |

---

## 14. Not working yet

These do not work as written yet:

- **`const`** declares a variable but does not prevent reassignment.
- **Spread and rest** (`[...a]`, `f(...args)`) do not parse.
- **Piping into a standard-library function** (`x |> string.upper()`) does not work; pipe into your own functions.
- **Type annotations** are checked by `naab-lang check`, not at run time.
- **`time.sleep()`** is not interrupted by the time limit: one long sleep runs to the end before the run is stopped.

---

## Where next

- [Language tour](docs/language-tour.md): every feature, polyglot blocks in depth, and the standard-library table.
- [README](README.md): the governance engine, agents, and configuration.
- [Chapter 21: Governance](docs/book/chapter21.md): the governance reference.
- [examples/](examples/): complete programs.
