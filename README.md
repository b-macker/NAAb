# NAAb — Stop AI Agents Before They Do Damage

[![CI](https://github.com/b-macker/NAAb/actions/workflows/ci.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/ci.yml)
[![Sanitizers](https://github.com/b-macker/NAAb/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/sanitizers.yml)
[![CodeQL](https://github.com/b-macker/NAAb/actions/workflows/codeql.yml/badge.svg)](https://github.com/b-macker/NAAb/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)
[![Discussions](https://img.shields.io/badge/Discussions-enabled-blue.svg)](https://github.com/b-macker/NAAb/discussions)

AI agents can read your secrets, encode them, and send them to an external API — all in three steps. NAAb can block that sequence **before the network call fires.**

```
$ naab-lang agent_task.naab
Error: Governance error: Behavioral sequence 'credential_exfiltration' blocked before execution: 'agent.send' would complete a dangerous 3-step pattern [HARD-MANDATORY]

  Rule: behavioral_sequences.credential_exfiltration

  Help:
  - This action would complete a known dangerous sequence.
  - The operation was blocked before execution.
```

The program read `API_SECRET_KEY` with `env.get`, base64-encoded it, and called `agent.send`. The send was refused before a request left the process, and the run exited with code 3. No prompt engineering, and no scanning of logs after the fact.

That refusal comes from a pattern you write in `govern.json` (below). Sequence detection is off until `behavioral_sequences.enabled` is true, and none of the built-in patterns covers this chain.

---

## How It Works

NAAb is a programming language with a governance engine built into the runtime. You write rules in `govern.json` — multi-step patterns, capability limits, secret scanning — and the engine enforces them while your code runs, not afterwards.

```json
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "behavioral_sequences": {
    "enabled": true,
    "patterns": [
      {
        "name": "credential_exfiltration",
        "sequence": ["env.get:*KEY*|*SECRET*|*TOKEN*", "encode|base64", "agent.send|http.post"],
        "level": "hard",
        "rationale": "Reading credentials then encoding and sending matches exfiltration."
      },
      {
        "name": "config_harvest",
        "sequence": ["file.read:*config*", "agent.send"],
        "level": "hard",
        "rationale": "Reading config files then sending content to an external agent."
      }
    ]
  }
}
```

Each step names an event (`env.get`, `file.read`, `agent.send`, `http.post`, ...) and can match its detail with a glob after `:`; `|` separates alternatives. The engine tracks events across the whole run, with a configurable gap between steps, and blocks the step that would complete the pattern. Listing `patterns` replaces the 16 built-in ones; those are advisory unless you set `"default_pattern_enforcement": "declared"`. `sandbox_level: "elevated"` lets the program read the environment at all: at `standard`, the default under `enforce`, the sandbox refuses `env.get` before any pattern is reached.

### What Else Gets Blocked

| Threat | How NAAb stops it |
|--------|-------------------|
| Credential exfiltration | A sequence pattern: `env.get` of a secret → encode → `agent.send`, refused before the send |
| Rewriting the policy | `govern.json`, its signature and the trusted keys are protected paths: the program cannot read or overwrite them through NAAb's standard library or embedded Python |
| Agent drift | Context Drift Detection scores 23 signals per turn (repetition, circular actions, mandate drift, failed validations, ...) into a coherence score; output admissibility can quarantine or block a reply whose score is too low |
| Secrets in agent traffic | With `code_quality.no_secrets` on, every prompt, response, tool argument and tool result is scanned against 19 secret formats, and a hit ends the run. It is off by default |
| Capability abuse | `shell.enabled: false` refuses every language that runs as a separate process; `network.enabled: false` and `filesystem.mode` are enforced by the runtime, not by the prompt |
| Tainted data reaching a sink | Taint tracking: a value from `env.get` reaching a shell block is refused, with the chain of where it came from |

### A Policy the Program Can't Rewrite

Rules live in `govern.json`, not in prompts. Sign it with Ed25519 and any later edit is an integrity block (exit 3) on the next run. A config change picked up during a run is accepted only if it tightens the rules (the ratchet).

```bash
mkdir -p keys
naab-lang --keygen ./keys/governance                          # writes keys/governance and keys/governance.pub
naab-lang --trust-key ./keys/governance.pub                   # trust the public key on this machine
naab-lang --signing-key ./keys/governance --sign-governance   # writes govern.json.sig
```

**What it does not contain.** Path rules reach NAAb's standard library and the embedded Python executor. They do not reach languages that run as a separate process (shell, Ruby, Node, PHP, ...) while shell is enabled; NAAb prints the advisory `CONTRA-013` when a config is in that state. The [protection map](docs/protection-map.md) records, for each language and forbidden action, which layer stops it: 39 of its 323 cells are open when run as root (36 as an ordinary user), all but four of the open and text-only cells in configurations where the project enables shell. See also the [security model](docs/SECURITY.md).

---

## See It In Action

<p align="center">
  <img src="demo/demo.svg" alt="NAAb Governance Demo — 7 scenes catching AI mistakes" width="800">
</p>

| Scene | AI Mistake | Governance Response |
|-------|-----------|-------------------|
| 1 | `.push()` in Python | Blocked — use `.append()` |
| 2 | `import fake_ai_toolkit` | Blocked — unknown module |
| 3 | `fn validate() { return true }` | Blocked — stub function |
| 4 | Hardcoded `sk-proj-...` API key | Blocked — secret detected |
| 5 | Empty `catch (e) { }` | Blocked — incomplete logic |
| 6 | `env.get()` → shell unsanitized | Blocked — taint violation |
| 7 | All issues fixed | Passes clean |

Run it yourself: `bash demo/governance_demo.sh` | Record: `bash demo/governance_demo.sh --record`

---

## The Problem: AI Code Drift

AI models generate code that looks right but isn't. Every session starts fresh — no memory of your security rules, naming conventions, or architecture decisions:

- **Hallucinated APIs** — `.push()` in Python, `print()` in JavaScript, `json.stringify()` instead of `json.dumps()`
- **Stubs shipped as "complete"** — `def validate(): return True`, functions with only `pass` or `NotImplementedError`
- **Security patterns bypassed** — hardcoded secrets, SQL injection, `except: pass` swallowing errors
- **Language misuse** — Python for heavy computation, JavaScript for file operations, Shell for complex logic

Prompts are suggestions. **`govern.json` is policy.** NAAb checks every polyglot block against your policy before it runs.

---

## At a Glance

| Capability | Details |
|---|---|
| **Governance Engine** | 50+ checks, four enforcement levels (hard / soft / advisory / detect), configured in `govern.json` |
| **Polyglot Execution** | 14 languages in one file — Python, JavaScript, TypeScript, Rust, C++, Go, C#, Ruby, PHP, Shell, Nim, Zig, Julia, and SQL (in-process SQLite) |
| **Smart Error Messages** | "Did you mean?" suggestions, and help text with a wrong and a right example |
| **Standard Library** | 25 modules — array, string, math, json, http, file, path, time, debug, dict, env, csv, regex, crypto, log, uuid, validate, process, io, collections, bolo, agent, governance, codegen, orchestra |
| **Language Features** | Generators/yield, interfaces, pattern matching with guards, f-strings, async/await, lambdas, closures, pipeline operator, destructuring, optional chaining |
| **CI/CD Integration** | SARIF (GitHub Code Scanning), JUnit XML (Jenkins/GitLab), JSON reports |
| **Bytecode VM** | Stack-based compiler + VM (the default engine), computed-goto dispatch, NaN-boxed values, constant folding. About 10x faster than the tree-walker on one recursion-and-loop benchmark (unoptimised build) |
| **Runtime Security** | Behavioral Sequence Detection, Context Drift Detection, taint tracking with lineage, subprocess containment |
| **Ed25519 Signing** | `--keygen`, `--sign-governance`, `--trust-key`; mid-run reloads can only tighten |
| **Agent Governance** | Per-agent roles and permissions, run budgets, standing leases, step-up challenges, output contracts, output admissibility, propose/commit, telemetry with a tamper-evident hash chain |
| **Live Guidance** | Project knowledge from markdown files, SQLite, HTTPS, commands or MCP servers, delivered to the agent or the terminal when governance sees the matching event |
| **MCP Server** | `naab-lang mcp` serves governed execution over the Model Context Protocol; each call runs in a fresh worker process |
| **Orchestration** | `orchestra` module — sequential refinement, consensus voting, convergence checks, admissible-candidate selection |
| **Dynamic Code Execution** | `codegen.run()` — runtime-generated code goes through the same checks as a static polyglot block |
| **Enterprise Features** | Policy inheritance (`extends`), telemetry forwarding to a webhook or SIEM, REST API with per-key scopes, governance C API with Go/Rust/Java/C#/Python bindings |

---

## Quick Start

```bash
# Clone and build
git clone --recursive https://github.com/b-macker/NAAb.git
cd NAAb
mkdir build && cd build
cmake ..
make naab-lang -j4

# Create a project: init writes naab.toml and a standard govern.json
mkdir ../hello && cd ../hello
../build/naab-lang init
```

```naab
// hello.naab
main {
    let name = "World"
    print("Hello, " + name + "!")
}
```

```bash
../build/naab-lang hello.naab      # run a file
../build/naab-lang                 # interactive REPL
```

NAAb looks for a `govern.json` in the program's directory and its parents, and refuses to run a program without one (exit 4). `naab-lang init --governance-preset relaxed|standard|strict|paranoid` picks how strict the generated file is; `--no-governance` waives the requirement, but a `govern.json` that is found still applies.

Detailed build instructions: [INSTALL.md](INSTALL.md) · Language guide: [USER_GUIDE.md](USER_GUIDE.md) · Release history: [CHANGELOG.md](CHANGELOG.md)

---

## Governance Engine

Drop a `govern.json` in your project root and every polyglot block is checked before it runs.

### What It Catches

| Category | Examples | Patterns |
|---|---|---|
| **Hallucinated APIs** | `.push()` in Python, `print()` in JS, `len()` in JS | 100+ patterns across languages |
| **Oversimplification** | `def validate(): return True`, `pass`-only bodies, identity functions | 60+ stub patterns |
| **Incomplete Logic** | `except: pass`, bare raises, `"something went wrong"` messages | 50+ patterns |
| **Security** | SQL injection, path traversal, shell injection, hardcoded secrets | 19 secret formats |
| **Code Quality** | TODO/FIXME, debug artifacts, mock data, hardcoded URLs/IPs | Dead code detection |
| **Taint Tracking** | Untrusted data (`env.get`, polyglot output) reaching sinks (shell, env) | Source/sink/sanitizer with prefix matching |
| **PII Exposure** | SSNs, credit card numbers, emails, phone numbers, IP addresses | Pattern-based, each kind switchable |

### Four Policy Levels

- **HARD** — Block execution. Code does not run. No override. Throws uncatchable `GovernanceHardError` (NAAb `try/catch` cannot intercept it). Process exits with code 3.
- **SOFT** — Block execution. Code does not run unless `--governance-override` is passed.
- **ADVISORY** — Warn and continue. Logged in audit trail. With advisory escalation enabled, a repeated advisory escalates to a HARD block.
- **DETECT** — Same detection as HARD but throws catchable exception. Used in test configurations where scripts verify violations via `try/catch`.

### govern.json Example

```json
{
  "version": "5.0",
  "mode": "enforce",

  "languages": {
    "allowed": ["python", "javascript", "go"],
    "blocked": ["php"]
  },

  "code_quality": {
    "no_secrets": { "level": "hard" },
    "no_sql_injection": { "level": "hard" },
    "no_oversimplification": { "level": "hard" },
    "no_incomplete_logic": { "level": "soft" },
    "no_hallucinated_apis": { "level": "soft" }
  },

  "restrictions": {
    "code_injection": { "level": "hard" },
    "shell_injection": { "level": "hard" }
  },

  "limits": {
    "code": { "max_lines_per_block": 200 },
    "timeout": { "global": 30 }
  }
}
```

Every setting, with its default and a comment: [`govern-template.json`](govern-template.json).

### Project Context Awareness

NAAb can read your existing project files and add rules from them — without overriding `govern.json`. Off by default; turn it on with `project_context.enabled`.

- **Layer 1:** LLM instruction files — `CLAUDE.md`, `.cursorrules`, `.github/copilot-instructions.md`, `gemini.md`
- **Layer 2:** Linter configs — `.editorconfig`, `.eslintrc`, `.prettierrc`, `biome.json`
- **Layer 3:** Project manifests — `package.json`, `Cargo.toml`, `go.mod`, `pyproject.toml`, `Gemfile`, `composer.json`

Each layer can be switched on and off.

### Custom Rules

Define your own regex-based governance rules:

```json
{
  "custom_rules": [
    {
      "id": "no-print-debugging",
      "name": "no_print_debugging",
      "pattern": "console\\.log|print\\(.*debug",
      "message": "Remove debug print statements before committing",
      "level": "soft"
    }
  ]
}
```

### CI/CD Integration

```bash
# GitHub Code Scanning (SARIF)
naab-lang app.naab --governance-sarif results.sarif

# Jenkins / GitLab CI (JUnit XML)
naab-lang app.naab --governance-junit results.xml

# Custom tooling (JSON)
naab-lang app.naab --governance-report results.json
```

The report flags go after the file name. `naab-gov lint app.naab` checks a file without running it.

> **[Build your govern.json interactively](https://b-macker.github.io/NAAb/governance.html)** | [Full governance reference (Chapter 21)](docs/book/chapter21.md)
>
> **[Security model & threat assumptions](docs/SECURITY.md)**

### Agent Governance

NAAb supports multi-agent environments where different AI agents have different permissions. Define per-agent configs in `govern.json`:

```json
{
  "agents": {
    "analyzer": {
      "provider": "gemini",
      "model": "gemini-2.5-flash",
      "api_key_env": "GEMINI_API_KEY",
      "system_prompt": "You are a code analyzer.",
      "max_turns": 20,
      "max_tokens": 4096,
      "allowed_languages": ["python", "javascript"],
      "allowed_actions": ["AGENT_SEND", "FS_READ"],
      "standing_lease_turns": 10,
      "risk_budget": 15,
      "output_contract": {
        "format": "json",
        "required_fields": ["severity", "category"],
        "regex_checks": { "severity": "^(low|medium|high|critical)$" }
      }
    }
  }
}
```

```bash
# Run with agent identity
naab-lang app.naab --agent-id analyzer

# View governance dashboard
naab-lang app.naab --agent-id analyzer --governance-dashboard
```

**Agent features:**
- **Standing Lease** — a time or turn limit on an agent's authorization. An expired lease forces a step-up challenge; passing it renews the lease.
- **Output Contracts** — check a reply's structure: required fields, field types, regexes (`format: "json"`).
- **Run Budgets** — `agent_dispatch.hard_stop` caps calls, tokens and time for the whole run, counting tool-loop turns, challenges and proposals too.
- **Risk Budget** — with `exposure_tracking.enabled`, each behavioral-sequence match and each turn on which a drift signal fires costs one unit, and admission refuses the next send when none is left.
- **Output Admissibility** — after drift scoring, a reply below the coherence threshold is quarantined, attested or blocked.
- **Propose / Commit** — `agent.propose()` generates candidate replies without changing any state; `agent.commit()` runs the full post-receive checks on the one you pick.
- **Validation** — `agent.record_validation(handle, passed)` feeds your test results into the agent's coherence score.
- **Key Rotation** — `api_key_env` accepts string or array. Dead keys (401) marked and rotated. `key_retry_after_seconds` enables revival.
- **Model Fallback** — `model` accepts string or array. On 404/503, next model in chain is tried.
- **Tool Execution** — `agent.register_tool()` + a governed tool loop: the tool must be listed in `govern.json` and registered, and its arguments and results are scanned.
- **Code Extraction** — `agent.extract_code(response, lang)` extracts code from markdown fences.
- **Environment Awareness** — `agent.environment(handle)` returns real-time config, state, and health.

### Runtime Security: BSD, CDD & Subprocess Containment

Beyond static checks, NAAb monitors runtime behavior:

- **Behavioral Sequence Detection (BSD)** — catches multi-step patterns like "read secret → encode → send", blocking the step that would complete one. Off by default. Your `patterns` replace the 16 built-in ones, which are advisory unless `default_pattern_enforcement` is `"declared"`. Write steps in event form (`agent.send`, `file.read`); the loader warns about a spelling no event matches.
- **Context Drift Detection (CDD)** — scores an agent conversation turn by turn on 23 signals: repeated failures, circular actions, scope creep, contradictions, response repetition, mandate alignment, failed validations and more. Each signal can be switched on or off globally or per agent, and an adaptive baseline learns what is normal for each agent before it penalises.
- **Taint Lineage** — every tainted value carries a chain showing where it was tainted and why, making governance violations actionable.
- **Governance Pulse** — real-time self-assessment (HEALTHY/DEGRADED/IMPAIRED) with hysteresis and evidence epochs. `governance.health()` returns instrumentation status.
- **Polyglot Subprocess Containment** — restrictions on child processes: no fork or exec at the `standard` sandbox level, a restricted PATH, environment scrubbing (variables in `env_vars.blocked_read` never reach a child), and a timeout with SIGKILL. It catches commands built at run time that a source scan cannot see.

### Live Guidance

`guidance.sources` lists where your project knowledge lives — markdown documents, a SQLite database, an `https://` URL, a command, or an MCP server. When governance observes something (an agent's first send, a drift signal, a failed validation, a violation), the matching section is delivered to the agent's next request or appended to the message shown in the terminal. Sources are re-read when they change, the governed program may not write them, and their text is scanned for secrets before delivery. See [docs/plan-live-guidance.md](docs/plan-live-guidance.md).

### MCP Server

`naab-lang mcp` serves governed execution over the Model Context Protocol on stdio, with the tools `naab_lease`, `naab_propose`, `naab_commit`, `naab_run` and `naab_verify_attestation`. No user code runs in the server process: every call runs in a fresh `naab-lang` worker, and behavioral-sequence events carry across calls from the same caller. See [docs/plan-mcp-service.md](docs/plan-mcp-service.md).

---

## Polyglot Execution

Use each language where it shines — Python for data science, Rust for performance, JavaScript for web, Go for concurrency, SQL for queries. Variables flow between languages automatically. No FFI, no serialization, no microservices.

**Supported languages:** Python · JavaScript · TypeScript · Rust · C++ · Go · C# · Ruby · PHP · Shell · Nim · Zig · Julia · SQL

A language runs only if `govern.json` allows it (`languages.allowed`) and its compiler or interpreter is installed. The standard `init` profile allows Python and Shell; the example below also needs `javascript` in that list.

> Zig and Julia executors ship with the runtime but are disabled by default in `naab.toml` — flip `zig = true` / `julia = true` under `[polyglot]` to enable them.

```naab
main {
    let numbers = [10, 20, 30, 40, 50]

    // Python: statistical analysis
    let stats = <<python[numbers]
import statistics
result = {
    "mean": statistics.mean(numbers),
    "stdev": statistics.stdev(numbers)
}
str(result)
>>

    // JavaScript: format as HTML
    let html = <<javascript
const data = "Stats: mean=30, stdev=15.81";
`<div class="result">${data}</div>`;
>>

    print(stats)
    print(html)
}
```

SQL blocks run in an in-process SQLite database with no filesystem access; the rows of the last query come back as a list of dicts:

```naab
main {
    let rows = <<sql
CREATE TABLE langs(name TEXT, year INT);
INSERT INTO langs VALUES ('python', 1991), ('go', 2009), ('rust', 2010);
SELECT name, year FROM langs WHERE year > 2000 ORDER BY year;
>>
    for row in rows {
        print(row["name"] + " " + string(row["year"]))
    }
}
```

### Key Polyglot Features

- **Variable binding** — Pass NAAb variables into polyglot blocks with `<<python[x, y]` (SQL receives them as bound parameters, never as text)
- **Persistent runtimes** — Keep interpreter state across multiple calls with the `runtime` keyword
- **JSON sovereign pipe** — Return structured data from any language with `naab_return()` or a `-> JSON` header
- **Error mapping** — Polyglot errors map back to NAAb source locations and flow into try/catch
- **Block-header awareness** — Go gets `package main`, PHP gets `<?php` automatically
- **Parallel execution** — on the tree-walker (`--tree-walk`), independent blocks run concurrently after a dependency analysis

---

## Language Features

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

## Standard Library

25 modules, with "Did you mean?" suggestions on a misspelled function.

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

## Developer Experience

### Smart Error Messages

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
- `console.log()` → "NAAb does not have a 'Console' object. Use: print(...)"
- `array.pussh()` → "Did you mean: push?", followed by the module's function list

### LLM-Friendly Syntax

NAAb accepts multiple keyword styles so AI-generated code works without manual edits:

- `function` / `func` / `fn` / `def` — all valid
- `let` / `const` — both declare a variable (`const` does not yet prevent reassignment)
- Semicolons — optional (accepted but not required)
- `return` — optional in single-expression functions

### Tooling

- **`naab-lsp`** — Language Server Protocol implementation (`tools/naab-lsp/`, `make naab-lsp`). Powers the [VS Code extension](vscode-naab/).
- **`naab-gov`** — standalone governance CLI (`src/cli/gov_main.cpp`, `make naab-gov`) for linting files and working with govern.json without running a program.
- **`naab-lang mcp`** — the MCP server described above.
- **Governance C API bindings** — embed the governance engine in other agent frameworks via `bindings/` (C#, Go, Java, Python, Rust), backed by the C API in `src/api/governance_c_api.cpp`.

---

## NAAb Ecosystem

Three tools built with NAAb — code governance, performance optimization, and data security:

| Project | Purpose | Key Features |
|---------|---------|--------------|
| **[NAAb BOLO](https://github.com/b-macker/naab-bolo)** | Code governance & validation | 50+ checks, SARIF reports, AI drift detection |
| **[NAAb Pivot](https://github.com/b-macker/naab-pivot)** | Code evolution & optimization | 3-60x speedups, proven correctness, 8 languages |
| **[NAAb Passage](https://github.com/b-macker/naab-passage)** | Data gateway & PII protection | Zero leakage, sovereign architecture, HIPAA/GDPR |

### NAAb BOLO — Code Governance & Validation

**[NAAb BOLO](https://github.com/b-macker/naab-bolo)** ("Be On the Lookout") catches oversimplified stubs, hallucinated APIs, and incomplete logic in AI-generated code.

```bash
# Scan for governance violations
naab-lang bolo.naab scan ./src --profile enterprise

# Generate SARIF report for CI
naab-lang bolo.naab report ./src --format sarif --output results.sarif

# AI governance validation
naab-lang bolo.naab ai-check ./ml-models
```

**50+ checks · 4 languages · 339 regression tests** → [Get started](https://github.com/b-macker/naab-bolo)

### NAAb Pivot — Code Evolution & Optimization

**[NAAb Pivot](https://github.com/b-macker/naab-pivot)** rewrites slow code in faster languages with mathematical proof of correctness.

```bash
# Analyze hotspots (Python → Rust candidates)
naab-lang pivot.naab analyze app.py

# Rewrite with proof
naab-lang pivot.naab rewrite app.py:expensive_loop --target rust --prove

# Result: 45x faster, semantically identical
```

**3-60x speedups · 8 source languages · Proven correct** → [Get started](https://github.com/b-macker/naab-pivot)

### NAAb Passage — Data Gateway & PII Protection

**[NAAb Passage](https://github.com/b-macker/naab-passage)** ensures zero PII leakage to LLMs, APIs, or untrusted systems with sovereign architecture.

```bash
# Start secure gateway
naab-lang main.naab

# All requests validated, PII blocked
curl -X POST http://localhost:8091/ -d '{"prompt": "SSN: 123-45-6789"}'
# → {"error": "POLICY_VIOLATION"}
```

**Zero leakage · Self-synthesizing · HIPAA/GDPR compliant** → [Get started](https://github.com/b-macker/naab-passage)

---

## Architecture

```
Source Code (.naab)
    |
  Lexer ──> Tokens
    |
  Parser ──> AST (recursive descent)
    |
  Governance Engine ──> Policy checks (govern.json)
    |
  ┌─────────────────────────────────────────────┐
  │  Compiler ──> Bytecode ──> VM (default)     │
  │       — OR —                                │
  │  Interpreter (visitor pattern, --tree-walk)  │
  └─────────────────────────────────────────────┘
    |
  ├── Native execution (NAAb code)
  ├── Python executor (embedded CPython; a subprocess when pybind11 is absent)
  ├── JavaScript executor (QuickJS, in-process)
  ├── SQL executor (SQLite, in-process)
  ├── Go/Rust/C++/C#/Nim/Zig/Julia executors (compile & run)
  ├── Ruby/PHP/TypeScript executors (interpreted)
  └── Shell executor (subprocess)
```

- About 158,000 lines of C++17 in `src/` and `include/`
- Over 440 test suites and programs in `run-all-tests.sh`
- 25 standard library modules
- Bytecode VM by default, tree-walker via `--tree-walk`
- Built with Abseil, fmt, spdlog, nlohmann/json, QuickJS, SQLite

---

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions and guidelines.

### Areas for Contribution
- VM optimization (`src/vm/`)
- Agent governance extensions (`src/runtime/governance_*.cpp`, `src/stdlib/agent_impl.cpp`)
- New standard library modules
- Documentation and tutorials
- IDE integrations (Vim, Emacs, IntelliJ, VS Code)
- Package registry (centralized module hosting)

---

## License

MIT License - see [LICENSE](LICENSE) for details.

**Brandon Mackert** - [@b-macker](https://github.com/b-macker)

---

_NAAb — Polyglot without the trip._
