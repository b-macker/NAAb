# Plan — function-scope effect envelopes

**Status:** design only. Nothing built. Written before code so the premises can
be checked rather than discovered.

This plan exists because an investigation into `code_quality.intent_validation`
found the intent system asking a question it cannot answer, while the mechanism
that *could* answer a neighbouring and more valuable question is absent. The
measurements behind that are in the commits on PR #245 and summarised below.

## Thesis

The engine has a **scope ladder for allowlists** and its bottom rung is missing.

| scope | config | enforcement | exists |
|---|---|---|---|
| program | `capabilities.filesystem` / `.shell` / `.network` / `.env_vars` | `checkFilesystemAllowed`, `checkNetworkAllowed`, `checkShellAllowed`, `checkEnvVarRead/Write` | yes |
| role | `agents.<name>.allowed_actions`, `.shell_allowed`, `.network_allowed` | same functions, second tier via `effectiveAgentId()` | yes |
| **function** | — | — | **no** |

Everything a NAAb function may do is decided at program or role scope. Inside a
program, every function has the union of all permissions the program holds, so a
`format_report()` helper carries the same authority as the one function that is
supposed to write files.

The intent system is the only thing that currently attempts per-function
restriction, and it does so by **matching English prose against identifiers**.
Measured over 263 (comment, function) pairs drawn from 1,280 `.naab` files —
both sides independently authored, every percentage reported by the engine — it
blocks **77% of real code** at a median overlap of **0%**. That is not a
threshold problem: every surface where intent vocabulary naturally appears is
excluded for a sound anti-gaming reason (function name = circular; local `let`
names = stuffing vector; comments and long strings = prompt-echo vector),
leaving only calls, parameters and short string literals.

So the proposal is not to improve prose matching. It is to ask the question
prose was standing in for: **what is this function allowed to DO**, checked
against what it does.

## What this is, and what it is NOT

Three mechanisms divide cleanly, and conflating them is the main risk to this
plan:

- **effect envelopes** catch **excess** — a function doing more than declared.
- **contracts** (`must_produce`, `must_vary`, `must_differentiate`,
  `must_derive_from`) catch **absence** — a function not doing its job. These
  execute the function and compare outputs; they are the correctness mechanism
  and they already work.
- **intent prose** reliably catches neither. That is the measurement above.

An envelope will not catch `fn compute_totals(rows) { return 0 }`. That stub
emits no effects at all and satisfies any envelope trivially. `must_produce`
catches it today. Anyone expecting envelopes to verify correctness will be
disappointed, and the docs must say so in those words.

## Premises that did NOT survive tracing

Recorded because each was believed, argued, and wrong — the same convention as
`plan-engine-observability.md`.

1. **"Build it on the BSD runtime event bus."** Proposed, then withdrawn. The
   bus is emitted only from `vm.cpp` (19 sites; `src/interpreter/` has none for
   `checkPreExecution`), and measured: the same program produces 4 BSD matches on
   the VM and **0** under `--tree-walk`. Anything built there is silently
   unenforced on one engine. It is also post-hoc — it observes and reports where
   the capability gate can block.

2. **"Resource limits are VM-only."** Claimed from grepping
   `call_dispatch.cpp` alone. Wrong: `checkArraySize` and `checkDictSize` are in
   `expressions.cpp`, `checkLoopIterations` in `interpreter.cpp`, and a
   `limits.data.array_size: 5` fixture blocks on **both** engines (exit 3, same
   message). Only `checkPreExecution` is genuinely VM-absent. Grepping one file
   of a multi-file engine path is not tracing.

3. **"Partial name credit threads the needle on intent overlap."** Argued twice
   from arithmetic, killed by measurement. Across the same 263 pairs: name
   excluded 77% blocked, 0.5 weight 71%, 0.75 weight 64%, full credit 44% — and
   only full credit lets a well-named empty function pass. The entire benefit
   sits at the weight that breaks the anti-gaming property. Nothing shipped.

## The design

### F1 — Config: `capabilities.functions.<name>.allowed_actions`

Lives in the `capabilities` family, not in `function_intents`. The permission is
a capability restriction and should be enforced like its siblings; putting it
beside the intent prose would read better at authoring time but split config
from mechanism. Reuses the **existing** action vocabulary verbatim —
`FS_READ`, `FS_WRITE`, `NET_CONNECT`, `SHELL_EXEC`, `PROCESS_EXEC`,
`AGENT_SEND`, `TOOL_EXEC` — so nothing new has to be learned or documented.

Absent key = unrestricted, exactly as `agents.<n>.allowed_actions` behaves when
empty. Additive and fail-open **by absence only**: once a function is listed,
anything not in its list is denied.

### F2 — Enforcement at the shared capability gate

`checkFilesystemAllowed`, `checkNetworkAllowed` and `checkPathAccess` are called
from **both** engines (`vm.cpp:1799`, `call_dispatch.cpp:1442` for
`filesystemAccessMode`). A third tier inside those functions inherits engine
parity structurally rather than by remembering to mirror it — which is how taint
parity broke (see the CLAUDE.md gotcha).

The two-tier shape already exists in each of those functions: a global check,
then `if (role.<x>_set && !role.<x>)` keyed on `effectiveAgentId()`. Function
scope is a third overlay in the same place, and it must narrow only: a function
grant can never exceed its role's, which can never exceed the program's.

### F3 — Attribution: mirror `t_active_tool_role`

The engine already solves "which identity is acting" with
`static thread_local std::string t_active_tool_role`
(`governance_engine.cpp:95`), read by `effectiveAgentId()` and maintained by
`pushActiveToolRole(role)` / `popActiveToolRole(prev)` — a save-restore pair so
it nests. `agent_impl.cpp:188-194` is the only caller.

Function scope needs the same thing for NAAb function entry/exit, pushed from
both engines' call paths. **This is the bulk of the work and the whole risk.**
It is not a config change; it touches `vm.cpp` and the interpreter, and a stack
that leaks on an exception path or a `return` out of a `try` attributes effects
to the wrong function. The existing precedent for that failure is
`emitTryEndsForLoopExit()` in the compiler.

### F4 — Bootstrap by recording, not by hand

Nobody will write these envelopes manually, and a feature that depends on
hand-authored per-function permission lists will not be adopted. The engine
already has this pattern: `--governance-record-baselines` /
`--governance-check-baselines`.

`--record-effects <path>` runs the program, observes which actions each function
actually performed, and emits a suggested `capabilities.functions` block. The
operator reviews and narrows it. This turns adoption from "author 40 permission
lists" into "run it once, delete what should not be there".

Note the honest limitation: recording sees only executed paths, so a recorded
envelope is a floor, not a complete description. The suggested block must be
labelled as observed-not-authoritative, and the docs must not imply otherwise.

### F5 — Ratchet

`capabilities.filesystem.mode` is already ratcheted (one of 68 loosening checks
in `governance_config.cpp`), as are the per-agent `shell_allowed` /
`network_allowed` twins. Function envelopes join that: adding an action to a
function's list mid-run is a loosening violation; removing one is a notice.
Adding a NEW function entry mid-run needs the same treatment that
`meta.allow_agent_addition_mid_run` gives new agents, or the ratchet is
escapable by renaming — that exact hole existed for agents and was closed.

### F6 — Helper error, and it must name the fix

Per the standing rule that advisory guidance has to be actionable and survive
being followed:

```
Undeclared effect in 'format_report': FS_WRITE
  Function may perform: FS_READ
  Attempted: file.write("out.txt")  at report.naab:42
  Either the write does not belong here, or the declaration is incomplete.
  To permit it:
    "capabilities": { "functions": { "format_report": {
        "allowed_actions": ["FS_READ", "FS_WRITE"] } } }
  Note: a function grant can only narrow its role's permissions, never widen
  them — adding FS_WRITE here has no effect if the role lacks it.
```

The last line matters: without it, an operator hits the containment rule as a
second, unexplained refusal.

### F7 — What it makes possible that nothing does today

- **Blast-radius reporting.** "Which functions can reach the network" becomes a
  config query rather than a code audit. Fits the existing report formats.
- **Closes the three static side-effect gaps** found in the same investigation,
  without touching the lexical scan: the all-or-nothing `intent_mentions_io`
  boolean (one I/O word in the intent grants *all* I/O, including
  `process.exec` and all twelve polyglot escapes — measured), the
  `let unused = file.write(...)` evasion (the scan reads the
  dead-code-stripped body; measured `Governance: PASS` with the file written),
  and the ordering where the 77%-false-positive keyword check returns before
  the side-effect scan runs.
- **Sharper taint sinks.** Taint currently asks "did tainted data reach a sink".
  With per-function effect declarations it can ask "did tainted data reach a
  sink in a function that was not supposed to have one".

## Open questions

1. **Granularity of PROCESS_EXEC vs SHELL_EXEC vs polyglot.** The action
   vocabulary has both `SHELL_EXEC` and (in the event enum) `PROCESS_EXEC`, and
   polyglot blocks are a third thing. These need one mapping decided once, or
   the same hole as `intent_mentions_io` reappears at a different altitude.
2. **Do envelopes attach to functions or to roles-plus-functions?** A function
   called by two different agent roles may legitimately need different
   permissions per caller. Deferred: start with function-only.
3. **Blocking vs accumulate-and-fail.** Blocking at the call is simplest and
   matches the capability gates. Accumulating and failing at exit gives a
   complete report of every violation in one run, which is better for the
   record-and-narrow workflow of F4.
4. **Anonymous/closure scope.** `fn(a, b) { ... }` passed to `array.sorted()`
   has no name to attribute to. Attribute to the enclosing named function, or
   introduce synthetic names.

## Test surface this needs before it ships

Non-negotiable, from the standing rules:

- Every gate must **fail when removed**, with a positive control that fires in
  the same run.
- At least one arm must expect **success** — a suite where every arm expects a
  refusal passes for free when the harness breaks, which happened three times in
  the investigation that produced this plan.
- **Engine parity**, asserted per engine rather than by assuming the shared gate
  covers both.
- A **containment** arm: a function grant exceeding its role must not widen it.
- A **ratchet** arm, including the rename-a-function escape from F5.
