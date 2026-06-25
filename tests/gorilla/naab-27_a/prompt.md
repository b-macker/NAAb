# Project: Enterprise Event Processing & Alerting Platform

Build a production-grade event processing platform that ingests structured log events,
classifies them by category, correlates related events into incidents, evaluates alert
rules with threshold-based triggering, computes operational metrics (MTTR, SLA compliance,
frequency analysis), generates timeline and summary reports, and uses LLM agents for
incident interpretation.

This is NOT a compliance engine. This is an event-driven operational monitoring system.

## Architecture

12 source files in `src/`:

```
src/
├── main.naab          # Test orchestrator (130+ tests across 12 suites)
├── models.naab        # Domain structs, enums, factory functions, label mappers
├── parser.naab        # Log entry parsing with regex patterns
├── classifier.naab    # Event classification by content analysis
├── correlator.naab    # Event correlation into incident groups
├── alerting.naab      # Alert rule evaluation, thresholds, escalation
├── metrics.naab       # Statistical computation (frequency, MTTR, SLA)
├── timeline.naab      # Time-windowed event sequencing
├── storage.naab       # In-memory event store, indexing, querying
├── exporter.naab      # CSV/JSON export, file output (taint sink)
├── analyzer.naab      # Anomaly detection + agent-based interpretation
└── validators.naab    # Input sanitization, schema validation
```

## Domain Model

### Enums (in models.naab)

```
EventSeverity { Critical, Major, Minor, Warning, Info, Debug }
EventCategory { Security, Performance, Infrastructure, Application, Network, Database }
IncidentStatus { Open, Acknowledged, Investigating, Mitigating, Resolved, Closed }
AlertPriority { P1, P2, P3, P4, P5 }
AlertState { Pending, Firing, Acknowledged, Silenced, Resolved }
ThresholdDirection { Above, Below, Equal, Change }
```

### Structs (in models.naab)

```
Event {
    id: string
    timestamp: int
    source: string
    severity: int
    message: string
    category: int
    subcategory: string
    tags: array
    metadata: dict
}

Incident {
    id: string
    title: string
    status: int
    severity: int
    priority: int
    events: array
    created_at: int
    updated_at: int
    acknowledged_by: string
    resolved_at: int
    tags: array
    root_cause: string
}

AlertRule {
    id: string
    name: string
    condition_field: string
    threshold_value: int
    direction: int
    severity: int
    cooldown_seconds: int
    notification_targets: array
    enabled: bool
}

Alert {
    id: string
    rule_id: string
    severity: int
    message: string
    triggered_at: int
    acknowledged_at: int
    state: int
    event_ids: array
}

TimelineEntry {
    timestamp: int
    event_id: string
    description: string
    severity: int
    source: string
}

ExportResult {
    format: string
    row_count: int
    hash: string
    content: string
}
```

### Factory Functions (in models.naab)
- `create_event(id, timestamp, source, severity, message, category, subcategory, tags, metadata)` -> new Event
- `create_incident(id, title, severity, priority, events)` -> new Incident (status=Open, created_at=time.now())
- `create_alert_rule(id, name, condition_field, threshold_value, direction, severity, cooldown_seconds, targets)` -> new AlertRule
- `create_alert(id, rule_id, severity, message, event_ids)` -> new Alert (state=Pending, triggered_at=time.now())
- `create_timeline_entry(timestamp, event_id, description, severity, source)` -> new TimelineEntry

### Label Mappers (in models.naab — MUST use match expressions)
- `severity_label(sev)` -> "Critical"/"Major"/"Minor"/"Warning"/"Info"/"Debug"/"Unknown"
- `event_type_label(cat)` -> "Security"/"Performance"/.../"Unknown"
- `status_label(status)` -> "Open"/"Acknowledged"/.../"Unknown"
- `priority_label(pri)` -> "P1"/"P2"/"P3"/"P4"/"P5"/"Unknown"
- `category_label(cat)` -> same as event_type_label (alias)
- `alert_state_label(state)` -> "Pending"/"Firing"/"Acknowledged"/"Silenced"/"Resolved"/"Unknown"
- `direction_label(dir)` -> "Above"/"Below"/"Equal"/"Change"/"Unknown"

## Module Specifications

### parser.naab — Log Entry Parsing
Requires: `use regex`, `use json`, `use time`
Imports: models.naab

**Functions:**
- `parse_log_entry(text)` -> {event, valid, errors}
  Parses structured log text (JSON or syslog-style). Uses try/catch for JSON parsing.
  Uses regex to extract fields from syslog format: `<timestamp> <source> <severity>: <message>`
  Returns parsed Event or null with error list.

- `parse_batch(lines)` -> {events, errors, success_count, fail_count}
  Parses array of log strings. Collects successes and failures.

- `extract_timestamp(text)` -> int or null
  Extracts ISO-8601 or Unix timestamp from text using regex.

- `extract_source(text)` -> string
  Extracts source/hostname from log line using regex.

- `normalize_severity(text)` -> int (enum value)
  Maps string severity ("ERROR", "WARN", "CRITICAL", etc.) to EventSeverity enum.

### classifier.naab — Event Classification
Requires: `use regex`, `use string`
Imports: models.naab

**Functions:**
- `classify_event(event)` -> {category, subcategory, confidence, tags}
  Analyzes event message and metadata to determine category. Uses match expression
  for primary classification. Examines keywords, patterns, and source.

- `batch_classify(events)` -> {results, total, classified_count, unknown_count}
  Classifies array of events. Must call classify_event for each.

- `extract_keywords(message)` -> array
  Extracts significant keywords from event message for classification.

- `calculate_confidence(event, category)` -> float (0.0-1.0)
  Computes classification confidence based on matching keyword count.

- `is_security_event(event)` -> bool
  Quick check for security-related events (auth failures, access denied, etc.)

### correlator.naab — Event Correlation
Requires: `use time`
Imports: models.naab

**Functions:**
- `correlate_events(events, time_window_seconds)` -> {groups, ungrouped, correlation_count}
  Groups related events by source + category within time windows. Uses Python polyglot
  for the grouping algorithm.

- `detect_incident(event_group, threshold)` -> {is_incident, incident, evidence}
  Determines if a correlated event group constitutes an incident. Uses match for
  severity-based thresholds.

- `find_related_events(events, event, max_depth)` -> array
  Recursively finds events related by source, category, or shared tags.
  MUST be recursive with base case.

- `merge_incidents(incident_a, incident_b)` -> {merged_incident, source_count, combined_events}
  Merges two incidents. Combines events, takes higher severity. Uses pipeline.

- `deduplicate_events(events)` -> {unique_events, duplicate_count, dedup_map}
  Removes duplicate events based on message + source + timestamp proximity. **JavaScript polyglot.**

### alerting.naab — Alert Rules & Escalation
Requires: `use time`, `use uuid`
Imports: models.naab

**Functions:**
- `evaluate_alert_rule(rule, events, current_state)` -> {triggered, alert, rule_id, severity}
  Evaluates a single alert rule against recent events. Uses match for direction
  comparison logic.

- `check_threshold(value, threshold, direction)` -> {exceeded, current_value, threshold, direction}
  Compares a value against a threshold with directional logic.

- `build_alert_chain(alerts, incidents)` -> {chain, root_cause, affected_services, depth}
  Traces alert-to-incident relationships to find root cause chains. **JavaScript polyglot.**

- `escalate_alert(alert, incidents, escalation_rules)` -> {escalated, new_priority, notification_targets, reason}
  Escalates alert priority based on duration, recurrence, or impact. return_keys_non_empty.

- `acknowledge_incident(incident, owner)` -> {incident, acknowledged_at, owner}
  Sets incident status to Acknowledged with owner and timestamp.

- `resolve_incident(incident, resolution_note)` -> {incident, resolved_at, resolution_time_seconds}
  Resolves incident, computes resolution time from creation.

- `get_active_incidents(incidents)` -> array
  Filters for non-resolved, non-closed incidents.

- `filter_events_by_time_range(events, start_time, end_time)` -> array
  Returns events within the specified time range.

### metrics.naab — Statistical Computation
Requires: `use math`, `use time`
Imports: models.naab

**Functions:**
- `compute_severity_distribution(events)` -> {distribution, total, dominant_severity, entropy}
  Computes distribution of events by severity. Python polyglot for entropy calculation.

- `compute_alert_statistics(alerts, incidents)` -> {total_alerts, by_severity, by_category, avg_response_time, escalation_rate}
  Comprehensive alert metrics. Python polyglot for statistical aggregation.

- `compute_event_frequency(events, window_seconds)` -> {events_per_minute, peak_rate, avg_rate, burst_windows}
  Time-series frequency analysis. Python polyglot for windowed counting.

- `calculate_sla_compliance(incidents, sla_targets)` -> {compliant, compliance_rate, violations, by_priority}
  Checks incident resolution times against SLA targets. Python polyglot.

- `calculate_mttr(incidents)` -> {mttr_seconds, mttr_minutes, by_severity, trend}
  Mean Time To Resolve computation. Python polyglot for statistical analysis.

### timeline.naab — Time-Based Event Sequencing
Requires: `use time`
Imports: models.naab

**Functions:**
- `generate_timeline(events, incidents)` -> {entries, start_time, end_time, duration_seconds}
  Builds chronological timeline from events and incident state changes. Uses pipeline.

- `detect_anomaly_window(timeline_entries, baseline_rate, deviation_factor)` -> {is_anomalous, window_stats, threshold_breaches}
  Identifies time windows where event rate exceeds baseline. Statistical detection.

- `find_gaps(timeline_entries, max_gap_seconds)` -> array
  Finds periods with no events (potential monitoring gaps).

- `compress_timeline(entries, merge_window_seconds)` -> array
  Merges timeline entries within a time window into summary entries.

- `get_window_events(events, center_time, window_seconds)` -> array
  Returns events within a time window centered on a timestamp.

### storage.naab — In-Memory Event Store
Requires: `use uuid`
Imports: models.naab

**Functions:**
- `store_event(store, event)` -> {store, success, event_id}
  Adds event to store. Value semantics — must return modified store.

- `query_events(store, filters)` -> {results, total, filtered}
  Queries store with filter dict (source, severity, category, time_range).

- `build_event_index(events)` -> {by_source, by_severity, by_category, by_hour}
  Builds multi-dimensional index. Uses pipeline. **JavaScript polyglot.**

- `count_events_by_source(events)` -> dict
  Counts events per source. Uses pipeline.

- `get_event_by_id(store, event_id)` -> Event or null

- `delete_events_before(store, cutoff_time)` -> {store, deleted_count}
  Removes events older than cutoff. Value semantics.

### exporter.naab — Export & File Output
Requires: `use json`, `use crypto`, `use file`, `use csv`
Imports: models.naab, validators.naab

**Functions:**
- `export_events_csv(events)` -> {csv_text, row_count, columns}
  Exports events to CSV format. Uses pipeline for transformation.

- `export_incidents_json(incidents)` -> {json_text, incident_count, hash}
  Exports incidents to JSON with integrity hash. **JavaScript polyglot.**

- `write_export_to_file(content, filepath)` -> {success, path, bytes_written}
  Writes export content to file. MUST sanitize before write (taint tracking).
  Must use try/catch.

- `format_event(event)` -> string
  Formats single event for display. Uses match + pipeline.

- `format_incident(incident)` -> string
  Formats incident for display. Uses match.

- `format_alert(alert)` -> string
  Formats alert for display. Uses pipeline.

### reports.naab — Report Generation
Requires: `use json`, `use crypto`, `use time`
Imports: models.naab, validators.naab

**Functions:**
- `generate_summary_report(events, incidents, alerts)` -> {report_text, hash, sections}
  Executive summary report. return_keys_non_empty enforced.

- `generate_detail_report(events, incidents, alerts, metrics_data)` -> {report_text, hash, event_count, incident_count}
  Detailed operational report with metrics.

- `rank_incidents_by_severity(incidents)` -> array
  Sorts incidents by severity then priority. Uses pipeline.

### analyzer.naab — Anomaly Detection & Agent Interpretation
Requires: `use agent`, `use json`
Imports: models.naab

**Functions:**
- `analyze_event_patterns(events, window_size)` -> {patterns, recurring_count, novel_count, recommendations}
  Pattern mining across events. Python polyglot for analysis.

- `analyze_anomalies(events, baseline)` -> {anomalies, baseline, deviations, risk_score}
  Statistical anomaly detection. Python polyglot.

- `check_agent_available(config_name)` -> {available, reason}
  Pre-flight agent check. Must map agent.check() result to contract keys.

- `interpret_incident(incident, context_events)` -> {interpretation, confidence, reasoning, suggested_actions}
  Uses agent for natural language incident analysis. Try/catch around agent calls.

- `generate_interpretation_prompt(incident, events)` -> string
  Builds prompt text from incident and event data.

### validators.naab — Sanitization & Validation
Requires: `use regex`, `use validate`
Imports: models.naab

**Functions:**
- `sanitize_string(input)` -> string (strips `<` and `>`, preserves JSON chars, trims)
- `sanitize_log_line(line)` -> string (strips ANSI escape codes, then sanitize_string)
- `validate_event_schema(event_dict)` -> {valid, errors}
  Validates required fields: id, timestamp, source, severity, message.
- `validate_email_address(email)` -> bool
- `validate_timestamp(ts)` -> bool

## Test Suites (130+ tests in main.naab)

1. **test_models** (~12 tests): Struct creation, label mappers, enum values
2. **test_parser** (~12 tests): JSON parsing, syslog parsing, timestamp extraction, batch parsing, error handling
3. **test_classifier** (~10 tests): Category classification, keyword extraction, confidence scoring, batch classification
4. **test_correlator** (~12 tests): Time-window grouping, incident detection, event deduplication, recursive related events, incident merging
5. **test_alerting** (~12 tests): Threshold evaluation, alert creation, escalation, acknowledgment, resolution, active filtering, time range filtering
6. **test_metrics** (~10 tests): Severity distribution, alert statistics, event frequency, SLA compliance, MTTR
7. **test_timeline** (~10 tests): Timeline generation, anomaly windows, gap detection, compression, windowed queries
8. **test_storage** (~10 tests): Store/query, indexing, counting, deletion, value semantics verification
9. **test_exporter** (~10 tests): CSV export, JSON export, file writing, formatting functions
10. **test_reports** (~8 tests): Summary report, detail report, incident ranking
11. **test_analyzer** (~8 tests): Pattern analysis, anomaly detection, agent availability, interpretation
12. **test_validators** (~8 tests): String sanitization, schema validation, email validation

## Required Language Feature Usage

### Match Expressions (minimum 12)
- All 7 label mapper functions in models.naab
- classify_event — primary category dispatch
- evaluate_alert_rule — direction comparison
- detect_incident — severity thresholds
- format_event — severity prefix formatting
- format_incident — status prefix formatting

### Recursive Functions (minimum 3)
- find_related_events — recursive event graph traversal
- validate_condition_tree (if conditions have nested structure)
- Any tree/graph traversal in correlation

### Pipeline Operator (minimum 8)
- generate_timeline, build_event_index, export_events_csv, format_event, format_alert,
  merge_incidents, rank_incidents_by_severity, count_events_by_source

### Python Polyglot (minimum 8)
- correlate_events, compute_severity_distribution, compute_alert_statistics,
  compute_event_frequency, analyze_event_patterns, analyze_anomalies,
  calculate_sla_compliance, calculate_mttr

### Try/Catch (minimum 3)
- parse_log_entry, write_export_to_file, interpret_incident

### Value Semantics Correctness
- store_event — must return modified store
- delete_events_before — must return modified store
- acknowledge_incident — must return modified incident dict
- resolve_incident — must return modified incident dict

### Taint Tracking
- All polyglot output and file.read data flows through sanitize_string before file.write
- exporter.naab is the primary taint sink — must import validators.naab

## What NOT to Do
- Do NOT reuse compliance engine code — this is a completely different system
- Do NOT write standalone .py/.js files
- Do NOT hardcode results, use placeholders, or stub functions
- Do NOT leave TODO/FIXME/STUB comments
- Do NOT use hedging comments: "simplified", "basic", "for now"
- Do NOT write empty/trivial functions
- Do NOT swallow errors silently (empty catch blocks — HARD blocked)
- Do NOT pad functions with dummy loops
- Do NOT use `return` inside Python polyglot blocks
- Do NOT use `dict["key"]` — HARD blocked. Use `dict.get("key")`
- Do NOT forget variable binding on polyglot blocks
- Do NOT modify govern.json — it is signed
