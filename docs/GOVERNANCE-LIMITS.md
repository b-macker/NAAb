# Governance: Known Limitations and Escape Hatches

NAAb's governance engine provides taint tracking, capability enforcement, hallucination detection, and code quality checks. This document honestly catalogs every known limitation, bypass path, and edge case where governance protection is incomplete.

**Philosophy:** No security layer is perfect. Documenting gaps lets users make informed decisions about their threat model rather than assuming false safety.

---

## 1. Taint Tracking

### What Works

Taint tracking is **name-based** — the `taint_set_` in `GovernanceEngine` tracks variable names that hold tainted data. The following paths all correctly propagate taint (verified by 24/24 tests in `tests/governance_v4/taint_aliasing/`):

- Direct assignment: `let copy = tainted`
- Reassignment: `target = tainted`
- String concatenation: `"prefix" + tainted`
- String interpolation: `"${tainted}"`
- Function return (identity, transform, double indirection)
- Dict/array store and retrieve: `d["k"] = tainted; let v = d["k"]`
- Dict/list literals containing tainted values
- Struct field assignment and retrieval
- JSON stringify/parse roundtrip
- Closures capturing tainted variables
- Multiple taint sources accumulate independently
- Sanitizer-prefixed functions (`sanitize_*`, `validate_*`, `escape_*`) clear taint

### Known Limitations

#### 1.1 Multi-Level Async Taint (By Design)

**Severity:** Medium
**Status:** Documented limitation, will not fix

Each `async` function gets a **snapshot** of the parent's taint state at creation time, not a live reference. This means:

```
main (tainted) → async1() → async2() → async3()
```

`async3` sees the taint snapshot from when `async2` was created, which was `async1`'s snapshot at creation time — before `async1` added its own taint. Multi-hop taint propagation (grandchild+ async) does not work.

**Rationale:** Async isolation via snapshot is a deliberate design choice. Shared mutable taint state across async boundaries would introduce race conditions.

**Mitigation:** Pass tainted values explicitly through function arguments rather than relying on transitive async taint.

**Test:** `tests/governance_v4/edge/test_edge_11_async_taint.naab`

#### 1.2 Name-Based Taint Doesn't Track Value Identity

**Severity:** Low
**Status:** By design

Taint tracks *variable names*, not *value identity*. In practice, this means:

- Taint follows the name through assignments and expressions
- The `lastReturnTainted` flag bridges function return boundaries
- `expressionContainsTaint()` checks all 15 AST expression types

In earlier versions, some aliasing paths broke taint (e.g., dict retrieval, function double indirection). As of v0.5.0, all tested aliasing paths preserve taint correctly.

**Edge case that could theoretically break:** A sufficiently complex chain of operations that creates new variable names without triggering any of the taint propagation checks. No such case has been found in testing.

#### 1.3 Catch Variable Taint Clearing

**Severity:** Low
**Status:** Fixed (BUG-AB), documented side effect

`try { ... } catch(e) { ... }` clears taint from the name `e` to prevent stale taint from a previous `catch` leaking. However, this clears taint for *all* variables named `e` in scope, not just the catch variable.

**Mitigation:** Use descriptive catch variable names (e.g., `catch(err)`) to avoid collision with tainted variables.

#### 1.4 Module Import Body Scan Skipped

**Severity:** Low
**Status:** By design (performance)

When a module is imported (`import "mod.naab" as m`), governance does not scan the imported module's function bodies for violations. Only runtime enforcement applies when those functions are called.

**Rationale:** Scanning all imported module bodies recursively would cause O(n) performance degradation on import chains. Runtime enforcement catches violations when code actually executes.

---

## 2. Polyglot Subprocess Escaping

### What Works

- Taint checks on bound variables before polyglot execution (all languages)
- Per-language sink detection (`shell_exec`, `python_exec`, `go_exec`, etc.)
- Capability enforcement (network, filesystem)
- Hallucination detection for wrong-language APIs
- Comment-aware pattern matching (as of b5c155a6)

### Known Limitations

#### 2.1 Polyglot Code Can Do Anything the Executor Allows

**Severity:** High
**Status:** Fundamental limitation

Once code enters a polyglot block (`<<python`, `<<shell`, etc.), NAAb governance cannot control what that code does. A `<<python` block can:

- Import any Python module
- Make network requests (even if `capabilities.network: false`)
- Read/write files (even if `capabilities.filesystem: "none"`)
- Execute shell commands via `os.system()`

Governance checks happen *before* execution (taint on bound variables, hallucination patterns, capability checks on the NAAb side). The polyglot runtime itself is unconstrained.

**Mitigation:**
- Use `capabilities` to document intent, not enforce it
- Sandbox polyglot execution at the OS level (containers, seccomp) for untrusted code
- The `sandbox` configuration limits CPU time and process count, but not syscalls

#### 2.2 Polyglot Output Taint Is Opt-In

**Severity:** Medium
**Status:** By design

The `polyglot_output` taint source marks the *result variable* as tainted when a polyglot block returns. But this requires `polyglot_output` to be listed in `taint_tracking.sources` in `govern.json`. If omitted, polyglot output is untainted.

Per-language taint (`polyglot_output:python`, `polyglot_output:shell`) provides finer control but requires explicit configuration.

#### 2.3 Persistent Python Runtime State

**Severity:** Medium
**Status:** Fixed (BUG-AA), but residual risk

Python blocks share a single CPython interpreter (`__main__` module). Variables set in one `<<python` block persist and are visible to later blocks in the same NAAb process. A previous block could:

- Monkey-patch stdlib functions
- Set global variables that alter subsequent block behavior
- Import modules with side effects

**Mitigation:** Each `<<python` block should be treated as running in a shared namespace. Use unique variable names or `del` cleanup.

---

## 3. Hallucination Detection

### What Works

- Per-language pattern matching (Python, JS, Go, Ruby, Shell, Nim)
- String literal stripping before matching (prevents false positives from code-in-strings)
- Comment stripping before matching (prevents false positives from code-in-comments)
- Cross-language confusion detection (e.g., `//` comments in Python block)

### Known Limitations

#### 3.1 Pattern-Based, Not Semantic

**Severity:** Low
**Status:** By design

Hallucination detection uses regex pattern matching, not semantic analysis. It can catch `console.log()` in Python but cannot determine if a function call is semantically correct for the target language.

**Example:** `sorted()` is valid in both Python and (hypothetically) a custom JS library. The detector cannot distinguish context-dependent validity.

#### 3.2 Custom Patterns Not Comment-Stripped

**Severity:** Low
**Status:** Fixed (b5c155a6)

As of commit b5c155a6, custom patterns in `govern.json` are matched against comment-stripped code. Previously, comments could trigger custom pattern matches.

---

## 4. Type System (`--strict-types`)

### What Works

- Type mismatch detection across all call paths (direct, nested, higher-order)
- Return type validation
- Variable type annotation checking
- Wrong arity detection

### Known Limitations

#### 4.1 Gradual Typing Only

**Severity:** Low
**Status:** By design

`--strict-types` only checks functions with explicit type annotations. Untyped functions are not checked. This is intentional — NAAb supports gradual typing.

#### 4.2 No Generic Types

**Severity:** Low
**Status:** Not implemented

There is no support for generic/parametric types (e.g., `List<int>`). Collection types are untyped at the type-checking level.

---

## 5. Governance Configuration

### What Works

- Auto-discovery of `govern.json` from script directory upward
- 3-tier enforcement (hard/soft/advisory)
- `--governance-override` CLI flag for soft rules
- Backward-compatible with legacy flat format

### Known Limitations

#### 5.1 Static Configuration Only

**Severity:** Low
**Status:** By design

`govern.json` is loaded once at startup. There is no hot-reload mechanism. Changes to governance rules require restarting the NAAb process.

#### 5.2 REPL Ignores Governance

**Severity:** Low
**Status:** By design

The REPL (`naab-repl`) does not load or enforce governance rules. This is intentional for interactive development workflows.

#### 5.3 Override Flag Is All-or-Nothing

**Severity:** Low
**Status:** By design

`--governance-override` bypasses all soft-level rules. There is no mechanism to override specific rules while keeping others enforced.

---

## 6. Scope and Capability Enforcement

### What Works

- Function-level scope patterns (`scopes` in govern.json)
- Network/filesystem capability checks
- Polyglot block count limits
- Resource limits (timeout, memory via sandbox)

### Known Limitations

#### 6.1 Capability Checks Are NAAb-Side Only

**Severity:** High
**Status:** Fundamental limitation

`capabilities.network: false` prevents NAAb's `http` stdlib from making requests, but a `<<python` block can freely `import requests` and make network calls. Capability enforcement is at the NAAb API level, not the OS level.

**Mitigation:** Use OS-level sandboxing (containers, seccomp, AppArmor) for hard capability enforcement.

#### 6.2 Filesystem Checks Don't Cover Polyglot

**Severity:** Medium
**Status:** Same as 6.1

`capabilities.filesystem: "read"` restricts NAAb's `file` stdlib module but not `open()` in `<<python` blocks.

---


---

## 7. Configuration Keys That Do Not Enforce What Their Name Suggests

Every key below loads without complaint, survives validation, and in several
cases ships in `govern-template.json` or is written by `naab governance init`.
None of them does what an operator would reasonably expect. They are listed here
because the failure mode is silent: the config is accepted, so there is nothing
to notice.

Two keys with this shape (`limits.code.max_functions`, `limits.code.max_variables`)
were **deleted** rather than documented. The rest are kept because something real
sits behind them, or because the name expresses a constraint worth keeping.

### 7.1 Inert toggle, live protection

The protection works. The key does not control it. Setting it to `false`
disables nothing.

| key | what actually protects you | where |
|---|---|---|
| `trust_policy.check_revocation` | revoked keys are skipped **unconditionally** | `trust_store.cpp`, which never reads governance config |
| `trust_policy.check_key_expiry` | expired keys skipped unconditionally | same |
| `limits.memory.total_mb`, `per_block_mb` | `RLIMIT_AS` / Windows Job memory | the **sandbox level** (128 MB at RESTRICTED), not governance |
| `limits.execution.parallel_blocks` | concurrency capped | a hardcoded `ThreadPool(2)` |
| `limits.data.input_size` | source size capped at 100 MB | `limits::checkStringSize` at `lexer.cpp` |
| `limits.timeout.per_block` | per-block CPU time capped | sandbox `max_cpu_seconds`, applied as `RLIMIT_CPU` |

Because these protections are unconditional, wiring the toggles would only ever
permit **weakening** — which is why they have been left alone.

### 7.2 Redundant with a key that works

| key | use this instead |
|---|---|
| `limits.execution.total_executions` | `limits.execution.polyglot_blocks` — live, and already counts polyglot *and* codegen executions across both engines |

### 7.3 Accepted, enforced by nothing, nothing else covering it

| key | note |
|---|---|
| `limits.data.string_length` | governance reader has no call sites. Runtime string *growth* is capped separately by `MAX_STRING_LENGTH` in the VM's concat path, so this is not unbounded — but the key does not control it |
| `limits.timeout.total_polyglot` | expresses a real constraint nothing else can state — cumulative time *in polyglot specifically*, as distinct from per-block CPU and whole-run wall clock. Unimplemented, deliberately kept |
| `limits.rate.cooldown_on_limit_ms` | ships with a plausible default of `100` for behaviour that does not exist. The live rate limiters **throw** on breach rather than throttling, and `RateLimiter` has no field a cooldown could occupy |
| `limits.code.max_total_polyglot_lines` | per-block size *is* capped; the program-wide total is not |
| `limits.code.max_nesting_depth` | parse depth is capped by `MAX_PARSE_DEPTH`; this key does not control it |
| `requirements.error_handling.require_try_catch`, `require_catch_body` | the check runs off a different field; these two sub-options are read nowhere |
| `restrictions.code_injection.block_ldap_injection`, `block_template_injection`, `block_xpath_injection` | no matching pattern exists anywhere in the engine |

### 7.4 Hardcoded safety constants that are defined and never used

`include/naab/limits.h` declares ten compile-time bounds. Four are enforced
(`MAX_ARRAY_SIZE`, `MAX_INPUT_STRING`, `MAX_POLYGLOT_BLOCK_SIZE`,
`MAX_STRING_LENGTH`), two more are enforced directly (`MAX_PARSE_DEPTH`,
`MAX_CALL_STACK_DEPTH`), and **four are referenced by nothing**:

    MAX_DICT_SIZE     (1,000,000 entries)  -- its guard has no callers. Note the
                                             GOVERNANCE dict limit is now wired;
                                             this hardcoded ceiling is separate
                                             and still unused. Wiring it would be
                                             a tightening with no opt-out, since
                                             dict literals are bounded today by
                                             MAX_ARRAY_SIZE at 10x this value.
    MAX_FILE_SIZE     (10 MB)              -- its guard has no callers
    MAX_AST_NODES     (1,000,000)
    MAX_LINE_LENGTH   (10,000)

These are not configuration, so no operator sets them expecting protection — but
they read as a defence-in-depth layer that is, in these four cases, absent.

### 7.5 Two config-shape traps

**An `enabled: false` inside a check's object does not disable it.** For checks
enabled by the *presence* of their block, the inner flag is not read, so
`{"enabled": false}` turns the check **on** — for `code_quality.no_secrets` that
means an uncatchable exit-3 block. The value is deliberately not honoured
(honouring it would silently disable checks that enforce today), but the engine
now warns. **To leave such a check off, omit its block entirely.**

**An unrecognised `level` silently disables.** Only `hard`, `soft`, `advisory`,
`detect` and `approval_required` are recognised; anything else — `"off"`,
`"none"`, or a typo, and the comparison is case-sensitive so `"HARD"` counts —
disables the check. The engine now warns and lists the valid values.

### 7.6 One behaviour worth knowing

`--timeout` and the govern.json global timeout combine with `std::max`, so the
CLI flag can **extend** a governance timeout, not only shrink it. This is
deliberate per the comment at the call site, but it makes the timeout a floor
rather than a ceiling, unlike every other governance limit.

---

## Summary Table

| Limitation | Severity | Category | Status |
|---|---|---|---|
| Multi-level async taint | Medium | Taint | By design |
| Polyglot code unconstrained | High | Subprocess | Fundamental |
| Capability checks NAAb-side only | High | Capabilities | Fundamental |
| Persistent Python state | Medium | Subprocess | Partially fixed |
| Polyglot output taint opt-in | Medium | Taint | By design |
| Filesystem checks skip polyglot | Medium | Capabilities | Fundamental |
| Catch variable name collision | Low | Taint | Documented |
| Module body scan skipped | Low | Taint | By design |
| Pattern-based hallucination | Low | Detection | By design |
| Static configuration only | Low | Config | By design |
| REPL ignores governance | Low | Config | By design |
| Override all-or-nothing | Low | Config | By design |
| Gradual typing only | Low | Types | By design |
| No generic types | Low | Types | Not implemented |
| `enabled: false` inside a check object turns it ON | **High** | Config | Warned, not honoured (§7.5) |
| Unrecognised `level` silently disables the check | **High** | Config | Warned (§7.5) |
| `limits.data.dict_size` unenforced | — | Limits | **Fixed** — now enforced in both engines |
| `trust_policy.*` toggles control nothing | Low | Config | Protection is unconditional (§7.1) |
| `limits.memory.*` controls nothing | Medium | Limits | Memory bounded by sandbox level (§7.1) |
| `cooldown_on_limit_ms` — behaviour does not exist | Low | Limits | Rate limiters throw, not throttle (§7.3) |
| Four `limits.h` constants referenced by nothing | Low | Limits | Absent defence layer (§7.4) |
| `--timeout` can extend a governance timeout | Low | Config | Deliberate; timeout is a floor (§7.6) |

**High-severity items** are fundamental to the architecture — polyglot blocks execute in the target language's runtime, which NAAb cannot constrain. Use OS-level sandboxing for untrusted code.

**Medium-severity items** have mitigations or partial fixes documented above.

**Low-severity items** are intentional design choices with clear rationale.
