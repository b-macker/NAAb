# Agent Governance Framework — Full Specification

You are building a **real NAAb module** — an agent governance framework that provides
telemetry, analytics, adaptive rule learning, role-based access control, and intelligent
language assignment for AI coding agents.

Read `CLAUDE.md` in this directory for the complete NAAb language reference. Read
`govern.json` for the governance rules that will enforce quality on YOUR generated code.

## What You Generate

8 `.naab` files:
1. `models.naab` — Shared data structures (enums, creators, validators)
2. `telemetry.naab` — Event collector + JSON logger
3. `dashboard.naab` — Report generator (reads logs, outputs summaries)
4. `adaptive.naab` — Pattern analyzer + rule proposal engine
5. `roles.naab` — Agent role enforcement
6. `language_scorer.naab` — Task-to-language assignment
7. `config.naab` — Config loader (reads govern.json agent_roles section)
8. `main.naab` — Test orchestrator (80 tests across 8 suites)

## Architecture

```
main.naab (test orchestrator)
  ├── imports models.naab
  ├── imports telemetry.naab
  ├── imports dashboard.naab
  ├── imports adaptive.naab
  ├── imports roles.naab
  ├── imports language_scorer.naab
  └── imports config.naab

telemetry.naab → imports models.naab
dashboard.naab → imports models.naab
adaptive.naab → imports models.naab
roles.naab → imports models.naab
language_scorer.naab → imports models.naab
config.naab → (no imports, reads JSON files)
```

## File-by-File Specification

---

## 1. models.naab — Shared Data Structures

### Enums (5)

```naab
enum EventType {
    GovernanceCheck,
    HelperTriggered,
    ExecutionComplete,
    RuleViolation,
    LanguageAssignment
}

enum Severity {
    Low,
    Medium,
    High,
    Critical
}

enum Enforcement {
    Hard,
    Soft,
    Advisory
}

enum AgentRole {
    Frontend,
    Backend,
    DataScience,
    DevOps,
    Security,
    Admin
}

enum CheckResult {
    Pass,
    Block,
    Warn
}
```

### Functions (10)

#### `create_event(agent_id, event_type, language, rule_name, result, message, timestamp)`
Creates a governance event record.
- Returns dict with keys: `agent_id`, `event_type`, `language`, `rule_name`, `result`, `message`, `timestamp`, `session_id`
- `session_id` defaults to empty string `""`
- All parameters stored as-is in the dict

#### `create_agent(id, name, role, allowed_paths, blocked_paths, allowed_languages)`
Creates an agent identity record.
- Returns dict with keys: `id`, `name`, `role`, `allowed_paths`, `blocked_paths`, `allowed_languages`
- `allowed_paths` and `blocked_paths` are arrays of path strings
- `allowed_languages` is an array of language name strings

#### `create_rule_proposal(pattern, proposed_rule, confidence, evidence_count, source_events)`
Creates a rule proposal from pattern analysis.
- Returns dict with keys: `pattern`, `proposed_rule`, `confidence`, `evidence_count`, `source_events`
- `confidence` is a float 0.0-1.0
- `source_events` is an array of event dicts

#### `create_score_result(language, score, task_type, reasons)`
Creates a language scoring result.
- Returns dict with keys: `language`, `score`, `task_type`, `reasons`
- `score` is an integer 0-100
- `reasons` is an array of strings

#### `create_session(id, agent_id, start_time)`
Creates a telemetry session.
- Returns dict with keys: `id`, `agent_id`, `start_time`, `events`, `status`
- `events` initialized to empty array `[]`
- `status` initialized to `"active"`

#### `validate_event(event)`
Validates an event dict has all required fields.
- Returns dict: `{valid, errors}`
- Required fields: `agent_id`, `event_type`, `language`, `rule_name`, `result`, `message`, `timestamp`
- `errors` is an array of strings describing missing/invalid fields
- `valid` is true only when errors is empty

#### `validate_agent(agent)`
Validates an agent dict has all required fields and valid role.
- Returns dict: `{valid, errors}`
- Required fields: `id`, `name`, `role`, `allowed_paths`, `blocked_paths`, `allowed_languages`
- Check that `role` is one of the valid AgentRole enum values
- `errors` is an array of strings

#### `event_type_name(event_type)`
Returns a human-readable string for an EventType enum value.
- `EventType.GovernanceCheck` → `"GovernanceCheck"`
- `EventType.HelperTriggered` → `"HelperTriggered"`
- `EventType.ExecutionComplete` → `"ExecutionComplete"`
- `EventType.RuleViolation` → `"RuleViolation"`
- `EventType.LanguageAssignment` → `"LanguageAssignment"`

#### `severity_name(severity)`
Returns a human-readable string for a Severity enum value.
- `Severity.Low` → `"Low"`, `Severity.Medium` → `"Medium"`, etc.

#### `role_name(role)`
Returns a human-readable string for an AgentRole enum value.
- `AgentRole.Frontend` → `"Frontend"`, `AgentRole.Backend` → `"Backend"`, etc.

---

## 2. telemetry.naab — Event Collection & Logging

**Imports:** `use json`, `use file`, `use time`, `import "./models.naab" as models`

### Functions (8)

#### `init_telemetry(output_dir)`
Initialize a telemetry collector.
- Returns `{collector, success}`
- `collector` is a dict: `{output_dir, events: [], session_count: 0}`
- `success` is `true`
- If output_dir doesn't exist, try to note it (but don't fail — file.write will create dirs)

#### `record_event(collector, event)`
Append a validated event to the collector.
- First call `models.validate_event(event)` — if invalid, return collector unchanged
- Append event to collector's events array
- Return the updated collector (value semantics — re-assign!)
- Remember: `collector.get("events")` then push, then `collector["events"] = events`

#### `record_governance_check(collector, agent_id, language, rule_name, passed, message)`
Convenience function that creates and records a GovernanceCheck event.
- `result` = `"pass"` if passed is true, `"block"` otherwise
- Creates event via `models.create_event(agent_id, models.EventType.GovernanceCheck, language, rule_name, result, message, time.now())`
- Records it via `record_event(collector, event)`
- Returns updated collector

#### `record_helper_triggered(collector, agent_id, helper_name, language, original_error)`
Convenience function that creates and records a HelperTriggered event.
- Creates event with event_type = `models.EventType.HelperTriggered`
- rule_name = helper_name, result = `"triggered"`, message = original_error
- Returns updated collector

#### `flush_events(collector, filepath)`
Write all collector events to a JSON file.
- Use `json.stringify(events)` then `file.write(filepath, json_string)`
- Returns `{success: true, events_written: len(events)}`
- If events is empty, still write `"[]"` and return events_written = 0

#### `load_events(filepath)`
Read events back from a JSON file.
- Use `file.read(filepath)` then `json.parse(content)`
- Returns the parsed array
- If file doesn't exist, return empty array

#### `get_session_events(collector, session_id)`
Filter collector events by session_id.
- Loop through events, collect those where `event.get("session_id") == session_id`
- Return filtered array

#### `get_events_by_agent(collector, agent_id)`
Filter collector events by agent_id.
- Loop through events, collect those where `event.get("agent_id") == agent_id`
- Return filtered array

---

## 3. dashboard.naab — Reporting & Analytics

**Imports:** `use json`, `use array`, `use string`, `use csv`, `use math`, `import "./models.naab" as models`

### Functions (8)

#### `generate_summary(events)`
Generate a summary report from an array of events.
- Returns dict: `{total_events, by_type, by_agent, by_language, by_result, time_range}`
- `total_events` = len(events)
- `by_type` = dict mapping event_type → count
- `by_agent` = dict mapping agent_id → count
- `by_language` = dict mapping language → count
- `by_result` = dict mapping result → count
- `time_range` = dict with `{start, end}` (first and last event timestamps, or "" if empty)
- Use loops to accumulate counts. Remember value semantics on the count dicts.

#### `top_violations(events, limit)`
Find the most common rule violations.
- Filter events to only those with result = "block" or result = "warn"
- Count occurrences of each rule_name
- Track which agent_ids were affected per rule
- Sort by count descending (use Python polyglot for sorting)
- Return array of `{rule_name, count, agents_affected}` limited to `limit` items
- `agents_affected` is the number of unique agents, not a list

#### `agent_scorecard(events, agent_id)`
Generate a scorecard for a specific agent.
- Filter events for this agent_id
- `total_checks` = count of GovernanceCheck events for this agent
- `pass_rate` = pass_count / total_checks (0.0 if total_checks == 0)
- `violations` = count of events with result "block"
- `most_common_violation` = rule_name with highest block count (or "" if none)
- `helper_effectiveness` = helpers_that_led_to_pass / total_helpers_triggered
  - A helper "led to pass" if: after a HelperTriggered event for agent+rule, a subsequent GovernanceCheck for the same agent+rule has result "pass"
  - Use Python polyglot for this correlation
- Returns `{agent_id, total_checks, pass_rate, violations, most_common_violation, helper_effectiveness}`

#### `helper_effectiveness_report(events)`
Analyze how effective each helper error message was.
- Group HelperTriggered events by helper_name (rule_name field)
- For each helper, count how many times it was triggered
- For each trigger, check if a subsequent Pass event exists for same agent+rule
- `led_to_fix` = count of triggers that were followed by a pass
- `effectiveness_rate` = led_to_fix / triggered (0.0 if triggered == 0)
- Use Python polyglot for the correlation analysis
- Returns dict: helper_name → `{triggered, led_to_fix, effectiveness_rate}`

#### `language_usage_report(events)`
Group events by language and compute per-language stats.
- For each language found in events:
  - `blocks_executed` = count of events with that language
  - `violations` = count of events with result "block" for that language
  - `pass_rate` = pass_count / total (0.0 if total == 0)
- Returns dict: language → `{blocks_executed, violations, pass_rate}`

#### `generate_text_report(events)`
Generate a formatted text report (human-readable).
- Use `generate_summary(events)` internally
- Format as a text table with headers and aligned columns
- Include sections: "Summary", "By Type", "By Language", "Top Violations"
- For top violations, use `top_violations(events, 5)`
- Return the formatted string

#### `export_report_csv(events, headers)`
Export events to CSV format.
- `headers` is an array of field names to include
- First row is headers joined by commas
- Each subsequent row extracts those fields from each event
- Use `csv.stringify` with the appropriate data structure
- Return CSV string

#### `trend_analysis(events, window_hours)`
Analyze trends over time windows.
- Bucket events by time window (window_hours apart)
- Use Python polyglot: parse timestamps, compute time deltas, bucket events
- For each bucket: compute violation count and pass rate
- `violation_trend` = "increasing", "decreasing", or "stable" based on first vs last bucket
- `pass_rate_trend` = "improving", "declining", or "stable"
- Returns `{periods, violation_trend, pass_rate_trend}`
- `periods` is an array of `{start, end, violation_count, pass_rate, event_count}`

---

## 4. adaptive.naab — Bidirectional Rule Learning

**Imports:** `use json`, `use array`, `use string`, `use math`, `import "./models.naab" as models`

### Functions (8)

#### `analyze_patterns(events, min_occurrences)`
Detect repeated violation patterns in event history.
- Pattern: same `rule_name` fails for same `agent_id` >= `min_occurrences` times
- Filter events to those with result = "block"
- Group by (agent_id, rule_name) pair
- Return array of patterns where count >= min_occurrences
- Each pattern: `{agent_id, rule_name, count, first_seen, last_seen}`
- `first_seen` and `last_seen` are timestamps from the matching events

#### `propose_rule(pattern, existing_rules)`
Generate a rule proposal based on a detected pattern.
- Use Python polyglot for statistical analysis
- `proposed_rule` = string describing the rule (e.g., "Block agent X from rule Y")
- `confidence` = calculated via `calculate_confidence()` logic
- `rationale` = explanation string based on pattern data
- `evidence_count` = pattern's count
- Returns `{proposed_rule, confidence, rationale, evidence_count}`

#### `evaluate_proposal(proposal, events)`
Simulate how a proposed rule would have affected past events.
- `would_catch` = count of past events this rule would have caught/prevented
- `false_positive_rate` = estimated rate of incorrect blocks (0.0-1.0)
  - Compare: how many events match the rule pattern but had result "pass"
- `recommendation` = "adopt" if false_positive_rate < 0.1, "review" if < 0.3, "reject" otherwise
- Returns `{would_catch, false_positive_rate, recommendation}`

#### `generate_helper_text(violation_pattern)`
Generate a suggested helper error message for a violation pattern.
- Based on the pattern's rule_name and occurrence data
- Create a helpful, actionable error message that would guide the agent
- Include: what went wrong, what to do instead, an example
- Return the helper text as a string

#### `merge_proposals(proposals)`
Deduplicate and rank an array of rule proposals.
- Two proposals are duplicates if they have the same `proposed_rule` string
- When merging duplicates: keep highest confidence, sum evidence_counts
- Sort by confidence descending
- Return merged array

#### `format_govern_json_patch(proposals)`
Convert proposals into a JSON patch for govern.json.
- Create a dict structure that could be merged into govern.json
- Each proposal becomes a rule entry under a "proposed_rules" section
- Use `json.stringify()` to return the JSON string
- Return valid JSON string

#### `get_repeat_offenders(events, threshold)`
Find agents who repeatedly violate rules.
- Count total violations (result = "block") per agent_id
- Filter to agents with violation_count >= threshold
- For each offender, find their top 3 most violated rules
- Return array of `{agent_id, violation_count, top_rules}`
- `top_rules` is an array of rule_name strings (up to 3)

#### `calculate_confidence(evidence_count, consistency, time_span)`
Calculate a confidence score for a rule proposal.
- `evidence_count` contribution: min(evidence_count / 20, 0.5) — caps at 0.5
- `consistency` contribution: consistency * 0.3 (consistency is 0.0-1.0)
- `time_span` contribution: min(time_span / 86400, 0.2) — caps at 0.2 (time_span in seconds)
- Total = sum of all three, clamped to 0.0-1.0
- Return the float

---

## 5. roles.naab — Multi-Agent Role Enforcement

**Imports:** `use json`, `use file`, `use string`, `use array`, `import "./models.naab" as models`

### Functions (8)

#### `load_roles(config_path)`
Load agent role configurations from a JSON config file.
- Read file, parse JSON
- Extract the `agent_roles` array (or return empty array if not present)
- Return array of role config dicts

#### `create_role(name, allowed_paths, blocked_paths, allowed_languages, max_complexity)`
Create a role configuration dict.
- Returns dict with all 5 keys: `name`, `allowed_paths`, `blocked_paths`, `allowed_languages`, `max_complexity`

#### `check_access(agent, file_path)`
Check if an agent is allowed to access a file path.
- Get agent's `allowed_paths` and `blocked_paths`
- First check blocked_paths: if file_path starts with any blocked path, return `{allowed: false, reason: "Path blocked: ..."}`
- Then check allowed_paths: if file_path starts with any allowed path, return `{allowed: true, reason: "Path allowed: ..."}`
- If no allowed_paths match, return `{allowed: false, reason: "Path not in allowed list"}`
- Empty allowed_paths means "all paths allowed" (return true)

#### `check_language_permission(agent, language)`
Check if an agent is allowed to use a specific language.
- Get agent's `allowed_languages` array
- If the language is in the array, return `{allowed: true, reason: "Language permitted"}`
- Otherwise return `{allowed: false, reason: "Language not permitted for this agent"}`
- Empty allowed_languages means "all languages allowed"

#### `enforce_role(agent, action_type, target)`
Combined enforcement: check both path access and language permission.
- `action_type` is one of: "read", "write", "execute", "import"
- `target` is a file path or language name
- For "execute" action_type, treat target as a language → use check_language_permission
- For other action_types, treat target as a file path → use check_access
- `enforcement` = "hard" for write/execute, "soft" for read/import
- Returns `{allowed, reason, enforcement}`

#### `get_agent_permissions(agent)`
Return a structured summary of an agent's permissions.
- Returns `{allowed_paths, blocked_paths, allowed_languages, restrictions}`
- `restrictions` is an array of human-readable restriction descriptions
- E.g., "Cannot access /deploy/*", "Limited to python, shell"

#### `validate_role_config(roles)`
Validate an array of role configurations.
- Check each role has required fields: name, allowed_paths, blocked_paths, allowed_languages
- Check for overlapping paths between roles (same path in both allowed and blocked)
- `warnings` for: roles with no allowed_paths (effectively blocked from everything)
- Returns `{valid, errors, warnings}`
- `valid` is true only when `errors` is empty

#### `role_summary(agents)`
Generate a formatted text table of all agents and their permissions.
- Columns: Agent Name | Role | Languages | Path Access
- One row per agent
- Return the formatted string

---

## 6. language_scorer.naab — Task-to-Language Assignment

**Imports:** `use json`, `use array`, `use string`, `use math`, `import "./models.naab" as models`

### Functions (8)

#### `score_language(task_description, language)`
Score how well a language fits a task.
- Use Python polyglot: keyword analysis of task_description
- Match against language strengths database
- Score 0-100 based on keyword matches and language capabilities
- Returns `{language, score, task_type, reasons}`
- `task_type` is detected via `detect_task_type()`
- `reasons` is array of strings explaining the score

**Language strengths (hardcoded knowledge base):**
- **python**: data analysis, machine learning, statistics, math, scripting, automation, text processing, json, csv, api
- **shell**: system administration, deployment, file management, automation, ops, devops, scripting, cron, backup
- **go**: concurrency, networking, microservices, performance, distributed systems, containers
- **rust**: systems programming, performance, safety, memory, embedded, low-level
- **javascript**: web, frontend, dom, browser, ui, react, node, api
- **nim**: performance, scripting, systems, cross-compilation

#### `rank_languages(task_description, available_languages)`
Rank all available languages for a task.
- Call `score_language()` for each language in `available_languages`
- Sort by score descending
- Return sorted array of score results

#### `assign_language(task_description, agent)`
Assign the best language for a task given an agent's constraints.
- Get agent's `allowed_languages`
- Rank those languages via `rank_languages()`
- Pick the highest-scoring language
- Also identify the second-best as `alternative`
- Returns `{language, score, alternative, reason}`
- `language` = best language name, `score` = its score
- `alternative` = second-best language name (or "" if only one available)
- `reason` = explanation string

#### `get_language_strengths()`
Return the hardcoded language strengths database.
- Returns dict mapping language name to `{strengths, weaknesses, best_for}`
- `strengths` = array of keyword strings
- `weaknesses` = array of keyword strings
- `best_for` = array of task type strings
- Include at least: python, shell, go, rust, javascript, nim

#### `detect_task_type(description)`
Detect the type of task from its description.
- Use Python polyglot: keyword matching
- Task types: "numerical", "text_processing", "system_ops", "web", "data_pipeline", "machine_learning", "networking", "general"
- Match keywords in the description to task types:
  - "numerical": calculate, compute, statistics, math, average, sum, regression
  - "text_processing": parse, format, extract, regex, transform, csv, json
  - "system_ops": deploy, backup, cron, monitor, server, docker, container
  - "web": http, api, rest, endpoint, frontend, html, css
  - "data_pipeline": etl, pipeline, transform, load, extract, batch, stream
  - "machine_learning": train, model, predict, classify, cluster, neural
  - "networking": socket, tcp, udp, dns, proxy, tunnel
- Return the best matching task type (most keyword hits)
- Default to "general" if no strong match

#### `validate_assignment(language, task_type)`
Validate whether a language is suitable for a task type.
- Cross-reference language strengths with task type
- `suitable` = true if language has strengths matching the task type
- `score` = 0-100 suitability score
- `warnings` = array of concerns (e.g., "Shell is not ideal for numerical computation")
- Returns `{suitable, score, warnings}`

#### `compare_languages(lang_a, lang_b, task_description)`
Compare two languages for a specific task.
- Score both languages via `score_language()`
- `winner` = language with higher score
- `margin` = score difference
- `reasoning` = explanation of why the winner is better
- Returns `{winner, margin, reasoning}`

#### `format_recommendation(assignment)`
Format an assignment result as a human-readable recommendation.
- Takes the dict from `assign_language()` as input
- Format as a clear recommendation string
- Include: recommended language, score, alternative, and reason
- Return the formatted string

---

## 7. config.naab — Configuration Loader

**Imports:** `use json`, `use file`

### Functions (6)

#### `load_config(config_path)`
Load and parse a govern.json configuration file.
- Read file content, parse JSON
- Return the full config dict
- If file doesn't exist, return empty dict `{}`

#### `get_agent_roles(config)`
Extract agent role configurations from config.
- Look for `config.get("agent_roles")` or `config.get("roles")`
- Return the array, or empty array if not present

#### `get_telemetry_config(config)`
Extract telemetry configuration with defaults.
- Look for `config.get("telemetry")` section
- Defaults: `{enabled: true, output_dir: "./telemetry_logs", format: "json", retention_days: 30}`
- Merge found config over defaults
- Returns `{enabled, output_dir, format, retention_days}`

#### `get_adaptive_config(config)`
Extract adaptive rule configuration with defaults.
- Look for `config.get("adaptive")` section
- Defaults: `{enabled: true, auto_propose: true, auto_apply: false, min_confidence: 0.8}`
- Returns `{enabled, auto_propose, auto_apply, min_confidence}`

#### `get_language_config(config)`
Extract language assignment configuration.
- Look for `config.get("language_assignment")` section
- Defaults: `{assignment_enabled: true, scoring_enabled: true, override_level: "soft"}`
- Returns `{assignment_enabled, scoring_enabled, override_level}`

#### `validate_config(config)`
Validate a configuration dict.
- Check for required top-level keys: `version`, `mode`
- Check that `mode` is one of: "enforce", "advisory", "disabled"
- Check `languages` section has `allowed` array
- `warnings` for: missing optional sections (telemetry, adaptive, etc.)
- Returns `{valid, errors, warnings}`

---

## 8. main.naab — Test Orchestrator

**Imports:** All 7 modules.

```naab
use json
use file
use time
use array
use string

import "./models.naab" as models
import "./telemetry.naab" as telemetry
import "./dashboard.naab" as dashboard
import "./adaptive.naab" as adaptive
import "./roles.naab" as roles
import "./language_scorer.naab" as scorer
import "./config.naab" as config
```

### Test Suites (8 suites, 10 tests each = 80 tests)

---

### test_models() — 10 tests

**Test 1: create_event returns all required keys**
```
let ev = models.create_event("agent-1", models.EventType.GovernanceCheck, "python", "no_secrets", "pass", "Clean", "2024-01-01T00:00:00")
Verify: ev has keys agent_id, event_type, language, rule_name, result, message, timestamp, session_id
```

**Test 2: create_agent returns all required keys**
```
let agent = models.create_agent("a1", "CodeBot", models.AgentRole.Backend, ["/src"], ["/deploy"], ["python", "shell"])
Verify: agent has keys id, name, role, allowed_paths, blocked_paths, allowed_languages
```

**Test 3: create_rule_proposal returns all required keys**
```
let prop = models.create_rule_proposal("pattern1", "Block dangerous calls", 0.8, 5, [])
Verify: prop has keys pattern, proposed_rule, confidence, evidence_count, source_events
```

**Test 4: validate_event catches missing fields**
```
let bad_event = {"agent_id": "a1"}  // missing most fields
let result = models.validate_event(bad_event)
Verify: result.get("valid") == false, len(result.get("errors")) > 0
```

**Test 5: validate_agent catches invalid role**
```
let bad_agent = {"id": "a1", "name": "Bot", "role": "InvalidRole", "allowed_paths": [], "blocked_paths": [], "allowed_languages": []}
let result = models.validate_agent(bad_agent)
Verify: result.get("valid") == false
```

**Test 6: event_type_name returns correct strings**
```
Verify: models.event_type_name(models.EventType.GovernanceCheck) == "GovernanceCheck"
Verify: models.event_type_name(models.EventType.HelperTriggered) == "HelperTriggered"
```

**Test 7: severity_name returns correct strings**
```
Verify: models.severity_name(models.Severity.Low) == "Low"
Verify: models.severity_name(models.Severity.Critical) == "Critical"
```

**Test 8: create_session has empty events and active status**
```
let session = models.create_session("s1", "agent-1", "2024-01-01T00:00:00")
Verify: len(session.get("events")) == 0
Verify: session.get("status") == "active"
```

**Test 9: create_score_result has all 4 keys**
```
let sr = models.create_score_result("python", 85, "numerical", ["good for math", "has numpy"])
Verify: sr has keys language, score, task_type, reasons
Verify: sr.get("score") == 85
```

**Test 10: role_name works for all 6 roles**
```
Verify: models.role_name(models.AgentRole.Frontend) == "Frontend"
Verify: models.role_name(models.AgentRole.Admin) == "Admin"
Verify: models.role_name(models.AgentRole.DataScience) == "DataScience"
```

---

### test_telemetry() — 10 tests

**Test 1: init_telemetry returns collector with empty events**
```
let result = telemetry.init_telemetry("./test_output")
let coll = result.get("collector")
Verify: result.get("success") == true
Verify: len(coll.get("events")) == 0
```

**Test 2: record_event appends to collector**
```
let coll = telemetry.init_telemetry("./test_output").get("collector")
let ev = models.create_event("a1", models.EventType.GovernanceCheck, "python", "rule1", "pass", "OK", time.now())
coll = telemetry.record_event(coll, ev)
Verify: len(coll.get("events")) == 1
```

**Test 3: record_governance_check creates proper event**
```
let coll = telemetry.init_telemetry("./test_output").get("collector")
coll = telemetry.record_governance_check(coll, "agent-1", "python", "no_secrets", true, "No secrets found")
let events = coll.get("events")
Verify: len(events) == 1
Verify: events[0].get("result") == "pass"
```

**Test 4: record_helper_triggered creates HelperTriggered event**
```
let coll = telemetry.init_telemetry("./test_output").get("collector")
coll = telemetry.record_helper_triggered(coll, "agent-1", "math_helper", "python", "undefined variable: round")
let events = coll.get("events")
Verify: len(events) == 1
Verify: events[0].get("result") == "triggered"
```

**Test 5: flush_events writes JSON file, returns correct count**
```
Build a collector with 3 events, flush to "./test_data/flush_test.json"
Verify: result.get("success") == true
Verify: result.get("events_written") == 3
Verify: file.exists("./test_data/flush_test.json") == true
```

**Test 6: load_events reads back what was written**
```
Flush events then load them back
Verify: len(loaded) == 3
Verify: loaded[0].get("agent_id") matches original
```

**Test 7: get_session_events filters correctly**
```
Record 3 events with session_id "s1" and 2 with "s2"
Set session_id on events before recording
let filtered = telemetry.get_session_events(coll, "s1")
Verify: len(filtered) == 3
```

**Test 8: get_events_by_agent filters correctly**
```
Record events from "agent-1" and "agent-2"
let filtered = telemetry.get_events_by_agent(coll, "agent-1")
Verify: only agent-1 events returned
```

**Test 9: Multiple agents, correct filter counts**
```
Record 4 events from agent-1, 3 from agent-2, 2 from agent-3
Verify: get_events_by_agent for agent-1 returns 4
Verify: get_events_by_agent for agent-3 returns 2
```

**Test 10: Round-trip: record → flush → load → verify all fields**
```
Create event with known values, record, flush, load, compare each field
Verify: every field matches original
```

---

### test_dashboard() — 10 tests

**Test 1: generate_summary returns all 6 keys**
```
Create 5 test events, call generate_summary
Verify: result has keys total_events, by_type, by_agent, by_language, by_result, time_range
```

**Test 2: generate_summary correct counts**
```
Create 5 events: 3 with result "pass", 2 with result "block"
Verify: result.get("total_events") == 5
Verify: by_result "pass" count == 3, "block" count == 2
```

**Test 3: top_violations returns sorted by count**
```
Create events: rule "A" blocked 5 times, rule "B" blocked 3 times, rule "C" blocked 1 time
let violations = dashboard.top_violations(events, 3)
Verify: violations[0].get("rule_name") == "A" (or whichever has count 5)
Verify: violations[0].get("count") == 5
```

**Test 4: agent_scorecard exact pass_rate**
```
Create 10 GovernanceCheck events for agent-1: 8 pass, 2 block
let card = dashboard.agent_scorecard(events, "agent-1")
Verify: card.get("total_checks") == 10
Verify: card.get("pass_rate") == 0.8
Verify: card.get("violations") == 2
```

**Test 5: agent_scorecard with 0 checks**
```
Create events for agent-2 but none for agent-3
let card = dashboard.agent_scorecard(events, "agent-3")
Verify: card.get("total_checks") == 0
Verify: card.get("pass_rate") == 0.0
```

**Test 6: helper_effectiveness_report per-helper stats**
```
Create HelperTriggered events + subsequent Pass events
Verify: report has entries for each helper
Verify: effectiveness_rate is correct (e.g., 2 triggered, 1 led to fix → 0.5)
```

**Test 7: language_usage_report groups correctly**
```
Create events: 4 python, 3 shell, 2 python blocks
let report = dashboard.language_usage_report(events)
Verify: report.get("python") has correct blocks_executed count
```

**Test 8: generate_text_report contains column headers**
```
let report = dashboard.generate_text_report(events)
Verify: report contains "Summary" (or similar header text)
Verify: len(report) > 50  // Not empty
```

**Test 9: export_report_csv has correct row count**
```
Create 5 events, export with headers ["agent_id", "rule_name", "result"]
let csv_str = dashboard.export_report_csv(events, ["agent_id", "rule_name", "result"])
Split by newlines, verify: 6 lines (1 header + 5 data)
```

**Test 10: trend_analysis returns non-empty periods**
```
Create events with different timestamps (spread over 48 hours)
let result = dashboard.trend_analysis(events, 24)
Verify: len(result.get("periods")) > 0
Verify: result has violation_trend and pass_rate_trend
```

---

### test_adaptive() — 10 tests

**Test 1: analyze_patterns finds repeated violations**
```
Create 5 block events from agent-1 with rule "no_secrets" and 3 from agent-1 with rule "naming"
let patterns = adaptive.analyze_patterns(events, 3)
Verify: len(patterns) >= 2  // Both patterns found (5 >= 3 and 3 >= 3)
```

**Test 2: analyze_patterns returns empty for < min_occurrences**
```
Create 2 block events (same agent+rule)
let patterns = adaptive.analyze_patterns(events, 5)
Verify: len(patterns) == 0
```

**Test 3: propose_rule returns all required keys**
```
let pattern = {agent_id: "a1", rule_name: "no_secrets", count: 10, ...}
let proposal = adaptive.propose_rule(pattern, [])
Verify: proposal has keys proposed_rule, confidence, rationale, evidence_count
```

**Test 4: evaluate_proposal returns would_catch count**
```
Create proposal + 20 events (15 would match)
let eval_result = adaptive.evaluate_proposal(proposal, events)
Verify: eval_result.get("would_catch") > 0
Verify: eval_result has keys would_catch, false_positive_rate, recommendation
```

**Test 5: generate_helper_text returns non-empty string**
```
let pattern = {rule_name: "no_secrets", count: 5}
let text = adaptive.generate_helper_text(pattern)
Verify: len(text) > 10
```

**Test 6: merge_proposals deduplicates same-pattern proposals**
```
Create 2 proposals with same proposed_rule but different confidence
let merged = adaptive.merge_proposals(proposals)
Verify: len(merged) == 1
Verify: merged[0] has highest confidence
```

**Test 7: format_govern_json_patch returns valid JSON string**
```
let patch = adaptive.format_govern_json_patch(proposals)
let parsed = json.parse(patch)
Verify: parsed is a dict (not null)
```

**Test 8: get_repeat_offenders filters by threshold**
```
Create events: agent-1 has 10 violations, agent-2 has 2
let offenders = adaptive.get_repeat_offenders(events, 5)
Verify: len(offenders) == 1
Verify: offenders[0].get("agent_id") == "agent-1"
```

**Test 9: calculate_confidence returns 0.0-1.0 range**
```
let low = adaptive.calculate_confidence(1, 0.1, 100)
let high = adaptive.calculate_confidence(50, 1.0, 100000)
Verify: low >= 0.0 and low <= 1.0
Verify: high >= 0.0 and high <= 1.0
Verify: high > low
```

**Test 10: Full pipeline: events → analyze → propose → evaluate**
```
Create 10 block events (same agent+rule)
let patterns = adaptive.analyze_patterns(events, 3)
let proposal = adaptive.propose_rule(patterns[0], [])
let eval_result = adaptive.evaluate_proposal(proposal, events)
Verify: eval_result.get("recommendation") is one of "adopt", "review", "reject"
```

---

### test_roles() — 10 tests

**Test 1: create_role returns all required keys**
```
let role = roles.create_role("developer", ["/src"], ["/deploy"], ["python"], 10)
Verify: role has keys name, allowed_paths, blocked_paths, allowed_languages, max_complexity
```

**Test 2: check_access allows path in allowed_paths**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.Backend, ["/src", "/tests"], ["/deploy"], ["python"])
let result = roles.check_access(agent, "/src/main.py")
Verify: result.get("allowed") == true
```

**Test 3: check_access blocks path in blocked_paths**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.Backend, ["/src"], ["/deploy", "/secrets"], ["python"])
let result = roles.check_access(agent, "/deploy/prod.sh")
Verify: result.get("allowed") == false
```

**Test 4: check_access blocks path not in allowed_paths**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.Frontend, ["/frontend"], [], ["javascript"])
let result = roles.check_access(agent, "/backend/api.py")
Verify: result.get("allowed") == false
```

**Test 5: check_language_permission allows listed language**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.Backend, [], [], ["python", "shell"])
let result = roles.check_language_permission(agent, "python")
Verify: result.get("allowed") == true
```

**Test 6: check_language_permission blocks unlisted language**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.Backend, [], [], ["python", "shell"])
let result = roles.check_language_permission(agent, "javascript")
Verify: result.get("allowed") == false
```

**Test 7: enforce_role combines path + language checks**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.Backend, ["/src"], ["/deploy"], ["python"])
let result1 = roles.enforce_role(agent, "write", "/src/module.py")
Verify: result1.get("allowed") == true
let result2 = roles.enforce_role(agent, "execute", "javascript")
Verify: result2.get("allowed") == false
```

**Test 8: get_agent_permissions returns correct structure**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.Backend, ["/src"], ["/deploy"], ["python"])
let perms = roles.get_agent_permissions(agent)
Verify: perms has keys allowed_paths, blocked_paths, allowed_languages, restrictions
Verify: len(perms.get("allowed_paths")) == 1
```

**Test 9: validate_role_config catches overlapping paths**
```
let bad_roles = [
    roles.create_role("dev", ["/src", "/deploy"], ["/deploy"], ["python"], 10)
]
let result = roles.validate_role_config(bad_roles)
Verify: len(result.get("errors")) > 0 or len(result.get("warnings")) > 0
```

**Test 10: role_summary contains all agent names**
```
let agents = [
    models.create_agent("a1", "FrontBot", models.AgentRole.Frontend, ["/frontend"], [], ["javascript"]),
    models.create_agent("a2", "BackBot", models.AgentRole.Backend, ["/src"], [], ["python"])
]
let summary = roles.role_summary(agents)
Verify: summary.contains("FrontBot") and summary.contains("BackBot")
```

---

### test_language_scorer() — 10 tests

**Test 1: score_language returns all required keys**
```
let result = scorer.score_language("calculate statistics from CSV data", "python")
Verify: result has keys language, score, task_type, reasons
```

**Test 2: Python scores higher for data analysis than Shell**
```
let py = scorer.score_language("analyze data and compute statistics", "python")
let sh = scorer.score_language("analyze data and compute statistics", "shell")
Verify: py.get("score") > sh.get("score")
```

**Test 3: rank_languages returns sorted array**
```
let ranked = scorer.rank_languages("deploy to production server", ["python", "shell", "go"])
Verify: len(ranked) == 3
Verify: ranked[0].get("score") >= ranked[1].get("score")  // sorted desc
```

**Test 4: assign_language respects agent's allowed_languages**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.Backend, [], [], ["python", "shell"])
let result = scorer.assign_language("analyze data", agent)
Verify: result.get("language") == "python" or result.get("language") == "shell"
```

**Test 5: assign_language falls back when first choice blocked**
```
let agent = models.create_agent("a1", "Bot", models.AgentRole.DevOps, [], [], ["shell"])
let result = scorer.assign_language("analyze data and compute statistics", agent)
Verify: result.get("language") == "shell"  // python would be better but not allowed
```

**Test 6: get_language_strengths returns all configured languages**
```
let strengths = scorer.get_language_strengths()
Verify: strengths.get("python") != null
Verify: strengths.get("shell") != null
Verify: strengths.get("go") != null
```

**Test 7: detect_task_type: compute statistics → numerical**
```
let task_type = scorer.detect_task_type("compute statistics and calculate average")
Verify: task_type == "numerical"
```

**Test 8: detect_task_type: deploy to server → system_ops**
```
let task_type = scorer.detect_task_type("deploy application to production server")
Verify: task_type == "system_ops"
```

**Test 9: validate_assignment: Python + numerical → suitable**
```
let result = scorer.validate_assignment("python", "numerical")
Verify: result.get("suitable") == true
Verify: result.get("score") > 50
```

**Test 10: compare_languages returns winner with reasoning**
```
let result = scorer.compare_languages("python", "shell", "calculate statistical regression")
Verify: result has keys winner, margin, reasoning
Verify: result.get("winner") == "python"
```

---

### test_config() — 10 tests

**Test 1: load_config reads govern.json successfully**
```
let cfg = config.load_config("./govern.json")
Verify: cfg.get("version") != null
```

**Test 2: get_agent_roles returns array from config**
```
let cfg = config.load_config("./govern.json")
let agent_roles = config.get_agent_roles(cfg)
Verify: type(agent_roles) == "array"  // may be empty, that's OK
```

**Test 3: get_telemetry_config returns defaults when not specified**
```
let cfg = {}
let tel = config.get_telemetry_config(cfg)
Verify: tel.get("enabled") == true
Verify: tel.get("format") == "json"
```

**Test 4: get_adaptive_config returns all required keys**
```
let cfg = {}
let adp = config.get_adaptive_config(cfg)
Verify: adp has keys enabled, auto_propose, auto_apply, min_confidence
```

**Test 5: validate_config passes for valid config**
```
let cfg = {"version": "3.0", "mode": "enforce", "languages": {"allowed": ["python"]}}
let result = config.validate_config(cfg)
Verify: result.get("valid") == true
```

**Test 6: validate_config catches missing required sections**
```
let cfg = {}  // missing version, mode, languages
let result = config.validate_config(cfg)
Verify: result.get("valid") == false
Verify: len(result.get("errors")) >= 2
```

**Test 7: get_language_config returns assignment settings**
```
let cfg = {}
let lang = config.get_language_config(cfg)
Verify: lang has keys assignment_enabled, scoring_enabled, override_level
```

**Test 8: Config round-trip: load → modify → validate**
```
let cfg = config.load_config("./govern.json")
cfg["mode"] = "advisory"
let result = config.validate_config(cfg)
Verify: result.get("valid") == true
```

**Test 9: Empty config returns safe defaults (not errors)**
```
let cfg = {}
let tel = config.get_telemetry_config(cfg)
let adp = config.get_adaptive_config(cfg)
Verify: tel.get("retention_days") == 30
Verify: adp.get("min_confidence") == 0.8
```

**Test 10: get_agent_roles returns empty array when no roles defined**
```
let cfg = {"version": "3.0", "mode": "enforce"}
let agent_roles = config.get_agent_roles(cfg)
Verify: len(agent_roles) == 0
```

---

### test_integration() — 10 tests

**Test 1: Full telemetry pipeline**
```
Init collector → record 5 events → flush to file → load from file → verify count and fields
```

**Test 2: Dashboard from telemetry**
```
Record 10 events (mix of pass/block) → generate_summary → verify counts match
```

**Test 3: Adaptive from telemetry**
```
Record 8 block events (same agent+rule) → analyze_patterns → verify pattern found
```

**Test 4: Role enforcement blocks write to forbidden path**
```
Create agent with blocked /deploy → enforce_role("write", "/deploy/script.sh") → verify blocked
```

**Test 5: Role enforcement allows read from allowed path**
```
Create agent with allowed /src → enforce_role("read", "/src/main.py") → verify allowed
```

**Test 6: Language assignment picks Python for statistics task**
```
Create agent with ["python", "shell"] → assign_language("calculate statistics") → verify python chosen
```

**Test 7: Language assignment respects role restrictions**
```
Create agent with only ["shell"] → assign_language("analyze data") → verify shell chosen (not python)
```

**Test 8: End-to-end governance lifecycle**
```
Create agent → governance check event (block) → helper triggered event → governance check (pass)
→ flush all to file → load → generate scorecard
Verify: scorecard shows 1 pass, 1 block, helper_effectiveness > 0
```

**Test 9: Config drives role enforcement**
```
Load config → create agent from config roles → enforce_role → verify matches config
```

**Test 10: Telemetry captures all event types**
```
Record one of each EventType → flush → load → generate_summary
Verify: by_type has 5 distinct types
```

---

## Test Data

A file `test_data/sample_events.json` is provided with 20 sample events for testing
load_events and dashboard functions. Your tests should primarily create their own test
data, but can reference this file for load testing.

## Important Implementation Notes

1. **Value semantics everywhere**: When you modify a dict inside a loop or after getting
   it from an array, you MUST re-assign it back. This is the #1 source of bugs.

2. **Python polyglot for sorting/correlation**: NAAb doesn't have a sort-by-key function.
   Use `<<python[data] -> JSON` with Python's `sorted()` and `json.dumps()`.

3. **Enum comparison**: Use `models.EventType.GovernanceCheck` directly in comparisons.
   Enums work like constants.

4. **Time handling**: Use `time.now()` for timestamps. For trend analysis, use Python
   polyglot to parse and compare timestamps.

5. **File I/O for telemetry**: Use `file.write(path, content)` and `file.read(path)`.
   Always use `json.stringify()` before writing and `json.parse()` after reading.

6. **Empty arrays/dicts**: Initialize with `[]` and `{}`. Don't use `null` as initial value.

7. **Function contracts**: govern.json defines return_keys for 20 functions. Every
   listed key MUST be present in the return dict. Missing keys = governance violation.

8. **Complexity floor**: Functions like `analyze_patterns`, `propose_rule`, `score_language`
   need real logic (score >= 20, must have branching or loops). Don't stub them.

9. **Test isolation**: Each test should create its own data. Don't rely on state from
   previous tests. Each test function is independent.

10. **Clean up test files**: If your tests write files (flush_events), write to
    `./test_data/` directory to keep things organized.

## Verification

After generating all 8 files, run:
```bash
cd tools/agent-governance && ../../build/naab-lang main.naab
```

Target: 80/80 tests pass, 0 governance violations.
