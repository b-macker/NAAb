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
3. **Connectors**: read-only parameter-bound `sqlite` (**built**, second PR); a `command` adapter (hook
   machinery plus stdout capture and containment); an `mcp` stdio client;
   `https`. External sources must pass `capabilities.network` (allowed/blocked
   hosts, https_only) -- *decided 2026-10-12* -- stricter than the telemetry
   webhook, which does not consult it (screened: `telemetry_forwarder.cpp` has
   no host-policy reference). Only declared fields leave the machine, scanned
   first. Slow sources prefetch between turns with a hard deadline at send time;
   the terminal channel uses only cached content.
4. **Outward**: a `naab_guidance` tool on `naab-lang mcp`.

## Alignment review after slice 1 (2026-10-12, #309 merged as `76c9617e`)

The owner's goals, collected from their own messages (not paraphrased from
memory), checked against what #309 shipped. Tiers: **observed** (a test or run
showed it), **traced** (read in the code), **unmeasured**.

| # | goal (owner's words, condensed) | status after #309 | evidence |
|---|---|---|---|
| 1 | govern.json is king | met | sources and rules live only in govern.json (traced; G-17 load warnings observed) |
| 2 | live sources: MCP, database, document, past context | **documents only** | slice 3 / slice 2 not built |
| 3 | easy to connect a source | partial | one block in govern.json for a file; no other type |
| 4 | dynamic, adaptable, organic | partial | live re-read observed (G-05); nothing adapts |
| 5 | internal and external trusted sources | **internal files only** | external sources need the network gate (decided, not built) |
| 6 | relevant, accurate, logical, **quick** | relevance observed (G-04, G-12a); verbatim text traced; **speed unmeasured** | no timing was ever taken |
| 7 | fluid mid-run changes | met | G-05 observed |
| 8 | no self-instruction loop | met inside path policy | G-09a-e observed; `<<sh>>` with shell on is the operator's grant |
| 9 | telemetry shows what was given, the full chain, so we know if it was used | **not met** | `GUIDANCE_DELIVERED` carries metadata and a sha256, not the text (traced); nothing links a delivery to the reply that followed it (traced) |
| 10 | agents can see the info | partial | the model sees it in its request (observed, G-02); the script's `guidance` field has no text (traced) |
| 11 | no extra steps, fires at runtime like errors | met | triggers fire with no script code (observed G-02/G-03/G-04) |
| 12 | complexity in the backend, automated | partial | selection and live reindex are automatic; no prefetch, cache or adaptation |
| 13 | the terminal shows the info at the end, like an error message | **met for blocks only** | G-10/G-11 observed; a delivery to an agent prints nothing (traced) |
| 14 | databases are the real enterprise test | **not started** | |
| 15 | govern-template.json kept current | met for slice 1 | `test_template_coverage.sh` observed |

What the review changes, in order (the next PR):

1. **Full-chain telemetry (goal 9).** `GUIDANCE_DELIVERED` carries the delivered
   text (already scanned), a `delivery_id`, and `lookup_ms` (so speed, goal 6,
   becomes observed rather than claimed). A new `GUIDANCE_RESPONSE` links the
   reply to the request that carried the guidance: which guidance terms the
   reply used (the same keyword-recall computation S13 already applies to
   user instructions, through the same extractor), the reply's content hash
   (matching `AGENT_RESPONSE`), and for a signal trigger whether that signal
   paid again on the reply's turn. `GUIDANCE_VALIDATION` records the first
   validation after a delivery. These are observations, not decisions (the
   owner's point that "did it work" cannot drive runtime stands): nothing
   reads them to change behaviour. Term recall is lexical echo, not proof of
   compliance -- the same caveat S13 carries.
2. **Terminal at the end (goal 13).** When guidance reached an agent, an
   end-of-run block lists each delivery: agent, turn, trigger, section, and
   how much of it the reply used. Automatic, no flag. Blocks keep their inline
   guidance.
3. **The script sees the text (goal 10).** The response's `guidance` entries
   carry `text` and `id`.
4. **Databases (goals 2, 5, 14).** A read-only SQLite source with a
   parameter-bound query is the first non-document source; it is in-process
   and a dependency NAAb already has. Enterprise servers (Postgres, MySQL,
   SQL Server) come through the command adapter and MCP client of slice 3,
   which also need the network gate.
5. **The test gaps listed after #309**: mid-run reload, `agent.batch`
   concurrency, the commit path, the `agents` filter, `signal:*` and prefix
   rules, PII withholding, and an agent arm on the tree-walker.

## Built after the review (second PR)

Items 1-5 of the review are built. Observed on this machine (local unoptimised
build), never on a real model:

- **Full chain.** `GUIDANCE_DELIVERED` now carries `text`, `delivery_id` and
  `lookup_ms`; `GUIDANCE_RESPONSE` joins each delivery to the reply
  (`turn_at_request`, `terms_used`/`recall`/`matched`, `trigger_recurred`),
  through both the behavioural-sequence path and the path where it is off;
  `GUIDANCE_VALIDATION` closes the chain at the next `record_validation`.
  G-18/G-18b/G-20 pin it; G-18's control is a reply that ignored the guidance
  reading `terms_used` 0.
- **Terminal at the end.** `-- Guidance: N delivered to agents (lookup X ms avg)
  --`, one line per delivery with the reply's uptake and the signal outcome,
  printed from `flushGroupedAdvisories()` (every exit path), silent when
  nothing was sent (G-19, control: guidance off).
- **Script sees the text** (G-21).
- **SQLite source** (`tests/governance_v4/test_guidance_sqlite.sh`, 16 arms).
  Observed lookups: about 0.2-0.4 ms per send for a markdown source and about
  0.9 ms for a sqlite query, including the secret/PII scan; a runaway query
  is interrupted at its 50 ms deadline and the program finished in under
  100 ms (two runs).
- **Gap tests**: agents filter (G-22), `signal:*` (G-23) and a prefix rule
  (G-10b), PII (G-24), a mid-run reload (G-25), two concurrent
  `agent.batch` calls of six handles (G-26), the commit path (G-27), the
  tree-walker (G-28).

**Changed while building:**
1. FTS5 failed every `MATCH` with "vtable constructor failed": its
   constructor queries `PRAGMA data_version`, which the authorizer's blanket
   PRAGMA refusal denied (reproduced with Python's sqlite3 and the same deny
   list). Only that pragma, in its read form, is now allowed.
2. Table-valued pragma functions (`pragma_table_info(...)`) reach the
   authorizer as reads, not as `SQLITE_PRAGMA`, so "PRAGMA is refused" is true
   of PRAGMA *statements* only. SQLite defines those functions only for
   side-effect-free pragmas over the same database; noted in the code, not
   designed around.
3. A reload is applied at the next `agent.send()`. G-25's first draft recorded
   a validation between the file change and that send, and the trigger was
   judged under the OLD rules -- correct engine behaviour, wrong test.
4. Under `signal:*` a delivery is attributed to the first paid signal queued
   (observed `signal:circular_actions`), not necessarily the one a reader
   expects.
5. `"*"` and a trailing-`*` prefix are the same rule: removing the `"*"`
   special case alone changed nothing, so the revert check removed both.

Revert check (observed): reply linkage -> G-18/G-18b; response text -> G-21;
agents filter -> G-22; end-of-run block -> G-19; validation linkage -> G-20;
database sidecar protection -> S-04; database policy read -> S-06; PII scan ->
G-24; ATTACH refusal -> S-05; query deadline -> S-07; wildcard and prefix
matching -> G-10b, G-23.

## Sources only (same PR, after the owner's review)

The owner's requirement (2026-10-12): "Don't want the settings in govern.json to
be too complicated or complex, should just be what source to add." The rules,
anchors, queries and switches above are what an engine needs, not what an
operator should have to write. The minimal configuration is now:

```json
"guidance": { "sources": ["docs/agent-playbook.md", "data/guidance.db"] }
```

- **Type from the extension**: `.db` `.sqlite` `.sqlite3` `.db3` are databases,
  anything else a markdown document. A source may still be an object
  (`path`, `name`, `type`, `query`, `max_rows`, `timeout_ms`), and the
  name-keyed map #309 shipped is still read.
- **Listing sources switches the feature on**; `"enabled": false` switches it
  off with the sources still listed (the template ships that way).
- **No `rules` key: the engine routes by itself.** Four automatic rules cover
  every source: `start`, `signal:*` and `validation_failed` to the agent,
  `violation:*` to the terminal. Each takes, per source, the first section or
  row NAMED for the trigger -- a heading or `topic` value compared loosely
  (case and `_ . - : /` ignored) with the full rule name or its last part, the
  signal's config key, or the starting agent's name -- up to 3 entries.
  Failing a name, only a strong keyword match is delivered (score 3 or more: a
  heading term, or three text terms); an agent's `start` takes names only.
- **Why names, not just keywords.** The keyword extractor drops words of three
  characters or fewer, so `code_quality.no_pii` has no usable keyword at all;
  only its name finds a section headed "No PII" (M-10). The threshold is a
  design choice, not a measurement: one shared word is a weak reason to
  interrupt a block or an agent, so the noise controls require that nothing be
  delivered when nothing is named and nothing matches strongly.
- **Explicit `rules` replace the automatic routes**, so the template documents
  the rule shape in `_comment_rules` and shows no example entry: an example
  rule there would have switched automatic routing off in every config copied
  from it (caught while reviewing the template; the coverage baseline lists
  the rule leaves under ENTRY SHAPE, beside plugin entries, for the same
  reason).
- **A database needs no SQL.** With no `query`, its `guidance` table is read
  like a document: the name column is the first of `topic`/`heading`/`title`/
  `name`, the text column the first of `body`/`text`/`content`/`guidance`; up
  to 5000 rows, re-read when the database or its `-wal` changes. The same
  read-only open, authorizer and deadline apply as for a query.

Tests: `tests/governance_v4/test_guidance_minimal.sh` (11 arms, every config
only a sources list). The controls are the point of the suite: with no section
named for the event and no strong match, nothing is delivered (M-02, M-03c,
M-04c, M-05c). Revert check (observed): with name matching removed, M-01, M-04
and M-10 fail -- the first draft's M-01, M-03 and M-06 had passed through
keyword overlap instead, which is why M-01 now asserts `"select":"name"` and
M-10 was added; with the database change signature ignored, M-06 (a row added
mid-run reaching the next request and not the one before) fails.

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
