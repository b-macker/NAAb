# `naab-gov taint`: the static information-flow report (Bundle 12 PR 2)

Report only, by the owner's decision (2026-11-04): it refuses nothing and runs
none of the program. It loads the govern.json the way every `naab-gov`
command does, so the config's `prerequisites` checks run, and a `command`
check starts the command it names (A122); the report points its own stdin at
the null device first, so such a command cannot read the caller's input. It exists so the flows it finds, and how often it is wrong, can be
measured before any of it is enforced. Code: `include/naab/taint_flow.h`,
`src/interpreter/taint_flow.cpp`, `src/cli/gov_taint.cpp`. Test:
`tests/governance_v4/test_taint_flow.sh`. Measurement:
`tools/taintflow/measure.py`.

## What it reports

For a program and its govern.json (the one a run of it would find, or
`--config PATH`), every place tainted data reaches a configured sink, as one of
two kinds:

- **value** -- the data reaching the sink is computed from a source: an
  explicit flow, the kind the run's taint check exists to judge.
- **control** -- the data is not, but it was set, or the sink was reached, only
  on a path tainted data chose: an implicit flow. No run-time check judges
  these today (A117: "an implicit flow through a STATEMENT ... those are the
  static check's job").

The shape that matters most is the bit-by-bit leak: `if t == "x" { http.get(A) }
else { http.get(B) }` sends no tainted byte and still tells the receiver one bit
of `t`; a loop over `t` sends all of it.

## How it decides

- Every **value** is judged by the shared expression rule
  (`include/naab/taint_rule.h`), the one both engines' AST walks use, and every
  name by the engine (`isTaintSource()`, `isSanitizer()`, `isTaintSink()`), so a
  report and a run under one govern.json agree on what a source, sanitizer and
  sink are. `isTaintSink()` is the sink match `taintedSinkDecision()` always
  used, split out; `polyglotSinkType()` is the polyglot sink name the two
  engines each wrote out, now one copy.
- **Statements** are what it adds. A branch, loop, short-circuit, match arm or
  catch whose choice depends on tainted data runs under a tainted control
  label: what it assigns is tainted by control, the sinks it calls are control
  flows, and an early exit under it (`return`, `break`, `continue`, `throw`)
  taints the rest of the function or loop. A try expression's value depends on
  whether it threw.
- **Loops** are solved to a fixed point (at most 24 iterations, then counted as
  not settled); **functions** are summarised per call shape -- each argument's
  taint, the lambdas it receives, the caller's control -- over rounds until
  recursion settles (at most 8). A **lambda** runs over the names it captured
  where it was made; a callback a stdlib call is given gets the call's other
  data as its parameters.
- **Reachability**: a block stops at a throw, return, break or a call that
  always throws, so code no path reaches is not judged.
- **Termination-insensitive**: whether a loop that depends on tainted data ends
  is not counted.

### What it cannot see (counted, never passed)

Each is counted under `not analysed` in the report, and a report that found
nothing while such a count is non-zero exits 2 (UNMEASURABLE), not 0:

- a call into an imported NAAb module (`use ./x`, `import`), and a registry
  block (`use BLOCK-ID as alias`);
- a call through a function value the analysis cannot resolve;
- a polyglot block given tainted data, or run under tainted control, whose
  language has no `<lang>_exec` sink configured -- foreign code's effects are
  not analysed;
- a polyglot runtime handle's methods;
- calls nested deeper than 64, a loop that does not settle, recursion that
  does not converge.

Not counted, and so not seen at all: what a NAAb callback a stdlib function
holds onto does later (an agent tool registered with `agent.register_tool` runs
with the model's arguments, which the analysis takes from the registration
call's own data); a flow through the process environment, files or the
network between two statements; the timing of anything.

## Agreement with the run (the test's cross-checks)

- **Part A**: every row of `taint_semantics.psv` (PR 1's table of what the run
  judges tainted, per expression kind), under both of its origins: the report
  calls the sink a value flow exactly where the run calls the value tainted. One
  over-report is pinned, `if_cond_unwind_cb`: the callback throws on every path,
  but whether `map_fn` calls it at all depends on the list, which the analysis
  does not know.
- **Part B**, R3: every line the VM refuses in `taint_flow.psv`'s programs is a
  value flow in the report. The other direction found **A120**: two value flows
  the run let through -- a stdlib callback's arguments from a tainted list, and
  a struct field store. Both are fixed in the run since (2026-11-05); their rows
  now pin `run` at the flow's line.

- **Part B**, R3 on programs nobody wrote for it: `tools/taintflow/crosscheck.py`
  runs every test program whose own govern.json makes the report list flows and
  compares, line by line, what the run flags with the report's value flows (see
  Measurement).

## Measurement

Observed on 2026-11-05, this container, at master `88235c40` plus this change
(the report binary `f86291c9...`). Two tools, both in the tree, both
report-only.

### What the report finds (`tools/taintflow/measure.py`)

Every `.naab` file under the roots, under ONE policy written for the
measurement (CONFIGURED: sources `env.get`, `file.read`, `io.read_line`,
`http.get`, `http.request`; sinks the ways out of a program -- `http.post/put/patch/request`,
`file.write/append`, `process.run`, `shell_exec`, `codegen.run`; sanitizers
`sanitize_`, `validate_`, `escape_`; `print` is NOT a sink, or the count
measures printing). The shared rule also treats every polyglot block's result as
tainted (V-GOV-006), as a run does, so a block's output is a source here too.

| | examples, docs, tools, stdlib | tests |
|---|---|---|
| files | 101 | 1,140 |
| flows / none / UNMEASURABLE / parse error | 18 / 60 / 19 / 4 | 96 / 752 / 268 / 24 |
| value flows (files) | 51 (15) | 228 (91) |
| control flows (files) | 71 (9) | 7 (5) |
| calls into an imported NAAb module, not followed (files) | 189 (16) | 4,666 (223) |
| polyglot blocks given tainted data with no `<lang>_exec` sink (files) | 44 (9) | 124 (38) |
| calls through a function value not resolved (files) | 1 (1) | 198 (31) |

The parse errors are files `naab-lang parse` refuses too (observed: 28 of 28, with two
files it parses as the control), among them a one-line polyglot block in
`examples/anti_patterns.naab` and an old syntax in `stdlib/result.naab`. Of the 1,140 test files, 386 have a govern.json of their
own that enables taint with a source and a sink (observed).

**The control flows, read by hand.** The 78 control flows (71 + 7) come from
24 decision points -- the branch or loop condition each one hangs on. All 24
were read at three lines each (the condition, its source, a sink under it),
not run, and the steps between were not traced: that a condition depends on its
source is the shared rule's judgement, which Part A pins against the run.

- **23 are real dependencies, and every one is the program acting on its
  input as designed**: a usage message and early return when an argument is
  missing (the three hivemind programs: every later write and process depends
  on whether the argument count was 0), a throw when a fixture disagrees
  (`examples/agent_harness`), a retry loop on a probe's output
  (`docs/book/.../Vigilant`), an error branch on an agent's reply, a length
  check before validating generated code (the living-script programs), a
  health check choosing a rollback (`tests/chapter_verification/...`). The
  report is right that the sink depends on the data; nothing about any of them
  is an exfiltration shape.
- **1 is an over-approximation**: `examples/enterprise/monitoring_system.naab:373`.
  Inside a `try`, a call given tainted data is assumed able to throw because of
  it (here `array.push(all_metrics, metrics)`), so the rest of the try -- the
  next iteration's shell block -- reads as depending on the data. `array.push`
  does not throw on its value's content.
- **None reaches a network sink**, but that says almost nothing: 1 of the 1,241
  programs calls an http send at all (screened: a grep for
  `http.post|put|patch|request(`).

### Agreement with the run (`tools/taintflow/crosscheck.py`)

For the 71 test programs whose OWN govern.json makes the report list flows,
each was run on the VM under that config (a scratch copy of its directory,
an empty HOME) and the lines its taint check flags were compared with the
report's value flows. Observed:

- 10 of the 71 flag nothing, so they are UNMEASURABLE here, not agreement: an
  outer gate stops each before any taint check runs. Nine are edge tests that
  `code_quality.complexity_floor` (SOFT) refuses at `main` -- the suite runs
  them with `--governance-override`, which this cross-check deliberately does
  not pass -- and in the tenth (`tests/gorilla/test12_adversarial_agents`) the
  report's one flow is a `file.write` the filesystem capability refuses first.
- In the other 61, the runs flag **279 lines; 270 are value flows in the
  report**. The other 9:
  - 4 are reached through an imported module, which the report does not follow
    and counts (`test_chaos_module_boundary.naab` 17/41/61,
    `test_edge_18_taint_interaction.naab` 63);
  - 2 are the right sink with the location `:0` -- `t ?? "fallback"` as a sink
    argument loses its line in the run's message (A121(d), fixed since: every
    operator was built with no location, and the VM read an argument's line);
    the report has both as value flows at their real lines;
  - 3 are the run flagging a clean value by name (A121(a)(b)): a `catch (e)`
    that shadows a tainted `e`, and a same-named variable in another function.
- The report's **275 value flows** in those programs: 270 flagged by the run,
  2 the `:0` pair, and 3 more:
  - `test_taint_aliasing.naab:334` and `test_subscript_taint.naab:139`, each a
    struct field store passed to a `<<shell[...]>>` block -- the run misses
    both (A120(b), in two tests nobody wrote for this; fixed since);
  - `test_edge_07_bypass_sanitizer.naab:189`, the report's own over-report:
    `if true { t = validate_input(t) }` -- the report does not fold a constant
    condition, so the path that skips the sanitizer counts.

So on these programs the report missed no explicit flow the run caught, apart
from what goes through an imported module. Two report misses found by this
check were fixed before it was written down: a statement's try-expression
control was dropped when a callback or call later in the same statement ran
statements of its own (rows `c_try_expr_then_lambda`, `c_try_expr_then_call`),
and a function declared inside another did not see the enclosing names (rows
`v_nested_capture`, `v_nested_async`, `v_nested_late`; found on
`tests/governance_v4/async/test_concurrent_async.naab:123`). Re-tracings: the
cross-check was run four times. Three of its instruments were broken and are
fixed in the tool: it first read the result row's `line` field, which can be a
stale check location (81 lines in 19 programs read as disagreements); its copy
of each config directory kept a relative symlink to a parent config, which
dangled, so one program read as a config the run refuses; and it handed the
programs its own stdin, which a `command` prerequisite then waited on (A122).
The last two runs gave the same numbers.

**Adversarial pass.** The claim most likely to be wrong is "23 of the 24
decision points are real dependencies": each was read at its condition, source
and sink, and the steps between were not traced. It would be shown wrong by a
chain in which the taint reaches the condition through an over-approximation
-- the may-throw rule, a weak update of a container, a loop's joined state --
rather than through data. The may-throw shape was searched for in every
origin (one found, counted above); the other two were not looked for. The next
most likely is "missed no explicit flow the run caught": it rests on 61
programs written to exercise taint, and a run-flagged line on a path the report
judges unreachable would show it wrong; none appeared, and programs outside
those 61 are unmeasured.

What this cannot show: agreement on paths the run did not take (the run judges
one path, the report all of them); anything in a program no test runs under a
taint config; and the control flows, which no run judges at all.

### Proposed bar for the integration step (the owner's decision)

This is a proposal from the measurement. Nothing here is decided.

1. **Value flows add little as a refusal of their own**: the run already
   refuses them, and where it does not (A120) the fix belongs in the run,
   which is where a refusal happens. The report's value flows are worth keeping
   as the cross-check that found A120 and A121.
2. **Control flows cannot be refused as they stand.** 23 of the 24 decision points read above
   are a program doing its job, so a gate on them would refuse every program
   that validates its input and exits. Options: (a) keep them report-only, as
   a review aid; (b) refuse only one declared shape -- a network sink, or a
   sink whose arguments differ by branch, under tainted control -- the
   bit-by-bit leak, which this tree barely exercises (1 program in 1,241 sends
   over http), so it would need its own corpus first; (c) an operator-declared
   declassification point, for example "this condition may decide"; this is
   the textbook remedy, and also a new way to say yes, so it would need its own
   ratchet entry.
3. **Before any of it refuses anything: follow imported modules.** 4,855 calls
   into imported NAAb modules (239 files) are not followed. A gate that does
   not follow them is avoided by moving the code into a module.
