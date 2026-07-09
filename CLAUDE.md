# NAAb Language — Internal Reference

Reference for Claude Code when working on the NAAb language source (this repository).

## Build

```bash
# from the repo root
mkdir -p build && cd build && cmake .. && make naab-lang -j4
```

- Debug (default): `cmake ..`
- Release: `cmake .. -DCMAKE_BUILD_TYPE=Release`
- After modifying CMakeLists.txt: `cmake ..` again before `make`

Binary lands at `build/naab-lang`. A second CLI, `naab-gov` (`src/cli/gov_main.cpp`), builds via `make naab-gov` for standalone govern.json work.

## Test

```bash
# Full suite — 438 tests, 0 unexpected failures
bash run-all-tests.sh   # from the repo root

# Security leak check — 874 checks, 0 failures
bash tests/security/test_error_msg_leaks.sh
```

Test categories in `tests/` (non-exhaustive — 40+ directories total): governance_v4, security, stdlib, vm, cli, e2e, integration, bugs, gorilla, scanner, formatter, lsp, platform, chaos, robustness, agent, unit, property.

Expected breakdown: ~374 pass, ~51 error-behavior (intentional failures), ~12 needs-tree-walk (VM-unsupported features), plus a platform-dependent missing-executor count (tests whose compilers are absent on the platform).

Run a single test: `./build/naab-lang tests/path/to/test.naab`

### Differential / Oracle / Fuzz pipeline

```bash
# Differential v2 — VM vs tree-walker output parity over a corpus
bash tests/differential/run_differential.sh

# Fuzz smoke — 300 deterministic seeds + regression seeds (runs on every PR)
bash tests/fuzzing/run_fuzz_smoke.sh

# Exact-arithmetic oracle suite (also invariant 7 of run_property_tests.sh)
PYTHONPATH=tools python3 -m naabfuzz oracle --naab build/naab-lang

# Reproduce / debug a fuzzer finding
PYTHONPATH=tools python3 -m naabfuzz repro --seed N
PYTHONPATH=tools python3 -m naabfuzz minimize --naab build/naab-lang --seed N
```

- Both suites run inside `run-all-tests.sh`; deep fuzzing (30 min grammar +
  libFuzzer) is nightly-only (`.github/workflows/fuzz-nightly.yml`).
- Engine forks are triaged with the oracle: `VM_BUG` / `TW_BUG` attribution.
  The pipeline never auto-allowlists — every entry in
  `tests/differential/divergences.json` and every accepted signature in
  `tests/fuzzing/known_findings.txt` MUST link to a section in
  `docs/engine-divergences.md`.
- Division is always double (`7/2 == 3.5`) in BOTH engines; `INT_MIN % -1`
  is `0` (see DIV-001/MOD-001 in `docs/engine-divergences.md`).
- `tools/naabfuzz` is python3-stdlib-only; oracle model facts (errors print
  to stdout with `Error:` prefix, `%.15g` doubles, `math.abs` returns float,
  the literal `-2147483648` lexes as a float) are locked in by
  `python3 -m naabfuzz selftest` and `tools/naabfuzz/vectors.py`.

## Source Layout

```
src/
├── lexer/          Tokenizer — keywords map, readString(), readInlineCode()
├── parser/         Recursive descent — parsePrimary(), error_hints
├── interpreter/    Tree-walker — visitor pattern, call_dispatch, governance_taint
├── vm/             Bytecode VM — stack-based dispatch loop, compiler (AST→bytecode)
├── runtime/        Governance engine, polyglot executors, trust_store, crypto_utils
├── stdlib/         23 modules (*_impl.cpp): array, string, math, file, json, csv,
│                   dict, path, env, time, regex, crypto, http, log, uuid, validate,
│                   process, debug, bolo, agent, codegen, orchestra, governance
├── cli/            main.cpp entry point — CLI flag parsing, subcommands
├── scanner/        C++ security scanner (SARIF output, 19 code quality checks)
├── api/            REST API (rest_api.cpp) + governance C API (governance_c_api.cpp,
│                   built as naab_governance static lib; Go/Rust/Java/C#/Python bindings in bindings/)
├── repl/           REPL with readline support
├── linter/         Static analysis
├── formatter/      Code formatter
├── analyzer/       Semantic analysis
├── platform/       OS abstraction (platform_posix.cpp, platform_win32.cpp)
├── profiling/      Performance profiling
├── testing/        Test framework internals
├── debugger/       Debugger implementation
├── utils/          Shared utilities
├── doc/            Doc generator
├── manifest/       Package manifest handling
├── packages/       Package manager
└── semantic/       Semantic analysis passes
include/naab/       All headers
```

## Key Architecture

### NaabVal (NaN-boxed values)
- `include/naab/naab_val.h` + `src/interpreter/naab_val.cpp`
- 8-byte inline values for int, double, bool, null
- Factory: `NaabVal::makeInt(42)`, `NaabVal::makeString("hi")`, `NaabVal::makeBool(true)`
- Type check: `val.isInt()`, `val.isString()`, `val.isBool()`, `val.isNull()`
- Extract: `val.asInt()`, `val.asString()`, `val.asBool()`
- Bridge: `NaabVal::fromLegacy(shared_ptr<Value>)`, `val.toLegacy()`
- Null check: `val.isNull()` — NOT `if (val)`

### Interpreter
- `src/interpreter/interpreter.cpp` — main visitor
- `src/interpreter/call_dispatch.cpp` — function call routing
- `src/interpreter/expressions.cpp` — expression evaluation
- `result_` (NaabVal) — last evaluated value
- `current_env_` — current scope (shared_ptr\<Environment>)
- `global_env_` — global scope
- `current_file_` — current source file (NOT `filename_`)
- Control flow: `returning_`, `breaking_`, `loop_depth_`
- Environment class: `class Environment` in `include/naab/interpreter.h` (~line 413)

### Bytecode VM
- `include/naab/vm.h` + `src/vm/vm.cpp` — stack-based dispatch
- `include/naab/compiler.h` + `src/vm/compiler.cpp` — AST to bytecode
- VM is default engine (`global_use_vm = true`), tree-walker via `--tree-walk`
- Computed goto dispatch on GCC/Clang, switch fallback elsewhere
- VM taint tracking: `taint_stack_` mirrors value stack

### Governance Engine
- `src/runtime/governance_engine.cpp` — main engine, signature verification
- `src/runtime/governance_checks.cpp` — 50+ individual checks
- `src/runtime/governance_config.cpp` — config loading from govern.json
- `src/runtime/governance_taint.cpp` — taint tracking (interpreter path)
- `src/runtime/trust_store.cpp` — Ed25519 trusted key management
- `src/runtime/crypto_utils.cpp` — Ed25519 sign/verify, SHA-256
- Behavioral contracts: `must_call` (function must call specified functions — regex `\bname\s*\(` on body text, non-transitive), `must_contain` (function body must match syntax patterns), `must_produce` (golden tests with type-strict comparison — string "0" does not match int 0), `min_arity`/`max_arity` (parameter count enforcement)
- Anti-gaming: magic number / hardcoded constant detection in polyglot blocks, oversimplification checks, complexity floor
- Module governance: VM compiler skips function-body governance checks during module loading (`!skip_main_` guard in `compiler.cpp`), matching tree-walker's `module_loading_depth_ == 0` guard
- Enforcement tiers: HARD (block, exit 3), SOFT (block unless `--governance-override`, exit 3 without override), ADVISORY (warn, continue), DETECT (block but catchable by NAAb try/catch — for test configs only)
- Exit codes: 0=success, 1=runtime, 2=quality gate, 3=HARD governance block, 4=config error
- **GovernanceHardError** (`class GovernanceHardError` in `governance.h`, ~line 2467): `enforce()` throws `GovernanceHardError` (inherits `std::runtime_error`) for all HARD-level blocks. NAAb `try/catch` cannot catch it — both tree-walker (`interpreter.cpp`: `visit(TryCatchExpr)`, `visit(TryStmt)` outer+inner) and VM (`vm.cpp`: dispatch loop + 3 inner catches) explicitly re-throw before generic handlers. `main.cpp` catches it and calls `_exit(3)`. When adding new catch sites that could intercept stdlib/governance exceptions, always add `catch (const governance::GovernanceHardError&) { throw; }` BEFORE `catch (const std::exception&)`. ADVISORY (non-escalated) and SOFT-with-override remain catchable.
- **env_vars enforcement**: `capabilities.env_vars` in govern.json — `blocked_read` (HARD), `allowed_read` (SOFT allowlist), `blocked_write`/`allowed_write`. Enforced in all 9 env stdlib access points via `checkEnvVarRead()`/`checkEnvVarWrite()` in `governance_engine.cpp`. blocked_read prevents value from ever entering memory (check fires before `std::getenv`).
- Decision rationale: govern.json sections accept optional `rationale` field; engine generates `decision_trace` per check. Both flow into all 5 report formats + audit trail via `CheckResult.rationale` and `CheckResult.decision_trace`
- Decision trace storage: `t_current_decision_trace` is `static thread_local` in `governance_engine.cpp` (NOT a class member) — thread-safe for concurrent polyglot/agent threads
- Mid-run reload (Governance Under Survivability): `reloadIfChanged()` detects govern.json mtime changes during agent turns, validates signature, enforces one-way ratchet (only tightening allowed), and surfaces structured notices via `agent.send()` return dict `governance_notices` field. Reload is triggered automatically before each `agent.send()` and `agent.batch()` call. Every reload attempt (accepted or rejected) emits a `CONFIG_ADJUSTMENT` telemetry event with changed agents, ratchet notices/violations, update_reason, and best-effort requesting handle/config. Accepted reloads also trigger a *scoped* CDD rate-window reset (`ContextDriftAnalyzer::onAgentConfigChanged()`) for agents whose behavior-affecting fields changed: signal snapshots and `post_baseline_checks` restart (and mandate keywords re-derive on system_prompt change) while learned baseline mean/stddev, coherence, and history are preserved — operator adjustments don't score as agent drift, and flip-flop reloads fall back to full base penalties instead of earning a grace period.
- Fail-closed enforcement: govern.json capabilities sync to sandbox at load and mid-run reload. `capabilities.shell.enabled: false` removes SYS_EXEC from sandbox; `capabilities.network.enabled: false` removes NET_CONNECT; `capabilities.filesystem.mode: "none"` removes FS_READ+FS_WRITE. Enforce mode defaults to `standard` sandbox (not `unrestricted`). Agent worker threads inherit sandbox config from main thread.
- Continuous governance (6 features):
  - **Handle anti-forge**: HMAC-SHA256 nonce ties `agent.create()` handle to server-side state. `validateHandle()` uses constant-time comparison. Forgery/replay/mutation rejected. Process secret from `/dev/urandom`.
  - **Temporal trust decay**: coherence erodes over time when idle. Config: `context_drift.temporal_decay_enabled`, `temporal_decay_per_minute`, `temporal_decay_grace_minutes`. Default off.
  - **Credential refresh**: dead API keys revive after cooldown. Config: `retry.key_retry_after_seconds` (0 = never revive). `isKeyDead()` helper used at all `dead_keys` call sites. AGENT_KEY_REVIVED telemetry on revival.
  - **Adaptive baselining**: per-agent baseline window observes normal signal rates before penalizing. Penalties only fire when signals exceed `mean + k*stddev`. Config: `context_drift.adaptive_baseline_enabled`, `adaptive_baseline_window`, `adaptive_baseline_sensitivity`. Default off.
  - **Step-up challenges**: at elevated governance levels, inject challenge prompt and score response (word count + keyword overlap with system_prompt). Pass recovers coherence, fail blocks send. Lease renewal gated on coherence floor. Failed challenges advance the turn counter (a failed challenge IS a turn — prevents death spiral where turn never advances). Config: `circuit_breaker.step_up_enabled`, `step_up_at_level`, `step_up_challenge`, `step_up_min_words`, `step_up_cooldown_turns`, `step_up_keyword_threshold`. Challenge telemetry includes response_length, output_tokens, thinking_tokens, keyword_ratio, context_prompts, challenge_type.
  - **Contextual challenges**: when `step_up_contextual = true`, challenge prompts are dynamically selected from DriftState data instead of using the canned `step_up_challenge` text. 5 types in priority order: (1) `tool_result` — asks agent to recall tool output (expected keywords from `tool_result_keywords`), (2) `plan_step` — asks about plan step N (from `plan_step_keywords`), (3) `instruction` — asks to summarize most recent user instruction (from `instruction_history`), (4) `entity` — asks about an entity and its relation to the task (from `entity_context`), (5) `mandate` — fallback to canned prompt. Contextual challenges use `scoreContextualChallengeRatio()` with lower threshold `step_up_contextual_threshold` (default 0.30 vs 0.40 for mandate). Config: `circuit_breaker.step_up_contextual`, `step_up_contextual_threshold`.
  - **Challenge history mode**: `step_up_challenge_history` controls conversation context sent with challenge — `"full"` (all messages, no cap), `"recent"` (last N messages, default), `"summary"` (DriftState summary preamble + last N messages). `step_up_history_recent_count` sets N (default 20). Summary mode embeds instruction keywords, plan steps, entities, and tool names as a compact text preamble in the challenge question — zero API cost, uses already-collected CDD metadata. Config: `circuit_breaker.step_up_challenge_history`, `step_up_history_recent_count`.
  - **Mandate reinforcement** (preventive, periodic): Prepends `[Task Reminder: {system_prompt}]` to user message every N turns. Config: `circuit_breaker.mandate_reinforcement_enabled`, `mandate_reinforcement_interval` (default 10), `mandate_reinforcement_message`. Zero extra API calls — modifies `messages_json.back()["content"]`. Ephemeral: sent to API but NOT stored in handle history. `MANDATE_INJECTION` telemetry event.
  - **Coherence correction** (reactive, CDD-triggered): Prepends graduated correction text when `DriftState.coherence_score` drops below threshold. Three severity tiers: gentle `[Task Reminder]` at coherence >= 0.7, firm `[IMPORTANT]` at 0.5-0.7, forceful `[CRITICAL - REFUSE]` below 0.5. Config: `circuit_breaker.coherence_correction_enabled`, `coherence_correction_threshold` (default 0.85), `coherence_correction_cooldown_turns` (default 5), `coherence_correction_message` (overrides graduated text if set). Priority: if both mandate reinforcement and correction trigger same turn, correction wins. Ephemeral injection, same pattern as mandate reinforcement.
  - **Context windowing**: Limits conversation history sent per API call to prevent O(n²) token growth. Per-agent `context_window` (int, 0 = unlimited) and `context_strategy` ("full"/"recent"/"summary"). When `context_strategy != "full"` and message count exceeds `context_window`, only the last N messages are sent with even-alignment (Gemini requires strict user/assistant alternation). In "summary" mode, `buildChallengeSummary()` prepends a DriftState preamble to the first windowed message. Ratchet-enforced (loosening blocked). Transcript includes `context_windowing` metadata. Agent environment exposes `limits.context_window` and `context_strategy`. Config: per-agent `context_window`, `context_strategy` in govern.json agents block.
  - **Separation of duties**: per-agent `network_allowed` (bool, same pattern as `shell_allowed`) and `allowed_actions` matrix (`["SHELL_EXEC", "NET_CONNECT", "FS_READ", "FS_WRITE", "AGENT_SEND", "TOOL_EXEC"]`). Enforced in `checkNetworkAllowed()`, `checkShellAllowed()`, `checkFilesystemAllowed()`, and `agentSend()`. `TOOL_EXEC` controls tool execution in agent tool loops. Ratchet enforcement prevents mid-run loosening of action matrix.
  - **Output admissibility**: Post-CDD gate on response coherence — symmetric to `checkAdmission()` (pre-send). Evaluates after CDD scoring, before response dict construction. Three actions: `block` (enforce() throws, response not returned — HARD/SOFT = GovernanceHardError, DETECT = catchable), `quarantine` (response returned with `admissibility.admissible = false`), `attest` (quarantine + Ed25519 signed attestation in response + telemetry). PulseVerdict IMPAIRED forces inadmissible regardless of score. Config: `circuit_breaker.output_admissibility.enabled`, `threshold` (default 0.70), `action` (default "quarantine"), `level` (for "block" only, clamped to minimum DETECT), `inadmissible_history` ("commit" default / "exclude" — whether quarantined/attested responses enter handle history), `gate_tool_calls` (bool, default false — coherence/pulse gate before each tool execution in the tool loop). Ratchet-enforced (enabled→disabled, threshold down, action rank down, exclude→commit, gate_tool_calls true→false = violations). Telemetry: `OUTPUT_ADMISSIBILITY_EVAL` (pass/fail), `OUTPUT_INADMISSIBLE` (fail only, includes `history_committed`). Dashboard: `OA Gate:` line.
  - **Split commit** (see `docs/transition-admissibility.md`): `agent.send()` separates accounting commits (tracker turns/tokens + `recordAutonomousAction()` exposure — committed BEFORE the post-receive gates, so blocked attempts still count toward the next admission projection) from conversation-state commits (history append happens AFTER CDD + OA gate — blocked turns, including catchable DETECT blocks, never enter handle history).
  - **Propose/commit** (selective adjudication): `agent.propose(handle, msg [, n])` generates up to `propose_candidates_max` (per-agent config, 0 = disabled, increase = ratchet violation) candidates with NO state commit — no history append, no turn increment, no CDD mutation, no tool execution (tool defs never sent). Each candidate carries an `admissibility` section (read-only score vs current DriftState snapshot) and an anti-forge `__proposal` HMAC nonce; authoritative content lives server-side in `s_pending_proposals`. `agent.commit(handle, proposal)` runs the post-receive pipeline exactly once (BSD AGENT_RESPONSE, CDD, full enforce-capable OA gate, split commit of history + turn). Proposals are single-use and invalidated by any subsequent send/propose/commit on the handle. Step-up-required / lease-expired states refuse propose (fail-closed — re-authorize via `agent.send()`). `orchestra.select_admissible(candidates [, spec])` is the pure ranking helper (spec reuses enforce_convergence pattern/required_fields). Telemetry: `AGENT_PROPOSE`, `AGENT_PROPOSAL_COMMIT`.
- **Governance Pulse** (`src/runtime/governance_engine.cpp`): PulseVerdict (HEALTHY/DEGRADED/IMPAIRED) with hysteresis, two-phase mutex, stepped recovery, BSD emission on transitions, dashboard line. `governance.health()` stdlib returns `{verdict, coherence, governance_level, governance_epoch, bsd_events, cdd_turns_analyzed, ...}`. Config: `governance_health` section.
- **Standing Lease**: TTL on agent authorization (Kerberos TGT analog). Per-agent `standing_lease_turns` and `standing_lease_seconds`. Expired lease forces step-up challenge. Renewed on pass. `lease_remaining` in agent environment.
- **Advisory Escalation**: Repeated advisories harden (OSHA violation analog). 2nd+ occurrence: weight multiplied. N-th (`soft_after`): escalate to SOFT block. Config: `advisory_escalation.enabled`, `soft_after`, `weight_multiplier`.
- **Evidence Epoch**: Monotonic counter incremented on pulse verdict change, governance level change, config reload. `governance_epoch` in agent environment + `governance.health()`. Prior-epoch evidence discounted via `consecutive_passes` reset.
- **Consequence-boundary hardening**: `verifyScoreIntegrity()` escalation-aware, `wasBlocked()` sees escalated advisories, `governance_epoch_` atomic, advisory history decay on epoch boundary, BSD/CDD evidence preservation across `updateConfig()`.
- **Non-binding refusal attestation**: tamper-evident proof of governance blocks. Signed attestation recorded when HARD block prevents execution.
- **Agent Output Contracts**: per-agent `output_contract` in govern.json agents block — `format`, `required_fields`, `field_types`, `regex_checks`. Validated after RESPONSE_SCAN. `CONTRACT_VIOLATION` telemetry event on failure. Config: `OutputContract` struct in `governance_config.h`.
- **RESPONSE_SUPPRESSED telemetry**: emitted when `content.empty()` after all retries — records handle_id, config_name, turn, reason, retries_used. Fills observability gap where empty responses looked like they never reached post-receive governance.
- **Content-aware CDD** (semantic governance): CDD has **21 signals** total (S21 `response_repetition` detects verbatim duplicates via content fingerprints). Fingerprints include SHA-256 hash of response content (first 200 chars), breaking mechanical "ARAS" pattern. Response keywords extracted via the SHARED code-aware extractor in `include/naab/keyword_extract.h` (single implementation used by both `agent_impl.cpp` and `behavioral_sequence.cpp` — `extractKeywordsLocal()` is a thin forwarder). Extraction: >3-char lowercased tokens, `kStopWords` (~55 English function words + LLM boilerplate), `kCodeStopWords` (language syntax like return/self/import — deliberately KEEPS topical words like class/main/pass), and camelCase/PascalCase/letter-digit boundaries ADDITIONALLY emit component words (`getHistory` → gethistory + history, `TodoItem` → todoitem + todo + item). This keeps code-only agents in the same token space as English mandates — without it, keyword-overlap signals (S10/S11/S13/S15) deterministically penalized code output regardless of quality. Extraction is symmetric (mandate/instruction/response all use the same function). Test: `tests/governance_v4/test_code_aware_keywords.sh`.
- **Per-agent CDD signal overrides**: per-agent `context_drift_signals` map in the govern.json agents block (keys = the canonical `kCddSignalKeys` config names in `behavioral_sequence.h`, e.g. `{"semantic_stability": false}`) overrides individual global `context_drift.signals.*` toggles for that agent's handles. Stored as bitmasks on `DriftState` (`signal_override_mask`/`signal_override_values`, preserved across `agent.reset()`); `sig_on()` gating in `recordTurn`; `effectiveSignal()` in agent_impl for pre-CDD input gates; unknown keys warn at parse. Exposed as `state.cdd_signal_overrides` in the agent environment. Ratchet: global `context_drift.signals.*` disable mid-run = violation (this closed a pre-existing gap — they were previously un-ratcheted; `exclude_infrastructure_errors` ratchets inverted), and per-agent overrides compare EFFECTIVE values, reported only when the override itself changed. Accepted reloads recompute live handles' masks via `onAgentConfigChanged()`. Test: `tests/governance_v4/test_per_agent_signals.sh`. Content-aware CDD signals:
  - `response_quality` (S8): fires when content/output token ratio < `response_quality_min_ratio` (0.3). Weight 0.08
  - `thinking_collapse` (S9): fires when rolling-window mean of thinking tokens drops below `thinking_collapse_ratio` (0.5) of baseline mean. Weight 0.06
  - `semantic_stability` (S10/F19): Jaccard similarity between consecutive response keyword sets < `semantic_stability_min_overlap` (0.25). Weight 0.10
  - `mandate_alignment` (S11): rolling-window (size 20) overlap between response and system_prompt keywords < `mandate_alignment_min` (0.15). Weight 0.12
  - `context_growth` (S12): input tokens exceed baseline by `context_growth_factor` (3.0x). Weight 0.07
  - `instruction_recall` (S13): user instruction keyword recall < `instruction_recall_min` (0.10). Weight 0.08. Keywords accumulated via `addInstructionKeywords()`
  - `plan_drift` (S14): agent response deviates from stated plan (regression, skip, abandonment). Weight 0.09. `extractPlanFromResponse()` parses numbered lists, "Step N:", "Phase N:"
  - `entity_consistency` (S15): entity-context Jaccard < `entity_context_min_overlap` (0.25). Weight 0.08
  - `instruction_conflict` (S16): user instructions with > `instruction_conflict_topic_overlap` (0.40) topic overlap + negation markers. Weight 0.10
  - `persona_fingerprint` (S17): response keyword count deviates > `persona_deviation_factor` (2.0) stddev from baseline. Weight 0.05
  - `tool_chain_integrity` (S18): tool result keyword recall < `tool_result_recall_min` (0.15) when agent references tool. Weight 0.08. `recordToolResult()` stores per-tool keywords
  - `claim_result_reconciliation` (S19): agent claims success but tool failed (or vice versa). Weight 0.12. `recordToolOutcome()` stores tool success/failure (NOT gated on `tool_success` — captures both). Success/failure word matching with ambiguity guard (both/neither = no fire). Rolling accuracy in `claim_accuracy_history` (deque, size 20). Dashboard: `Reconcil: N mismatches, N integrity violations, accuracy=0.XX`. Telemetry: `RECONCILIATION_TURN` events
  - `prompt_compliance` (S20): detects when agent substantively complies with off-topic prompts instead of refusing/redirecting. Weight 0.10. 3-gate algorithm: (1) prompt-to-mandate keyword overlap < `prompt_compliance_mandate_min` (0.10) = off-topic prompt, (2) response output_tokens >= `prompt_compliance_response_min_tokens` (50) = substantive response, (3) no refusal indicators in response keywords (16 words: focused, redirect, sorry, cannot, task, etc.). All 3 gates must pass to fire penalty. Per-turn prompt keywords stored in `DriftState.turn_prompt_keywords` via `setTurnPromptKeywords()` (same API pattern as `recordToolOutcome()`), cleared after each CDD check. Depends on `mandate_keywords` being populated — initialization guard checks for S11 OR S20 enabled. Dashboard: `Compliance: N off-topic compliance events, prompt_alignment=X.XX`
  - All CDD signals default enabled (S8-S21). Surfaced in: environment dict, SEMANTIC_TURN telemetry, transcript CDD section, response `semantic` section. S19 additionally produces `RECONCILIATION_TURN` telemetry and dashboard line. Environment dict includes `claim_mismatch_count`, `claim_accuracy`, `prompt_compliance_count`. Can be individually disabled via `context_drift.signals.<name>: false` in govern.json (globally — mid-run disable is a ratchet violation) or per-agent via the agents-block `context_drift_signals` map
- **Infrastructure error classification**: API errors passed to CDD are prefixed `"infrastructure:"` (3 call sites in `agent_impl.cpp`). When `signals.exclude_infrastructure_errors` is true (default), these errors bypass the `repeated_failures` signal entirely. Prevents the "challenge death spiral" where API 500s cascade into coherence drop → governance escalation → step-up challenge → challenge also fails → all sends blocked. Only affects CDD error deque; does NOT change user-facing error messages, telemetry, or retry logic
- **Health warning recovery awareness**: `checkGovernanceHealth()` suppresses "perfect coherence" warnings during adaptive baseline window and for 3 turns after coherence recovery (recovery resets to 1.0, which is expected behavior not a detection bypass).
- **AGENT_RESPONSE telemetry enrichment**: now includes `content_hash` (SHA-256 of first 500 chars) and `content_length` for post-hoc audit.
- **Truncation tracking**: AgentTracker counts truncated responses; emits advisory when majority (>50%) of responses are truncated.
- **RESPONSE_TRUNCATED telemetry**: emitted when `agent_resp.truncated` is true (stop_reason is MAX_TOKENS/max_tokens). Records thinking_tokens, configured_max, thinking_budget, content_length. Three-case stderr warning: (1) thinking consumed budget with no config, (2) thinking configured but too small, (3) pure response overflow.
- **Thinking budget**: `thinking_budget` per-agent config field. `-1` = provider default (backward compatible), `0` = disable thinking, `>0` = enable with budget. Gemini: expands `maxOutputTokens = max_tokens + thinking_budget` and sends `thinkingConfig.thinkingBudget`. Anthropic: sends separate `thinking` block. Ratchet-enforced (loosening blocked). `agent.send()` response includes `truncated` (bool) and `usage.thinking_tokens` (int). Agent environment includes `limits.thinking_budget`.
- **Token floor (`min_tokens`)**: per-agent config field, default 0 (no floor). Effective request budget = `max(max_tokens, min_tokens)` (`effectiveMaxTokens()` in `agent_provider.cpp`) — prevents an operator from crippling an agent mid-run via a tiny `max_tokens`. Ratchet raise-only: lowering or removing the floor is a violation, as is reducing `max_tokens` below the floor. Agent environment includes `limits.min_tokens`.
- **CDD rate-normalization floor (`context_drift.rate_normalized_floor`)**: when `rate_normalized` is on, a firing signal always pays at least this fraction of its base weight (default 0.5) — blocks the boiling-frog slow-drift attack. Ratchet: lowering the floor mid-run is a violation; enabling `rate_normalized` itself mid-run is also a violation (it strictly reduces penalties). See `docs/security-decisions.md` for rejected-proposal rationale (including why `governance.resign()` must not exist).
- **BSD pattern normalization**: `matchesStep()` normalizes UPPERCASE_UNDERSCORE pattern names (e.g., `"AGENT_SEND"`) to lowercase dot-notation (`"agent.send"`) for matching against `eventTypeToString()` output. Both formats accepted in govern.json patterns.
- **Cross-agent BSD defaults**: 2 built-in cross-agent patterns added to `buildDefaultPatterns()` (behavioral_sequence.cpp): `cross_agent_file_relay` (FILE_WRITE→FILE_READ, gap=5) and `cross_agent_tool_chain` (TOOL_CALL→AGENT_SEND→TOOL_CALL, gap=10). Only match between agent contexts (non-agent file ops filtered out). User-defined patterns replace defaults entirely (no merge).
- **Agent creation admission guard**: `agentCreate()` blocks tool callbacks from spawning agents unless parent agent's `allowed_actions` includes `AGENT_SEND`. `max_unique_agents` (ExposureTrackingConfig) enforced inside existing mutex lock scope — prevents unbounded agent trees.
- **Gap O enforcement**: `agentSend()` checks `t_in_tool_execution_for_handle` — blocks recursive self-send where a tool calls `agent.send()` on its own invoking handle.
- **govern.json self-protection**: `addGovernanceProtectedPaths()` auto-adds govern.json, .sig sidecar, and `~/.naab/trusted-keys` to `blocked_paths` after config load. Survives mid-run reload (re-applied before ratchet check, so removal fails ratchet). Skips for inline configs (`govern_json_dir_` empty).
- **Temporal coupling enforcement**: `checkTemporalCoupling()` uses `enforce()` with configurable `level` field (default ADVISORY). Config: `temporal_coupling.level` in govern.json. HARD level throws GovernanceHardError (propagates to main.cpp _exit(3)).

### Enterprise Readiness
- **Polyglot reload**: govern.json capability changes hot-reload polyglot executor configs mid-run.
- **Telemetry forwarding**: webhook and SIEM forwarding of JSONL telemetry events. Config: `telemetry.forwarding` with `webhook_url`, `siem_url`, `batch_size`, `flush_interval_ms`.
- **REST API multi-key auth**: `api.auth` section with `keys` array, each key has `id`, `key` (or `key_env`), `permissions` (array of `"read"`, `"write"`, `"admin"`). Scoped access control for the REST governance API.
- **govern.json extends/inheritance**: `"extends": "./path/to/parent.json"` loads parent config. Child overrides parent. Array fields use `meta.inheritance.merge_arrays`: `"replace"` (default) or `"append"` (dedup concat). Max depth: `meta.inheritance.max_depth` (default 5). Parent must pass signature verification.

### Polyglot Execution
- `src/runtime/*_executor.cpp` — 12 language executors (Python, JS, Go, Rust, C++, C#, Nim, Shell, Ruby, PHP, Julia, Zig)
- `src/runtime/language_registry.cpp` — executor registration
- `<<python ... >>` syntax — `>>` must be at line start to close block
- Executor base: `executeWithReturn()`/`callFunction()` use NaabVal
- **Subprocess containment** (`src/runtime/subprocess_helpers.h/cpp`): `SubprocessContainment` struct applied to all polyglot child processes via `execute_subprocess_with_pipes()`. 5-layer defense: RLIMIT_NPROC=0 (blocks fork/subprocess), PATH restriction, env scrubbing, timeout (SIGKILL), allow_exec/allow_fork flags. `SubprocessContainment::fromCurrentSandbox()` reads current sandbox level: `standard` → fork+exec blocked, `elevated` → fork allowed, `unrestricted` → no restrictions. Pre-execution source scanning (governance checks) catches static patterns; containment catches runtime-constructed commands that evade static analysis.

### Stdlib Notable
- `array.sort()` mutates in place, `array.sorted()` returns a new sorted array (non-mutating)
- Both support optional comparator: `arr.sorted(fn(a, b) { return a - b })`
- `sorted()` is registered in `hasFunction()` but NOT in `isMutatingFunction()`
- VM has inline dot-notation for `sorted` in `callBuiltinMethod()`; tree-walker uses module path `array.sorted(arr)`
- Agent `agent.send()` auto-strips markdown code fences (` ```json ... ``` `) from LLM responses before returning content
- Agent resilience: `api_key_env`/`model` accept string or array (key rotation, model fallback). `retry` block configures backoff+jitter. `agent_dispatch.hard_stop` sets run-level budgets
- Per-agent `api_base` (govern.json agents block) overrides the provider endpoint base URL — https only, except loopback `http://127.0.0.1|localhost|[::1]` (test stubs, see `tests/helpers/agent_stub.py`). Any mid-run change = ratchet violation.
- `agent.propose(handle, msg [, n])` / `agent.commit(handle, proposal)` — candidate generation without state commit + single-candidate commit (see Propose/commit above). Gated by per-agent `propose_candidates_max` (default 0 = disabled).
- `orchestra.select_admissible(candidates [, spec])` — pure ranking over admissible candidates; accepts the propose result dict or its candidates list.
- `agent.key_health(name)` returns `{available, active, dead}` — key rotation status
- `agent.dispatch_status()` returns run-level counters — calls, tokens, time, hard stop status
- `agent.send()` responses include `trace` dict — model, provider, api_key_env, attempts, latency_ms, fallback_used
- `agent.usage()` includes `retries`, `fallbacks`, `total_latency_ms`
- `agent.environment(handle)` returns current environment snapshot — config limits, remaining capacity, coherence state, key health, dispatch proximity
- `agent.create()` handle includes `environment` dict (birth snapshot); `agent.send()` response includes updated `environment` dict (live state)
- Pipeline upstream provenance: downstream stages see `state.upstream_provenance` dict with trust-calibration signals from upstream (model_used, was_fallback, retries, coherence_at_output, keys_dead, keys_active, latency_ms, stage index, pressure). Not present for first stage or non-pipeline agents.
- Agent environment includes `challenges_passed`/`challenges_failed` counters (from step-up challenges)
- Agent handles include `__nonce` field (HMAC anti-forge — do not modify or copy between handles)
- **Agent tool execution**: `agent.register_tool(name, function, schema)` registers NAAb functions as LLM-callable tools. Governed by 7 defense layers (declaration, admission, argument scan, scoped sandbox, result scan, budget check, behavioral). Config: `tools_enabled` (master switch, default false), `tools` (allowlist), `max_tool_calls_per_turn`, `max_tool_loop_turns`, `tool_result_max_chars`, `tool_result_max_total_chars`, `tool_timeout_seconds`. Dual-gate: tool must be in govern.json `tools[]` AND registered via `register_tool()`. `TOOL_EXEC` in `allowed_actions` required if action matrix is non-empty.
- `agent.send()` tool fields: `tool_calls_made`, `tool_loop_turns`, `tool_results` (array of `{name, success, latency_ms}`), `tool_budget_remaining`, `tool_loop_exit_reason`
- `agent.usage()` tool fields: `tool_calls_total`, `tool_calls_blocked`, `tool_total_latency_ms`
- `agent.environment()` tool fields: `permissions.tools_enabled`, `permissions.tools_registered`, `permissions.tools_available`, `state.tool_calls_total`, `state.tool_calls_blocked`, `state.tool_total_latency_ms`
- BSD event types: `TOOL_CALL`, `TOOL_RESULT`, `TOOL_ERROR`, `TOOL_BLOCKED`. Default patterns include `tool_data_exfil`, `tool_env_harvest`, `tool_shell_escape`, `tool_rapid_fire`
- Dashboard: `Tools: N calls (N blocked, Nms)` when tool execution occurred
- Tool config fields are ratchet-enforced (can only tighten mid-run, never loosen)
- **`agent.extract_code(response, lang)`** — extract code from markdown fences. Searches for `` ```lang `` or `` ``` `` fences, prefers matching `lang` hint, returns longest block if multiple found. Strips surrounding conversational text. Returns input unchanged if no fence found. More powerful than the auto-strip applied by `agent.send()`.
- **Codegen module** (`src/stdlib/codegen_impl.cpp`): `codegen.run(lang, code)` — governed dynamic code execution. Routes runtime-generated code through the same 39+ governance checks as static polyglot blocks. `codegen.run_with_args(lang, code, args)` — same with variable bindings. `codegen.run_strict(lang, code, args)` — throws `std::runtime_error` on non-zero exit code (catchable by NAAb `try/catch`). `codegen.supported_languages()` — list available languages. `codegen.is_enabled()` — check if codegen is enabled in govern.json. Config: `codegen` section in govern.json with per-call limits, cumulative limits, taint policy, nesting prevention.
- **Orchestra module** (`src/stdlib/orchestra_impl.cpp`): multi-agent workflow building blocks.
  - `orchestra.sequential_refinement(handles, prompt [, iterations])` — returns a plan dict `{pattern, handles, prompt, iterations, description}`. NAAb code uses the plan to drive `agent.send()` loops.
  - `orchestra.consensus_vote(votes_dict)` — takes `{votes: ["APPROVED", ...]}`, tallies verdicts, returns `{verdict, majority, approved, rejected, review, total}`.
  - `orchestra.enforce_convergence(response, spec)` — validates response against spec (regex `pattern` or `required_fields`). Returns `{valid, error_message}`. No retry loop — caller implements retries.
- **Governance module** (`src/stdlib/governance_impl.cpp`): `governance.health()` — returns pulse verdict and instrumentation status without API call.

### Scanner Code Quality Checks (19 checks in `checks_code_quality.cpp`)
- Baseline checks (16): empty_catch, catch_and_ignore, magic_numbers, magic_strings, dead_code_after_return, dead_conditional, dead_conditional_dataflow, god_functions, god_classes, deep_nesting, boolean_function_returns, complex_boolean_expr, long_parameter_list, mutable_global_state, recursive_no_base_case, string_concat_in_loop
- `null_coalesce_non_nullable` — flags `??` on comparisons/bool literals that can never be null
- `assigned_never_read` — detects `let`-assigned variables never referenced after declaration, with special `sort()` mutation hint
- `hash_sanitize_mismatch` — one-level data flow tracing: finds `crypto.sha256(X)`, traces X back through `let` assignments, checks if any component variable is also passed to `sanitize_*()`/`validate_*()`
- Pattern: guard on `isEnabled(CAT, "check_name")`, use `findFuncEnd()` for function scope, `addIssue()` to report

### Telemetry
- `GovernanceEngine::writeTelemetry()` in `governance_reports.cpp` writes JSONL events
- Each event includes `run_id` (timestamp-pid, generated once in `loadFromFile()`) for separating runs in shared output files
- Agent telemetry event types (35): `ADMISSION_EVAL`, `AGENT_CHALLENGE_FAIL`, `AGENT_CHALLENGE_PASS`, `AGENT_FALLBACK`, `AGENT_HARD_STOP`, `AGENT_KEY_DISABLED`, `AGENT_KEY_REVIVED`, `AGENT_PROPOSE`, `AGENT_PROPOSAL_COMMIT`, `AGENT_RESPONSE`, `AGENT_RETRY`, `AGENT_TOOL_BLOCKED`, `AGENT_TOOL_CALL`, `AGENT_TOOL_LOOP_END`, `AGENT_TOOL_LOOP_START`, `AGENT_TOOL_REGISTERED`, `AGENT_TOOL_RESULT`, `AGENT_TOOL_SCAN_HIT`, `BSD_MATCH`, `CDD_TURN`, `CODEGEN_EXEC`, `CONFIG_ADJUSTMENT`, `CONTRACT_VIOLATION`, `GOVERNANCE_HEALTH_WARNING`, `GOVERNANCE_LEVEL_CHANGE`, `MANDATE_INJECTION`, `OUTPUT_ADMISSIBILITY_EVAL`, `OUTPUT_INADMISSIBLE`, `POLYGLOT_EXEC`, `PROMPT_SCAN`, `RECONCILIATION_TURN`, `RESPONSE_SCAN`, `RESPONSE_SUPPRESSED`, `RESPONSE_TRUNCATED`, `SEMANTIC_TURN`
- Telemetry forwarding: `telemetry.forwarding` config enables webhook/SIEM push of events
- Tamper-evident hash chain: each telemetry event includes `prev_hash` linking to previous event, creating an immutable audit trail

### Agent Interaction Transcript
- Opt-in JSONL log capturing the full lifecycle of every `agent.create()` and `agent.send()` call with complete content visibility — prompts, responses, governance scan details, CDD signals, retry attempts, tool args/results, challenges, errors
- **Separate from telemetry**: NOT mixed into the tamper-evident hash chain. This is an audit/debug log, not a tamper-evident chain
- Config: `telemetry.transcript` section in govern.json — `enabled` (bool), `output_file` (JSONL path), `agents` (array — empty = all agents, `["name"]` = filter)
- `writeAgentTranscript()` in `governance_reports.cpp` — same fopen/flock/fwrite pattern as telemetry but simpler (no hash chain, no webhook)
- `isTranscriptAgent()` — returns false if disabled, true if agents array empty, otherwise checks name membership
- 22 hook points in `agentSend()` (`agent_impl.cpp`), gated by `transcript_active` flag — zero overhead when disabled
- Two entry types: `agent_create` (handle_id, agent, config snapshot) and `agent_send` (turn, messages, api_params, attempts, raw_response, fence_stripped, tool_loop, response_scan, cdd, governance state, processed_response, error flag)
- Error path: `agentSend()` wrapped in try/catch — GovernanceHardError caught before re-throw, partial transcript written with `error: true` and `error_message`
- Windows: all `localtime_r` calls use `#ifdef _WIN32` / `localtime_s` guards (3 sites in agent_impl.cpp)
- Test: `tests/governance_v4/test_transcript.sh` — 28 assertions (disabled-by-default, transcript writing, agent name filtering, field depth, JSON validity)

## Conventions

### Error Messages
```cpp
throw std::runtime_error(
    "Category error: What went wrong\n\n"
    "  Got: <actual>\n  Expected: <expected>\n\n"
    "  Help:\n  - Explanation\n\n"
    "  Example:\n    x Wrong: bad_code\n    v Right: good_code\n"
);
```

**Security rule:** Error messages must NEVER leak bypass flags, sanitizer lists, config key paths, or governance internals. `tests/security/test_error_msg_leaks.sh` enforces this — run it after modifying any error text.

### govern.json is Primary
All settings belong in govern.json first. CLI flags are overrides only. Never add behavior that requires a CLI flag without a govern.json equivalent.

### Adding a New Stdlib Function
1. Add implementation in `src/stdlib/<module>_impl.cpp`
2. Register in the module's init function
3. Add error messages following the `"  Help:\n  - ..."` format
4. Run `bash run-all-tests.sh` to verify

### Adding a New AST Node
1. Add to `NodeKind` enum in `include/naab/ast.h`
2. Create class extending `Expr`/`Stmt` in `ast.h`, add `accept()` in `ast_nodes.cpp`
3. Add `visit()` to `ASTVisitor` in `ast.h`, declare in `interpreter.h`
4. Implement visitor in `interpreter.cpp` (use NaabVal for result_)
5. Add compiler support in `src/vm/compiler.cpp` + VM opcode in `src/vm/vm.cpp`
6. Add parser rule in `src/parser/parser.cpp`, hook into appropriate parse method

### Adding a Governance Check
1. Add check function in `src/runtime/governance_checks.cpp` — call `clearTrace()` as first statement (or right after the enabled guard); call `addTrace()` for key decisions
2. Wire into governance engine dispatch
3. Add config key to governance_config.cpp — include `parseRationale()` call
4. Add `std::string rationale;` to the config struct in `governance.h`
5. Add rule_name mapping in `lookupRationale()` in `governance_engine.cpp`
6. Run security test after: `bash tests/security/test_error_msg_leaks.sh`
7. Never include bypass instructions in the error message

### Modifying Error Messages
Always run `bash tests/security/test_error_msg_leaks.sh` after changing any error text. The test scans all error strings for leaked bypass flags like `--no-governance`, `--governance-override`, sanitizer function names, etc.

## CLI Flags (Key Subset)

| Flag | Purpose |
|------|---------|
| `--tree-walk` | Use tree-walker instead of VM |
| `--governance-dashboard` | Print governance summary to stderr |
| `--governance-report` | Detailed governance report |
| `--governance-sarif` | SARIF format output |
| `--no-governance` | Skip governance (dev only) |
| `--gc-threshold N` | GC allocation threshold |
| `--gc-stats` | Print GC statistics |
| `--keygen PATH` | Generate Ed25519 signing keypair |
| `--sign-governance` | Sign govern.json |
| `--sign-baseline` | Sign drift baseline |
| `--trust-key PATH` | Install a public key |
| `--list-keys` | List trusted key fingerprints |
| `--env NAME` | Select governance environment |
| `--timeout N` | Execution timeout (seconds) |

## Gotchas

- **nlohmann/json.hpp** — keep in .cpp only, never in headers
- **`result_`** is NaabVal — use `.isNull()` not `if (result_)`
- **CLI flags must be in BOTH** global pre-scan AND run command flag loop in main.cpp
- **VM taint** mirrors interpreter taint but uses `taint_stack_` — changes must be made in both paths
- **`current_file_`** not `filename_` for the current source file
- **No Julia/Zig on Termux** — tests skip gracefully
- **Polyglot `>>` delimiter** must be at line start (after optional whitespace)
- **`t_current_decision_trace`** is `static thread_local` in governance_engine.cpp — not a class member in the header
- **Bare dict keys** — `{name: "val"}` is equivalent to `{"name": "val"}`. Unquoted keys are syntactic sugar.
- **`??` in match arms** — null coalescing works inside match arm bodies: `"x" => d.get("a") ?? "default"`
- **Top-level `const`/`let` parse error** — NAAb only allows `use`, `import`, `export`, `struct`, `enum`, `fn`, and `main {}` at file scope. Constants and variables MUST be declared inside `main {}` or a function.
- **Sandbox fail-closed on `mode: enforce`** — if govern.json has `"mode": "enforce"` but no `security.sandbox_level`, the runtime silently upgrades the sandbox from `unrestricted` → `standard`. This blocks `file.read("/absolute/path")` and other absolute-path operations. Set `"security": { "sandbox_level": "elevated" }` to restore access. The runtime logs `[governance] Sandbox: upgraded unrestricted → standard` to stderr when this happens.
- **`array.sort()` mutates, `array.sorted()` does not** — `sort()` is in `isMutatingFunction()`, `sorted()` is not. Both live in `array_impl.cpp` and VM's `callBuiltinMethod()`.
- **Agent responses auto-strip markdown fences** — `agent.send()` strips ` ```json ... ``` ` wrapping when the entire response is a single fenced block. Only strips when fence is at start/end (with optional whitespace). Does not strip partial fences or multiple fence blocks.
- **Blocked turns never enter history (split commit)** — CDD/OA-blocked `agent.send()` turns (including catchable DETECT-level blocks) do NOT append to the handle's message history; the tracker/exposure accounting still commits. Quarantine/attest disposition is controlled by `output_admissibility.inadmissible_history`.
- **GovernanceHardError catch-and-rethrow required** — any new `catch (const std::exception&)` or `catch (const std::runtime_error&)` in interpreter.cpp or vm.cpp that could intercept governance exceptions MUST have a preceding `catch (const governance::GovernanceHardError&) { throw; }`. Without this, HARD governance violations become catchable by NAAb try/catch.
