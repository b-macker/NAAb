# Plan — engine observability and config validation

**Status:** **E1–E6 all landed.** Plan complete; only E7 remains, deferred as a stdlib signature change.

This file exists on disk because the previous version of this plan lived only in
conversation and was lost to a context summary — its E3 could not be recovered
from git, docs, or scratch. Anything worth executing later is written here.

## Thesis

Everything built externally compensates for the engine not explaining itself, and
it splits four ways:

1. **Conclusions without inputs** — pressure without its terms; a level change
   without the counter that caused it.
2. **Silence without cause** — a signal that did not fire and one that *could
   not* fire are the same observation.
3. **Configs stating an intent the engine ignores** — six rotation keys with no
   retry; windowing with a factor that defeats it.
4. **Runs without identity** — telemetry cannot say which config produced it.

None of this changes what the engine *enforces*. That is deliberate: the
campaign's evidence names threshold suspects but no values, so anything moving a
threshold is unsupported. Observability and config validation are supported by
everything found.

**Test of success:** `report.py` should shrink. Its Sections 3–4 are
reconstructions; if this plan lands they become reads. A forensic tool
re-deriving what the engine already computed is a standing bug report about the
engine's observability — and this one re-derived it wrong twice before it was
right.

---

## Landed

| # | What | Commit | Verified |
|---|---|---|---|
| E1 | Budget errors name the scope that bound, both call sites | `38366e5` | `test_limit_attribution.sh` |
| E2 | `pressure_detail`, calm counter, join key, signal-key suggestion | `596bd82`, `1bbbd79` | residual 0.0 over 36 rows; `test_signal_key_suggestion.sh` |
| E5 | Health check gains the false-positive direction | `2bca69d` | `test_health_floor_symmetry.sh` |
| E4 | CONTRA-011 / CONTRA-012, with one premise corrected | `d9b1626` | `test_config_contradictions.sh` |
| E3 | `RunStart` carries config fingerprint + mandate digest | `cf02e28` | `test_run_identity.sh` |
| E6 | `signals_off` / `signals_starved` + one-shot `SIGNAL_INERT` | `2d46a4c` | `test_signal_evaluability.sh` |
| — | Join key fixed after it shipped broken | `fd89579` | `test_telemetry_join_key.sh` |
| — | Findings + open-investigations recorded | `096fecd` | docs only |

E2 shipped a defect worth remembering: `turn` on `AGENT_RESPONSE` was emitted as
`current_turn + 1`, verified against a tool-FREE scenario, and was wrong by the
number of tool round-trips. **A verification narrower than the claim it supports
is not evidence for the claim.** Every item below therefore names the case its
test must cover, not just the assertion.

---

## E5 — Make the health check symmetric — **LANDED** (`2bca69d`)

`checkGovernanceHealth()` (`governance_engine.cpp:6599`) has three checks, all
asking *is detection being bypassed?*

1. BSD received 0 events → instrumentation failure
2. CDD analyzed 0 turns → instrumentation failure
3. Coherence perfect (1.0) after 10+ turns → suspicious

**Verified: none asks whether governance is producing false positives.** Every
keyed run floored coherence by turn 5–8 with no agent misbehaving and the engine
said nothing. The asymmetry is arguably a design bug — built to catch an agent
evading detection, blind to governance evading correctness.

Add the mirror: coherence floored or near-floored within the first N turns →
probable false positive, naming the top contributing signals. Consider folding in
the aggregate (33 of 36 responses inadmissible in run 6, never surfaced as one
statement).

**Regression surface (traced):**
- Returns a string; the caller (`agent_impl.cpp:4528`) prints to stderr and emits
  `GOVERNANCE_HEALTH_WARNING`. It **never calls `enforce()`** — no escalation, no
  blocking, no advisory-escalation interaction. Safest item in the plan.
- **Runs per turn**, unlike contradictions. A new warning without one-shot
  latching prints every turn for the rest of the run. Use the
  `thinking_inert_announce` pattern (a DriftState flag + a single emit).
- Suites that *deliberately* floor coherence would gain a new stderr line. Any
  asserting exact stderr break. Enumerate them before writing.
- A second emitter exists end-of-run (`governance_reports.cpp:1561`). If touched,
  it must use `chainPrevLocked()` and increment `chained_events_this_run_` — that
  exact writer previously desynced the chain.

**Vacuity:** floored-coherence-without-misbehaviour run → warning fires; healthy
run → silence (without this control a warning that always fires passes); disable
the check → the first assertion fails.

---

## E4 — Config-contradiction checks — **LANDED** (`d9b1626`)

Two footguns that cost four keyed runs, both the exact shape
`detectContradictions()` already handles — configured, believed working,
structurally unable to fire:

- `api_key_env` lists 6 keys but `retry.max_attempts == 1`. Killed runs 4 and 5
  with all six keys exported. **The premise as originally written — "rotation is
  inert, because rotation happens on retry" — is FALSE**, and tracing caught it
  before the warning shipped: `key_offset` advances on every send and persists on
  the tracker, so keys do rotate between calls, and a key returning a
  `skip_key_on` code is still retired on the first attempt. What `max_attempts: 1`
  removes is failover *within* a call — the first key error aborts the call that
  hit it. The shipped wording says that.
- `context_growth` + no `context_window` + `adaptive_baseline_enabled: false` +
  high `max_turns` → fires permanently from ~turn 8 and never recovers. Three
  readers took its firing for drift. **Premise verified exactly**: the S12
  input-token baseline is set once and only EMA-updated when adaptive baselining
  is on, so it freezes while the resent history keeps input tokens climbing.

**Regression surface (traced — this was the gate on the plan, and it passes):**
- `detectContradictions()` has ONE caller: `governance_config.cpp:5192`, inside
  `loadFromFile`. It runs **once per process, at initial load**.
- `reloadIfChanged()` calls `loadFromJson()`, **not** `loadFromFile` — mid-run
  reloads do **not** re-run contradiction detection. The plan's central fear
  ("one firing per turn is not fine") does not apply.
- Results do go through `enforce(rule_name, c.level, …)`, so advisory escalation
  is reachable in principle — but `AdvisoryEscalationConfig.enabled` defaults
  **false** and `soft_after` is **3**, counted per rule name. A once-per-process
  advisory cannot reach it.
- `contradiction_detection.enabled` defaults **true**, `max_level` defaults
  **ADVISORY**, and **no config in the repo raises it** (checked all 139
  `govern*.json`). CONTRA-001's hardcoded SOFT is real but does not affect new
  patterns.
- Actual risk is therefore **noise, not blockage**: a misfiring pattern warns on
  every run of 139 configs. Suites asserting exact stderr are the breakage path.
- **Blind spot the plan did not note:** because reload never re-checks, a config
  that becomes self-contradictory *via mid-run reload* is never validated.
  Worth recording; out of scope for this increment.

**Vacuity:** each pattern needs a **negative fixture** — a config that does *not*
contradict — asserting silence. Without it, a pattern that always fires passes
its own test. Plus a positive fixture per pattern, and removal of the pattern
must fail it.

---

## E3 — Runs carry their own identity — **LANDED** (`cf02e28`)

`RunStart` (`governance_reports.cpp:272`) should carry a config fingerprint and
per-agent mandate digest. Evidence: the prose-arm round trip — `report.py` reads
`src/govern.json`, which need not be the config that ran, and telemetry could not
settle it.

**Regression surface (traced — the plan called this trivial; it is not):**
- `GovernanceRules` uses **50 unordered containers** and **no canonical
  serializer exists**. Hashing the resolved rules naively yields a
  **non-deterministic** fingerprint, so E3's own vacuity test ("same arm twice →
  same fingerprint") would flake.
- Therefore scope to **Tier A**: SHA-256 of the loaded config file bytes (plus
  each `extends` parent, in load order), and per-agent `system_prompt` digests
  emitted sorted by agent name. Deterministic by construction, and it answers the
  question that actually arose — *which config ran*.
- Tier B (canonical serialization of resolved rules, capturing CLI overrides) is
  a separate, larger piece of work. Do not start it inside E3.
- `RunStart` is a chained anchor: new fields sit inside the hashed payload, so
  `--verify-telemetry-chain` and the `RunEnd` count must be re-verified (E2's
  additions passed 28/28; same discipline).

**Vacuity:** both directions are required — two arms → fingerprints differ
(without which a constant passes); one arm twice → fingerprints match (without
which a random value passes).

---

## E6 — Per-signal evaluability — **LANDED** (`2d46a4c`)

Generalise `THINKING_UNREPORTED`: S9 already distinguishes "did no thinking" from
"provider did not report thinking", with a one-shot event when the signal goes
silently inert. The campaign hit the same problem for S12 (baseline unset), S17
(inert unless adaptive baselining is on), S23 (default off) and S20 (depends on
`mandate_keywords`). A `signals_evaluable` field or `SIGNAL_INERT` event turns
"the signal is silent" into "silent because X" — the campaign's thesis as a
feature, and what would have caught CONTRA-007 and the circuit-breaker ladder
gate without an accident.

**Regression surface (traced):** not one choke point. Gating is uniform in shape
— `sig_on(config_->signals.X, SIG_X) && <precondition>` — but the *precondition*
differs per signal and is already written as the second conjunct at each of ~23
sites. So this is 23 mechanical determinations plus DriftState flags and one
emit site. Real design work; correctly ordered last, after E2 made signal
internals visible.

---

## E7 — Phase labels on turns (deferred)

Phases are annotated with `print("PHASE|X|start")` and gates grep stdout. An
optional label on `agent.send()` would put phase in telemetry and let every gate
read one source. **Deferred: changes a stdlib signature.**

---

## What execution changed about the plan

Each item is annotated above with what it claimed; this records where the claim
did not survive contact.

- **E4's central risk did not exist.** The plan called it "the gate on the whole
  plan" because contradictions reach `enforce()` and advisory escalation hardens
  repeats into SOFT blocks. `detectContradictions()` has one caller inside
  `loadFromFile`, so it runs once per process, and `reloadIfChanged()` calls
  `loadFromJson` instead — the advisory cannot repeat, so it cannot escalate.
- **E4's rotation premise was wrong.** "Six keys with `max_attempts: 1` makes
  rotation inert, because rotation happens on retry" is false: `key_offset`
  advances every send and persists on the tracker. What is lost is failover
  *within* a call. Shipping the original wording would have put a false statement
  into an operator-facing warning.
- **E3 was not trivial.** ~50 unordered containers and no canonical serializer
  meant the natural implementation was non-deterministic, and its own vacuity
  test would have flaked rather than failed.
- **E5's premise held exactly**, and it was the safest item — no `enforce()` path
  at all. The correction was operational: it runs per turn, so the warning needed
  a one-shot latch.
- **E2 shipped a defect that this plan's own rules caught later.** The join key
  was verified on a tool-free scenario and was wrong by the tool round-trip
  count. Hence the third standing rule below.

- **E6 broke two suites, and the breakage was informative.** Both detected
  "signal X fired" by grepping the whole `CDD_TURN` line for a bare signal name.
  `signals_starved` contains signal names, so a starved signal read as a fired one
  — the opposite conclusion. Their line-wide grep was always imprecise; the new
  field made it bite. Detection is now scoped to `signals_detail`. The regression
  surface traced for E6 covered enforcement behaviour and missed the question
  *who greps telemetry for signal names* — a reminder that "additive" is a claim
  about consumers, not just about code.
- **E6's three-state model was needed on first contact.** `scope_creep` and
  `vocabulary_contraction` gate on `turn_types`, derived further down the same
  function, so they are reported **unknown** rather than guessed at.

Fire rates were measured against the in-tree corpus *before* writing E4, not
after: CONTRA-012 on 36/190 agents, CONTRA-011 on 17/190. A contradiction that
fires on the default config is noise rather than a finding.

## Order and rationale

**E5 → E4 → E3 → E6.**

- **E5 first**: the only item that is arguably an engine *design* bug, and the
  safest — no `enforce()` path at all.
- **E4 second**: its regression trace was the gate on the whole plan and it
  passed; risk is bounded to stderr noise.
- **E3 third**: needs the Tier A/Tier B scoping decision above before writing.
- **E6 last**: genuine design work across 23 signals.

## Standing rules for every item

1. Trace callers, guards and threading before writing — not additivity by
   assumption. E4's fear evaporated and E3's triviality did too, both only under
   tracing.
2. Every new gate must **fail when removed**, and every absence assertion needs a
   positive control.
3. The test must cover the case the claim covers. E2's join key passed 39/39 on a
   scenario that could not exercise the path it was wrong on.
