# A6: screening pass over DriftState and AgentTracker state fields

Register row A6 observes that the A2 sweep **cannot find the class of defect A4
and A5 belong to**. A2 screened for *config keys parsed but enforced by nothing*.
A4 (`granted_capabilities` never written) and A5 (`prev_turn_blocked_caps` never
recordable) are **state fields**, invisible to a key-oriented scan, and both were
found by accident while tracing B6.

This is the pass A6 asks for: for every member of `DriftState` and
`AgentTracker`, does anything **write** it, and can the writer be **reached**?

Tool: `tools/screen_state_fields.py` (exit 0 = the instrument's self-test passed).

## Result

**134 members screened, 2 flagged.**

| struct | members | written-and-read | flagged |
|---|---:|---:|---:|
| `DriftState` (`behavioral_sequence.h:158-484`) | 112 | 111 | 1 |
| `AgentTracker` (`agent_impl.cpp:77-102`) | 22 | 21 | 1 |

| # | field | shape | consequence | status |
|---|---|---|---|---|
| A6a | `DriftState::pipeline_depth` | never written | **misreported to scripts and to signed evidence** | traced + empirically checked, NOT acted on |
| A6b | `AgentTracker::lease_granted_turn` | written 3×, read 0× | dead store only; not exposed anywhere | traced, NOT acted on |

Base-rate note, since A6 predicts a poor one: A4 and A5 were the first two
members examined, and 132 of the remaining 134 came back clean. The one
substantive finding is a **namesake** of a live mechanism, not an absent feature
— which is A6's own stated caution, hit on the first result.

## A6a — `DriftState::pipeline_depth` is a dead mirror of a live mechanism

There are two nearly identically named things:

| | `GovernanceEngine::pipeline_depths_` | `DriftState::pipeline_depth` |
|---|---|---|
| declared | `governance.h:3729` (a map) | `behavioral_sequence.h:201` (an int) |
| written | `governance_engine.cpp:6598`, from `setPipelineDepth()`, called at `agent_impl.cpp:6553/6563` | **nothing, anywhere** |
| read | `governance_engine.cpp:6703-6705` | `behavioral_sequence.cpp:2516` (`snapshotState`), `agent_impl.cpp:800` (`agent.environment()`) |
| effect | `checkAdmission` denies when `depth > max_pipeline_depth` (`:6707`) | reporting only |

**Pipeline depth limiting works.** What is dead is the copy that gets *reported*.
An unwritten field does not imply an absent feature — A6 says so, and this is the
case it was warning about. Reporting this as "pipeline depth tracking is broken"
would have been a confident wrong answer from a correct grep.

The consequence is confined to evidence, and is the same second-order shape A4
had (`capabilities_granted` empty for every agent that ever ran):

- `agent.environment(handle).state.pipeline_depth` is **always 0**, including for
  an agent inside a pipeline.
- `snapshotState` writes `pipeline_depth: 0` into `cdd_snapshot`, which rides
  inside the hashed telemetry payload — so the *tamper-evident* record says depth
  0 for a run the engine admitted against a non-zero depth.

### Empirical check, with positive control

Stub-backed, keyless, isolated trust store. Two agents, one real
`agent.pipeline()`.

| observation | value | role |
|---|---|---|
| `PIPELINE_OK stages=` | 10 | the pipeline actually ran |
| `state.pipeline_depth` | **0** | the finding |
| `state.capabilities_granted` | `['environment','filesystem','network','execution']` | **positive control** |
| `state.mandate_alignment` | 0.75 | second control — a computed value, so the DriftState is exercised, not fresh |

The control is chosen deliberately: `capabilities_granted` is **A4's field**,
repaired in #196, read from the same `drift_opt` on the line *adjacent* to the
one under test (`agent_impl.cpp:805` vs `:800`). It returning four entries proves
the readout path is live and the DriftState is populated in this very run, so the
0 is attributable to the missing write and not to a dead reporting path.

`coherence` was rejected as a control after being measured: it reports 1.0, which
is also its default, so a fresh DriftState and a fully exercised one are
indistinguishable through it. ("A sweep over explicit values cannot see a
default.")

### Blind spot, stated rather than omitted

The run does **not** demonstrate that the live depth was non-zero. The only
observable consequence of `pipeline_depths_` is admission denial, which needs
`depth > max_pipeline_depth` with `max > 0`; a single-level pipeline sits at depth
1, and `0` means unlimited, so **no positive limit can ever be exceeded by a
non-nested pipeline**. Confirmed: `max_pipeline_depth: 1` did not block the same
pipeline. Depth 2 requires a pipeline nested inside a tool callback, which this
harness does not build. The live side is therefore established by source trace,
not by measurement, and that distinction is the reason this row is not "done".

### Not fixed

Populating the field is not additive: `snapshotState` output is chain-protected
evidence, and per-agent snapshots would begin reporting a value they have never
reported. Same deliberate call as A4 and the A2 keys.

## A6b — `AgentTracker::lease_granted_turn` is a dead store

Written at `agent_impl.cpp:1226` (init 0), `:2418` (set to `current_turn` on
renewal) and `:6866` (reset on `agent.reset()`). Read by nothing.

`leaseExpiredLocked()` (`agent_impl.cpp:346`) — the only lease decision — reads
`lease_expires_turn` for the turn-based half and `lease_granted_time` for the
wall-clock half. Neither is this field.

Materially different from A6a and the table should not blur them: nothing is
misreported, because `lease_granted_turn` is not exposed in
`agent.environment()`, in `snapshotState`, or in telemetry. It is dead weight,
not false evidence. Deleting it is a tidy-up; the one thing that would make it
load-bearing — reporting lease age to an operator — does not exist yet.

## What the instrument got wrong, and why that is in this document

The screening tool produced **four successive rounds of false findings** before
the numbers stabilised. Recorded because the failure modes generalise, and
because three of the four would have shipped as register rows:

| round | flagged | cause | direction |
|---:|---:|---|---|
| v1 | 9 | write regex could not see `member[i] = …` or a `.sub` chain | invents findings |
| v2 | 17 | statement-joining, but the chain regex greedily ate `.push_back(` | invents findings |
| v2a | 5 | `\w*` backtracked `.insert` → `.inser`, defeating the lookahead | invents findings |
| v2b | 9 | "skip the declaration" was file-scoped, so a struct declared in the file that uses it lost **all** its evidence | invents findings |
| v2c | 2 | hand-maintained accessor list missed `drift_states_[handle_id]`, `tracker_it->second`, `pp` | invents findings |

Every error fell the same way — toward reporting a live field as dead. The
self-test is what caught them: a list of members known to be written, checked
before any table is read. Without it, v2b's "five members declared and never
touched" reads exactly like a discovery.

Two of the method doc's four EFFECT checks were hit directly: **indirection**
(`auto& sightings = state.entity_context[entity]`, written through the alias two
lines later) and **namesakes** (A6a itself). The accessor-list problem is the
third, **alias**, which is the same failure A2's sub-sweep (2) hit when 11 of 11
flagged `enabled` flags turned out to be alias reads.

The tool now takes any receiver expression rather than a curated list. That
trades alias blindness for namesake risk, which is why **every flagged row is
hand-traced** and why the tool's output is a table of outcomes rather than a
count.

### The self-test guarded one direction; it now guards both

Independent review caught this and it is the sharper half of the story. The
original `SELF_TEST_WRITTEN` is a positive control in the false-**dead**
direction only — the direction that produced four rounds of invented findings.
The complement was unguarded, and it is the direction that **hides** defects.

Demonstrated rather than argued, twice, by two people independently. Adding

```cpp
struct TotallyUnrelated { int pipeline_depth = 0; };
static void bogus() { TotallyUnrelated u; u.pipeline_depth = 7; }
```

to any file under `src/` moved `DriftState::pipeline_depth` from NEVER WRITTEN
to `ok`, took the flagged count from **1 to 0**, and left the self-test reporting
**zero failures**. The permissive accessor matches `anything.field`, so a write
to a same-named member on any unrelated struct counts as a write to this one.
That is the price of dropping the curated accessor list — which was itself the
right call, since the curated version missed `drift_states_[handle_id]`,
`tracker_it->second` and `pp`, each producing a false finding.

`SELF_TEST_UNWRITTEN` now holds the namesake half. Re-running the injection with
it in place fails loudly and names the offending file and line, exit 1.

The set contains the A6 findings themselves. That is intentional and not
circular: masking either one *is* the failure above, so the control fires on it.
When a finding is fixed — `pipeline_depth` populated — this test **should** fail;
that is the prompt to update the control and the register row in the same change
that alters the behaviour.

**Whether the blind spot actually bit was checked separately, and it did not.**
A re-scan of all 119 parsed `DriftState` members by write-*receiver* left 11 with
receivers not immediately attributable; 10 are the `it->second` map-iterator
idiom, which is a genuine DriftState handle. The eleventh, `config_name`, exists
on both `DriftState` and `AgentTracker` — and `DriftState::config_name` is
genuinely written at `behavioral_sequence.cpp:2836` via `it->second`. So no
finding was hiding behind a namesake at the time of the screen. The control
exists so that stays true rather than being re-established by hand.

### Round five: the instrument's own reporting broke, on one platform only

Registering the tool put it on the Windows runner for the first time, and it
went red there while every Linux job stayed green. The cause was one character.

The outcome column read `NEVER WRITTEN — reads: ...`. Python encodes stdout with
the **locale's** encoding, so the same em dash failed the two platforms in
opposite directions: MSYS2's cp1252 wrote it as the single byte `0x97` and the
baseline parser died reading it back as UTF-8, while a bare C locale raises
`UnicodeEncodeError` inside the tool. Neither is visible from a normal
UTF-8 Linux shell.

Three distinct defects sat in that one line, and each got its own control:

| defect | fix | control |
|---|---|---|
| output encoding depended on the locale | the tool prints ASCII only | A6S-03, which greps the output for non-ASCII on **every** platform |
| a decode error read as a result | parser opens with `errors="replace"` | reintroducing the byte now passes A6S-02 and fails A6S-03 — the failure is named correctly |
| a crashed parser reported as drift | `PARSE_RC` checked before comparing | an injected parser fault reports "the baseline PARSER failed", not "drifted" |

The third is the one worth keeping. `A6S-02` printed **`flagged set drifted from
the pinned baseline`** and listed `<none parsed>` as the actual set — a
confident, specific, entirely wrong finding, produced by an exception it never
looked at. That is the method doc's *empty output is not a negative result*,
reached not by reasoning about it but by shipping it. A baseline gate that
cannot tell "nothing was flagged" from "nothing was read" will eventually
report the wrong one, and the direction it fails in is the direction that hides
findings.

This is the fifth instrument failure in a row, and the first that the self-test
could not have caught: `SELF_TEST_WRITTEN` and `SELF_TEST_UNWRITTEN` both guard
the *classification*, which was correct on Windows — `A6S-01` passed there. What
broke was the path from a correct answer to a reader, which no control covered
until now.

### The tool is registered, because an unrun tool is not coverage

It shipped unregistered. Register B10 had just established the failure that
creates: three of `tests/gorilla`'s four source-text greps sat latent because
nothing ran them, and a source-text grep passes whether or not anything fires.
A screening pass nobody runs decays into a claim about the past.

`tests/self-audit/test_state_field_screen.sh` now runs it in
`run-all-tests.sh` and pins the flagged set as a baseline, mirroring A2's
`inert_keys_baseline.txt`: A6S-01 asserts the tool's own self-test passes in both
directions, A6S-02 asserts the flagged set is exactly the two registered findings,
and A6S-03 asserts the output is ASCII (see round five above).
A new unwritten or unread field fails CI instead of waiting to be found by
accident — which A6 observes has happened four times out of four.
