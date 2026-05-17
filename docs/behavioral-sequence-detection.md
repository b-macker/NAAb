# Behavioral Sequence Detection & Context Drift Detection

## Investigation Summary

Comprehensive investigation of the NAAb governance runtime to determine where
a Behavioral Sequence Detection (BSD) and LLM Context Drift Detection (CDD)
subsystem would integrate.

---

## What We Currently Have

| Mechanism | Scope | Temporal? | Limitation |
|-----------|-------|-----------|------------|
| **Co-occurrence detection** | Single code block | No — spatial only | Only checks if 4 signals are in the *same* code |
| **Cumulative risk scoring** | Per-execution | Partially — accumulates over time | No ordering, no decay, no pattern matching |
| **Agent turn tracking** | Per-conversation | Yes — counts turns/tokens | Only checks limits, doesn't analyze *what* happened |
| **Taint lineage** | Per-value | Yes — source-to-sink flow | Single variable flow, not cross-variable choreography |
| **Drift detection (21 gates)** | Per-execution vs baseline | Snapshot comparison | Point-in-time, not behavioral over time |
| **Agent review** | Per-source file | No — reviews static code | Doesn't watch runtime behavior across turns |
| **Telemetry JSONL** | Append-only log | Records events | **Write-only** — nothing reads it back for analysis |
| **Audit chain (HMAC)** | Append-only log | Tamper-evident | Integrity proof only, no pattern detection |

**The gap:** Nothing currently says "event A happened, then B, then C — that
sequence is dangerous." The scoring adds weights but doesn't care about
**order**. The co-occurrence checks spatial proximity but not **temporal**
proximity.

---

## What's Needed: Two New Subsystems

### 1. Behavioral Sequence Detection (BSD)

Detects dangerous **chains of runtime actions** across turns/checks:

```
env.get("AWS_KEY") -> base64_encode() -> http.post()  = exfiltration chain
file.read("/etc/shadow") -> string.split() -> agent.send()  = data leak chain
disable_logging() -> modify_binary() -> enable_logging()  = cover-tracks chain
```

### 2. LLM Context Drift Detection (CDD)

Detects when an agent **loses coherence** over a long conversation:

```
Turn 1-5: focused on "implement auth middleware"
Turn 6-8: starts refactoring unrelated modules
Turn 9-12: contradicts its own earlier decisions
Turn 13+: generates code that conflicts with established patterns
```

---

## Existing Architecture (Key Components)

### Event Sources

| Source | File | Line | Events |
|--------|------|------|--------|
| enforce() | governance_engine.cpp | 503-615 | Every check pass/fail with CheckResult |
| callStdlibMethod() | vm.cpp | ~1720 | env.get, file.read, http.post |
| Shell interpolation | vm.cpp | taint sink | shell_exec with bound vars |
| agentSend() | agent_impl.cpp | ~282 | agent_turn, agent_response |
| Sandbox interception | main.cpp | 122-149 | net_connect, fs_write |
| reloadIfChanged() | governance_engine.cpp | 1470 | config_tightened |

### CheckResult Structure (governance.h:1586-1604)

Every governance check produces:
- `rule_name`: e.g., "code_quality.no_secrets"
- `level`: EnforcementLevel (HARD, SOFT, ADVISORY, APPROVAL_REQUIRED)
- `passed`: Boolean
- `decision_trace`: vector<string> — sequential steps to verdict
- `rationale`: Why this rule is at this tier
- `category`, `severity`, `cwe_ids`, `owasp_ids`
- `file`, `line`: Source location

### Cumulative Scoring (governance_engine.cpp:535-566)

- `cumulative_score_`: Monotonic accumulator (never decreases)
- `score_contributions_`: Per-rule tracking
- Zones: GREEN (< yellow), YELLOW (< red), RED (>= red)
- Only ADVISORY findings accumulate
- Saturates at 100,000

### Agent Turn Tracking (agent_impl.cpp:39-43)

```cpp
struct AgentTracker {
    int turns = 0;
    int input_tokens = 0;
    int output_tokens = 0;
};
static std::unordered_map<int, AgentTracker> s_trackers;
```

- Server-side (immune to handle mutation)
- Enforces max_turns and max_total_tokens
- Conversation history stored in handle["messages"]

### Telemetry JSONL (governance_reports.cpp:643-710)

Per-event fields:
- agent_id, event_type, rule_name, result, message
- timestamp (ISO 8601), file, line
- category, severity, level, cwe[], owasp[]

Append-only with exclusive lock. Currently **write-only** — no reader/analyzer.

### Agent Review (agent_review.cpp)

Multi-phase:
1. Detection: N agents emit FINDING|category|description
2. Validation: 1 agent filters confirmed vs false positive
3. Scoring: accumulate weighted findings into zones
4. Voice: synthesize remediation guide

Checks intent mismatches, evasion, cosmetic sanitizers, but only on **static code**.

---

## Integration Design

### Architecture Diagram

```
+-------------------------------------------------------------+
|                    govern.json                                |
|  "behavioral_sequences": { patterns: [...], window: 50 }    |
|  "context_drift": { coherence_threshold: 0.7, ... }         |
+---------------------------+---------------------------------+
                            | parsed by
                            v
+-------------------------------------------------------------+
|              governance_config.cpp                            |
|  parseBehavioralSequences() -> rules_.behavioral_sequences   |
|  parseContextDrift()        -> rules_.context_drift          |
+---------------------------+---------------------------------+
                            | used by
                            v
+-------------------------------------------------------------+
|              governance_engine.cpp (NEW)                      |
|                                                              |
|  +----------------------------------+                       |
|  |  EventRingBuffer (deque<Event>)  | <-- all interceptions |
|  |  - shell_exec, env_read, net_*   |     feed into this    |
|  |  - file_read, file_write         |                       |
|  |  - agent_send, agent_response    |                       |
|  |  - taint_violation, check_pass   |                       |
|  +----------------+-----------------+                       |
|                   |                                          |
|    +--------------v--------------+  +---------------------+ |
|    |  SequencePatternMatcher     |  |  DriftAnalyzer      | |
|    |  - FSM per pattern          |  |  - intent vector    | |
|    |  - window/gap checking      |  |  - coherence score  | |
|    |  - fires on completion      |  |  - contradiction log| |
|    +--------------+--------------+  +----------+----------+ |
|                   |                            |             |
|                   v                            v             |
|    enforce("behavioral_sequence.*")  enforce("drift.*")     |
|         -> cumulative_score_              -> cumulative_score|
|         -> decision_trace                 -> decision_trace  |
|         -> SARIF/JSON/dashboard           -> governance_notes|
+-------------------------------------------------------------+
```

### Hook Points (Where Events Enter the Ring Buffer)

| Location | Events | File:Line |
|----------|--------|-----------|
| callStdlibMethod() | env.get, file.read, http.post | vm.cpp:~1720 |
| Shell interpolation check | shell_exec with vars | vm.cpp:taint sink |
| agentSend() | agent_turn, agent_response | agent_impl.cpp:~282 |
| enforce() | any check pass/fail | governance_engine.cpp:503 |
| Sandbox interception | net_connect, fs_write | main.cpp:122-149 |
| reloadIfChanged() | config_tightened | governance_engine.cpp:1470 |

---

## Proposed govern.json Schema

```json
"behavioral_sequences": {
    "enabled": true,
    "level": "soft",
    "window_size": 50,
    "decay_turns": 20,
    "patterns": [
        {
            "name": "data_exfiltration",
            "sequence": ["env.get|file.read", "encode|base64|compress", "http.post|agent.send"],
            "max_gap": 10,
            "level": "hard"
        },
        {
            "name": "credential_harvest",
            "sequence": ["env.get:*KEY*|env.get:*SECRET*", "string.concat|+", "file.write|http.post"],
            "max_gap": 5,
            "level": "hard"
        },
        {
            "name": "reconnaissance_escalation",
            "sequence": ["file.read:/etc/*", "process.exec", "net.connect"],
            "max_gap": 15,
            "level": "soft"
        }
    ]
},
"context_drift": {
    "enabled": true,
    "level": "advisory",
    "coherence_threshold": 0.6,
    "max_contradictions": 3,
    "check_interval_turns": 5,
    "signals": {
        "intent_divergence": true,
        "repeated_failures": true,
        "circular_actions": true,
        "scope_creep": true
    }
}
```

---

## Key Data Structures (Proposed)

### Event Ring Buffer

```cpp
enum class RuntimeEventType {
    ENV_READ, ENV_WRITE,
    FILE_READ, FILE_WRITE,
    NET_CONNECT, NET_SEND,
    SHELL_EXEC,
    AGENT_SEND, AGENT_RESPONSE,
    TAINT_VIOLATION, TAINT_SANITIZED,
    CHECK_PASS, CHECK_FAIL,
    ENCODE, DECODE,
    CONFIG_RELOAD
};

struct RuntimeEvent {
    RuntimeEventType type;
    std::string detail;         // e.g., "env.get('AWS_KEY')", "file.read('/etc/shadow')"
    std::string file;
    int line = 0;
    int turn = 0;              // agent turn number (0 if not in agent context)
    int64_t timestamp_ns;      // monotonic clock
    size_t sequence_id;        // global monotonic counter
};

// In GovernanceEngine:
static constexpr size_t MAX_EVENT_BUFFER = 200;
std::deque<RuntimeEvent> event_buffer_;
size_t event_sequence_counter_ = 0;
```

### Sequence Pattern (FSM)

```cpp
struct SequenceStep {
    std::vector<std::string> match_any;  // OR-list: ["env.get", "file.read"]
    std::string glob_filter;             // optional: "*KEY*", "/etc/*"
};

struct SequencePattern {
    std::string name;                    // "data_exfiltration"
    std::vector<SequenceStep> steps;     // ordered sequence
    int max_gap = 10;                    // max events between consecutive matches
    int decay_turns = 20;               // events older than N turns don't match
    EnforcementLevel level = EnforcementLevel::SOFT;
    std::string rationale;
};

struct PatternMatchState {
    size_t current_step = 0;             // how far along the FSM
    size_t last_match_seq_id = 0;        // sequence_id of last matched event
    int last_match_turn = 0;            // turn of last matched event
    std::vector<RuntimeEvent> matched;   // events that matched each step
};

// Per-pattern, per-execution:
std::unordered_map<std::string, PatternMatchState> pattern_states_;
```

### Context Drift Analyzer

```cpp
struct DriftState {
    // Intent tracking
    std::vector<std::string> declared_intents;   // from govern.json
    std::vector<std::string> observed_actions;   // what the agent actually did

    // Coherence metrics
    int contradictions = 0;              // times agent contradicted itself
    int repeated_failures = 0;           // same error hit N times
    int scope_creep_count = 0;           // actions outside declared intent
    int circular_action_count = 0;       // A->B->A->B loops

    // Per-turn action fingerprint (for detecting loops)
    std::deque<std::string> turn_fingerprints;   // hash of actions per turn

    // Scoring
    double coherence_score = 1.0;        // starts at 1.0, decays on drift signals
};
```

---

## Detection Algorithm

### Behavioral Sequence Matching

```
On each new RuntimeEvent:
  1. Append to event_buffer_ (evict oldest if > MAX_EVENT_BUFFER)
  2. For each SequencePattern P:
     a. Get or create PatternMatchState for P
     b. If state.current_step == 0:
        - Check if event matches P.steps[0]
        - If yes: advance state, record match
     c. If state.current_step > 0:
        - Check decay: if (event.turn - state.last_match_turn) > P.decay_turns:
          Reset state to step 0
        - Check gap: if (event.sequence_id - state.last_match_seq_id) > P.max_gap:
          Reset state to step 0 (but re-check step 0 match)
        - Check if event matches P.steps[state.current_step]:
          If yes: advance state, record match
     d. If state.current_step == P.steps.size():
        - PATTERN COMPLETE! Fire enforcement.
        - emit CheckResult with decision_trace showing full chain
        - Reset state
```

### Context Drift Detection

```
On each agent.send() response:
  1. Compute turn_fingerprint = hash(actions_this_turn)
  2. Check circular: if fingerprint matches recent N turns -> circular_action_count++
  3. Check scope: if actions don't match any declared_intent -> scope_creep_count++
  4. Check contradiction: if this turn undoes a previous turn's work -> contradictions++
  5. Check repeated_failures: if same error string seen > N times -> repeated_failures++
  6. Update coherence_score:
     score -= 0.1 * circular_actions_this_turn
     score -= 0.15 * scope_creep_this_turn
     score -= 0.2 * contradictions_this_turn
     score = max(0.0, score)
  7. If score < coherence_threshold:
     - Fire advisory/soft enforcement
     - Include in governance_notices to agent
     - decision_trace: ["coherence dropped to 0.4", "3 circular actions detected", ...]
```

---

## Implementation Order

1. **EventRingBuffer** — the event infrastructure (deque + emit points)
2. **SequencePatternMatcher** — FSM-based pattern detection
3. **Config parsing** — govern.json schema for patterns
4. **Integration with enforce()** — fire CheckResults on match
5. **Context Drift signals** — circular detection, scope creep
6. **DriftAnalyzer** — coherence scoring across turns
7. **Governance notices** — feed back to agent via agent.send() response

---

## Files to Create/Modify

| File | Action | Purpose |
|------|--------|---------|
| `include/naab/behavioral_sequence.h` | CREATE | Event types, ring buffer, pattern matcher, drift analyzer structs |
| `src/runtime/behavioral_sequence.cpp` | CREATE | Implementation of BSD + CDD |
| `include/naab/governance.h` | MODIFY | Add BehavioralSequenceConfig, ContextDriftConfig to GovernanceRules |
| `src/runtime/governance_config.cpp` | MODIFY | Parse new sections from govern.json |
| `src/runtime/governance_engine.cpp` | MODIFY | Add emitEvent() calls, wire check dispatch |
| `src/vm/vm.cpp` | MODIFY | Emit events at interception points |
| `src/stdlib/agent_impl.cpp` | MODIFY | Emit agent events, drift check on response |
| `docs/govern-template.json` | MODIFY | Add behavioral_sequences and context_drift sections |
| `tests/governance_v4/behavioral_sequence/` | CREATE | Test suite |

---

## Key Architectural Decisions

1. **Ring buffer, not unbounded log** — memory-safe with configurable window
2. **Decay/expiry** — events older than N turns stop matching (prevents stale false positives)
3. **Pattern = FSM** — each defined sequence becomes a finite state machine; partial matches track progress
4. **Feeds into existing scoring** — matched sequences get weighted and accumulate like any other advisory
5. **Reports auto-consume** — CheckResult with decision_trace flows to all 5 report formats for free
6. **Agent-aware** — drift detection hooks into agent.send() where conversation history is available
7. **Zero-cost when disabled** — guard on `!rules_.behavioral_sequences.enabled` at emit points
8. **Thread-safe** — ring buffer protected by existing results_mutex_ or new dedicated mutex
