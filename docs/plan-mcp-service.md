# Plan — governed execution over MCP

**Status:** slice 1 built (`src/api/mcp_server.cpp`, test
`tests/mcp/test_mcp_stdio.sh`). Written before the code so the premises could
be checked rather than discovered; where building it changed the design, the
change is recorded here beside the premise it replaced. Every fact below says how it was established:
**measured** (a probe ran and its effect was observed, on a 4-core Linux
container with a Release build, under configs written for the probe),
**traced** (followed through the source to the point of effect, not executed),
or **sourced** (read from a primary specification). Premises that did not
survive tracing are recorded, not deleted.

## Why this exists

NAAb exposes one network door today, `naab-lang api` (`src/api/rest_api.cpp`).
It runs every request in the server process, on the tree-walker. The tree-walker
is being phased out, and moving the door onto the VM looked like the job. It is
not, because the worst defects come from sharing one process, not from the
engine. Measured against the current server:

| finding | how |
|---|---|
| A variable set by one request's `<<python>>` block was read by the next request; the same for a shell variable. A fresh-server control read `ABSENT`. | measured |
| One request whose Python block read a null pointer through `ctypes` killed the server for everyone; an ordinary error left it running. | measured |
| With 16 slow requests on the default 8-thread pool, `/health` took 24.6 s to answer (one run). The queue is unbounded. | measured |
| Agent trackers, run-level budgets and registered tools are process-wide statics (`agent_impl.cpp:51-169`), shared by every request. | traced |
| An inline tainted expression reached `file.write` at taint level HARD on the tree-walker (exit 0); the VM blocks it (exit 3). | measured |
| The VM uses the same process-wide Python namespace: a second `<<python>>` block in one VM program reads the first block's variable. So an in-process VM server would leak the same way. | measured |

The application's main callers are AI agents ("Stop AI agents before they do
damage", README). The protocol AI agents use to call tools is the Model Context
Protocol. Nothing in the repository implements it yet (history pass: no commit,
doc or code mentions MCP).

## The design in one picture

```
lease     the caller declares the capabilities it needs
          -> a grant: its request ∩ the project policy, with a time limit
propose   source code -> the static verdict, from a fresh worker running
          `naab-lang --lint-only`; no user code executes
          -> a single-use commit token bound to the program's hash
commit    token -> a fresh worker runs exactly that program;
          governance events stream back as they happen
          -> output, verdict, and a signed execution attestation
```

**Execution is isolated; oversight is continuous.** Every propose and commit
runs in a new `naab-lang` process, so no data, interpreter state or crash
carries from one call to the next. The *governance memory* is per caller: a
server-side ledger of the caller's recent behavioural-sequence events seeds each
worker's sequence detector. That is what catches an exfiltration split across
calls (read a secret in call 1, encode in call 2, send in call 3). A stateless
per-request design cannot see that sequence, and it is the README's headline
example. The ledger holds governance events: an event type, a detail string
and an age. **Correction from building it:** the detail is the call's first
argument truncated to 64 characters, so for `env.get('API_KEY')` it is a name,
but for `crypto.base64_encode(x)` it is the start of the data being encoded.
Pattern globs match details, so the ledger keeps them. It lives in server
memory and in a 0700 per-call directory, and progress notifications carry the
event type only, so no detail reaches the caller.

## Terminology

No new subsystem names. Each piece reuses a concept NAAb already has, for
agents, so an operator who knows those knows these:

| here | existing concept it reuses |
|---|---|
| lease | standing lease (`standing_lease_turns` / `_seconds`): a time limit on authorization |
| propose / commit | `agent.propose` / `agent.commit`: generate without committing, then commit exactly one candidate |
| grant | function-scope capabilities: the effective set is the intersection down the stack |
| attestation | execution attestations (`emitAttestation`, `governance_reports.cpp:478`) |
| ledger events | the behavioural-sequence `RuntimeEvent` (`behavioral_sequence.h:53`) |

## Protocol surface

**Transport:** stdio, one JSON-RPC 2.0 message per line (newline-delimited), as
the MCP stdio transport specifies (sourced). The client launches
`naab-lang mcp` as a subprocess; it is one caller's session. A remote,
stateless HTTP transport is phase 2.

**Versions (sourced from the specification repository's `schema.ts`):**

| revision | session | how the server knows the version |
|---|---|---|
| 2025-11-25 | `initialize` request / `notifications/initialized` | negotiated once at `initialize` |
| 2026-07-28 | none: no `initialize`; servers MUST implement `server/discover` | per request, in `_meta["io.modelcontextprotocol/protocolVersion"]` |

The server answers both. A request carrying the 2026 `_meta` key is handled
statelessly; a session that opened with `initialize` is handled as 2025-11-25.
Results in 2026 mode carry the required `resultType`.

**Methods:** `initialize`, `notifications/initialized`, `server/discover`,
`ping`, `tools/list`, `tools/call`, `notifications/cancelled` (client to
server), `notifications/progress` (server to client, only when the request
carried a `progressToken`). Errors use the JSON-RPC codes -32700, -32600,
-32601, -32602 and -32603.

**Tools:**

| tool | input | output (`structuredContent`) |
|---|---|---|
| `naab_lease` | `capabilities: [FS_READ, FS_WRITE, NET_CONNECT, SHELL_EXEC, ENV_READ, ENV_WRITE, AGENT_SEND]`, `ttl_seconds?` | `lease_id`, `granted`, `denied`, `expires_at` |
| `naab_propose` | `code`, `lease_id?` | `verdict` (`pass` / `blocked` / `quality_gate` / `error`), `findings`, `policy_sha256`, `program_sha256`, `token` (only on `pass`) |
| `naab_commit` | `token` | `verdict`, `exit_code`, `output`, `governance`, `attestation` |
| `naab_run` | `code`, `lease_id?` | propose and commit in one call (still checked first); same output as commit |
| `naab_verify_attestation` | `attestation` | `valid`, `reason`, `key_fingerprint` |

A call without a lease runs under the project policy alone (no narrowing). A
lease can only narrow: `granted` is the requested set intersected with what the
policy allows.

## How each part is built

### Transport: a small new module, not the language server's

`tools/naab-lsp/json_rpc.{h,cpp}` is JSON-RPC 2.0, but it takes `int` IDs only
(MCP IDs may be strings) and frames with `Content-Length` headers (MCP stdio is
newline-delimited). Traced; not reusable as-is. Its nesting-depth guard
(V-LSP-006, `lsp_server.cpp:88`) is reused, so a deeply nested message cannot
overflow the parser's stack.

### Workers: the CLI run path, not a replica of it

- **One copy of the checks.** The worker is `naab-lang` itself. The CLI run path
  (`main.cpp`, ~1,600 lines from 1105) holds every governance step: integrity,
  blocked flags, whole-source checks, intent preflight, preflight scanner, agent
  review, compile, run, reports. A second in-process copy would drift; that is
  how most past gaps here happened (`docs/investigation-method.md`, "A fix lands
  in one copy").
- **Same policy as the project.** The program is written to a uniquely named
  hidden file in the project directory and run from there. Discovery walks up
  the directory tree from the script (`governance_engine.cpp:820`, traced), so
  the worker loads the project's own `govern.json`. There is no config-path flag
  a worker could be pointed elsewhere with.
- **`propose` runs none of the caller's code.** `--lint-only` exits right
  before execution (`main.cpp:2581`); contract tests (`must_produce`) run
  later (`main.cpp:2646`). Traced. Exit codes: 0 pass, 2 quality gate, 3 HARD
  block. **A premise raised and then withdrawn on reachability:** the VM's
  library-block import loop (`use BLOCK-ID as x`, `main.cpp:2534-2575`) calls
  `executor->execute(code)` above the lint exit, which read as "a lint runs
  installed blocks". It cannot run: the VM compiler has rejected
  `use BLOCK-...` since `944febc4` (`compiler.cpp:1079`, traced: every
  `use` that is not a `./` or `../` path throws), so compilation fails before
  the loop is reached. Measured with one installed block whose top level
  writes a marker: a normal tree-walker run writes it (the positive control
  that the block is reachable at all), while a VM run fails at compile and
  `--tree-walk --lint-only` exits before block imports, and neither writes it.
  The loop looks unreachable on the VM; only paths that compile a `use BLOCK-`
  were checked, so "dead" holds at the traced tier. Nothing in this slice
  changes it.
- **`agent_review` makes LLM calls during propose** when the project configures
  it (`main.cpp:2343`, before the lint exit, traced). Propose is then as slow
  as that review; that is the project's choice, not this door's.
- **Containment of the worker is not the polyglot containment.** Every
  `async fn` call is a new OS thread (`vm.cpp:3830`, traced), and threads count
  against the per-user process limit: run as `nobody` with a limit of 3, an
  async program failed at once; without it, it ran (measured). A per-user limit
  is shared by every worker, so it must not be the worker's limit. Per-worker
  limits belong in a cgroup (phase 3).
- **Async and parallel agents keep working inside a worker** (measured, single
  runs): 4 × `time.sleep(1)` async 1.05 s vs 4.03 s serial; 6 agent calls via
  `agent.batch` 1.06 s vs 6.05 s sequential.

### Lease grants: one more intersection in one helper

Every capability gate calls one helper, `checkFunctionCapability()`, and
top-level code resolves through the `default` entry ("top level resolves via
default", traced). A grant becomes one more set in that intersection, at HARD,
so it narrows every frame and can never widen one. The same grant also strips
the worker's sandbox, the way `syncGovernanceToSandbox` already does for
`capabilities.*`. That covers polyglot code, which the helper does not see:
no `SHELL_EXEC` marks shell disabled by policy (every separate-process language
refused), no `NET_CONNECT` removes network, no `FS_WRITE` makes the filesystem
read-only. Tightening only.

### The per-caller ledger: replay, then stream

- **What the detector needs.** Sequence windows count intervening EVENTS
  (`max_gap`), and patterns also expire after `decay_seconds` (default 300) and
  `decay_turns` (`governance.h:1324`, traced). So replaying the caller's recent
  events in order, each stamped with its real age, reproduces the detector's
  state exactly. The final step is blocked before it runs by
  `checkPreExecution()` (`vm.cpp:1938`, traced: agent.send, file read/write,
  http.*, encode, process.*, env.get/list). The VM is the only engine that calls
  it, which suits a VM-only door.
- **Seed:** before the run, the worker replays the ledger's events through
  `recordEvent()` and ignores any match those historical events complete. A
  sequence already blocked in an earlier call must not be charged again.
- **Stream:** each event the worker records is written at once, as a JSON line,
  to a **pipe** the server reads, not a file. The engine has exactly two
  `recordEvent()` call sites (`emitEvent`, `governance_engine.cpp:7411`, and
  the consumed final step in `checkPreExecution`, `:9045`; traced), and both
  write through one helper. The pipe feeds both the ledger and the progress
  notifications. Because the server reads it as events happen, a worker
  killed by cancel, timeout or crash loses nothing it already recorded, and a
  program cannot take back what it has written. A file could be truncated by
  a subprocess language that NAAb's path policy does not reach (CONTRA-013),
  which would erase the program's own history. The worker sets close-on-exec
  on the descriptor, so polyglot children do not inherit it. In-process
  Python could still write to it. That can only add events, and adding
  benign events between two steps is something any program can already do by
  calling `file.read` eleven times (`max_gap` counts events). So it opens no
  new evasion. The server parses the pipe as untrusted input: bounded line
  length and count, known event types only.
- **Replay fidelity, traced in `behavioral_sequence.cpp:441-560`:**
  `recordEvent()` keeps a timestamp it is given (it fills one in only when the
  field is empty), and both decay checks compare stored timestamps. So
  stamping each replayed event at `now - age` reproduces time decay.
  Sequence IDs are reassigned contiguously, which reproduces `max_gap` as long
  as the ledger holds every event the caller produced (it does; it does not
  filter). Turn decay is NOT reproduced: turns are per agent handle and
  handles do not survive a process. Replayed events carry turn 0, which the
  detector treats as "no turn" (`ev.turn > 0 && state.last_match_turn > 0`),
  so only time decay applies to them. That direction keeps a pattern alive
  longer, never shorter.
- **The blocked step is history too.** `checkPreExecution()` records the step
  it refuses (`:9045`) so that the pattern resets. The stream therefore
  carries it, and replay resets the pattern in the next worker the same way.
- **Bound:** the ledger keeps the newest `behavioral_sequences.window_size`
  events (default 200), the same bound as the detector's own ring buffer.
  Known difference: the detector's per-pattern state is not bounded by its
  buffer, so a pattern with `max_gap` above `window_size` can keep a step
  in-process that the ledger has dropped. The ledger size is therefore the
  larger of `window_size` and the largest `max_gap × steps` among the active
  patterns.
- **Inherits the project's settings.** BSD is off unless
  `behavioral_sequences.enabled` is true, and the built-in patterns run at
  ADVISORY unless `default_pattern_enforcement` is `"declared"` (traced,
  `governance.h`). Cross-call detection is exactly as strong as in-process
  detection under the same config. With BSD off, the server says so once at
  startup on stderr.
- **Commits from one caller run one at a time while BSD is enabled.** Each
  worker is seeded when it starts. Two commits in flight together therefore
  cannot see each other's events. A caller could start "read the secret",
  then, while it runs, "encode" and "send" in parallel calls, each seeded
  before the others' events existed. Serialising one caller's commits closes
  that. Parallelism stays available where it costs nothing to detection:
  proposes run concurrently, separate callers run concurrently, and one
  program can still use `async` and `agent.batch`. With BSD disabled, commits
  run concurrently up to `api.max_concurrent`.

### Commit tokens

Single-use, held server-side, bound to the program's SHA-256, the policy's
SHA-256, the lease and the session. The policy hash covers every file in the
`extends` chain and the `.sig` beside each, because a parent can change while
the child's bytes do not. The program bytes stay on the server, so commit runs
exactly what was checked and the client cannot swap code between the two
calls. If the policy changed since propose, commit is refused: propose again.
Tokens expire with the lease, and in any case after `limits.timeout.global`.

### Streaming and cancellation

When a `tools/call` carries a `progressToken`, each streamed governance event
becomes a `notifications/progress` message. `notifications/cancelled` for an
in-flight call kills the worker's process group, which stops even Python stuck
inside a C call. The in-process timeout cannot do that (CLAUDE.md, timeouts
gotcha).

### The output is a new egress, and gets the existing scan

On the command line a program's output goes to the person who ran it. Here it
goes to an AI agent, whose context is sent to a model provider. The server
therefore runs the response scan that `agent.send()` already applies to model
output (`checkSecrets()` / `checkPii()`) over the program's output before
returning it, under the same keys (`code_quality.no_secrets`, `no_pii`; off by
default, and at least SOFT when on). The server loads the project policy for
this; no user code runs in its process. On a hit, the output is withheld and
the verdict is `blocked`. The scan is bounded the same way as every scanner
regex (CLAUDE.md, scanner-regex gotcha).

### Attestations

Signed when `audit.provenance.sign_records` is on and
`audit.provenance.signing_key` names a key, the same condition, key and
Ed25519-over-sorted-key-JSON scheme as existing attestations
(`governance_reports.cpp:492-498`, traced). An attestation is evidence only if
the attested program cannot read the key that signs it. Today it can:
`addGovernanceProtectedPaths()` protects `govern.json`, its `.sig` and the
trusted-keys directory, not the provenance signing key. Slice 1 adds the key
to the protected paths. That is a tightening on every path, the command line
included. A subprocess language can still read it where NAAb's path policy
does not reach (CONTRA-013). Fields: call id, tool, program
SHA-256, policy SHA-256, whether the policy file is signed, the grant, verdict,
exit code, output SHA-256, event count, the previous attestation's SHA-256 (a
chain per session), times, and the key's fingerprint. With no key configured,
attestations are still produced and hashed, carry `"signed": false`, and say
so.

## Configuration: existing keys first

| need | key | new? |
|---|---|---|
| per-call timeout | `limits.timeout.global` | no |
| ledger size | `behavioral_sequences.window_size` | no |
| attestation key | `audit.provenance.signing_key` | no |
| the policy itself | the whole project `govern.json` | no |
| concurrent calls | `api.max_concurrent` (default: CPU cores) | yes, the only one |

`api.max_concurrent` sits in the existing `api` section, which already holds
the REST door's timeout and limits; this is the same door.

## Internal worker flags

The server passes the worker three flags. They are plumbing between two NAAb
processes, not user features (govern.json stays primary), and each can only
tighten:

| flag | effect | why it cannot loosen |
|---|---|---|
| `--event-seed FILE` | replay prior events into the detector | adds history, so more sequences complete; never removes one |
| `--event-stream-fd N` | write each recorded event to an inherited pipe as it happens | output only |
| `--lease-grant A,B,…` | intersect every capability check with the grant | intersection only |

Like every CLI flag, each goes in both the global pre-scan and the run loop in
`main.cpp`. The seed file lives in a per-call directory the server creates and
removes; the worker reads it at startup, before any user code runs, so
changing it later changes nothing. Known limit: code in a subprocess language
can still reach a file path NAAb's own path policy does not cover (CONTRA-013;
`docs/protection-map.md`). Under `capabilities.shell.enabled: false` those
languages are refused outright.

## Tests

Suite `tests/mcp/test_mcp_stdio.sh`, driven by a Python standard-library client.
Every refusal is paired with a success through the same fixture, and each arm
names what it fails on when its mechanism is removed.

| arm | asserts | fails when |
|---|---|---|
| MC-00 | handshake (2025-11-25 `initialize`) and `tools/list` list the tools | server not built / transport broken |
| MC-01 | 2026-07-28 without `initialize`: `server/discover`, `tools/list` and `tools/call` carry `resultType`; an unknown version is -32022 with `supported`/`requested` | 2026 path missing |
| MC-02 | `naab_run` on a trivial program returns its output (positive control for every arm below) | workers broken |
| MC-03 | a violation is `blocked` with no token; a clean program's propose leaves its marker file unwritten and its commit writes it | propose executes code, or commit does not run the proposal |
| MC-04 | a replayed token is refused | tokens not single-use |
| MC-05 | commit after `govern.json` changed is refused (MC-03's unchanged commit is the control) | token not bound to policy |
| MC-06 | lease without `FS_WRITE`: a write is refused; with it, the same program writes | grant not enforced / grant blocks everything |
| MC-06s | lease without `SHELL_EXEC`: a `<<sh>>` block is refused; with it, it runs | the grant does not reach the sandbox |
| MC-06a | lease without `AGENT_SEND`: `agent.send` is refused before the provider is called; with it, the reply arrives from a loopback stub | the capability helper does not intersect the lease |
| MC-07 | a capability the policy denies is in `denied`, not `granted`; the same request in a project that allows it is granted; an unknown action is -32602 | grant is a union |
| MC-08 | no leak: call 1 sets a Python global, call 2 reads `ABSENT`; the same two blocks inside ONE call read the value (control) | workers shared |
| MC-09 | crash containment: a crashing call fails alone (`crashed`), the next call succeeds | crash reaches the server |
| MC-10 | split sequence: secret read, unrelated call, encode, write to the outbox across four calls → the write is blocked before it happens (no outbox file); the write alone runs and the whole sequence in one call is blocked (controls) | ledger not seeding |
| MC-11 | after that block, the same write alone runs: the sequence is not charged again | replay re-enforces history, or the refused step is not streamed |
| MC-12 | a cancelled call's worker is gone within 1 s and the server answers the next call | cancel does not kill |
| MC-13 | progress notifications arrive before the result, and none without a `progressToken` | streaming broken |
| MC-14 | attestations are signed, verify, fail when one field changes, and chain | signature not checked |
| MC-14k | the attested program cannot read the signing key; it can read another file in the same directory (control) | signing key not protected |
| MC-14u | with no key configured, attestations are produced and say `signed: false` | unsigned path broken |
| MC-15 | malformed JSON -32700, unknown method -32601, unknown tool -32602; the server keeps serving | parse errors kill the server |
| MC-17 | concurrent split: A reads the secret and waits; B (waits, then encodes) and C (writes the outbox) are issued while A runs → C is blocked; with BSD disabled a fast call overtakes a slow one (control: calls really overlap) | commits not serialised per caller |
| MC-18 | with `no_secrets` on, a secret-shaped value built at runtime is withheld and the call `blocked`; with it off the same output is returned (control) | output not scanned |
| MC-06e | lease without `ENV_READ`: embedded Python's `os.environ` reads no secret; with it, it reads the value (control) | the lease gates only `env.get()`, not polyglot env |
| MC-18e | with `no_secrets` on, a secret written to stderr is withheld from `findings`; with it off the value reaches `findings` (control) | stderr (the `findings` channel) not scanned |
| MC-19 | events streamed before a crash survive it: a read followed by a crash, then encode, then the write → the write is blocked | events kept only when the worker exits cleanly |

The sequence's last step writes a local "outbox" file rather than calling
`http.get` on a loopback stub, as first planned: the http module refuses
private and loopback addresses unconditionally (SSRF protection), so a stub
could never receive and the control arm read UNMEASURABLE. A local write is
observable, needs no network, and is pre-checked by the same
`checkPreExecution()` path.

Plus: the error-message leak suite (which now scans `mcp_server.cpp`, since
its error strings go straight to an AI caller), the dead-interpreter gate (MC-02
is the positive control), and the mutation run below.

**Mutation run** (observed, this container): each mechanism removed in turn,
the binary rebuilt, the suite run. The arm that names the mechanism must go
red.

| mutant | removes | expected red | result |
|---|---|---|---|
| M01 | seeding the worker from the ledger | MC-10, MC-17, MC-19 | caught (exactly those three) |
| M02 | per-caller commit ordering | MC-17 | caught |
| M03 | streaming the step `checkPreExecution()` refuses | MC-11 | caught |
| M04 | replay enforcing what it completes | MC-11 | caught |
| M05 | the lease's narrowing of the capability switches | MC-06s | **missed at first**: MC-06s used a `<<sh>>` block, which reaches the lease helper through `checkShellAllowed()`, so the sandbox layer was never isolated. MC-06s now uses embedded Python's `open()` and `os.system()`, which only the sandbox sees. Re-run: caught |
| M06 | the lease check in `checkFunctionCapability()` | MC-06a | caught |
| M07 | single-use tokens | MC-04 | caught |
| M08 | binding the token to the policy hash | MC-05 | caught |
| M09 | the output scan | MC-18 | caught |
| M10 | protecting the signing key | MC-14k | caught |
| M11 | cancellation | MC-12 | caught |
| M12 | progress notifications | MC-13 | caught |
| M13 | signature verification (always valid) | MC-14 | caught |
| M14 | ledger filled only when the worker exits cleanly | MC-19 | caught |
| M15 | event details in progress messages | MC-13d | caught |

All fifteen are caught, and no mutant turned an arm red that it was not
expected to, so the arms are specific as well as sensitive. The miss is the
finding worth keeping: an arm can pass for a layer it never reaches when
another layer answers first.

## Performance

Baselines (measured): `naab-lang` starts and runs a trivial program in a median
30 ms (25 serial runs), 40 ms with a Python or shell block; `--version` alone
takes 26 ms, so most of the cost is process start. A CPU-heavy loop ran in
0.81 s on the VM against 5.74 s on the tree-walker (one sample each).

| target, slice 1 | how it is measured |
|---|---|
| propose p50 under 60 ms on this box | report-only timing arm |
| a cancelled worker gone within 1 s | MC-12 |
| no per-call state survives between calls | MC-08 |
| server memory flat across 200 calls | report-only |

Phase 1b adds a warm pool: workers started ahead and parked on stdin until a
job arrives. That hides the 26 ms start from latency, not from CPU. Built only
if slice-1 measurements show the start dominates.

## Phases

1. **This slice:** `naab-lang mcp` over stdio; the five tools; workers;
   lease; ledger; attestations; cancel; streaming; the output scan; the
   signing key protected from the program it attests; the tests above.
2. **1b:** the warm worker pool, gated on the slice-1 measurements.
3. **2:** stateless Streamable HTTP for remote callers. The ledger keyed by the
   authenticated API key (`api.keys`), in SQLite (already a dependency), so any
   server can serve any call. Same tools and the same job semantics, plus the
   job API that long agent workflows need. The REST `/execute` door is retired
   once this is in place.
4. **3:** Linux per-worker isolation through nsjail: namespaces, seccomp,
   cgroups (`pids.max` for the thread limit above).

Later, separate tracks: an A2A adapter, OpenTelemetry export, and a governance
gateway that speaks the model providers' own APIs.

## Risks and open questions

- **MCP is still changing.** Within a year it moved tasks out of the core
  and removed sessions (sourced). The tools above are a thin adapter over the
  propose/commit/attestation contract, so a spec change touches the adapter
  only.
- **The ledger is shared state again.** It is kept small, append-only and
  unreachable from user code, and holds events, never values.
- **Identity.** In stdio mode the caller is whoever launched the server. In
  phase 2 the ledger must key on the authenticated identity, not the lease,
  or a caller resets its history by asking for a new lease.
- **File placement.** Hidden program files in the project root keep relative
  imports resolving as they do for a script there. They are removed after each
  call, and stale ones are removed at server start.
- **A lease that grants `SHELL_EXEC` grants what a shell can reach.** A program
  with shell can start another `naab-lang` that discovers its own policy, or
  none (CLAUDE.md, "governance is discovered per process"). The grant and the
  path policy then bind nothing in that child. Narrow leases are meaningful
  only without `SHELL_EXEC`. The `naab_lease` result says so whenever it
  grants it.
- **Cancel kills the worker's process group.** A child that calls `setsid()`
  leaves the group and survives. Phase 3's cgroup kill is the complete
  answer.
- **History lives as long as the server.** Restarting the stdio server starts
  an empty ledger. That is no weaker than today, where every CLI run starts an
  empty detector; the ledger extends the detector across the calls of one
  session. Phase 2 persists it per API key.
- **Not carried across calls:** advisory escalation counts, CDD state, taint
  (no value crosses a call; a value the caller copies from one call's output
  into the next call's source arrives untainted), and the function-capability
  advisory summary. Each is per call, as each is per run on the command
  line.

## How sure this is

**Re-tracings: three independent passes, and each changed the design.**
The first, before writing, found that the language server's JSON-RPC cannot be
reused (integer IDs, header framing) and that a per-user process limit cannot
contain a worker. The second was the adversarial pass below. It changed four
things: the stream became a pipe; commits are serialised per caller; output
gets the secret scan; the token covers the `extends` chain. It also raised a
fifth, that `--lint-only` runs library blocks. The third pass was writing the
tests, which ran that claim: it did not survive a reachability check (above).
The same pass added protection of the attestation signing key. A fourth pass
was building it, and changed three more things: the sequence's last step (a
loopback stub cannot receive; above); commit order is arrival order, not lock
order, because two commits racing for a mutex would let C run before B and the
caller's intended order is the order it sent them; and the stdin reader is
bounded, because `std::getline` held a line of any size before the size check
ran. Each pass has changed something, so the stopping rule is not met; the
mutation run below is the check that the arms detect what they claim to.

A fifth pass came from outside: an independent read-only session re-audited the
built PR and found three gaps this document's own adversarial pass had missed or
scoped too confidently — stderr was an unscanned egress (the scan covered stdout
only); a lease without `ENV_READ` gated `env.get()` but not embedded Python's
`os.environ` or children; and the attestation signing key, though a protected
path, is still readable by a `<<sh>>` block because path policy does not reach
subprocess languages. All three were reproduced here with controls. The first
two were fixed (arms MC-18e, MC-06e) -- the second only partly: re-measured
after the fix, embedded Python reads the withheld values back from the
scrub's own save dict, and `/proc/self/environ` still shows them to any
program that can read files. That remainder and the third finding are the
general `blocked_read` and signing-key exposure under `elevated`, which go
in their own follow-up PR (open-investigations A32), scoped in "What this
does not do" rather than marked closed. That an external pass found three real gaps is the evidence that
the internal stopping rule was not yet met — the rule itself predicts this.

**Adversarial pass.** Each claim judged most likely to be wrong, what would
show it wrong, and whether it was checked:

| claim | what would show it wrong | checked |
|---|---|---|
| replaying events with their ages reproduces the detector's state | `recordEvent()` overwriting a supplied timestamp, or decay reading the wall clock | yes: traced, it keeps a supplied timestamp; turn decay does not replay (recorded above) |
| `propose` executes none of the caller's code | anything between parse and the lint exit that runs code | yes: the library-block import loop looked like such a path, but it is unreachable on the VM (compile rejects the syntax) and runs after lint on the tree-walker; measured both |
| an attestation is evidence | the attested program reading the signing key | partly: the key is a protected path (arm MC-14k), but path policy does not reach subprocess languages, so in a shell-enabled project a `<<sh>>` block reads the key (CONTRA-013). Scoped, not closed: the follow-up PR (open-investigations A32) measures every route to the key — see "What this does not do" |
| the per-caller ledger closes cross-call sequences | concurrent commits seeded before each other's events | yes: found; commits serialised, arm MC-17 |
| the stream cannot be used to hide history | a program rewriting the stream | yes: a file could be truncated; changed to a pipe, arm MC-19 |
| no new egress | output reaching a model provider with no scan | yes, both channels: stdout (arm MC-18) and the stderr carried back in `findings` (arm MC-18e). The first build scanned stdout only — an independent review found the stderr gap and it was reproduced and fixed |
| a lease narrows env everywhere, not only env.get() | embedded Python's os.environ, a child, or `/proc/self/environ` still showing the vars | partly. The first build gated only env.get(); the same review found embedded Python still read the environment, and a denied ENV_READ now forces the allowlist env scrub (arm MC-06e). Re-measured after that fix (observed, one run each): the scrub keeps the popped values in `_naab_saved_env`, a dict in the block's own namespace, readable under a lease with no capabilities; and `/proc/self/environ` still shows the value to `file.read`, `io.read_file`, Python `open()` and a `<<sh>>` child reading its parent's -- the general `blocked_read` gap, left to the follow-up PR (open-investigations A32), see "What this does not do" |
| propose stays under 60 ms | compile time, or `agent_review`, dominating | measured: p50 34-35 ms, p90 53-56 ms over 15 serial proposes of a trivial program (observed, two runs, this container, Release, no `agent_review`); a large program or a configured `agent_review` is not measured |
| the arms detect what they claim | an arm that stays green with its mechanism removed | mutation run: each mechanism removed in turn, results below |
| a send to a loopback stub observes exfiltration | the http module refusing loopback | yes, and it does refuse (SSRF, unconditional); the arms use a local write instead |

**Direction of error.** The dangerous direction is false reassurance: the ledger
misses a cross-call sequence and the door looks safer than one long script.
Each design choice that could fall either way was taken toward detection.
Replayed turns skip turn decay, so patterns live longer. The ledger is sized to
the largest pattern reach. Forged events can only add history. Commits from one
caller are serialised. The cost is the other direction: a sequence can complete
across calls that an operator thinks of as unrelated, and the caller is blocked
(MC-11 guards against charging the same sequence twice). That is the direction
that was checked harder: every arm that expects a block has a control expecting
success through the same fixture.

**Blind spots: not measured, which is not the same as absent.**

- Windows and macOS. The pipe, process-group kill and close-on-exec are POSIX
  in the plan; Windows needs a job object and an inherited handle, and is
  untested.
- Interop with MCP clients other than one. The suite's own client is written
  from the same reading of `schema.ts` as the server, so it cannot catch a
  misreading. The official MCP Python SDK (2.3.0) was run against the server
  as an independent client, once on each revision (observed): `initialize`
  negotiated 2025-11-25 and `discover` returned the supported versions; both
  listed the five tools, ran a program, delivered progress (event types only),
  blocked a sequence split across two calls, and committed a proposal. It is
  not in CI, because CI would have to download the SDK. The TypeScript SDK
  and desktop clients are untested.
- Process start under concurrency, memory per worker under load, and
  production hardware. Every timing here is serial, on an idle 4-core
  container.
- nsjail: not installed here; phase 3 is untested on this machine.
- The 7× VM figure and the 24.6 s head-of-line figure are single samples.

## What this does not do

- It does not make subprocess languages obey `blocked_paths`; that is phase 3
  (nsjail) or `capabilities.shell.enabled: false`. One consequence worth naming:
  in a project that both signs attestations and leaves shell enabled, a
  `<<sh>>` block (or any subprocess language) can read the attestation signing
  key off disk, because the protected-path rule reaches NAAb's stdlib and
  embedded Python but not subprocess languages (CONTRA-013). A lease without
  `SHELL_EXEC` closes it; so does `capabilities.shell.enabled: false`; the
  general fix is phase-3 OS sandboxing. An attestation therefore proves what
  ran, not that the key was unreadable in a shell-enabled project. Every
  route to the key (stdlib, embedded Python, each subprocess language, both
  engines, enforce and audit) is measured in the follow-up PR
  (open-investigations A32), not here.
- It does not withhold the environment from code running in the worker. A
  lease without `ENV_READ` denies `env.get()` and scrubs every child's
  environment, and it empties embedded Python's `os.environ` (arm MC-06e) --
  but the scrub keeps the popped values in `_naab_saved_env`, a dict in the
  block's own namespace, and a `<<python>>` block reads them back under a
  lease with no capabilities at all (observed, one run). The worker's
  environment block is also unchanged, so `/proc/self/environ` shows the
  value to `file.read`, `io.read_file` and Python `open()` when `FS_READ`
  is granted, and a `<<sh>>` child reads its parent's when `SHELL_EXEC` is
  (observed, one run, `elevated`). Until the follow-up PR
  (open-investigations A32) lands, a denied `ENV_READ` stops `env.get()`
  and the child environment and nothing else. Every one of these routes
  exists for `env_vars.blocked_read` in any project (`_naab_saved_env` came
  with the #302 scrub); none is added by the server.
- It does not make a lease refusal and a program failure distinguishable by
  verdict. A lease denying `env.get()` surfaces as a sandbox violation
  (`verdict: error`, exit 1); one denying `file.write()` surfaces as a
  governance block (`verdict: blocked`, exit 3). Both are the engine's own
  exit codes for those two denials, and the server reports them faithfully
  rather than guessing a caller's intent; a caller distinguishes them by the
  `findings` text, not the verdict.
- It does not put any NAAb program in the server's own process. No user code
  ever runs there.
- It does not replace the in-process C API: embedders that call governance
  checks from their own process keep using it.
