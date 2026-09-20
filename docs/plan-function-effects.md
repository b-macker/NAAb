# Plan — function-scope capabilities

**Status:** design, nothing built. Written before code so the premises can be
checked rather than discovered. Every file:line below was traced, and the
premises that did not survive tracing are recorded rather than deleted.

## Terminology

Called **function-scope capabilities**, not "effect envelopes". The whole value
of this design is that it is not a new subsystem: it is `capabilities` at a
third scope, reusing the existing action vocabulary, enforced inside functions
that already exist, ratcheted by the machinery that already ratchets its
siblings. An operator who knows `capabilities.filesystem` and
`agents.<n>.allowed_actions` already knows this.

A new concept name would invite a new config section, new defaults and new
docs — which is how this repo acquired 75 pinned inert keys and a flag with
zero uses across 146 configs. If it needs its own name, it has been made too
separate from capabilities. "Envelope" survives only as informal shorthand for
the computed set, never in a config key, an error message or a heading.

## What this is for

**A confused-deputy defence.** Today every function in a program holds the union
of the program's authority. A restricted caller gets anything done by asking a
permissive callee, and nothing in the engine notices, because authority is
decided at program and role scope only.

| scope | config | enforcement | exists |
|---|---|---|---|
| program | `capabilities.filesystem` / `.shell` / `.network` / `.env_vars` | `checkFilesystemAllowed`, `checkNetworkAllowed`, `checkShellAllowed`, `checkEnvVarRead/Write` | yes |
| role | `agents.<n>.allowed_actions`, `.shell_allowed`, `.network_allowed` | same functions, second tier via `effectiveAgentId()` | yes |
| **function** | — | — | **no** |

`code_quality.intent_validation` is the only thing attempting per-function
restriction, and it matches English prose against identifiers. Measured over 263
(comment, function) pairs from 1,280 `.naab` files — both sides independently
authored, every percentage engine-reported — it blocks **77% of real code** at a
median overlap of **0%**. That is structural, not a threshold: every surface
where intent vocabulary naturally appears is excluded for a sound anti-gaming
reason (function name = circular, local `let` names = stuffing vector, comments
and long strings = prompt-echo vector), leaving only calls, parameters and short
string literals.

So this plan does not try to improve prose matching. It asks the question prose
was standing in for: **what is this function allowed to DO.**

## What it is NOT

- **function-scope capabilities** catch **excess** — doing more than declared.
- **contracts** (`must_produce`, `must_vary`, `must_differentiate`) catch
  **absence** — not doing the job. They execute the function and compare
  outputs. They already work.
- **intent prose** reliably catches neither. That is the measurement above.

A function's capabilities will not catch `fn compute_totals(rows) { return 0 }`. That stub
performs no actions and satisfies any declaration trivially; `must_produce` catches
it today. Documentation must say this in those words.

## Premises that did NOT survive tracing

1. **"Build it on the BSD runtime event bus."** Withdrawn. Emission exists only
   in `vm.cpp`; measured 4 BSD matches on the VM and **0** under `--tree-walk`
   for the same program. It is also post-hoc, where the capability gate blocks.
2. **"Resource limits are VM-only."** Wrong, from grepping `call_dispatch.cpp`
   alone. `checkArraySize`/`checkDictSize` are in `expressions.cpp`,
   `checkLoopIterations` in `interpreter.cpp`; a `limits.data.array_size: 5`
   fixture blocks on **both** engines. Only `checkPreExecution` is VM-absent.
3. **"Partial name credit threads the needle on intent overlap."** Argued twice
   from arithmetic, killed by measurement: 77% / 71% / 64% / 44% blocked at name
   weights excluded / 0.5 / 0.75 / full, and only full credit lets a well-named
   empty function pass. The whole benefit sits at the weight that breaks the
   anti-gaming property. Nothing shipped.
4. **"Bootstrap needs a `--record-effects` flag."** Withdrawn — see F4. The
   precedent it copied has **zero** adoption: across 146 `govern.json` files in
   this repo, `governance.record_baselines` is set in **2** (both template
   copies, both `false`), and `--governance-record-baselines` appears in no
   test, example, workflow or doc.

## Design

### F1 — Config: `capabilities.functions`

```json
"capabilities": {
  "functions": {
    "default":       { "allowed_actions": ["FS_READ"] },
    "write_report":  { "allowed_actions": ["FS_READ", "FS_WRITE"] },
    "main":          { "allowed_actions": ["FS_READ", "FS_WRITE", "AGENT_SEND"] }
  }
}
```

Lives in the `capabilities` family because it is a capability restriction
enforced like its siblings, and reuses the existing action vocabulary verbatim
(F9 completes it). `default` covers every function without an entry — **not**
"absent means unrestricted", which would protect only what someone remembered to
list. `main` is an ordinary function in the AST and needs an entry like any
other; say so, or the first restrictive default breaks every project.

### F2 — Enforcement at the gates BOTH engines already call

`filesystemAccessMode()` is consulted at `vm.cpp:1799` and
`call_dispatch.cpp:1442`; `checkPolyglotBlock`, `checkNetworkAllowed`,
`checkPathAccess`, `checkDangerousCall`, `checkFunctionContract` are likewise
called from both engines. A third tier inside those functions inherits engine
parity **structurally** rather than by remembering to mirror it — which is how
taint parity broke (CLAUDE.md gotcha).

The two-tier shape already exists in each: a global check, then
`if (role.<x>_set && !role.<x>)` keyed on `effectiveAgentId()`. Function scope is
a third overlay in the same place.

### F3 — Attribution: copy `ScopedToolContext`

The engine already solves "which identity is acting":
`static thread_local std::string t_active_tool_role`
(`governance_engine.cpp:95`), read by `effectiveAgentId()` (`:6893`), maintained
by `pushActiveToolRole`/`popActiveToolRole` (`:6897`, `:6903`) — and critically,
driven by an **RAII guard**, `ScopedToolContext` in `agent_impl.cpp:183-196`,
whose destructor pops. Exception unwind is therefore already correct.

Function scope needs the same thing for NAAb function entry/exit, pushed from
both engines' call paths, as an RAII guard and never as a manual push/pop pair.
This is the bulk of the work. A stack that leaks on an exception path or a
`return` out of a `try` attributes effects to the wrong function; the precedent
for getting that wrong is `emitTryEndsForLoopExit()` in the compiler.

### F8 — Composition: intersection down the call stack (STRUCTURAL)

**Without this the feature is defeated by extracting a helper, and is advice
rather than a boundary.** If `format_report()` declares `[FS_READ]` and calls
`write_helper()` declaring `[FS_WRITE]`:

- *innermost-wins* → the caller escapes its own declaration by delegating. This is the
  split-delegation pattern `agent_review`'s prompt already hunts for.
- *intersection* → the effective set is `[]`; the deputy cannot be used to
  launder authority.

Effective capabilities = intersection of every entry on the function stack, then
intersected with the role, then with the program. Same monotonic-narrowing
principle the engine already applies for role ⊆ program, extended to depth.

The usability cost is real and is the security property: a shared utility must
be callable from its most restrictive caller. Expect this to surface as "my
logger stopped working", and the helper error (F6) must name the *caller* whose
declaration caused the intersection, not just the function that attempted the call.

No new machinery — intersection is computed over the F3 stack that must exist
anyway.

### F9 — Complete the action vocabulary

Enforced in the matrix today: `FS_READ`, `FS_WRITE`, `NET_CONNECT`,
`SHELL_EXEC`, `AGENT_SEND`, `TOOL_EXEC`.

Missing, and needed before function-scope capabilities can express what they
are for:

- **`ENV_READ` / `ENV_WRITE`** — zero occurrences in the matrix. Without them an
  declaration cannot say "this function may not read environment variables",
  which
  is the primary credential-exfiltration concern and the thing
  `capabilities.env_vars` governs globally. Enforcement points already exist:
  `checkEnvVarRead()` / `checkEnvVarWrite()`, called from all 9 env access sites.
- **`PROCESS_EXEC`** — an event-enum name only; distinct from `SHELL_EXEC`.
- **`POLYGLOT_EXEC`** — currently only a telemetry event name
  (`governance_checks.cpp:6676`). Needed for F7's polyglot closure, enforced at
  `checkPolyglotBlock`, which both engines call.

Decide the `SHELL_EXEC` / `PROCESS_EXEC` / `POLYGLOT_EXEC` mapping **once**, or
the all-or-nothing hole that `intent_mentions_io` has reappears at a new
altitude.

### F4 — Bootstrap is the error message, not a flag or a mode

Nobody passes a flag, and the precedent proves it (premise 4 above). An agent
writing code runs the program, reads stderr and iterates; it will never invoke a
record mode, and neither will a human setting up a project.

So there is no record mode. At `advisory`, one run reports **every** undeclared
effect with the exact declaration to add. The operator or reviewer pastes them
and tightens to `hard`. That is the ratchet's own direction, so progression is
enforced rather than encouraged, and the loop is the one already proven for
`must_produce`.

This also settles "block on first vs accumulate": `advisory` accumulates,
`hard` blocks. Two levels, no new concept.

### F5 — Ratchet — SHIPPED

`capabilities.filesystem.mode` is already ratcheted (one of 68 loosening checks
in `governance_config.cpp`), as are per-agent `shell_allowed` /
`network_allowed`. Function-scope capabilities join that.

**The plan's premise here did not survive implementation.** It said adding a new
function entry mid-run needs an opt-in like `meta.allow_agent_addition_mid_run`,
"or the ratchet is escapable by renaming — that exact hole existed for agents
and was closed". Two things are wrong with that.

First, the analogy breaks on `default`. A new *agent* is a new identity with no
prior permission to compare against, which is why the agent ratchet needed an
explicit opt-in. A new *function entry* always has a prior effective permission:
its own entry, else `default`, else unrestricted. So the comparison the agent
case could not make is exactly the one available here.

Second, entry-level bookkeeping gets the direction wrong in both directions.
Adding an entry can tighten (grants less than `default`) or loosen (grants
more); removing one can tighten (`default` is stricter) or loosen (`default` is
looser); and editing `default` silently moves every function with no entry.
A name-by-name diff of the raw map mishandles all four.

So the check compares **effective** permissions per name over the union of both
maps — no opt-in flag, no new config key. Gaining an action is a violation,
losing one a notice; unrestricted → restricted is a notice, the reverse a
violation. Deleting the section needs no special case: every previously listed
function is in the union and reports its own transition to unrestricted.

What a config ratchet still cannot see is a rename in the *source*, which drops
that function through to `default`. That is a code change, not a config change,
and it is the reason `default` exists rather than "absent means unrestricted".

Test: `tests/governance_v4/test_function_capability_ratchet.sh` — FR-03/05/07
are the arms a naive diff waves through, FR-04/06/09 their same-shape controls
in the tightening direction, and FR-10 the one that checks an accepted reload is
actually installed rather than merely reported.

### F6 — Helper error, naming the fix and the containment rule

```
Undeclared effect in 'format_report': FS_WRITE
  Effective capabilities: FS_READ
    from function 'format_report'   [FS_READ]
    narrowed by caller 'render_all' [FS_READ, FS_WRITE]
  Attempted: file.write("out.txt")  at report.naab:42

  Either the write does not belong here, or the declaration is incomplete.
  To permit it, add FS_WRITE to 'format_report':
    "capabilities": { "functions": { "format_report": {
        "allowed_actions": ["FS_READ", "FS_WRITE"] } } }
  Note: a function grant can only narrow what its role and the program already
  permit — adding FS_WRITE here has no effect if either lacks it.
```

The provenance lines are required by F8: under intersection the blocking
declaration often belongs to a *caller*, and without naming it the operator edits
the wrong entry. The closing note prevents hitting containment as a second,
unexplained refusal.

### F10 — Telemetry

`CAPABILITY_VIOLATION` with function, effective capabilities, attempted action, call
stack, source location, and the enforcement level. Every other subsystem emits
one and the audit trail needs it. Chain rules in CLAUDE.md apply: seed with
`chainPrevLocked(fp)` and increment `chained_events_this_run_` under
`telemetry_hash_mutex_`, or `--verify-telemetry-chain` reports a BREAK on a file
nobody touched.

### F7 — Hole closures (verified)

| hole | closure | evidence |
|---|---|---|
| auto-apply is self-granting | sign `govern.json` | **verified**: editing a signed config → `INTEGRITY BLOCK` exit 3 on the next run. `NAAB_GOVERN_KEY` is scrubbed at `subprocess_helpers.cpp:63,710`, `persistent_process_executor.cpp:91`, and inside the embedded interpreter at `python_c_executor.cpp:191`, so generated code cannot reach the key |
| polyglot bypass (CONTRA-013) | `POLYGLOT_EXEC` as a declarable action at `checkPolyglotBlock` | partial — see below |
| absent = unrestricted | `capabilities.functions.default` (F1) | — |
| advisory window | not separate; bounded by signing + ratchet | — |
| attribution leak | RAII guard | `ScopedToolContext` already does exactly this |

**The polyglot closure is partial and must be documented as such.** Declaring
`POLYGLOT_EXEC` stops a function from *running* a polyglot block; once one runs,
nothing contains what it does. There is no OS-level path enforcement —
`landlock_create_ruleset` returns `ENOSYS` on the dev kernel and `seccomp`
appears once in the tree, inside an error string. This converts an uncontainable
bypass into a declarable permission. That is an improvement and it is not
containment.

## Build order

Each phase is independently useful and independently testable.

1. **F3 attribution stack** (RAII, both engines) + **F10 telemetry**. Ships
   value alone: taint violations, BSD events and every error message can name
   the enclosing function, which **nothing can do today** — `RuntimeEvent`
   carries `file` and `line` and no function.
2. **F9 vocabulary** — `ENV_READ`/`ENV_WRITE` in the matrix first; useful to
   per-agent `allowed_actions` immediately, before function scope exists.
3. **F1 + F2 + F6** — enforced at one gate (`FS_WRITE`) end to end,
   advisory only. Smallest thing that demonstrates the loop.
4. **F8 composition** — before widening to more gates, because it changes the
   semantics of everything built in phase 3.
5. **F5 ratchet** (shipped), then **F4 hard level** and the remaining gates.

## Test surface — non-negotiable

From the standing rules, each earned by a failure in the investigation that
produced this plan:

- Every gate must **fail when removed**, with a positive control firing in the
  same run.
- At least one arm must expect **success**. A suite where every arm expects a
  refusal passes for free when the harness breaks — that happened three times
  during this investigation (masked sandbox, shell-path handoff, unisolated
  trust store).
- **Engine parity asserted per engine**, not assumed from the shared gate.
- **Containment**: a function grant exceeding its role must not widen it.
- **Composition (F8)**: a permissive callee under a restrictive caller must be
  refused — this is the arm that distinguishes a boundary from advice.
- **Ratchet**, including the rename-a-function escape.
- **Attribution under unwind**: throw from inside a nested call and assert the
  stack is correct afterwards.
