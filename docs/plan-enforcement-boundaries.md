# Plan: one policy, two enforcement mechanisms

Status: in progress. Stages 1 and 2 landed in #223, stage 3 in this branch.
Stage 4 (the process edge) is open.

This plan exists because four separately-reported findings turned out to be one
defect wearing four coats, and patching them individually would have left the
shape that produced them intact.

## The defect

Enforcement is written at call sites rather than at boundaries, so its coverage
equals whoever remembered to type it. Every finding traced in this campaign is
that sentence with different nouns:

- the sandbox capability check written sixteen times in sixteen executors, most
  of the copies wrong (F41, fixed in #222)
- the sandbox installed by the command line and not by the other entry points
  (F29, fixed in #223)
- `govern.json` protected from NAAb programs but not from the package manager
  (F39, fixed in #221)
- the audit-log signature check conditioned on the entry carrying a signature
  (F31, fixed in #218)

Underneath all of them, absence of context means permission. No sandbox
installed reads as nothing to check. Forgetting and allowing are the same code
path, so a gap never announces itself.

## What was measured

Provenance matters here, so each row says how it was established. "Measured"
means a probe was run and its effect observed; "traced" means read in the source
and not executed; "unmeasurable" means the probe could not be made to run and
the question is open.

| fact | how |
|---|---|
| Of seven entry points constructing an `Interpreter`, two install a `ScopedSandbox` (`main.cpp`, `context.cpp`); four REPL sources and `rest_api.cpp` install none | traced |
| The one `ScopedSandbox` in `main.cpp` (line 1369) sits INSIDE the `if (command == "run")` branch opened at line 1105; `calibrate` (4042) and `race` (4394) are sibling branches outside its scope | traced |
| A Rust block posted to `/api/v1/execute` under `sandbox_level: "restricted"` EXECUTES and writes a file, on a build that carries the #222 gate | measured |
| A HARD governance block over REST terminates the whole daemon: benign request succeeds, blocked request returns an empty reply, next request is connection-refused | measured |
| `race` does not execute a single-block file even at `elevated`, so it cannot be used to test entry-point coverage with that input | unmeasurable |
| NAAb's `file.read` is HARD-blocked by `capabilities.filesystem.blocked_paths` while a `<<python>>` block in the SAME program reads the same file and governance reports PASS | measured |
| Landlock headers are present on the development machine and the syscall returns ENOSYS, so per-process path enforcement is not portable | measured |
| A project configured `allowed_paths: ["."]` reads its own `govern.json`; the same config with `allowed_paths` empty cannot. One broad allow voids `addGovernanceProtectedPaths()` | measured |
| `checkPathAccess()` contained two precedence rules — capabilities let any allow cancel every block, the agent overlay applied blocks first and unconditionally | traced |

The last two together are the source-of-truth problem: the configuration file
describes a path policy, and that policy reaches NAAb's own standard library and
nothing else. A reader of `govern.json` cannot tell.

## The design

**One policy. Two enforcement mechanisms. Never a silent gap between them.**

A function call cannot span a process edge, so "call the decision function
everywhere" is not implementable as stated. What can be everywhere is the
policy. The enforcement is two different machines, and the requirement is that
neither silently claims the other's coverage.

1. **In process**: one decision function, `decide(subject, action, resource)`,
   fed by the governance config, called by the standard library, the agent role
   checks, the polyglot dispatcher and the request handlers. Today those are
   four code paths with three different vocabularies for paths.
2. **Absent context denies**: a missing sandbox yields a deny-all policy rather
   than no policy. Forgetting to install enforcement then fails closed and
   loudly.
3. **At the process edge**: the same policy compiles into whatever the operating
   system will enforce, and the runtime reports which mechanism it got. Where
   the platform provides none, NAAb says so at load time rather than letting the
   config imply a coverage it does not have.

## Staging

Each stage is verifiable on its own, and each test is written before the change
it protects so that it starts red.

1. **Entry-point parity harness.** One scenario, run through every entry point
   that can execute code, asserting identical verdicts. Starts red on the REST
   arm. Enumerated from the binary's own subcommand list where possible, per the
   rule in `investigation-method.md`: a list you typed is correct on the day you
   typed it.
2. **Invert the default.** Absent sandbox denies; the open entry points install
   policy explicitly. Stage 1 turns green. This is the change with real blast
   radius and needs its own survey of everything that currently runs with no
   sandbox by accident.
3. **Collapse the path vocabularies** into the single decision function, with
   precedence decided once and most specific winning. This is also the fix for
   F9, where a broad allowed path currently cancels a specific blocked path.
   *Done.* `decidePathAccess()` is that function, and both layers of
   `checkPathAccess()` now call it. Precedence is longest-prefix-wins with ties
   denying; the agent overlay passes `DenyWins` because a role narrows the
   project policy and must never widen it.

   One vocabulary is deliberately left outside it. `Sandbox::isPathAllowed()`
   has an allow list and no deny list, so it has no precedence to unify — its
   fragmentation is a duplicate MATCHER, not a second answer to the same
   question, and folding it in would be a refactor with no verdict to change.
   Stage 4 is where the sandbox and the governance policy have to agree.
4. **Process edge.** Compile the policy to an operating system ruleset where
   available, report the mechanism on the governance dashboard, and warn when
   the configuration promises more than the platform can deliver.

Stages 1 and 2 close the class. Stages 3 and 4 are what make the configuration
file truthful.

## Standing rules this campaign produced

Both are already in `investigation-method.md` and are repeated here because they
are why the stages are ordered this way.

- **Enumerate from the system, not from the report.** The audit named two
  ungated languages. Enumerating the registry found three. A test built from the
  report would have gone green with a hole still open.
- **A fixture built by hand can grant the property you are testing for.** The
  negative arm must be built by omission, not by neutralisation.
