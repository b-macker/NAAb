# DevOps Incident Management Platform — Full Specification

## Overview

Build a DevOps Incident Management Platform in NAAb. The platform ingests alerts from
monitoring systems, correlates them into incidents, assigns on-call responders, tracks
resolution timelines against SLA policies, handles escalations, generates reports, and
performs agent-based root cause analysis using Gemini LLMs.

**All code goes in `.naab` files inside the `src/` directory.** Do NOT create standalone
`.py`, `.js`, or `.go` files. Polyglot blocks embed Python/Shell inside `.naab` files.

## File Structure

Create these 10 files in `src/`:

```
src/
├── main.naab         # Test orchestrator — 90 tests, 9 suites
├── models.naab       # Structs, enums, factory functions
├── alerts.naab       # Alert ingestion, parsing, classification
├── incidents.naab    # Incident lifecycle, correlation
├── responders.naab   # On-call management, assignment
├── escalation.naab   # Escalation engine, dependency traversal
├── sla.naab          # SLA computation, compliance tracking
├── reports.naab      # Report generation, file output
├── validators.naab   # Sanitization, validation
└── rca.naab          # Agent-based root cause analysis
```

**Import paths:** All files are in the same directory. Use `import "models.naab" as models`.
Do NOT use `import "src/models.naab"` — that looks for `src/src/models.naab`.

## Implementation Order

Build files in this order (each file depends only on files above it):

1. `models.naab` — no dependencies
2. `validators.naab` — imports models
3. `alerts.naab` — imports models
4. `incidents.naab` — imports models
5. `responders.naab` — imports models
6. `escalation.naab` — imports models
7. `sla.naab` — imports models
8. `reports.naab` — imports models, validators
9. `rca.naab` — imports models
10. `main.naab` — imports all 9 modules

---

## Module 1: models.naab — Domain Models

No imports needed (uses only built-in types).
Requires: `use time`

### Enums (all exported)

```naab
export enum Severity {
    Critical,
    High,
    Medium,
    Low
}

export enum IncidentStatus {
    Open,
    Acknowledged,
    Investigating,
    Resolved,
    Closed
}

export enum AlertSource {
    Prometheus,
    Grafana,
    CloudWatch,
    PagerDuty,
    Manual
}

export enum EscalationLevel {
    L1,
    L2,
    L3,
    Executive
}

export enum ResponderRole {
    Engineer,
    SRE,
    Manager,
    Executive
}
```

### Structs (all exported)

```naab
export struct Alert {
    id: string
    source: int
    message: string
    severity: int
    timestamp: int
    tags: array
    raw_log: string
}

export struct Incident {
    id: string
    title: string
    severity: int
    status: int
    alerts: array
    responder_id: string
    created_at: int
    updated_at: int
    resolved_at: int
    sla_deadline: int
    correlation_id: string
}

export struct Responder {
    id: string
    name: string
    email: string
    role: int
    on_call: bool
    current_load: int
    max_load: int
    skills: array
}

export struct Escalation {
    id: string
    incident_id: string
    from_level: int
    to_level: int
    reason: string
    escalated_at: int
}

export struct PostMortem {
    id: string
    incident_id: string
    root_cause: string
    timeline: array
    action_items: array
    created_by: string
}

export struct SLAPolicy {
    id: string
    name: string
    severity: int
    response_minutes: int
    resolution_minutes: int
}
```

### Exported Functions

#### `create_alert(id, source, message, severity, timestamp, tags, raw_log)`
Returns a new Alert struct with all fields set from parameters.

#### `create_incident(id, title, severity, alerts, sla_deadline)`
Returns a new Incident struct. Sets:
- status = IncidentStatus.Open
- created_at = time.now()
- updated_at = time.now()
- resolved_at = 0
- responder_id = ""
- correlation_id = ""

#### `create_responder(id, name, email, role, on_call, max_load, skills)`
Returns a new Responder struct. Sets current_load = 0.

#### `create_sla_policy(id, name, severity, response_min, resolution_min)`
Returns a new SLAPolicy struct with all fields set from parameters.

#### `severity_name(sev)`
Returns string name for Severity enum value:
- Severity.Critical -> "Critical"
- Severity.High -> "High"
- Severity.Medium -> "Medium"
- Severity.Low -> "Low"
- default -> "Unknown"

Use if/else if chain (not match expression — match arms can't have blocks).

#### `status_name(status)`
Returns string name for IncidentStatus enum value:
- IncidentStatus.Open -> "Open"
- IncidentStatus.Acknowledged -> "Acknowledged"
- IncidentStatus.Investigating -> "Investigating"
- IncidentStatus.Resolved -> "Resolved"
- IncidentStatus.Closed -> "Closed"
- default -> "Unknown"

#### `source_name(source)`
Returns string name for AlertSource enum value:
- AlertSource.Prometheus -> "Prometheus"
- AlertSource.Grafana -> "Grafana"
- AlertSource.CloudWatch -> "CloudWatch"
- AlertSource.PagerDuty -> "PagerDuty"
- AlertSource.Manual -> "Manual"
- default -> "Unknown"

#### `validate_alert(alert)`
Returns dict: `{valid: bool, errors: array}`
Checks:
- alert.get("id") is non-empty string -> if empty, push "id is required" to errors
- alert.get("message") is non-empty string -> if empty, push "message is required"
- alert.get("tags") is not null -> if null, push "tags is required"
- valid = (len(errors) == 0)

#### `validate_responder(responder)`
Returns dict: `{valid: bool, errors: array}`
Checks:
- responder.get("id") is non-empty -> if empty, push "id is required"
- responder.get("name") is non-empty -> if empty, push "name is required"
- responder.get("max_load") > 0 -> if not, push "max_load must be positive"
- valid = (len(errors) == 0)

### main {} block
Empty or minimal — this file is imported by others, main block is not executed.
```naab
main {}
```

---

## Module 2: validators.naab — Sanitization & Validation

Requires: `use regex`, `use validate`, `use string`
Imports: `import "models.naab" as models`

### Exported Functions

#### `sanitize_string(input)`
Clears taint for file.write operations.
```
if input == null: return ""
let s = string.trim(input)
s = string.replace(s, "<", "")
s = string.replace(s, ">", "")
return s
```
Do NOT strip `"`, `{`, `}`, `[`, `]` — these are valid in JSON output.

#### `sanitize_log_line(line)`
Strips ANSI escape codes then sanitizes:
```
if line == null: return ""
let cleaned = regex.replace(line, "\x1b\\[[0-9;]*m", "")
return sanitize_string(cleaned)
```
Note: `\x1b` is the escape character. The regex pattern matches ANSI color codes.

#### `validate_incident_data(data)`
Returns dict: `{valid: bool, errors: array}`
```
let errors = []
let title = data.get("title")
if title == null || title == "":
    push "title is required"
let sev = data.get("severity")
if sev == null:
    push "severity is required"
valid = len(errors) == 0
```

#### `validate_email_address(email)`
Returns bool: `validate.email(email)`

#### `validate_ip_address(ip)`
Returns bool: `validate.ip(ip)`

#### `validate_severity(sev)`
Returns bool. Checks if sev equals any of:
`models.Severity.Critical`, `models.Severity.High`, `models.Severity.Medium`, `models.Severity.Low`

### main {} block
```naab
main {}
```

---

## Module 3: alerts.naab — Alert Ingestion & Classification

Requires: `use regex`, `use string`, `use json`
Imports: `import "models.naab" as models`

### Exported Functions

#### `parse_log_line(raw_line)`
Returns dict: `{timestamp, level, service, message}`

Parse using regex:
```
let pattern = "(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}) \\[(\\w+)\\] (\\w+): (.+)"
let m = regex.search(raw_line, pattern)
```
If match found: extract groups. `regex.search` returns the matched string or null.
Use `regex.find_groups(raw_line, pattern)` to get capture groups as an array.
- groups[1] = timestamp, groups[2] = level, groups[3] = service, groups[4] = message

If no match: return `{timestamp: null, level: null, service: null, message: raw_line}`

#### `classify_alert(alert)`
Returns dict: `{category, confidence, patterns_matched}`

Check the alert message (use `alert.get("message")`) with `string.contains()` (case-insensitive — convert message to lower first with `string.lower()`):

```
let msg = string.lower(alert.get("message"))
let patterns_matched = 0
let category = "unknown"
let confidence = 50

if string.contains(msg, "oom") || string.contains(msg, "out of memory") || string.contains(msg, "memory"):
    category = "memory"
    confidence = 90
    patterns_matched = patterns_matched + 1
else if string.contains(msg, "5xx") || string.contains(msg, "503") || string.contains(msg, "502") || string.contains(msg, "500"):
    category = "http_error"
    confidence = 85
    patterns_matched = patterns_matched + 1
else if string.contains(msg, "timeout") || string.contains(msg, "timed out"):
    category = "timeout"
    confidence = 80
    patterns_matched = patterns_matched + 1
else if string.contains(msg, "disk") || string.contains(msg, "storage") || string.contains(msg, "space"):
    category = "disk"
    confidence = 85
    patterns_matched = patterns_matched + 1
else if string.contains(msg, "cpu") || string.contains(msg, "load"):
    category = "cpu"
    confidence = 75
    patterns_matched = patterns_matched + 1
```

Return `{category: category, confidence: confidence, patterns_matched: patterns_matched}`

#### `correlate_alerts(alerts, window_seconds)`
Returns array of arrays (groups of correlated alerts).

Algorithm:
1. If alerts is empty, return []
2. Sort alerts by timestamp (use index-based iteration and compare timestamps)
3. Initialize groups = [], current_group = [first alert]
4. For each subsequent alert:
   - Get service from alert tags (first tag, or "unknown" if tags empty)
   - Get previous alert's service
   - If timestamp difference <= window_seconds AND same service: add to current_group
   - Else: push current_group to groups, start new current_group
5. Push final current_group to groups
6. Return groups

#### `deduplicate_alerts(alerts)`
Returns array with duplicates removed.

Algorithm:
1. Initialize result = [], seen = []
2. For each alert:
   - Build key = alert.get("message") + "|" + string(alert.get("source"))
   - Check if any item in seen has same key AND timestamp within 60 seconds
   - If not seen: push alert to result, push {key, timestamp} to seen
3. Return result

#### `build_alert_pipeline(raw_lines)`
Returns array of dicts, each with {alert, classification}.

Uses the pipeline operator `|>`:
```naab
fn parse_to_alert(raw_line) {
    let parsed = parse_log_line(raw_line)
    let sev = if parsed.get("level") == "ERROR" { models.Severity.Critical }
              else if parsed.get("level") == "WARN" { models.Severity.High }
              else { models.Severity.Medium }
    let alert = models.create_alert(
        "pipeline-" + string(math.random(1000, 9999)),
        models.AlertSource.Prometheus,
        parsed.get("message") ?? raw_line,
        sev,
        0,
        [parsed.get("service") ?? "unknown"],
        raw_line
    )
    return alert
}

fn classify_single(alert) {
    let classification = classify_alert(alert)
    return {"alert": alert, "classification": classification}
}
```

For each raw_line: `raw_line |> parse_to_alert |> classify_single`
Collect results into array and return.

#### `score_alert_urgency(alert, active_incidents)`
Returns int 0-100.

Python polyglot with `-> JSON`:
```
let result = <<python[alert, active_incidents] -> JSON
import json
severity = alert.get("severity", 3)  # default Low
base_scores = {0: 80, 1: 60, 2: 40, 3: 20}
score = base_scores.get(severity, 20)

# Check if any active incident has same service
alert_tags = alert.get("tags", [])
alert_service = alert_tags[0] if alert_tags else "unknown"
for inc in active_incidents:
    inc_alerts = inc.get("alerts", [])
    for ia in inc_alerts:
        ia_tags = ia.get("tags", [])
        if ia_tags and ia_tags[0] == alert_service:
            score += 10
            break
    break

# Category bonus
msg = alert.get("message", "").lower()
if "oom" in msg or "memory" in msg or "5xx" in msg or "503" in msg:
    score += 10

score = min(score, 100)
json.dumps({"score": score})
>>
```
Return `int(result.get("score"))`.

### main {} block
```naab
main {}
```

---

## Module 4: incidents.naab — Incident Lifecycle

Requires: `use uuid`, `use time`, `use json`
Imports: `import "models.naab" as models`

### Exported Functions

#### `open_incident(title, severity, alerts, sla_policies)`
Returns dict: `{incident, sla_deadline_minutes}`

```
let inc_id = uuid.v4()
let corr_id = uuid.v4()
let now = time.now()

// Find matching SLA policy
let deadline_minutes = 0
let sla_deadline = 0
for policy in sla_policies {
    if policy.get("severity") == severity {
        deadline_minutes = policy.get("response_minutes")
        sla_deadline = now + deadline_minutes * 60
        break
    }
}

let incident = models.create_incident(inc_id, title, severity, alerts, sla_deadline)
// Override correlation_id (create_incident sets it to "")
let inc_dict = {
    "id": inc_id,
    "title": title,
    "severity": severity,
    "status": models.IncidentStatus.Open,
    "alerts": alerts,
    "responder_id": "",
    "created_at": now,
    "updated_at": now,
    "resolved_at": 0,
    "sla_deadline": sla_deadline,
    "correlation_id": corr_id
}
return {"incident": inc_dict, "sla_deadline_minutes": deadline_minutes}
```

#### `acknowledge_incident(incident, responder_id)`
Returns dict (updated incident fields).

If `incident.get("status") != models.IncidentStatus.Open`: return incident unchanged.
Otherwise: set status = Acknowledged, responder_id, updated_at = time.now().

**Value semantics:** Build a new dict or copy-and-modify. Example:
```naab
let result = {
    "id": incident.get("id"),
    "title": incident.get("title"),
    "severity": incident.get("severity"),
    "status": models.IncidentStatus.Acknowledged,
    "alerts": incident.get("alerts"),
    "responder_id": responder_id,
    "created_at": incident.get("created_at"),
    "updated_at": time.now(),
    "resolved_at": incident.get("resolved_at"),
    "sla_deadline": incident.get("sla_deadline"),
    "correlation_id": incident.get("correlation_id")
}
return result
```

#### `start_investigation(incident)`
If status != Acknowledged: return incident unchanged.
Set status = Investigating, updated_at = time.now().

#### `resolve_incident(incident, resolution_notes)`
If status != Investigating: return incident unchanged.
Set status = Resolved, resolved_at = time.now(), updated_at = time.now().

#### `close_incident(incident)`
If status != Resolved: return incident unchanged.
Set status = Closed, updated_at = time.now().

#### `merge_incidents(primary, secondary)`
Returns dict: `{merged, absorbed_count}`
```
let combined_alerts = primary.get("alerts") + secondary.get("alerts")
let absorbed = len(secondary.get("alerts"))
// Build merged incident dict with combined alerts
let merged = { ... primary fields ..., "alerts": combined_alerts }
return {"merged": merged, "absorbed_count": absorbed}
```

#### `get_incidents_by_status(incidents, status)`
Returns filtered array:
```naab
let result = []
for inc in incidents {
    if inc.get("status") == status {
        result.push(inc)
    }
}
return result
```

#### `calculate_mttr(incidents)`
Returns float (hours). Mean Time To Resolve for resolved incidents.

Python polyglot:
```
let mttr = <<python[incidents] -> JSON
import json
resolved = [i for i in incidents if i.get("status") == 3]  # Resolved=3
if not resolved:
    json.dumps({"mttr": 0.0})
else:
    total_seconds = sum(i["resolved_at"] - i["created_at"] for i in resolved)
    avg_hours = (total_seconds / len(resolved)) / 3600.0
    json.dumps({"mttr": round(avg_hours, 2)})
>>
```
Return `result.get("mttr")`.

### main {} block
```naab
main {}
```

---

## Module 5: responders.naab — On-Call Management

Requires: `use validate`, `use math`
Imports: `import "models.naab" as models`

### Exported Functions

#### `register_responder(roster, responder)`
Returns dict: `{roster, success}` (+ optional error key)

```
// Validate email
let email = responder.get("email")
if !validate.email(email) {
    return {"roster": roster, "success": false, "error": "invalid email"}
}

// Check duplicate
for r in roster {
    if r.get("id") == responder.get("id") {
        return {"roster": roster, "success": false, "error": "duplicate id"}
    }
}

// Add to roster
roster.push(responder)
return {"roster": roster, "success": true}
```

#### `find_best_responder(roster, severity, required_skills)`
Returns dict: `{responder, score, reason}`

```
let best = null
let best_score = -1
let best_reason = "no available responder"

for r in roster {
    if !r.get("on_call") { continue }
    if r.get("current_load") >= r.get("max_load") { continue }

    let score = 0

    // On-call bonus (doubled for Critical)
    if severity == models.Severity.Critical {
        score = score + 60
    } else {
        score = score + 30
    }

    // Low load bonus
    let load_ratio = r.get("current_load") / r.get("max_load")
    if load_ratio < 0.5 {
        score = score + 20
    }

    // Skills overlap
    let r_skills = r.get("skills")
    for skill in required_skills {
        if array.contains(r_skills, skill) {
            score = score + 10
        }
    }

    if score > best_score {
        best = r
        best_score = score
        best_reason = "on-call, skills match"
    }
}

if best == null {
    return {"responder": null, "score": 0, "reason": "no available responder"}
}
return {"responder": best, "score": best_score, "reason": best_reason}
```

#### `assign_responder(roster, responder_id, incident_id)`
Returns dict: `{roster, responder, success}`

Find responder by id. Increment current_load. **Value semantics — must re-assign in roster!**
```
let found = null
for i in 0..len(roster) {
    let r = roster[i]
    if r.get("id") == responder_id {
        let new_load = r.get("current_load") + 1
        r["current_load"] = new_load
        roster[i] = r   // VALUE SEMANTICS: re-assign!
        found = r
        break
    }
}
if found == null {
    return {"roster": roster, "responder": null, "success": false}
}
return {"roster": roster, "responder": found, "success": true}
```

#### `release_responder(roster, responder_id)`
Returns dict: `{roster, success}`

Find responder by id. Decrement current_load (minimum 0). Re-assign in roster.
```
for i in 0..len(roster) {
    let r = roster[i]
    if r.get("id") == responder_id {
        let new_load = int(math.max(0, r.get("current_load") - 1))
        r["current_load"] = new_load
        roster[i] = r
        return {"roster": roster, "success": true}
    }
}
return {"roster": roster, "success": false}
```

#### `get_on_call(roster)`
Returns array of responders where on_call == true.
```
let result = []
for r in roster {
    if r.get("on_call") == true {
        result.push(r)
    }
}
return result
```

#### `calculate_responder_load(responder)`
Returns dict: `{load_percent, available_capacity, overloaded}`
```
let current = responder.get("current_load")
let max_l = responder.get("max_load")
let load_pct = if max_l > 0 { int((current * 100) / max_l) } else { 100 }
let avail = if max_l > current { max_l - current } else { 0 }
let over = current >= max_l
return {"load_percent": load_pct, "available_capacity": avail, "overloaded": over}
```

#### `validate_contact_info(responder)`
Returns dict: `{valid, errors}`
```
let errors = []
let email = responder.get("email")
if !validate.email(email) {
    errors.push("invalid email: " + string(email))
}
return {"valid": len(errors) == 0, "errors": errors}
```

### main {} block
```naab
main {}
```

---

## Module 6: escalation.naab — Escalation Engine

Requires: `use uuid`, `use time`, `use array`
Imports: `import "models.naab" as models`

### Exported Functions

#### `check_escalation_needed(incident, sla_policies, current_time)`
Returns dict: `{needed, reason, target_level}`

```
let created = incident.get("created_at")
let elapsed = current_time - created
let status = incident.get("status")
let severity = incident.get("severity")

// Find matching SLA policy
let policy = null
for p in sla_policies {
    if p.get("severity") == severity {
        policy = p
        break
    }
}
if policy == null {
    return {"needed": false, "reason": "no matching SLA policy", "target_level": models.EscalationLevel.L1}
}

let response_limit = policy.get("response_minutes") * 60
let resolution_limit = policy.get("resolution_minutes") * 60

// Check resolution breach (more severe)
if elapsed > resolution_limit && status != models.IncidentStatus.Resolved && status != models.IncidentStatus.Closed {
    return {"needed": true, "reason": "resolution SLA breached", "target_level": models.EscalationLevel.L3}
}

// Check response breach
if elapsed > response_limit && status == models.IncidentStatus.Open {
    return {"needed": true, "reason": "response SLA breached", "target_level": models.EscalationLevel.L2}
}

return {"needed": false, "reason": "within SLA", "target_level": models.EscalationLevel.L1}
```

#### `escalate_incident(incident, escalations, from_level, to_level, reason)`
Returns dict: `{incident, escalation, escalations}`

```
let esc_id = uuid.v4()
let esc = new models.Escalation {
    id: esc_id,
    incident_id: incident.get("id"),
    from_level: from_level,
    to_level: to_level,
    reason: reason,
    escalated_at: time.now()
}
escalations.push(esc)

// If escalating to L3 or Executive, bump severity to Critical
let updated_incident = incident
if to_level == models.EscalationLevel.L3 || to_level == models.EscalationLevel.Executive {
    updated_incident = {
        "id": incident.get("id"),
        "title": incident.get("title"),
        "severity": models.Severity.Critical,
        "status": incident.get("status"),
        "alerts": incident.get("alerts"),
        "responder_id": incident.get("responder_id"),
        "created_at": incident.get("created_at"),
        "updated_at": time.now(),
        "resolved_at": incident.get("resolved_at"),
        "sla_deadline": incident.get("sla_deadline"),
        "correlation_id": incident.get("correlation_id")
    }
}

return {"incident": updated_incident, "escalation": esc, "escalations": escalations}
```

#### `get_escalation_chain(incident_id, escalations)`
Returns array of escalations matching incident_id.
```
let chain = []
for esc in escalations {
    if esc.get("incident_id") == incident_id {
        chain.push(esc)
    }
}
return chain
```

#### `traverse_dependencies(service_graph, root_service, visited)`
**RECURSIVE function.** Returns array of all dependent services reachable from root_service.

```naab
export fn traverse_dependencies(service_graph, root_service, visited) {
    // Base case: not in graph
    if !service_graph.has(root_service) {
        return []
    }
    // Base case: already visited (cycle detection)
    if array.contains(visited, root_service) {
        return []
    }

    visited.push(root_service)
    let deps = service_graph.get(root_service)
    let all_affected = []

    for dep in deps {
        if !array.contains(visited, dep) {
            all_affected.push(dep)
            let sub = traverse_dependencies(service_graph, dep, visited)
            for s in sub {
                if !array.contains(all_affected, s) {
                    all_affected.push(s)
                }
            }
        }
    }

    return all_affected
}
```

#### `calculate_blast_radius(service_graph, failed_service)`
Returns dict: `{affected_services, depth, critical_path}`

```
let visited = []
let affected = traverse_dependencies(service_graph, failed_service, visited)
let depth = 0
// Calculate depth by counting levels in BFS-like fashion
// Simple approach: depth = how many "hops" the deepest affected service is
let current_level = [failed_service]
let seen = [failed_service]
let d = 0
while len(current_level) > 0 {
    let next_level = []
    for svc in current_level {
        let neighbors = service_graph.get(svc) ?? []
        for n in neighbors {
            if !array.contains(seen, n) {
                next_level.push(n)
                seen.push(n)
            }
        }
    }
    if len(next_level) > 0 {
        d = d + 1
    }
    current_level = next_level
}

return {
    "affected_services": affected,
    "depth": d,
    "critical_path": affected
}
```

#### `auto_escalate_batch(incidents, sla_policies, escalations, current_time)`
Returns dict: `{escalated, escalations, summary}`

```
let escalated_count = 0
let summary = []
for i in 0..len(incidents) {
    let inc = incidents[i]
    let check = check_escalation_needed(inc, sla_policies, current_time)
    if check.get("needed") == true {
        let target = check.get("target_level")
        let esc_result = escalate_incident(inc, escalations, models.EscalationLevel.L1, target, check.get("reason"))
        escalations = esc_result.get("escalations")
        incidents[i] = esc_result.get("incident")
        escalated_count = escalated_count + 1
        summary.push({"incident_id": inc.get("id"), "target_level": target})
    }
}
return {"escalated": escalated_count, "escalations": escalations, "summary": summary}
```

### main {} block
```naab
main {}
```

---

## Module 7: sla.naab — SLA Computation

Requires: `use math`, `use time`, `use json`
Imports: `import "models.naab" as models`

### Exported Functions

#### `check_sla_compliance(incident, sla_policy, current_time)`
Returns dict: `{compliant, response_compliant, resolution_compliant, time_remaining_minutes, breach_severity}`

```
let created = incident.get("created_at")
let response_limit = sla_policy.get("response_minutes") * 60
let resolution_limit = sla_policy.get("resolution_minutes") * 60

// Response compliance: was incident acknowledged within response window?
let ack_time = incident.get("updated_at")  // approximation: updated_at is set on acknowledge
let status = incident.get("status")
let response_elapsed = if status != models.IncidentStatus.Open { ack_time - created } else { current_time - created }
let response_ok = response_elapsed <= response_limit

// Resolution compliance
let resolved_at = incident.get("resolved_at")
let resolution_elapsed = if resolved_at > 0 { resolved_at - created } else { current_time - created }
let resolution_ok = resolution_elapsed <= resolution_limit

// Time remaining
let remaining_seconds = (created + resolution_limit) - current_time
let remaining_minutes = int(math.floor(remaining_seconds / 60))

let is_compliant = response_ok && resolution_ok
let breach_sev = if !is_compliant { models.severity_name(incident.get("severity")) } else { "none" }

return {
    "compliant": is_compliant,
    "response_compliant": response_ok,
    "resolution_compliant": resolution_ok,
    "time_remaining_minutes": remaining_minutes,
    "breach_severity": breach_sev
}
```

#### `calculate_sla_metrics(incidents, sla_policies)`
Returns dict: `{total, compliant_count, breach_count, compliance_rate, avg_response_minutes, avg_resolution_minutes}`

Python polyglot with `-> JSON`:
```
let now = time.now()
let metrics = <<python[incidents, sla_policies, now] -> JSON
import json
total = len(incidents)
if total == 0:
    json.dumps({"total": 0, "compliant_count": 0, "breach_count": 0,
                "compliance_rate": 0.0, "avg_response_minutes": 0.0, "avg_resolution_minutes": 0.0})
else:
    compliant = 0
    response_times = []
    resolution_times = []
    for inc in incidents:
        sev = inc.get("severity", 3)
        policy = None
        for p in sla_policies:
            if p.get("severity") == sev:
                policy = p
                break
        if policy is None:
            continue
        created = inc.get("created_at", 0)
        resolved = inc.get("resolved_at", 0)
        resp_limit = policy.get("response_minutes", 60) * 60
        res_limit = policy.get("resolution_minutes", 180) * 60
        resp_elapsed = (inc.get("updated_at", now) - created) if inc.get("status", 0) != 0 else (now - created)
        res_elapsed = (resolved - created) if resolved > 0 else (now - created)
        response_times.append(resp_elapsed / 60.0)
        resolution_times.append(res_elapsed / 60.0)
        if resp_elapsed <= resp_limit and res_elapsed <= res_limit:
            compliant += 1
    breach = total - compliant
    rate = round(compliant / total, 2) if total > 0 else 0.0
    avg_resp = round(sum(response_times) / len(response_times), 2) if response_times else 0.0
    avg_res = round(sum(resolution_times) / len(resolution_times), 2) if resolution_times else 0.0
    json.dumps({"total": total, "compliant_count": compliant, "breach_count": breach,
                "compliance_rate": rate, "avg_response_minutes": avg_resp, "avg_resolution_minutes": avg_res})
>>
return metrics
```

#### `predict_breach_risk(incident, sla_policy, current_time)`
Returns dict: `{risk_score, risk_level, estimated_resolution_minutes}`

Python polyglot with `-> JSON`:
```
let prediction = <<python[incident, sla_policy, current_time] -> JSON
import json
created = incident.get("created_at", 0)
elapsed = current_time - created
total_allowed = sla_policy.get("resolution_minutes", 60) * 60
ratio = elapsed / total_allowed if total_allowed > 0 else 1.0
risk_score = min(int(ratio * 100), 100)
if risk_score >= 80:
    risk_level = "critical"
elif risk_score >= 60:
    risk_level = "high"
elif risk_score >= 40:
    risk_level = "medium"
else:
    risk_level = "low"
est_remaining = max(0, (total_allowed - elapsed)) / 60.0
json.dumps({"risk_score": risk_score, "risk_level": risk_level, "estimated_resolution_minutes": round(est_remaining, 1)})
>>
return prediction
```

#### `generate_sla_summary(incidents, sla_policies)`
Returns dict: `{by_severity, overall_compliance, worst_breach}`

Uses pipeline pattern:
```naab
fn group_by_severity(incidents) {
    let groups = {}
    for inc in incidents {
        let sev_name = models.severity_name(inc.get("severity"))
        let existing = groups.get(sev_name) ?? []
        existing.push(inc)
        groups[sev_name] = existing
    }
    return groups
}

fn compute_compliance(grouped) {
    // ... compute per-group compliance rates
}
```

Use `|>` to chain: `incidents |> group_by_severity |> compute_compliance_per_group`

Returns:
- by_severity: dict of severity_name -> {count, compliant_count, compliance_rate}
- overall_compliance: float (total compliant / total)
- worst_breach: severity name with lowest compliance rate, or "none"

#### `get_breach_timeline(incidents, sla_policies)`
Returns array of dicts: `{incident_id, severity, breach_type, breached_at_minutes}`
For each incident that breached SLA, compute when the breach occurred (elapsed minutes).
Sort by breached_at_minutes ascending.

### main {} block
```naab
main {}
```

---

## Module 8: reports.naab — Report Generation

Requires: `use json`, `use csv`, `use crypto`, `use file`, `use time`, `use string`
Imports: `import "models.naab" as models`, `import "validators.naab" as validators`

### Exported Functions

#### `generate_incident_report(incident, escalations, responder)`
Returns dict: `{report_text, hash, sections}`

Build report_text as a multi-line string:
```
let title = incident.get("title")
let sev = models.severity_name(incident.get("severity"))
let status = models.status_name(incident.get("status"))
let resp_name = if responder != null { responder.get("name") } else { "Unassigned" }

let text = "Incident Report: " + title + "\n"
text = text + "Severity: " + sev + "\n"
text = text + "Status: " + status + "\n"
text = text + "Responder: " + resp_name + "\n"
text = text + "Escalations: " + string(len(escalations)) + "\n"

let hash = crypto.sha256(text)
return {"report_text": text, "hash": hash, "sections": ["summary", "timeline", "escalations", "responder"]}
```

#### `generate_postmortem(incident, root_cause, timeline_events, action_items)`
Returns dict: `{postmortem, report_text}`

```
let pm = {
    "id": "PM-" + string(time.now()),
    "incident_id": incident.get("id"),
    "root_cause": root_cause,
    "timeline": timeline_events,
    "action_items": action_items,
    "created_by": "system"
}

let text = "Post-Mortem: " + incident.get("title") + "\n"
text = text + "Root Cause: " + root_cause + "\n"
text = text + "Action Items: " + string(len(action_items)) + "\n"
for item in action_items {
    text = text + "- " + item + "\n"
}

return {"postmortem": pm, "report_text": text}
```

#### `export_incidents_csv(incidents, headers)`
Returns string (CSV formatted).

```
let rows = []
for inc in incidents {
    let row = []
    for h in headers {
        let val = inc.get(h) ?? ""
        row.push(string(val))
    }
    rows.push(row)
}
return csv.stringify(rows, headers)
```

#### `generate_dashboard_data(incidents, responders, sla_policies)`
Returns dict: `{open_count, mttr, sla_compliance, by_severity, by_status}`

```
// Count open incidents (Open, Acknowledged, Investigating)
let open_count = 0
let resolved_times = []
for inc in incidents {
    let s = inc.get("status")
    if s == models.IncidentStatus.Open || s == models.IncidentStatus.Acknowledged || s == models.IncidentStatus.Investigating {
        open_count = open_count + 1
    }
    if s == models.IncidentStatus.Resolved || s == models.IncidentStatus.Closed {
        let ra = inc.get("resolved_at")
        let ca = inc.get("created_at")
        if ra > 0 && ca > 0 {
            resolved_times.push(ra - ca)
        }
    }
}

// MTTR
let mttr = 0.0
if len(resolved_times) > 0 {
    let total = 0
    for t in resolved_times { total = total + t }
    mttr = (total / len(resolved_times)) / 3600.0
}

// By severity
let by_severity = {}
for inc in incidents {
    let sev_name = models.severity_name(inc.get("severity"))
    let count = by_severity.get(sev_name) ?? 0
    by_severity[sev_name] = count + 1
}

// By status
let by_status = {}
for inc in incidents {
    let st_name = models.status_name(inc.get("status"))
    let count = by_status.get(st_name) ?? 0
    by_status[st_name] = count + 1
}

// SLA compliance (simplified)
let sla_compliance = if len(incidents) > 0 { 1.0 } else { 0.0 }

return {
    "open_count": open_count,
    "mttr": mttr,
    "sla_compliance": sla_compliance,
    "by_severity": by_severity,
    "by_status": by_status
}
```

#### `build_audit_trail(events)`
Returns dict: `{trail, hash_chain}`

```
let trail = []
let hash_chain = []
let prev_hash = "genesis"

for event in events {
    let ts = time.now()
    let hash_input = event + prev_hash
    let hash = crypto.sha256(hash_input)
    trail.push({"event": event, "timestamp": ts, "hash": hash})
    hash_chain.push(hash)
    prev_hash = hash
}

return {"trail": trail, "hash_chain": hash_chain}
```

#### `write_report_to_file(report_text, filepath)`
Returns dict: `{success, path, bytes_written}`

**CRITICAL: Must sanitize before file.write to pass taint tracking!**
```
let clean = validators.sanitize_string(report_text)
try {
    file.write(filepath, clean)
    return {"success": true, "path": filepath, "bytes_written": len(clean)}
} catch (e) {
    return {"success": false, "path": filepath, "bytes_written": 0}
}
```

### main {} block
```naab
main {}
```

---

## Module 9: rca.naab — Agent-Based Root Cause Analysis

Requires: `use agent`, `use json`, `use string`
Imports: `import "models.naab" as models`

### Exported Functions

#### `check_agent_available(config_name)`
Returns dict: `{available, reason}`
```
let result = agent.check(config_name)
let is_valid = result.get("valid")
let err = result.get("error") ?? "ready"
return {"available": is_valid == true, "reason": err}
```

#### `generate_rca_prompt(incident, logs)`
Returns string.
```
let sev_name = models.severity_name(incident.get("severity"))
let prompt = "Analyze this incident:\n"
prompt = prompt + "Title: " + string(incident.get("title")) + "\n"
prompt = prompt + "Severity: " + sev_name + "\n"
prompt = prompt + "Status: " + models.status_name(incident.get("status")) + "\n"
prompt = prompt + "\nRelated logs:\n"
for log_line in logs {
    prompt = prompt + "  " + string(log_line) + "\n"
}
prompt = prompt + "\nWhat is the most likely root cause? Respond with: root_cause, confidence (0-100), reasoning, and suggestions."
return prompt
```

#### `analyze_root_cause(incident, logs, service_graph)`
Returns dict: `{root_cause, confidence, reasoning, suggestions}`

```naab
export fn analyze_root_cause(incident, logs, service_graph) {
    // Pre-flight check
    let avail = check_agent_available("rca_agent")
    if avail.get("available") != true {
        return {
            "root_cause": "unavailable",
            "confidence": 0,
            "reasoning": "agent not configured: " + string(avail.get("reason")),
            "suggestions": []
        }
    }

    try {
        let handle = agent.create("rca_agent")

        // Turn 1: Send incident context
        let prompt = generate_rca_prompt(incident, logs)
        let resp1 = agent.send(handle, prompt)
        let content = resp1.get("content") ?? ""

        // Turn 2: Send service graph for dependency context
        let graph_str = json.stringify(service_graph)
        let follow_up = "Here is the service dependency graph: " + graph_str + "\nDoes this change your analysis? What is the final root cause?"
        let resp2 = agent.send(handle, follow_up)
        let final_content = resp2.get("content") ?? content

        return {
            "root_cause": final_content,
            "confidence": 1,
            "reasoning": "agent analysis complete",
            "suggestions": ["review service dependencies", "check resource limits"]
        }
    } catch (e) {
        return {
            "root_cause": "unavailable",
            "confidence": 0,
            "reasoning": "agent error: " + string(e),
            "suggestions": []
        }
    }
}
```

#### `batch_analyze(incidents, logs)`
Returns array of RCA result dicts.

```naab
export fn batch_analyze(incidents, logs) {
    let results = []
    let agent_configs = ["rca_agent", "rca_agent_b1", "rca_agent_b2"]

    // Create handles for up to 3 agents
    let handles = []
    let messages = []
    let valid_indices = []

    for i in 0..len(incidents) {
        if i >= len(agent_configs) { break }
        let config_name = agent_configs[i]
        let avail = check_agent_available(config_name)
        if avail.get("available") == true {
            try {
                let h = agent.create(config_name)
                handles.push(h)
                let prompt = generate_rca_prompt(incidents[i], logs)
                messages.push(prompt)
                valid_indices.push(i)
            } catch (e) {
                results.push({
                    "root_cause": "unavailable",
                    "confidence": 0,
                    "reasoning": "agent creation failed: " + string(e),
                    "suggestions": []
                })
            }
        } else {
            results.push({
                "root_cause": "unavailable",
                "confidence": 0,
                "reasoning": "agent not available",
                "suggestions": []
            })
        }
    }

    // Batch send
    if len(handles) > 0 {
        try {
            let responses = agent.batch(handles, messages)
            for j in 0..len(responses) {
                let resp = responses[j]
                let content = resp.get("content") ?? ""
                let success = resp.get("success")
                if success == false {
                    results.push({
                        "root_cause": "unavailable",
                        "confidence": 0,
                        "reasoning": resp.get("error") ?? "batch call failed",
                        "suggestions": []
                    })
                } else {
                    results.push({
                        "root_cause": content,
                        "confidence": 1,
                        "reasoning": "batch analysis complete",
                        "suggestions": []
                    })
                }
            }
        } catch (e) {
            for idx in valid_indices {
                results.push({
                    "root_cause": "unavailable",
                    "confidence": 0,
                    "reasoning": "batch error: " + string(e),
                    "suggestions": []
                })
            }
        }
    }

    // Fill remaining with unavailable
    while len(results) < len(incidents) {
        results.push({
            "root_cause": "unavailable",
            "confidence": 0,
            "reasoning": "no agent available",
            "suggestions": []
        })
    }

    return results
}
```

### main {} block
```naab
main {}
```

---

## Module 10: main.naab — Test Orchestrator

Imports all 9 modules:
```naab
import "models.naab" as models
import "validators.naab" as validators
import "alerts.naab" as alerts
import "incidents.naab" as incidents
import "responders.naab" as responders
import "escalation.naab" as escalation
import "sla.naab" as sla
import "reports.naab" as reports
import "rca.naab" as rca
```

Requires: `use uuid`, `use time`, `use math`, `use json`, `use agent`, `use array`, `use file`

### Test Functions

Each test function follows this pattern:
```naab
fn test_models() {
    let passed = 0
    let total = 0

    // Test 1: ...
    total = total + 1
    if condition { passed = passed + 1 }

    // ... 10 tests total

    return [passed, total]
}
```

### main {} block

```naab
main {
    let grand_passed = 0
    let grand_total = 0

    let r1 = test_models()
    print("test_models: " + string(r1[0]) + "/" + string(r1[1]))
    grand_passed = grand_passed + r1[0]
    grand_total = grand_total + r1[1]

    let r2 = test_alerts()
    print("test_alerts: " + string(r2[0]) + "/" + string(r2[1]))
    grand_passed = grand_passed + r2[0]
    grand_total = grand_total + r2[1]

    let r3 = test_incidents()
    print("test_incidents: " + string(r3[0]) + "/" + string(r3[1]))
    grand_passed = grand_passed + r3[0]
    grand_total = grand_total + r3[1]

    let r4 = test_responders()
    print("test_responders: " + string(r4[0]) + "/" + string(r4[1]))
    grand_passed = grand_passed + r4[0]
    grand_total = grand_total + r4[1]

    let r5 = test_escalation()
    print("test_escalation: " + string(r5[0]) + "/" + string(r5[1]))
    grand_passed = grand_passed + r5[0]
    grand_total = grand_total + r5[1]

    let r6 = test_sla()
    print("test_sla: " + string(r6[0]) + "/" + string(r6[1]))
    grand_passed = grand_passed + r6[0]
    grand_total = grand_total + r6[1]

    let r7 = test_reports()
    print("test_reports: " + string(r7[0]) + "/" + string(r7[1]))
    grand_passed = grand_passed + r7[0]
    grand_total = grand_total + r7[1]

    let r8 = test_validators()
    print("test_validators: " + string(r8[0]) + "/" + string(r8[1]))
    grand_passed = grand_passed + r8[0]
    grand_total = grand_total + r8[1]

    let r9 = test_rca()
    print("test_rca: " + string(r9[0]) + "/" + string(r9[1]))
    grand_passed = grand_passed + r9[0]
    grand_total = grand_total + r9[1]

    print("")
    print("TOTAL: " + string(grand_passed) + "/" + string(grand_total))
}
```

---

## Test Data Constants

Use these exact values in tests for reproducible results:

### Timestamps
- Base time: use `time.now()` for current-time tests
- For MTTR tests: use fixed timestamps:
  - incident1: created_at = 1000, resolved_at = 4600 (1 hour = 3600s)
  - incident2: created_at = 2000, resolved_at = 9200 (2 hours = 7200s)
  - MTTR = (3600 + 7200) / 2 / 3600 = 1.5 hours

### SLA Policies (create in test setup)
```naab
let critical_sla = models.create_sla_policy("SLA-C", "Critical SLA", models.Severity.Critical, 15, 60)
let high_sla = models.create_sla_policy("SLA-H", "High SLA", models.Severity.High, 30, 120)
let medium_sla = models.create_sla_policy("SLA-M", "Medium SLA", models.Severity.Medium, 60, 180)
let low_sla = models.create_sla_policy("SLA-L", "Low SLA", models.Severity.Low, 120, 300)
let sla_policies = [critical_sla, high_sla, medium_sla, low_sla]
```

### Service Dependency Graph (for escalation tests)
```naab
let service_graph = {
    "api-gateway": ["auth-service", "user-service"],
    "auth-service": ["database", "cache"],
    "user-service": ["database"],
    "database": [],
    "cache": []
}
```
- traverse from "api-gateway" -> ["auth-service", "user-service", "database", "cache"] (4 services)
- depth = 2

### Log Lines (for alert tests)
```naab
let log_lines = [
    "2024-01-15T10:30:00 [ERROR] api: OOM killed process 1234",
    "2024-01-15T10:30:05 [WARN] cache: High memory usage 95%",
    "2024-01-15T10:31:00 [ERROR] database: Connection timeout after 30s"
]
```

### Responder Roster (for responder tests)
```naab
let alice = models.create_responder("R1", "Alice", "alice@example.com", models.ResponderRole.SRE, true, 5, ["kubernetes", "networking"])
let bob = models.create_responder("R2", "Bob", "bob@example.com", models.ResponderRole.Engineer, true, 3, ["database", "python"])
let charlie = models.create_responder("R3", "Charlie", "charlie@example.com", models.ResponderRole.Manager, false, 2, ["management"])
```

---

## Critical Gotchas for Implementation

1. **No top-level `let`/`const`** — all variables inside `main {}` or functions
2. **`new` required for structs** — `new Alert { id: "A1", ... }` not `Alert { id: "A1" }`
3. **Cross-module enums** — `models.Severity.Critical` (3-level dot access)
4. **Cross-module structs** — `new models.Alert { ... }` or use factory function `models.create_alert(...)`
5. **`dict.get("key")` not `dict["key"]`** — bracket access THROWS, HARD blocked by scanner
6. **Variable binding on ALL polyglot blocks** — `<<python[var1, var2]` or `<<python[]` or `<<shell[]`
7. **No `return` in Python blocks** — last expression is the return value
8. **Value semantics in loops** — `roster[i] = r` after mutating `r`
9. **`regex.search()` not `regex.match()`** — match/test don't exist
10. **`validate.email()` not `string.match()`** — use the validate module
11. **Pipeline `|>` needs single-arg functions** — create wrappers for multi-arg
12. **Taint: sanitize before file.write** — call `validators.sanitize_string()` before every `file.write()`
13. **`use debug` causes error** — debug is auto-imported, never write `use debug`
14. **Python `-> JSON` returns NAAb dict** — don't double-parse with `json.parse()`
15. **`||` returns boolean** — use `??` for null coalesce: `x ?? "default"`
16. **`catch (e)` needs parens** — `catch e` without parens is a syntax error
17. **`else if` not `elif`** — elif does not exist in NAAb
18. **`export` each item individually** — no batch `export { ... }` syntax
19. **Import paths relative to file** — `import "models.naab"` not `import "src/models.naab"`
20. **`time.format_timestamp()` not `time.format()`** — function name includes `_timestamp`
