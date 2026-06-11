# Agent Orchestration Demos

NAAb as a practical agent orchestration language — with built-in governance,
tamper-evident audit trails, and adaptive safety controls.

## Prerequisites

```bash
# Build NAAb
cd ~/.naab/language/build && cmake .. && make naab-lang -j4

# Set your API key (Gemini)
export GK6=your-gemini-api-key
```

## Running

```bash
# From the language root directory:

# With governance (the real experience — dashboard + telemetry):
./build/naab-lang demos/agent-orchestration/01-single-agent.naab

# With full decision chain (adds JSON report):
./build/naab-lang run --governance-report demos/agent-orchestration/report.json \
  demos/agent-orchestration/08-traceability-proof.naab

# Quick test without governance:
./build/naab-lang --no-governance demos/agent-orchestration/01-single-agent.naab
```

## Demos

| # | File | Pattern | What It Shows |
|---|------|---------|---------------|
| 01 | `01-single-agent.naab` | One-shot | Pre-flight check, single call, run accounting |
| 02 | `02-multi-turn-tools.naab` | Tool use | Tool governance (dual-gate), conversation state |
| 03 | `03-parallel-review.naab` | Fan-out | Parallel execution, independent governance per agent |
| 04 | `04-pipeline-refine.naab` | Pipeline | Sequential chain, upstream provenance (trust signals) |
| 05 | `05-consensus-gate.naab` | Consensus | Multi-agent voting, convergence validation |
| 06 | `06-self-correcting-codegen.naab` | Self-correction | Governance blocks bad code, agent fixes it |
| 07 | `07-adaptive-orchestration.naab` | Adaptive | governance.health() drives orchestration decisions |
| 08 | `08-traceability-proof.naab` | Audit trail | Full decision chain + hash chain verification |

## What You'll See

Every demo with governance produces two outputs:

**1. Program output** (stdout) — the demo's results

**2. Governance dashboard** (stderr) — automatic summary:
```
─── Agent Governance Summary ───
Mode:       enforce | Sandbox: elevated
Checks:     23 passed, 0 blocked
Telemetry:  25 events → telemetry.jsonl
CDD:        coherence=0.92 vel=-0.01 (2 turns)
Dispatch:   2 calls (0 retries, 1834 tokens, 1240ms)
Pulse:      HEALTHY (23 checks, 2 consecutive passes, epoch 1)
────────────────────────────────
```

## What Happens When Things Go Wrong

**Demo 06** — The agent generates Python code. Governance runs 39+ checks on it.
If the code violates any rule (imports, complexity, secrets), it gets blocked.
The orchestrator feeds the error back, and the agent self-corrects.

**Demo 07** — The orchestrator checks `governance.health()` before each send.
If coherence is dropping (DEGRADED), it switches to conservative prompts.
If governance is IMPAIRED, it skips the call entirely.

These aren't theoretical — the governance engine runs the same checks on agent
output that it runs on human-written code.

## Traceability & Proofs

NAAb produces three layers of proof:

**Layer 1: Dashboard** — What was checked, what passed, what was blocked and why.
Printed to stderr after every governed run.

**Layer 2: Governance Report** — Per-check decision trace with step-by-step
reasoning. Add `--governance-report report.json` to any run.

**Layer 3: Telemetry Hash Chain** — Every governance event (PROMPT_SCAN,
ADMISSION_EVAL, AGENT_RESPONSE, ...) is written to `telemetry.jsonl` with
cryptographic hash linking. Delete or modify any event and the chain breaks.

Verify the chain:
```bash
bash demos/agent-orchestration/verify-chain.sh
```

Inspect raw events:
```bash
cat demos/agent-orchestration/telemetry.jsonl | python3 -m json.tool
```

## Why NAAb for Orchestration

- **Governance catches bad agents** — 39+ checks on every agent.send(), codegen, and tool call
- **Self-correction is a pattern** — feed governance errors back, agents fix themselves
- **Adaptive orchestration** — governance.health() and agent.environment() let code react to system state
- **Provenance is automatic** — pipeline stages carry upstream trust signals (model, retries, coherence)
- **Parallel is a primitive** — fan_out/batch are single function calls, not thread management
- **Tool use is governed** — 7-layer defense on every tool call, dual-gate enforcement
- **Tamper-evident audit trails** — hash-chained telemetry proves no events were deleted or modified

## Configuration

All agent configs live in `govern.json`. Key settings:

- `mode: "enforce"` — governance is active and blocking
- `dashboard: true` — auto-print governance summary after each run
- `telemetry.enabled: true` — hash-chained events logged to JSONL
- `scoring.enabled: true` — cumulative risk scoring (yellow/red thresholds)
- `codegen.enabled: true` — governed dynamic code execution
- `context_drift.enabled: true` — CDD coherence tracking
- `governance_health.enabled: true` — pulse verdict (HEALTHY/DEGRADED/IMPAIRED)
