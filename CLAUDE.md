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
# Full suite — 396 tests, 1 pre-existing failure (test_drift_detection.sh)
cd ~/.naab/language && bash run-all-tests.sh

# Security leak check — 738 checks, 0 failures
bash tests/security/test_error_msg_leaks.sh
```

Test categories in `tests/`: governance_v4, security, stdlib, vm, cli, e2e, integration, bugs, gorilla, scanner, polyglot, formatter, lsp, platform, chaos, robustness.

Expected breakdown: ~334 pass, ~51 error-behavior (intentional failures), ~11 needs-tree-walk (VM-unsupported features).

Run a single test: `./build/naab-lang tests/path/to/test.naab`

## Source Layout

```
src/
├── lexer/          Tokenizer — keywords map, readString(), readInlineCode()
├── parser/         Recursive descent — parsePrimary(), error_hints
├── interpreter/    Tree-walker — visitor pattern, call_dispatch, governance_taint
├── vm/             Bytecode VM — stack-based dispatch loop, compiler (AST→bytecode)
├── runtime/        Governance engine, polyglot executors, trust_store, crypto_utils
├── stdlib/         22 modules (*_impl.cpp): array, string, math, file, json, csv,
│                   dict, path, env, time, regex, crypto, http, log, uuid, validate,
│                   process, debug, bolo, agent, codegen, orchestra
├── cli/            main.cpp entry point — CLI flag parsing, subcommands
├── scanner/        C++ security scanner (SARIF output, 18 code quality checks)
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
- Behavioral contracts: `must_call` (function must call specified functions — regex `\bname\s*\(` on body text, non-transitive), `must_contain` (function body must match syntax patterns), `must_produce` (golden tests with type-strict comparison — string "0" does not match int 0), `min_arity`/`max_arity` (parameter count enforcement)
- Anti-gaming: magic number / hardcoded constant detection in polyglot blocks, oversimplification checks, complexity floor
- Module governance: VM compiler skips function-body governance checks during module loading (`!skip_main_` guard in `compiler.cpp`), matching tree-walker's `module_loading_depth_ == 0` guard
- Enforcement tiers: HARD (block, exit 3), SOFT (block unless `--governance-override`, exit 3 without override), ADVISORY (warn, continue), DETECT (block but catchable by NAAb try/catch — for test configs only)
- Exit codes: 0=success, 1=runtime, 2=quality gate, 3=HARD governance block, 4=config error
- **GovernanceHardError** (`governance.h:2157`): `enforce()` throws `GovernanceHardError` (inherits `std::runtime_error`) for all HARD-level blocks. NAAb `try/catch` cannot catch it — both tree-walker (`interpreter.cpp`: `visit(TryCatchExpr)`, `visit(TryStmt)` outer+inner) and VM (`vm.cpp`: dispatch loop + 3 inner catches) explicitly re-throw before generic handlers. `main.cpp` catches it and calls `_exit(3)`. When adding new catch sites that could intercept stdlib/governance exceptions, always add `catch (const governance::GovernanceHardError&) { throw; }` BEFORE `catch (const std::exception&)`. ADVISORY (non-escalated) and SOFT-with-override remain catchable.
- **env_vars enforcement**: `capabilities.env_vars` in govern.json — `blocked_read` (HARD), `allowed_read` (SOFT allowlist), `blocked_write`/`allowed_write`. Enforced in all 9 env stdlib access points via `checkEnvVarRead()`/`checkEnvVarWrite()` in `governance_engine.cpp`. blocked_read prevents value from ever entering memory (check fires before `std::getenv`).
- Decision rationale: govern.json sections accept optional `rationale` field; engine generates `decision_trace` per check. Both flow into all 5 report formats + audit trail via `CheckResult.rationale` and `CheckResult.decision_trace`
- Decision trace storage: `t_current_decision_trace` is `static thread_local` in `governance_engine.cpp` (NOT a class member) — thread-safe for concurrent polyglot/agent threads
- Mid-run reload (Governance Under Survivability): `reloadIfChanged()` detects govern.json mtime changes during agent turns, validates signature, enforces one-way ratchet (only tightening allowed), and surfaces structured notices via `agent.send()` return dict `governance_notices` field. Reload is triggered automatically before each `agent.send()` and `agent.batch()` call.
- Fail-closed enforcement: govern.json capabilities sync to sandbox at load and mid-run reload. `capabilities.shell.enabled: false` removes SYS_EXEC from sandbox; `capabilities.network.enabled: false` removes NET_CONNECT; `capabilities.filesystem.mode: "none"` removes FS_READ+FS_WRITE. Enforce mode defaults to `standard` sandbox (not `unrestricted`). Agent worker threads inherit sandbox config from main thread.
- Continuous governance (6 features):
  - **Handle anti-forge**: HMAC-SHA256 nonce ties `agent.create()` handle to server-side state. `validateHandle()` uses constant-time comparison. Forgery/replay/mutation rejected. Process secret from `/dev/urandom`.
  - **Temporal trust decay**: coherence erodes over time when idle. Config: `context_drift.temporal_decay_enabled`, `temporal_decay_per_minute`, `temporal_decay_grace_minutes`. Default off.
  - **Credential refresh**: dead API keys revive after cooldown. Config: `retry.key_retry_after_seconds` (0 = never revive). `isKeyDead()` helper used at all `dead_keys` call sites. AGENT_KEY_REVIVED telemetry on revival.
  - **Adaptive baselining**: per-agent baseline window observes normal signal rates before penalizing. Penalties only fire when signals exceed `mean + k*stddev`. Config: `context_drift.adaptive_baseline_enabled`, `adaptive_baseline_window`, `adaptive_baseline_sensitivity`. Default off.
  - **Step-up challenges**: at elevated governance levels, inject challenge prompt and score response (word count + keyword overlap with system_prompt). Pass recovers coherence, fail blocks send. Config: `circuit_breaker.step_up_enabled`, `step_up_at_level`, `step_up_challenge`, `step_up_min_words`, `step_up_cooldown_turns`, `step_up_keyword_threshold`. Challenge state tracked in AgentTracker (server-side).
  - **Separation of duties**: per-agent `network_allowed` (bool, same pattern as `shell_allowed`) and `allowed_actions` matrix (`["SHELL_EXEC", "NET_CONNECT", "FS_READ", "FS_WRITE", "AGENT_SEND", "TOOL_EXEC"]`). Enforced in `checkNetworkAllowed()`, `checkShellAllowed()`, `checkFilesystemAllowed()`, and `agentSend()`. `TOOL_EXEC` controls tool execution in agent tool loops. Ratchet enforcement prevents mid-run loosening of action matrix.
- **Governance Pulse** (`src/runtime/governance_engine.cpp`): PulseVerdict (HEALTHY/DEGRADED/IMPAIRED) with hysteresis, two-phase mutex, stepped recovery, BSD emission on transitions, dashboard line. `governance.health()` stdlib returns `{verdict, coherence, governance_level, governance_epoch, bsd_events, cdd_turns_analyzed, ...}`. Config: `governance_health` section.
- **Standing Lease**: TTL on agent authorization (Kerberos TGT analog). Per-agent `standing_lease_turns` and `standing_lease_seconds`. Expired lease forces step-up challenge. Renewed on pass. `lease_remaining` in agent environment.
- **Advisory Escalation**: Repeated advisories harden (OSHA violation analog). 2nd+ occurrence: weight multiplied. N-th (`soft_after`): escalate to SOFT block. Config: `advisory_escalation.enabled`, `soft_after`, `weight_multiplier`.
- **Evidence Epoch**: Monotonic counter incremented on pulse verdict change, governance level change, config reload. `governance_epoch` in agent environment + `governance.health()`. Prior-epoch evidence discounted via `consecutive_passes` reset.
- **Consequence-boundary hardening**: `verifyScoreIntegrity()` escalation-aware, `wasBlocked()` sees escalated advisories, `governance_epoch_` atomic, advisory history decay on epoch boundary, BSD/CDD evidence preservation across `updateConfig()`.
- **Non-binding refusal attestation**: tamper-evident proof of governance blocks. Signed attestation recorded when HARD block prevents execution.
- **Agent Output Contracts**: per-agent `output_contract` in govern.json agents block — `format`, `required_fields`, `field_types`, `regex_checks`. Validated after RESPONSE_SCAN. `CONTRACT_VIOLATION` telemetry event on failure. Config: `OutputContract` struct in `governance_config.h`.
- **RESPONSE_SUPPRESSED telemetry**: emitted when `content.empty()` after all retries — records handle_id, config_name, turn, reason, retries_used. Fills observability gap where empty responses looked like they never reached post-receive governance.
- **BSD pattern normalization**: `matchesStep()` normalizes UPPERCASE_UNDERSCORE pattern names (e.g., `"AGENT_SEND"`) to lowercase dot-notation (`"agent.send"`) for matching against `eventTypeToString()` output. Both formats accepted in govern.json patterns.

### Enterprise Readiness
- **Polyglot reload**: govern.json capability changes hot-reload polyglot executor configs mid-run.
- **Telemetry forwarding**: webhook and SIEM forwarding of JSONL telemetry events. Config: `telemetry.forwarding` with `webhook_url`, `siem_url`, `batch_size`, `flush_interval_ms`.
- **REST API multi-key auth**: `api.auth` section with `keys` array, each key has `id`, `key` (or `key_env`), `permissions` (array of `"read"`, `"write"`, `"admin"`). Scoped access control for the REST governance API.
- **govern.json extends/inheritance**: `"extends": "./path/to/parent.json"` loads parent config. Child overrides parent. Array fields use `meta.inheritance.merge_arrays`: `"replace"` (default) or `"append"` (dedup concat). Max depth: `meta.inheritance.max_depth` (default 5). Parent must pass signature verification.

### Polyglot Execution
- `src/runtime/*_executor.cpp` — 12 language executors (Python, JS, Go, Rust, C++, C#, Nim, Shell, Ruby, PHP, Julia, Zig)
- `src/runtime/language_registry.cpp` — executor registration
- `<<python ... >>` syntax — `>>` must be at line start to close block
- Executor base: `executeWithReturn()`/`callFunction()` use NaabVal
- **Subprocess containment** (`src/runtime/subprocess_helpers.h/cpp`): `SubprocessContainment` struct applied to all polyglot child processes via `execute_subprocess_with_pipes()`. 5-layer defense: RLIMIT_NPROC=0 (blocks fork/subprocess), PATH restriction, env scrubbing, timeout (SIGKILL), allow_exec/allow_fork flags. `SubprocessContainment::fromCurrentSandbox()` reads current sandbox level: `standard` → fork+exec blocked, `elevated` → fork allowed, `unrestricted` → no restrictions. Pre-execution source scanning (governance checks) catches static patterns; containment catches runtime-constructed commands that evade static analysis.

### Stdlib Notable
- `array.sort()` mutates in place, `array.sorted()` returns a new sorted array (non-mutating)
- Both support optional comparator: `arr.sorted(fn(a, b) { return a - b })`
- `sorted()` is registered in `hasFunction()` but NOT in `isMutatingFunction()`
- VM has inline dot-notation for `sorted` in `callBuiltinMethod()`; tree-walker uses module path `array.sorted(arr)`
- Agent `agent.send()` auto-strips markdown code fences (` ```json ... ``` `) from LLM responses before returning content
- Agent resilience: `api_key_env`/`model` accept string or array (key rotation, model fallback). `retry` block configures backoff+jitter. `agent_dispatch.hard_stop` sets run-level budgets
- `agent.key_health(name)` returns `{available, active, dead}` — key rotation status
- `agent.dispatch_status()` returns run-level counters — calls, tokens, time, hard stop status
- `agent.send()` responses include `trace` dict — model, provider, api_key_env, attempts, latency_ms, fallback_used
- `agent.usage()` includes `retries`, `fallbacks`, `total_latency_ms`
- `agent.environment(handle)` returns current environment snapshot — config limits, remaining capacity, coherence state, key health, dispatch proximity
- `agent.create()` handle includes `environment` dict (birth snapshot); `agent.send()` response includes updated `environment` dict (live state)
- Pipeline upstream provenance: downstream stages see `state.upstream_provenance` dict with trust-calibration signals from upstream (model_used, was_fallback, retries, coherence_at_output, keys_dead, keys_active, latency_ms, stage index, pressure). Not present for first stage or non-pipeline agents.
- Agent environment includes `challenges_passed`/`challenges_failed` counters (from step-up challenges)
- Agent handles include `__nonce` field (HMAC anti-forge — do not modify or copy between handles)
- **Agent tool execution**: `agent.register_tool(name, function, schema)` registers NAAb functions as LLM-callable tools. Governed by 7 defense layers (declaration, admission, argument scan, scoped sandbox, result scan, budget check, behavioral). Config: `tools_enabled` (master switch, default false), `tools` (allowlist), `max_tool_calls_per_turn`, `max_tool_loop_turns`, `tool_result_max_chars`, `tool_result_max_total_chars`, `tool_timeout_seconds`. Dual-gate: tool must be in govern.json `tools[]` AND registered via `register_tool()`. `TOOL_EXEC` in `allowed_actions` required if action matrix is non-empty.
- `agent.send()` tool fields: `tool_calls_made`, `tool_loop_turns`, `tool_results` (array of `{name, success, latency_ms}`), `tool_budget_remaining`, `tool_loop_exit_reason`
- `agent.usage()` tool fields: `tool_calls_total`, `tool_calls_blocked`, `tool_total_latency_ms`
- `agent.environment()` tool fields: `permissions.tools_enabled`, `permissions.tools_registered`, `permissions.tools_available`, `state.tool_calls_total`, `state.tool_calls_blocked`, `state.tool_total_latency_ms`
- BSD event types: `TOOL_CALL`, `TOOL_RESULT`, `TOOL_ERROR`, `TOOL_BLOCKED`. Default patterns include `tool_data_exfil`, `tool_env_harvest`, `tool_shell_escape`, `tool_rapid_fire`
- Dashboard: `Tools: N calls (N blocked, Nms)` when tool execution occurred
- Tool config fields are ratchet-enforced (can only tighten mid-run, never loosen)
- **`agent.extract_code(response, lang)`** — extract code from markdown fences. Searches for `` ```lang `` or `` ``` `` fences, prefers matching `lang` hint, returns longest block if multiple found. Strips surrounding conversational text. Returns input unchanged if no fence found. More powerful than the auto-strip applied by `agent.send()`.
- **Codegen module** (`src/stdlib/codegen_impl.cpp`): `codegen.run(lang, code)` — governed dynamic code execution. Routes runtime-generated code through the same 39+ governance checks as static polyglot blocks. `codegen.run_with_args(lang, code, args)` — same with variable bindings. `codegen.run_strict(lang, code, args)` — throws `std::runtime_error` on non-zero exit code (catchable by NAAb `try/catch`). `codegen.supported_languages()` — list available languages. `codegen.is_enabled()` — check if codegen is enabled in govern.json. Config: `codegen` section in govern.json with per-call limits, cumulative limits, taint policy, nesting prevention.
- **Orchestra module** (`src/stdlib/orchestra_impl.cpp`): multi-agent workflow building blocks.
  - `orchestra.sequential_refinement(handles, prompt [, iterations])` — returns a plan dict `{pattern, handles, prompt, iterations, description}`. NAAb code uses the plan to drive `agent.send()` loops.
  - `orchestra.consensus_vote(votes_dict)` — takes `{votes: ["APPROVED", ...]}`, tallies verdicts, returns `{verdict, majority, approved, rejected, review, total}`.
  - `orchestra.enforce_convergence(response, spec)` — validates response against spec (regex `pattern` or `required_fields`). Returns `{valid, error_message}`. No retry loop — caller implements retries.
- **Governance module** (`src/stdlib/governance_impl.cpp`): `governance.health()` — returns pulse verdict and instrumentation status without API call.

### Scanner Code Quality Checks (18 checks in `checks_code_quality.cpp`)
- Checks 1-15: original checks (empty_catch, magic_numbers, dead_code_after_return, god_functions, deep_nesting, etc.)
- Check 16: `null_coalesce_non_nullable` — flags `??` on comparisons/bool literals that can never be null
- Check 17: `assigned_never_read` — detects `let`-assigned variables never referenced after declaration, with special `sort()` mutation hint
- Check 18: `hash_sanitize_mismatch` — one-level data flow tracing: finds `crypto.sha256(X)`, traces X back through `let` assignments, checks if any component variable is also passed to `sanitize_*()`/`validate_*()`
- Pattern: guard on `isEnabled(CAT, "check_name")`, use `findFuncEnd()` for function scope, `addIssue()` to report

### Telemetry
- `GovernanceEngine::writeTelemetry()` in `governance_reports.cpp` writes JSONL events
- Each event includes `run_id` (timestamp-pid, generated once in `loadFromFile()`) for separating runs in shared output files
- Agent telemetry event types (25): `ADMISSION_EVAL`, `AGENT_CHALLENGE_FAIL`, `AGENT_CHALLENGE_PASS`, `AGENT_FALLBACK`, `AGENT_HARD_STOP`, `AGENT_KEY_DISABLED`, `AGENT_KEY_REVIVED`, `AGENT_RESPONSE`, `AGENT_RETRY`, `AGENT_TOOL_BLOCKED`, `AGENT_TOOL_CALL`, `AGENT_TOOL_LOOP_END`, `AGENT_TOOL_LOOP_START`, `AGENT_TOOL_REGISTERED`, `AGENT_TOOL_RESULT`, `AGENT_TOOL_SCAN_HIT`, `BSD_MATCH`, `CDD_TURN`, `CODEGEN_EXEC`, `CONTRACT_VIOLATION`, `GOVERNANCE_HEALTH_WARNING`, `GOVERNANCE_LEVEL_CHANGE`, `PROMPT_SCAN`, `RESPONSE_SCAN`, `RESPONSE_SUPPRESSED`
- Telemetry forwarding: `telemetry.forwarding` config enables webhook/SIEM push of events
- Tamper-evident hash chain: each telemetry event includes `prev_hash` linking to previous event, creating an immutable audit trail

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
- **`array.sort()` mutates, `array.sorted()` does not** — `sort()` is in `isMutatingFunction()`, `sorted()` is not. Both live in `array_impl.cpp` and VM's `callBuiltinMethod()`.
- **Agent responses auto-strip markdown fences** — `agent.send()` strips ` ```json ... ``` ` wrapping when the entire response is a single fenced block. Only strips when fence is at start/end (with optional whitespace). Does not strip partial fences or multiple fence blocks.
- **GovernanceHardError catch-and-rethrow required** — any new `catch (const std::exception&)` or `catch (const std::runtime_error&)` in interpreter.cpp or vm.cpp that could intercept governance exceptions MUST have a preceding `catch (const governance::GovernanceHardError&) { throw; }`. Without this, HARD governance violations become catchable by NAAb try/catch.
