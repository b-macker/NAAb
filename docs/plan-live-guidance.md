# Plan — live governance guidance

**Status:** slice 1 built (`src/runtime/governance_guidance.cpp`, test
`tests/governance_v4/test_guidance.sh`). Written before the code so the premises
could be checked rather than discovered; where building it changed the design,
the change is recorded beside the premise it replaced ("**Changed while
building**"). Every fact says how it was
established: **traced** (followed through the source to the point of effect, not
executed), **measured** (a probe ran and its effect was observed), or
**decided** (the project owner's call, with the date).

## Why this exists

NAAb's error messages are written for the model reading them: every block carries
`Help:` and `Example:` text, and an agent in a terminal acts on it. That text is
compiled into the binary. Nothing carries *project* knowledge (a team's playbook,
past runs, a database of decisions) to an agent or to the terminal at the moment
governance knows something went wrong. The parts that come closest, traced:

| mechanism | direction | content | when |
|---|---|---|---|
| helper errors (`Help:`/`Example:`) | engine -> terminal | compiled-in strings | every block |
| per-section `rationale` | govern.json -> reports, dashboard (up to 3 blocks) | static | end of run |
| mandate reinforcement / coherence correction (`agent_impl.cpp`, the `MANDATE_INJECTION` block) | engine -> agent, ephemeral prepend | `system_prompt` or fixed text | every N turns / coherence below threshold |
| contextual step-up challenges | engine -> agent | DriftState | elevated level |
| `project_context` (`project_context.cpp`) | project docs -> **rules** | CLAUDE.md, linters, manifests | once, at load |
| hooks (`fireHook`) | engine -> outside | event vars | violation, start, end; output never read |
| agent tools (`register_tool`) | agent pulls | whatever the script registers | when the model calls |

`project_context` runs the other way: it turns docs into constraints. The pull
route (a script registering a tool that reads a doc) already exists; what does not
is delivery triggered by what governance just observed, governed and logged.

#164 measured the engine's steering and recorded the second motive: steering is
wired and works on a stub, but "the block decision has no memory of the steer" --
correction repeats on a timer and never asks whether the last one helped.

## The design

The component that blocks is the one that knows exactly why: which rule, which
drift signal, which validation failed and with what detail. Use that to select
the matching piece of operator-written project knowledge, deliver it through the
same governed channel, and record exactly what was delivered.

```json
"guidance": {
  "enabled": true,
  "sources": { "playbook": { "type": "file", "path": "docs/agent-playbook.md" } },
  "rules": [
    { "on": "start",                    "from": "playbook#onboarding", "to": ["agent"], "agents": ["developer"] },
    { "on": "signal:mandate_alignment", "from": "playbook#stay-on-task", "to": ["agent"] },
    { "on": "validation_failed",        "from": "playbook", "to": ["agent"] },
    { "on": "violation:code_quality.no_placeholders", "from": "playbook#stubs", "to": ["terminal"] }
  ]
}
```

- **Sources** are declared only in govern.json. Slice 1 has one type, `file`: a
  markdown document split into sections at its headings, addressed by GitHub-style
  heading slugs (`#stay-on-task`).
- **Live.** A source is re-read when its size or modification time changes, so an
  operator steers a running agent by editing a file -- no reload, no re-signing,
  no ratchet. *Decided 2026-10-12: "it should be allowed to change, needs to be
  fluid."* The content has no authority: it is text; every capability gate still
  applies. The *set* of sources changes only through an ordinary govern.json
  reload.
- **Triggers** are things the engine already computes:
  - `start` -- the first send of a handle;
  - `signal:<key>` -- a drift signal PAID a penalty on a turn that was actually
    analysed (`cddAnalysisSeq` moved; an interval-skipped turn re-shows stale
    state, see CLAUDE.md "CDD_TURN analyzed"). Keys are the govern.json config
    names (`kCddSignalKeys`), not the telemetry labels, which diverge for S1-S7.
    `signal:*` matches any;
  - `validation_failed` -- `agent.record_validation(h, false, detail)`; the
    detail's keywords drive keyword selection;
  - `violation:<rule>` -- any `enforce()`; a trailing `*` matches a prefix
    (`violation:code_quality.*`).
- **Selection.** `source#slug` delivers that section, subsections included. A
  bare `source` ranks sections by overlap with the trigger's keywords through the
  shared extractor (`keyword_extract.h`): heading matches weigh 3, text matches 1,
  ties go to document order, at least one overlap is required. Deterministic and
  logged (`score`). No model writes, rewrites or summarises guidance: what is
  delivered is exactly what the source says.
  **Changed while building:** (1) the planned `select` key is gone -- the anchor
  already says which mode applies, and a second key could only disagree with it.
  (2) Keyword selection scores and delivers a section's OWN text (up to its first
  subheading), not its full body. Scoring the full body let the document's title
  section match every topic through its children and win every selection: the
  first run of G-04 delivered the whole playbook for a database failure and
  G-12a returned "Agent playbook" for "migration rollback schema".
- **Channels.**
  - `agent` -- prepended to the next request only, never stored in history,
    after the prompt scan and after the S13/S20 keyword extraction (so guidance
    cannot be scored as the user's instruction). A trigger observed after reply N
    is delivered with request N+1; the text is resolved at delivery, so an edit
    between the two is what arrives.
  - `terminal` -- a `Project guidance (<file> > <heading>):` section appended to
    the block message in `enforce()`, after the refusal attestation and the hook
    have used the original text.
  - script -- `agent.send()`'s response carries `guidance` (what this send
    delivered: trigger, source, section, chars, sha256) when the feature is on,
    and `governance.guidance(topic)` lets a script pull entries (and wrap them as
    an agent tool).
- **Budgets.** `max_chars_per_turn` (agent), `max_chars_terminal`, and
  `cooldown_turns`: one handle is not re-sent the same section within N turns.

## The channel is itself governed

Each invariant is pinned by a refusal paired with a success through the same
fixture (`tests/governance_v4/test_guidance.sh`).

1. **A source is read only if the filesystem policy lets the project read it**
   (`pathPolicyDenial`, decide-only). A guidance file cannot be the route by which
   a blocked file reaches a model provider; govern.json, its signature, the
   trusted keys and the signing key are in the auto-protected list and so can
   never be sources.
2. **The governed program cannot write a guidance source.** No self-instruction
   loop: an agent's output may influence *which* operator-written entry is
   selected (validation detail keywords), never *what it says*. *Decided
   2026-10-12: "don't want a self-instruction loop, would compromise
   governance."* Enforced in `checkPathAccessImpl()` for writes, so it covers
   `file`/`io`, embedded Python's `open()` (the audit hook), both engines.
   **Not** subprocess languages: with shell enabled a `<<sh>>` block is outside
   path policy (CONTRA-013). *Decided 2026-10-12: "if shell is on then the
   user/dev would have allowed full access just like they opened a terminal
   window."* Documented, not refused.
3. **Secrets and PII.** Guidance text goes through the secret and PII patterns
   before any channel; a hit withholds that entry (`GUIDANCE_WITHHELD`), it does
   not block the run. Like the prompt scan, this is live only when
   `code_quality.no_secrets`/`no_pii` are enabled.
   **Changed while building:** this premise first said "through
   `checkSecrets`/`checkPii`". Those two ENFORCE on a hit (a HARD `no_secrets`
   ends the run) and record a pass on a miss, under `results_mutex_` -- which the
   terminal channel already holds inside `enforce()`'s advisory branch (traced,
   fixed before it ran). Guidance uses decide-only twins, `secretFinding()` and
   `piiFinding()`, sharing the compiled patterns and match logic so the two
   decisions cannot drift.
4. **Provenance.** Every delivery emits a chained `GUIDANCE_DELIVERED` event
   (channel, trigger, source, section, chars, sha256 of the delivered text,
   selection method and score); the transcript holds the full text.
5. **Off by default.** With `guidance` absent or disabled, every path is
   byte-identical to before.
6. **Guidance never changes an outcome.** It is appended on the HARD/SOFT throw
   paths, so an exception escaping it (the PII matcher rethrows `bad_alloc` as
   `runtime_error`) would have replaced a `GovernanceHardError` with a catchable
   type. Every call site catches guidance failures and keeps the block (traced
   during review, before shipping). Each guidance call also holds a
   `shared_ptr` to the rules for its duration, so a mid-run reload cannot free
   the config it is reading.

## Ratchet

Guidance grants and withholds no capability, so its keys are classified
`neutral` in `ratchet_classification.txt`. Removing a source mid-run also removes
its write protection -- harmless, since it is then no longer a source.

## Measuring whether it helped (deferred, not in the runtime)

*Owner's point, 2026-10-12: the outcome can only be known after the agent has
used the guidance, which is hard at runtime.* Agreed, and so nothing at runtime
decides on it. The runtime records each delivery; a report reads the telemetry
afterwards and, per delivery, checks whether the triggering rule or signal
recurred in the next N turns and how coherence moved, marking a window
undetermined when another intervention (correction, challenge) landed in it. That
is correlation. Causation needs arms -- a stub (as #164) or an optional holdout
that withholds a random fraction of deliveries on live runs. Slice 2.

## Slices

1. **Core** (built): `file` sources, the four triggers, the three channels,
   path gating, write protection (hard links included), scanning, telemetry,
   transcript, pull API, template, ratchet classification, `extends`
   inheritance. Test: `tests/governance_v4/test_guidance.sh`, 25 arms; each
   mechanism was removed in turn and its arm went red (write gate -> G-09a/b/c/e,
   secret scan -> G-06 with G-06b still green, policy read -> G-07/G-08, extends
   -> G-16, agent injection and terminal suffix -> the delivery arms and
   G-10/G-11, cooldown -> G-03 `nnYYYYY`, live re-read -> G-05, signal queue ->
   G-03).
2. **Measured**: the after-run efficacy report; a `history` source of facts from
   previous runs' telemetry (engine-written fields only; the telemetry file needs
   write protection first -- it is not auto-protected today, traced in
   `addGovernanceProtectedPaths()`, and the hash chain is unkeyed, so it detects
   edits but not a correctly chained forged append).
3. **Connectors**: read-only parameter-bound `sqlite`; a `command` adapter (hook
   machinery plus stdout capture and containment); an `mcp` stdio client;
   `https`. External sources must pass `capabilities.network` (allowed/blocked
   hosts, https_only) -- *decided 2026-10-12* -- stricter than the telemetry
   webhook, which does not consult it (screened: `telemetry_forwarder.cpp` has
   no host-policy reference). Only declared fields leave the machine, scanned
   first. Slow sources prefetch between turns with a hard deadline at send time;
   the terminal channel uses only cached content.
4. **Outward**: a `naab_guidance` tool on `naab-lang mcp`.

## Known limits, slice 1

- `agent.propose()` builds its own request and has no injection site (traced:
  `MANDATE_INJECTION` appears only in `agentSend`), so proposals receive no agent
  guidance.
- `violation:` triggers deliver to the terminal only; an agent-path violation
  (response scan, tool scan) is attributed to an agent NAME, not a handle, and
  routing it to a handle's next send is slice 2.
- Guidance that pulls an agent toward words outside its mandate can make S11
  (`mandate_alignment`) fire. Pinned by a test arm rather than designed away.
- A stub proves wiring, not that a real model follows guidance.
