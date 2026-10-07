# llm_bridge findings

What the archived runs under `tools/llm_bridge/runs/` show. The tool is
described in `tools/llm_bridge/README.md`.

The runs were made by another session (`claude/busy-thompson-lkdn8c`). Each of
its findings is a claim until checked, so every row below says what it rests on:

| tier | meaning here |
|---|---|
| **replayed** | its fixture was replayed through `tests/helpers/agent_stub.py` against the harness and binary at master `050cc9fe`, and the stated outcome (exit code, telemetry events, step verdicts) came back. This verifies what governance did with those replies; it does not re-verify how the replies were produced |
| **claimed** | stated in the run's commit message and not independently checked |

Provenance of the model replies: they are **authored by the bridge's
director**, Claude subagent "personas" told which role to play (see
`persona_preamble.txt`), not responses from a provider API. Token counts are
**estimates** (`ceil(max(words*1.3, chars/4))`) and thinking is unreported, so
any finding that rests on S8, S9, S12 or S23 rests on the estimate. S9 cannot
fire at all through the bridge.

## Runs

| run | worker | config | archived exit | replay exit | what happened | tier |
|---|---|---|---|---|---|---|
| A1 | haiku | shipped | 1 | 1 | Worker made 4 tool calls, which is its `max_tool_calls_per_turn`. The tool loop ended (`AGENT_TOOL_LOOP_END exit_reason=max_tool_calls_per_turn`) **without a final model turn**, so the content was empty (`RESPONSE_SUPPRESSED`). The harness then died on `json.parse("")` at audit step 1 | replayed |
| B1 | opus | shipped | 1 | 1 | same as A1 | replayed |
| C1 | sonnet | shipped | 1 | 1 | same as A1 | replayed |
| D-A1 | haiku | budget 8 | 1 | 1 | Worker answered in prose; the output contract refused it (`CONTRACT_VIOLATION`), program ended with "output contract violation". Claimed: a true catch, with a persona-memory confound | replayed (outcome) |
| D-B1 | opus | budget 8 | 0 | 0 | Step 1 exact; steps 2-4 failed validation (S22, extra findings rejected by the closed-world ground truth); step 4 quarantined at coherence 0.52 (`OUTPUT_INADMISSIBLE`); verdict REJECTED. Claimed: S10 + S15 fired on the plan-driven topic shift, and critics and judge cited `admissible=false` | replayed (outcome) |
| D-C1 | sonnet | budget 8 | 1 | 1 | Steps 1-2 passed, 3-4 failed validation. Both critics' replies were refused by the output contract (`CONTRACT_VIOLATION` x2). Under `agent.fan_out` a refusal reaches the script as `{error, success:false, content:""}`, and the harness died on `json.parse("")` | replayed |
| D-S1 | opus | budget 8 | 0 | 0 | D-B1's models with style profiles; verdict REJECTED. Claimed: no profile changed a JSON-mandated reply, so style is not measurable through this harness | replayed (outcome) |

"budget 8" is the one named deviation `worker-tool-budget-8`
(`agents.worker.max_tool_calls_per_turn` 4 -> 8, made in `patch_config.py`). It
LOOSENS the shipped governance and was chosen because every shipped-config run
ended at step 1. D-* runs are never pooled with A1/B1/C1.

Reproduce a row (needs `build/naab-lang`; prints the exit code and writes
`RUN_DIR/harness/out/telemetry.jsonl`):

```bash
tools/llm_bridge/run_harness.sh /tmp/rp-A1 --replay tools/llm_bridge/runs/A1/fixture.json
tools/llm_bridge/run_harness.sh /tmp/rp-DC1 --replay tools/llm_bridge/runs/D-C1/fixture.json --deviation worker-tool-budget-8
```

## Open items

1. **The harness crashes on an empty reply** (`examples/agent_harness/src/harness.naab`,
   `json.parse(response.get("content"))` at the worker step, and
   `json.parse(responses.get(i).get("content"))` over the `fan_out` critics).
   Two routes reach it, both replayed: the worker spending its whole tool budget
   (A1, B1, C1) and a critic's reply refused by its output contract (D-C1).
   Neither checks `success` or empty content. An example of governed agents
   should treat these as a failed step and a void vote. It should not crash.
   **Open; needs a decision on those two semantics.**
2. **Running out of tool budget gives the model no last turn.** At
   `max_tool_calls_per_turn` the loop exits and the model never sees the
   results of its last calls, so the reply is empty. Whether the budget should
   grant one final tool-less turn is a design question: it costs one more model
   call per exhausted budget. **Open.**
3. **The output contract does not apply to an empty reply.** `agentSend()`
   validates only `if (config && !content.empty() && ...)`
   (`src/stdlib/agent_impl.cpp`, output contract block). The guard has been
   there since the contract was introduced (`99447293`), which records empty
   replies as `RESPONSE_SUPPRESSED` telemetry. Its commit gives no reason for
   exempting them. So a role whose contract requires fields can hand the script
   `""`. Traced, not decided. Making an empty reply a contract violation would
   tighten behaviour, so it needs its own change. **Open.**
