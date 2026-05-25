# NAAb Language — Internal Reference

Reference for Claude Code when working on the NAAb language source at `~/.naab/language/`.

## Build

```bash
cd ~/.naab/language/build && cmake .. && make naab-lang -j4
```

- Debug (default): `cmake ..`
- Release: `cmake .. -DCMAKE_BUILD_TYPE=Release`
- After modifying CMakeLists.txt: `cmake ..` again before `make`

Binary lands at `build/naab-lang`.

## Test

```bash
# Full suite — 391 tests, 1 pre-existing failure (test_bsd_cdd_fixes.sh)
cd ~/.naab/language && bash run-all-tests.sh

# Security leak check — 112 checks, 0 failures
bash tests/security/test_error_msg_leaks.sh
```

Test categories in `tests/`: governance_v4, security, stdlib, vm, cli, e2e, integration, bugs, gorilla, scanner, polyglot, formatter, lsp, platform, chaos, robustness.

Expected breakdown: ~332 pass, ~48 error-behavior (intentional failures), ~11 needs-tree-walk (VM-unsupported features).

Run a single test: `./build/naab-lang tests/path/to/test.naab`

## Source Layout

```
src/
├── lexer/          Tokenizer — keywords map, readString(), readInlineCode()
├── parser/         Recursive descent — parsePrimary(), error_hints
├── interpreter/    Tree-walker — visitor pattern, call_dispatch, governance_taint
├── vm/             Bytecode VM — stack-based dispatch loop, compiler (AST→bytecode)
├── runtime/        Governance engine, polyglot executors, trust_store, crypto_utils
├── stdlib/         20 modules (*_impl.cpp): array, string, math, file, json, csv,
│                   dict, path, env, time, regex, crypto, http, log, uuid, validate,
│                   process, debug, bolo, agent
├── cli/            main.cpp entry point — CLI flag parsing, subcommands
├── scanner/        C++ security scanner (SARIF output)
├── libnaab-governance/  C API for external agent framework integration (Go, Rust, Java, C# bindings)
├── api/            REST API (rest_api.cpp)
├── repl/           REPL with readline support
├── linter/         Static analysis
├── formatter/      Code formatter
├── analyzer/       Semantic analysis
├── platform/       OS abstraction (platform_posix.cpp, platform_win32.cpp)
├── profiling/      Performance profiling
├── testing/        Test framework internals
├── debugger/       Debugger implementation
├── utils/          Shared utilities
├── doc/            Doc generator
├── manifest/       Package manifest handling
├── packages/       Package manager
└── semantic/       Semantic analysis passes
include/naab/       All headers
```

## Key Architecture

### NaabVal (NaN-boxed values)
- `include/naab/naab_val.h` + `src/interpreter/naab_val.cpp`
- 8-byte inline values for int, double, bool, null
- Factory: `NaabVal::makeInt(42)`, `NaabVal::makeString("hi")`, `NaabVal::makeBool(true)`
- Type check: `val.isInt()`, `val.isString()`, `val.isBool()`, `val.isNull()`
- Extract: `val.asInt()`, `val.asString()`, `val.asBool()`
- Bridge: `NaabVal::fromLegacy(shared_ptr<Value>)`, `val.toLegacy()`
- Null check: `val.isNull()` — NOT `if (val)`

### Interpreter
- `src/interpreter/interpreter.cpp` — main visitor
- `src/interpreter/call_dispatch.cpp` — function call routing
- `src/interpreter/expressions.cpp` — expression evaluation
- `result_` (NaabVal) — last evaluated value
- `current_env_` — current scope (shared_ptr\<Environment>)
- `global_env_` — global scope
- `current_file_` — current source file (NOT `filename_`)
- Control flow: `returning_`, `breaking_`, `loop_depth_`
- Environment class is at `interpreter.h:371` — NOT `environment.h` (unused)

### Bytecode VM
- `include/naab/vm.h` + `src/vm/vm.cpp` — stack-based dispatch
- `include/naab/compiler.h` + `src/vm/compiler.cpp` — AST to bytecode
- VM is default engine (`global_use_vm = true`), tree-walker via `--tree-walk`
- Computed goto dispatch on GCC/Clang, switch fallback elsewhere
- VM taint tracking: `taint_stack_` mirrors value stack

### Governance Engine
- `src/runtime/governance_engine.cpp` — main engine, signature verification
- `src/runtime/governance_checks.cpp` — 50+ individual checks
- `src/runtime/governance_config.cpp` — config loading from govern.json
- `src/runtime/governance_taint.cpp` — taint tracking (interpreter path)
- `src/runtime/trust_store.cpp` — Ed25519 trusted key management
- `src/runtime/crypto_utils.cpp` — Ed25519 sign/verify, SHA-256
- Behavioral contracts: `must_call` (function must call specified functions — regex `\bname\s*\(` on body text, non-transitive), `must_contain` (function body must match syntax patterns)
- Anti-gaming: magic number / hardcoded constant detection in polyglot blocks, oversimplification checks, complexity floor
- Module governance: VM compiler skips function-body governance checks during module loading (`!skip_main_` guard in `compiler.cpp`), matching tree-walker's `module_loading_depth_ == 0` guard
- Enforcement tiers: HARD (block), SOFT (block + override), ADVISORY (warn)
- Exit codes: 0=success, 1=runtime, 2=quality gate, 3=HARD governance block, 4=config error
- Decision rationale: govern.json sections accept optional `rationale` field; engine generates `decision_trace` per check. Both flow into all 5 report formats + audit trail via `CheckResult.rationale` and `CheckResult.decision_trace`
- Decision trace storage: `t_current_decision_trace` is `static thread_local` in `governance_engine.cpp` (NOT a class member) — thread-safe for concurrent polyglot/agent threads
- Mid-run reload (Governance Under Survivability): `reloadIfChanged()` detects govern.json mtime changes during agent turns, validates signature, enforces one-way ratchet (only tightening allowed), and surfaces structured notices via `agent.send()` return dict `governance_notices` field. Reload is triggered automatically before each `agent.send()` and `agent.batch()` call.
- Fail-closed enforcement: govern.json capabilities sync to sandbox at load and mid-run reload. `capabilities.shell.enabled: false` removes SYS_EXEC from sandbox; `capabilities.network.enabled: false` removes NET_CONNECT; `capabilities.filesystem.mode: "none"` removes FS_READ+FS_WRITE. Enforce mode defaults to `standard` sandbox (not `unrestricted`). Agent worker threads inherit sandbox config from main thread.

### Polyglot Execution
- `src/runtime/*_executor.cpp` — 12 language executors (Python, JS, Go, Rust, C++, C#, Nim, Shell, Ruby, PHP, Julia, Zig)
- `src/runtime/language_registry.cpp` — executor registration
- `<<python ... >>` syntax — `>>` must be at line start to close block
- Executor base: `executeWithReturn()`/`callFunction()` use NaabVal

## Conventions

### Error Messages
```cpp
throw std::runtime_error(
    "Category error: What went wrong\n\n"
    "  Got: <actual>\n  Expected: <expected>\n\n"
    "  Help:\n  - Explanation\n\n"
    "  Example:\n    x Wrong: bad_code\n    v Right: good_code\n"
);
```

**Security rule:** Error messages must NEVER leak bypass flags, sanitizer lists, config key paths, or governance internals. `tests/security/test_error_msg_leaks.sh` enforces this — run it after modifying any error text.

### govern.json is Primary
All settings belong in govern.json first. CLI flags are overrides only. Never add behavior that requires a CLI flag without a govern.json equivalent.

### Adding a New Stdlib Function
1. Add implementation in `src/stdlib/<module>_impl.cpp`
2. Register in the module's init function
3. Add error messages following the `"  Help:\n  - ..."` format
4. Run `bash run-all-tests.sh` to verify

### Adding a New AST Node
1. Add to `NodeKind` enum in `include/naab/ast.h`
2. Create class extending `Expr`/`Stmt` in `ast.h`, add `accept()` in `ast_nodes.cpp`
3. Add `visit()` to `ASTVisitor` in `ast.h`, declare in `interpreter.h`
4. Implement visitor in `interpreter.cpp` (use NaabVal for result_)
5. Add compiler support in `src/vm/compiler.cpp` + VM opcode in `src/vm/vm.cpp`
6. Add parser rule in `src/parser/parser.cpp`, hook into appropriate parse method

### Adding a Governance Check
1. Add check function in `src/runtime/governance_checks.cpp` — call `clearTrace()` as first statement (or right after the enabled guard); call `addTrace()` for key decisions
2. Wire into governance engine dispatch
3. Add config key to governance_config.cpp — include `parseRationale()` call
4. Add `std::string rationale;` to the config struct in `governance.h`
5. Add rule_name mapping in `lookupRationale()` in `governance_engine.cpp`
6. Run security test after: `bash tests/security/test_error_msg_leaks.sh`
7. Never include bypass instructions in the error message

### Modifying Error Messages
Always run `bash tests/security/test_error_msg_leaks.sh` after changing any error text. The test scans all error strings for leaked bypass flags like `--no-governance`, `--governance-override`, sanitizer function names, etc.

## CLI Flags (Key Subset)

| Flag | Purpose |
|------|---------|
| `--tree-walk` | Use tree-walker instead of VM |
| `--governance-dashboard` | Print governance summary to stderr |
| `--governance-report` | Detailed governance report |
| `--governance-sarif` | SARIF format output |
| `--no-governance` | Skip governance (dev only) |
| `--gc-threshold N` | GC allocation threshold |
| `--gc-stats` | Print GC statistics |
| `--keygen PATH` | Generate Ed25519 signing keypair |
| `--sign-governance` | Sign govern.json |
| `--sign-baseline` | Sign drift baseline |
| `--trust-key PATH` | Install a public key |
| `--list-keys` | List trusted key fingerprints |
| `--env NAME` | Select governance environment |
| `--timeout N` | Execution timeout (seconds) |

## Gotchas

- **Two Environment classes exist** — use `interpreter.h:371`, not `environment.h`
- **nlohmann/json.hpp** — keep in .cpp only, never in headers
- **`result_`** is NaabVal — use `.isNull()` not `if (result_)`
- **CLI flags must be in BOTH** global pre-scan AND run command flag loop in main.cpp
- **VM taint** mirrors interpreter taint but uses `taint_stack_` — changes must be made in both paths
- **`current_file_`** not `filename_` for the current source file
- **No Julia/Zig on Termux** — tests skip gracefully
- **Polyglot `>>` delimiter** must be at line start (after optional whitespace)
- **`t_current_decision_trace`** is `static thread_local` in governance_engine.cpp — not a class member in the header
- **Bare dict keys** — `{name: "val"}` is equivalent to `{"name": "val"}`. Unquoted keys are syntactic sugar.
- **`??` in match arms** — null coalescing works inside match arm bodies: `"x" => d.get("a") ?? "default"`
- **Top-level `const`/`let` parse error** — NAAb only allows `use`, `import`, `export`, `struct`, `enum`, `fn`, and `main {}` at file scope. Constants and variables MUST be declared inside `main {}` or a function.
- **Sandbox fail-closed on `mode: enforce`** — if govern.json has `"mode": "enforce"` but no `security.sandbox_level`, the runtime silently upgrades the sandbox from `unrestricted` → `standard`. This blocks `file.read("/absolute/path")` and other absolute-path operations. Set `"security": { "sandbox_level": "elevated" }` to restore access. The runtime logs `[governance] Sandbox: upgraded unrestricted → standard` to stderr when this happens.
