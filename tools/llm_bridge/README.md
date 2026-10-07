# llm_bridge: live model output for NAAb's agent governance, without API keys

Every agent test in this repository talks to `tests/helpers/agent_stub.py`,
which replays canned, well-formed responses. Governance has therefore only
been tested against behaviour someone wrote down. The bridge replaces the stub
for **exploratory** runs: each request NAAb makes is answered by a *director*,
who can be a person or a Claude session driving subagent "personas". That
means real, varied, imperfect model output, including good output that
governance might wrongly flag.

Not for CI: it is slow (minutes per request) and non-deterministic by design.
What it finds is pinned for CI by exporting the captured exchange as a stub
fixture, which replays deterministically (see *Replay*).

Python 3 standard library only.

## Files

| file | what it is |
|---|---|
| `bridge.py` | the loopback Gemini `generateContent` server, plus the director's CLI (`next`, `render`, `respond`, `wait`, `autoreply`, `export-fixture`) |
| `run_harness.sh` | runs `examples/agent_harness` with every agent's `api_base` on the bridge (or, with `--replay`, on `agent_stub.py`) |
| `patch_config.py` | the ONLY changes made to the harness's governance for a bridge run, each recorded in `config_changes.json` |
| `persona_preamble.txt` | the exact operating instructions every persona subagent receives before its first request |
| `collect_run.py` | archives a run (evidence + replay fixture + compact persona exchange) into `runs/<label>/` |
| `runs/<label>/` | archived runs; see `docs/llm-bridge-findings.md` for what they show |

## Protocol

NAAb sends Gemini-format requests to a per-agent `api_base`. The engine
accepts plain `http` only for loopback (`127.0.0.1`, `localhost`, `[::1]`),
which is where the bridge binds. For each
`POST /v1beta/models/<model>:generateContent`, the bridge:

1. writes `queue/requests/<n>.json`, which holds the route (the `AH-<ROLE>`
   token at the start of the system prompt), system prompt, messages, tool
   declarations and generation config. The raw body goes to `<n>.raw.json`;
2. blocks until `queue/responses/<n>.json` exists;
3. returns a Gemini response with the same shape as `agent_stub.py`'s
   `gemini_body()`, and logs the pair, timestamped, to `queue/bridge_log.jsonl`.

`n` counts across all agents; `route_seq` counts within one role, and is what
an intervention ledger refers to.

### Token counts are ESTIMATES

The director produces text, not tokens. `usageMetadata` is
`ceil(max(words * 1.3, chars / 4))` for the prompt and for the reply, and
`thoughtsTokenCount` is **omitted**, so thinking is *unreported*, which is not
the same as zero. Every logged response carries `"tokens_estimated": true`.

Why not `words * 1.3` alone: the first control run (below) showed it
undercounts compact JSON. The stub's critic verdict
`{"verdict": "APPROVED", "issues": []}` estimated at 6 tokens. That is below
S23 `response_degenerate`'s 8-token floor, so S23 fired on a response the stub
run does not flag; a real tokenizer counts about 10–12. `chars / 4` is also
the engine's own fallback when a provider omits the count
(`agent_provider.cpp`, `normalizeResponse`).

**Any finding that rests on a token-keyed CDD signal rests on this estimate:**
S8 `response_quality`, S9 `thinking_collapse` (cannot fire: thinking is
unreported), S12 `context_growth`, and S23 `response_degenerate`.

`MAX_TOKENS` is emulated. A reply whose estimate exceeds the request's
`maxOutputTokens` is cut at a word boundary and returned with
`finishReason: MAX_TOKENS`, as a provider would. Disable it with
`serve --no-truncate`.

## Running

```bash
# from the repo root, after building build/naab-lang
tools/llm_bridge/run_harness.sh RUN_DIR &        # starts bridge + harness
python3 tools/llm_bridge/bridge.py next --queue RUN_DIR/queue   # pending requests, rendered
# ... answer request n ...
python3 tools/llm_bridge/bridge.py respond --queue RUN_DIR/queue n \
    --persona worker --model sonnet --style none \
    --from-transcript ~/.claude/projects/<proj>/<session>/subagents/agent-<id>.jsonl \
    --then-next 60
```

`respond` takes the reply from a file (`--reply-file`), from stdin, or, the
preferred way, straight from a persona subagent's transcript
(`--from-transcript`). Every response records its provenance (persona, model,
style, intervention, reply source) in `responses/<n>.json`, and the server
copies that into the bridge log.

### Personas (subagents as the model)

Each persona is a subagent that receives `persona_preamble.txt` once, followed
by each request rendered as text (`bridge.py render`). The rendering contains
the system prompt, the declared tools, the generation settings and the
conversation, and nothing else. Personas call tools by writing
`CALL <tool> <json>` lines, which the bridge turns into Gemini `functionCall`
parts.

**Reply extraction, and why it is not simply "the hand-back".** A subagent
returns its result through a `SubagentHandback` tool call. Haiku personas
under the `CALL`-line protocol often do not make that call: they write their
reply as plain text, and the agent harness then nudges them
("`[handback-send-enforce]` … call SubagentHandback"). Their answers to the
nudges are reactions to the harness, not to the request; under the `CALL`
protocol they come back as text-form fake hand-backs. So the reply to a
request is the persona's output **before any nudge**: its hand-back if it made
one, otherwise its pre-nudge text. Every response records which source it came
from. Mentioning the hand-back in the preamble made this worse: two variants
were tried, and the second put fake `<SubagentHandback>` markup into the reply
itself. The preamble therefore does not mention it.

## Controls (run before trusting a persona run)

| control | what it shows | result |
|---|---|---|
| `examples/agent_harness/run.sh --stub` | reference result on this build | green, `REPORT\|bytes=1381` |
| `run_harness.sh DIR --autoreply examples/agent_harness/fixtures/stub_responses.json` | the bridge path and config copy reproduce the stub run | identical stdout; CDD and output-admissibility telemetry identical event for event |
| the same with an AWS-style test key planted in the worker's step-2 answer | the bridge path still carries a refusal | exit 3, `code_quality.no_secrets` HARD |
| `export-fixture`, then `run_harness.sh DIR --replay fixture.json` | a captured run replays deterministically through the stub | CDD and admissibility telemetry identical to the bridge run |

## Replay

```bash
python3 tools/llm_bridge/bridge.py export-fixture --queue RUN_DIR/queue > fixture.json
tools/llm_bridge/run_harness.sh REPLAY_DIR --replay fixture.json
```

The fixture carries the bridge's token estimates explicitly, keeps thinking
unreported (`thinking_tokens: null`) and preserves `MAX_TOKENS` cuts. A replay
is exact only while the engine sends the same sequence of requests: a change
that alters the sequence shifts every later response, as with any stub
fixture. The two critic handles share one route, so a replay hands out their
answers in arrival order.

## What the run setup changes, and what it cannot measure

`patch_config.py` changes the run's copy of the harness's `govern.json` in
these ways, and in no others:

- `api_base` on every agent;
- the wall-clock budgets: per-call HTTP timeout, cumulative agent time, and
  run timeout;
- the wall-clock half of each standing lease.

A director-answered call takes minutes; a provider call takes seconds. Left as
shipped, those settings would end runs, or force step-up challenges, on the
instrument's latency rather than on anything a model did.

The cost: **lease expiry by elapsed time is not measured by any bridge run**.
The turn-based half of the lease is unchanged. Two other things also measure
the director rather than the model:

- BSD pattern decay windows (300 s by default) stretch relative to turns, so
  BSD under-detects here;
- `temporal_coupling`'s timing correlation.

Persona-side limits, which bound every finding:

- Every persona is a Claude model. Personas differ in capability and style but
  share underlying habits. Style profiles change format only; a Claude model
  styled like another vendor still fails like Claude.
- Personas are Claude Code subagents. Their environment loads this
  repository's `CLAUDE.md`, which describes the governance in detail; the
  director never supplies it. The preamble tells them to ignore it, and the
  findings report what share of persona output mentions NAAb terms.
- A persona keeps a memory across requests, which a real model behind a
  stateless API does not have. Each request is still delivered whole, and the
  persona is told only the request counts. A persona can still remember
  content the engine withheld: windowed-out turns, quarantined responses, and
  injections that were only ever sent ephemerally.
- Personas call tools through a text protocol, not native function calling.
  Haiku's tool-call habits through this protocol may differ from its native
  ones.
