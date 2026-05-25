# Compliance Rule Engine — Full Specification

## Overview

Build a Compliance Rule Engine in NAAb. The engine manages regulatory rules with
complex condition trees, evaluates entities against those rules using recursive
evaluation, resolves rule dependencies via topological sort, detects circular
dependencies, computes risk scores, generates compliance reports, and uses LLM
agents for natural-language rule interpretation.

**All code goes in `.naab` files inside the `src/` directory.** Do NOT create standalone
`.py`, `.js`, or `.go` files. Polyglot blocks embed Python/Shell inside `.naab` files.

## File Structure

Create these 12 files in `src/`:

```
src/
├── main.naab          # Test orchestrator — 120 tests, 12 suites
├── models.naab        # Structs, enums, factory functions
├── rules.naab         # Rule parsing, validation, condition trees
├── conditions.naab    # Condition evaluation (recursive tree walker)
├── entities.naab      # Entity registry, attribute management
├── evaluator.naab     # Rule evaluation engine, batch processing
├── dependencies.naab  # Dependency resolution, cycle detection
├── risk.naab          # Risk scoring, portfolio analysis
├── reports.naab       # Report generation, file output (taint sink)
├── remediation.naab   # Remediation planning, effort estimation
├── validators.naab    # Sanitization, validation (taint clearing)
└── interpreter.naab   # Agent-based rule interpretation
```

**Import paths:** All files are in the same directory. Use `import "models.naab" as models`.
Do NOT use `import "src/models.naab"` — that looks for `src/src/models.naab`.

## Implementation Order

Build files in this order (each depends only on files above it):

1. `models.naab` — no dependencies
2. `validators.naab` — imports models
3. `conditions.naab` — imports models
4. `rules.naab` — imports models
5. `entities.naab` — imports models
6. `evaluator.naab` — imports models, conditions, rules
7. `dependencies.naab` — imports models
8. `risk.naab` — imports models
9. `reports.naab` — imports models, validators
10. `remediation.naab` — imports models
11. `interpreter.naab` — imports models
12. `main.naab` — imports all 11 modules

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
    Low,
    Info
}

export enum RuleStatus {
    Active,
    Draft,
    Deprecated,
    Suspended
}

export enum ComplianceResult {
    Pass,
    Fail,
    Skip,
    Error
}

export enum ConditionOp {
    Equals,
    NotEquals,
    GreaterThan,
    LessThan,
    GreaterOrEqual,
    LessOrEqual,
    Contains,
    Matches,
    And,
    Or,
    Not
}

export enum EntityType {
    Server,
    Database,
    Application,
    Network,
    User,
    Policy
}

export enum RemediationPriority {
    Immediate,
    High,
    Medium,
    Low,
    Deferred
}
```

### Structs (all exported)

```naab
export struct Rule {
    id: string
    name: string
    description: string
    severity: int        # Severity enum value
    status: int          # RuleStatus enum value
    category: string     # e.g., "access_control", "encryption", "audit"
    conditions: array    # array of Condition dicts (tree structure)
    dependencies: array  # array of rule IDs this rule depends on
    tags: array          # array of strings
    version: int
}

export struct Condition {
    op: int              # ConditionOp enum value
    field: string        # entity attribute name (leaf nodes)
    value: string        # comparison value (leaf nodes)
    children: array      # child Condition dicts (for And/Or/Not)
}

export struct Entity {
    id: string
    name: string
    entity_type: int     # EntityType enum value
    attributes: dict     # key-value pairs the rules evaluate against
    tags: array
    owner: string
    created_at: int
}

export struct Finding {
    rule_id: string
    entity_id: string
    result: int          # ComplianceResult enum value
    severity: int        # Severity enum value
    message: string
    evidence: string
    timestamp: int
}

export struct RemediationStep {
    id: string
    finding_id: string
    action: string
    priority: int        # RemediationPriority enum value
    estimated_hours: int
    blocked_by: array    # array of step IDs
    completed: bool
}
```

### Exported Functions

**Factory functions:**
- `create_rule(id, name, description, severity, category, conditions, dependencies, tags)` -> new Rule struct
  - Sets status=RuleStatus.Active, version=1
- `create_condition(op, field, value, children)` -> dict with {op, field, value, children}
  - Returns a dict (NOT struct) — conditions are nested dict trees
- `create_entity(id, name, entity_type, attributes, tags, owner)` -> new Entity struct
  - Sets created_at=time.now()
- `create_finding(rule_id, entity_id, result, severity, message, evidence)` -> new Finding struct
  - Sets timestamp=time.now()

**Name mappers (MUST use match expressions, not if/else chains):**
- `severity_name(sev)` -> string
  - MUST use match: `match sev { Severity.Critical => "Critical", Severity.High => "High", Severity.Medium => "Medium", Severity.Low => "Low", Severity.Info => "Info", _ => "Unknown" }`
- `status_name(status)` -> string
  - MUST use match: Active => "Active", Draft => "Draft", Deprecated => "Deprecated", Suspended => "Suspended"
- `result_name(result)` -> string
  - MUST use match: Pass => "Pass", Fail => "Fail", Skip => "Skip", Error => "Error"
- `op_name(op)` -> string
  - MUST use match for ALL 11 operators: Equals => "==", NotEquals => "!=", GreaterThan => ">", LessThan => "<", GreaterOrEqual => ">=", LessOrEqual => "<=", Contains => "contains", Matches => "matches", And => "AND", Or => "OR", Not => "NOT"
- `entity_type_name(et)` -> string
  - MUST use match: Server => "Server", Database => "Database", Application => "Application", Network => "Network", User => "User", Policy => "Policy"
- `priority_name(p)` -> string
  - MUST use match: Immediate => "Immediate", High => "High", Medium => "Medium", Low => "Low", Deferred => "Deferred"

**Validation:**
- `validate_rule(rule)` -> dict: {valid, errors}
  - Checks: id non-empty, name non-empty, category non-empty, conditions is array with len > 0
- `validate_entity(entity)` -> dict: {valid, errors}
  - Checks: id non-empty, name non-empty, owner non-empty

---

## Module 2: validators.naab — Sanitization & Validation

Requires: `use regex`, `use validate`
Imports: models.naab

**Exported Functions:**
- `sanitize_string(input)` -> string
  - If null: return ""
  - Strip `<` and `>`. Preserve `"`, `{`, `}`, `[`, `]`.
  - Trim whitespace.
- `sanitize_log_line(line)` -> string
  - Strip ANSI escape codes via regex. Then sanitize_string.
- `validate_rule_syntax(definition)` -> dict: {valid, errors}
  - definition is a dict with fields: id, name, category, severity, conditions
  - Required: id (non-empty string), name (non-empty), category (one of: "access_control", "encryption", "audit", "network", "data_protection", "authentication")
  - Severity must be valid enum value (0-4)
  - conditions must be non-empty array
  - errors = array of error strings
- `validate_email_address(email)` -> bool
- `validate_condition_tree(conditions)` -> dict: {valid, errors, depth}
  - RECURSIVE: validates each condition in the tree
  - Leaf nodes (Equals, NotEquals, etc.): field must be non-empty, value must be non-empty
  - Branch nodes (And, Or): children must be non-empty array
  - Not nodes: children must have exactly 1 element
  - depth = max depth of the tree
  - Returns {valid: false, errors: [...], depth} if any node invalid

---

## Module 3: conditions.naab — Condition Evaluation (Recursive)

Requires: `use regex`
Imports: models.naab

This is the core recursive evaluator. Each condition is a tree node — leaves compare
entity attributes, branches combine results with AND/OR/NOT.

**Exported Functions:**
- `evaluate_condition(condition, entity_attributes)` -> dict: {passed, evidence}
  - RECURSIVE FUNCTION — this is the heart of the engine
  - Uses match on condition.op to dispatch:
  - **Leaf operators** (Equals, NotEquals, GreaterThan, LessThan, GreaterOrEqual, LessOrEqual):
    - Get field value from entity_attributes: `entity_attributes.get(condition.field)`
    - If field is null: {passed: false, evidence: "missing field: " + field}
    - Compare based on op (use match expression to select comparison):
      - Equals: `string(attr_val) == string(condition.value)`
      - NotEquals: `string(attr_val) != string(condition.value)`
      - GreaterThan: `int(attr_val) > int(condition.value)`  (etc.)
    - evidence: field + " " + op_name(op) + " " + value + " -> " + result
  - **Contains**: `string.contains(string(attr_val), condition.value)`
  - **Matches**: `regex.matches(string(attr_val), condition.value)`
  - **And**: evaluate ALL children recursively. passed = all children passed.
    - evidence: join child evidences with " AND "
    - Short-circuit: if any child fails, stop and return false
  - **Or**: evaluate ALL children recursively. passed = any child passed.
    - evidence: join child evidences with " OR "
  - **Not**: evaluate single child. passed = !child.passed.
    - evidence: "NOT(" + child_evidence + ")"

- `count_conditions(condition)` -> int
  - RECURSIVE: counts total nodes in condition tree
  - Leaf node: return 1
  - Branch node: 1 + sum of count_conditions on each child

- `flatten_conditions(condition)` -> array
  - RECURSIVE: returns flat array of all leaf conditions in the tree
  - Branch nodes: recurse into children, concatenate results
  - Leaf nodes: return [condition]

- `max_depth(condition)` -> int
  - RECURSIVE: returns max depth of tree
  - Leaf: return 1
  - Branch: 1 + max(max_depth of each child)

---

## Module 4: rules.naab — Rule Parsing & Management

Requires: `use json`, `use regex`
Imports: models.naab

**Exported Functions:**
- `parse_rule_definition(text)` -> dict: {rule, valid, errors}
  - text is a JSON string defining a rule
  - Uses try/catch EXPRESSION to parse:
    ```
    let parsed = try { json.parse(text) } catch (e) { null }
    ```
  - If parsed is null: {rule: null, valid: false, errors: ["invalid JSON"]}
  - Extract fields: id, name, description, severity, category, conditions, dependencies, tags
  - Build conditions tree from parsed.conditions array (recursive dict structure)
  - Create Rule via models.create_rule()
  - If any required field missing: {rule: null, valid: false, errors: [...]}

- `parse_condition_tree(raw_conditions)` -> array of condition dicts
  - RECURSIVE: converts raw JSON arrays into condition dict trees
  - Each raw condition has: op (string), field, value, children
  - Maps op string to ConditionOp enum: use match expression
    - "==" => Equals, "!=" => NotEquals, ">" => GreaterThan, "<" => LessThan,
    - ">=" => GreaterOrEqual, "<=" => LessOrEqual,
    - "contains" => Contains, "matches" => Matches,
    - "AND" => And, "OR" => Or, "NOT" => Not
  - For branch nodes (And/Or/Not): recursively parse children array
  - Returns array of condition dicts

- `filter_rules_by_category(rules, category)` -> array
- `filter_rules_by_severity(rules, min_severity)` -> array
  - Returns rules with severity <= min_severity (Critical=0 is most severe)
- `filter_active_rules(rules)` -> array
  - Returns rules where status == RuleStatus.Active
- `get_rule_by_id(rules, id)` -> Rule or null
- `update_rule_status(rule, new_status)` -> dict (rule with updated status and version+1)
- `merge_rule_tags(rule, new_tags)` -> dict (rule with combined unique tags)
  - Pipeline: existing tags |> combine with new_tags |> deduplicate

---

## Module 5: entities.naab — Entity Registry

Requires: `use uuid`
Imports: models.naab

**Exported Functions:**
- `register_entity(registry, entity)` -> dict: {registry, success}
  - Checks for duplicate id. If duplicate: {registry, success: false, error: "duplicate id"}
  - Pushes entity to registry. Returns {registry, success: true}
- `get_entity_by_id(registry, id)` -> Entity or null
- `get_entities_by_type(registry, entity_type)` -> array
- `update_entity_attribute(registry, entity_id, key, value)` -> dict: {registry, success}
  - Finds entity by id. Updates attributes dict. Re-assigns in registry (value semantics!).
  - Returns {registry, success: true} or {registry, success: false} if not found
- `get_entity_attributes(entity)` -> dict
  - Returns entity.attributes
- `filter_entities_by_tag(registry, tag)` -> array
  - Returns entities whose tags array contains the given tag
- `count_entities_by_type(registry)` -> dict
  - Pipeline: registry |> group by entity_type |> count per group
  - Returns dict like {"Server": 3, "Database": 2, ...}

---

## Module 6: evaluator.naab — Rule Evaluation Engine

Requires: `use time`
Imports: models.naab, conditions.naab, rules.naab

This module ties rules to entities and produces findings.

**Exported Functions:**
- `evaluate_rule(rule, entity)` -> dict: {passed, rule_id, severity, message, evidence}
  - Checks rule.status == Active, else return {passed: true, ..., message: "rule not active", evidence: "skipped"}
  - For each condition in rule.conditions:
    - Call conditions.evaluate_condition(cond, entity.attributes)
    - ALL conditions must pass for the rule to pass
  - Use match expression for result classification:
    ```
    let result_msg = match severity_level {
        models.Severity.Critical => "CRITICAL violation",
        models.Severity.High => "High-severity violation",
        models.Severity.Medium => "Medium-severity issue",
        _ => "Low-priority finding"
    }
    ```
  - evidence = combined evidence from all condition evaluations

- `evaluate_entity(rules, entity)` -> dict: {entity_id, total_rules, passed, failed, findings}
  - Evaluates ALL active rules against this entity
  - findings = array of Finding dicts for failed rules
  - Pipeline: rules |> filter active |> evaluate each |> collect findings

- `batch_evaluate(rules, entities)` -> dict: {results, total, passed_count, failed_count}
  - Python polyglot with `-> JSON`:
  - For each entity, evaluate all rules. Aggregate results.
  - results = array of entity evaluation dicts
  - total = total rule-entity evaluations
  - passed_count, failed_count across all evaluations

- `aggregate_findings(findings, group_by)` -> dict
  - group_by is "severity", "category", or "entity"
  - Groups findings by the specified field and counts per group
  - Uses match expression to select grouping field:
    ```
    let key = match group_by {
        "severity" => models.severity_name(f.severity),
        "category" => f.get("category") ?? "unknown",
        "entity" => f.get("entity_id"),
        _ => "other"
    }
    ```
  - Returns dict of group -> count

- `get_critical_findings(findings)` -> array
  - Returns findings with severity Critical or High

---

## Module 7: dependencies.naab — Dependency Resolution

Imports: models.naab

**Exported Functions:**
- `build_dependency_graph(rules)` -> dict
  - Returns dict: rule_id -> array of dependency rule_ids
  - Only includes Active rules

- `resolve_rule_dependencies(rules)` -> dict: {order, cycles, unresolvable}
  - TOPOLOGICAL SORT — determines safe evaluation order
  - Python polyglot with `-> JSON`:
  - Kahn's algorithm: find rules with 0 dependencies, add to order, remove edges, repeat
  - If nodes remain after algorithm: cycles exist
  - order = array of rule_ids in evaluation order
  - cycles = array of rule_id arrays (each cycle)
  - unresolvable = rule_ids stuck in cycles

- `detect_circular_dependencies(graph)` -> dict: {has_cycles, cycles, affected_rules}
  - RECURSIVE DFS-based cycle detection
  - Uses three-color marking: white (unvisited), gray (in progress), black (done)
  - When DFS hits a gray node: cycle found
  - Tracks the cycle path for reporting
  - affected_rules = all rule_ids involved in any cycle

- `traverse_dependency_tree(graph, root_id, visited)` -> array
  - RECURSIVE depth-first traversal
  - Base case: root_id not in graph OR root_id in visited -> return []
  - Marks visited, recurses into dependencies
  - Returns all transitive dependencies

- `calculate_dependency_depth(graph, rule_id, memo)` -> int
  - RECURSIVE with memoization
  - Leaf (no dependencies): return 0
  - Otherwise: 1 + max depth of dependencies
  - memo dict caches results to avoid recomputation
  - MUST handle cycles: if rule_id already in memo with sentinel -1, return 0

---

## Module 8: risk.naab — Risk Scoring

Requires: `use math`
Imports: models.naab

**Exported Functions:**
- `calculate_risk_score(entity_findings)` -> dict: {score, risk_level, contributing_factors, breakdown}
  - Python polyglot with `-> JSON`:
  - Severity weights: Critical=100, High=75, Medium=50, Low=25, Info=5
  - score = sum(weight * count_per_severity), normalized to 0-100
  - risk_level via match expression:
    ```
    let risk_level = match true {
        score >= 80 => "critical",
        score >= 60 => "high",
        score >= 40 => "medium",
        score >= 20 => "low",
        _ => "minimal"
    }
    ```
  - (But actually implement this inside the Python block for contract compliance)
  - contributing_factors = array of {severity, count, weight} dicts
  - breakdown = dict with per-severity scores

- `calculate_portfolio_risk(entity_results)` -> dict: {total_entities, avg_risk_score, high_risk_count, by_category}
  - Python polyglot with `-> JSON`:
  - Aggregates risk scores across all entities
  - high_risk_count = entities with risk_level "critical" or "high"
  - by_category = dict of category -> avg risk score

- `generate_risk_matrix(entities, findings)` -> dict: {matrix, high_risk_cells, coverage}
  - Pipeline: entities |> cross with severity levels |> fill counts
  - matrix = dict of entity_type -> {Critical: count, High: count, ...}
  - high_risk_cells = array of {entity_type, severity} pairs where count > threshold
  - coverage = fraction of matrix cells with at least 1 finding

- `calculate_compliance_metrics(findings, rules)` -> dict: {total_rules, compliance_rate, by_severity, by_category, trend}
  - Python polyglot with `-> JSON`:
  - compliance_rate = passed / total evaluations
  - by_severity = dict of severity_name -> {passed: n, failed: n, rate: float}
  - by_category = dict of category -> {passed: n, failed: n, rate: float}
  - trend = "improving" if compliance_rate >= 0.8, "stable" if >= 0.5, "declining" otherwise

- `rank_entities_by_risk(entity_results)` -> array
  - Pipeline: entity_results |> calculate risk scores |> sort descending |> return

---

## Module 9: reports.naab — Report Generation (Taint Sink)

Requires: `use json`, `use crypto`, `use file`
Imports: models.naab, validators.naab

**Exported Functions:**
- `generate_compliance_report(findings, rules, entities)` -> dict: {report_text, hash, sections}
  - report_text: formatted text with compliance summary, findings list, per-entity breakdown
  - hash: crypto.sha256(report_text)
  - sections: ["summary", "findings", "by_entity", "by_severity", "recommendations"]

- `generate_audit_report(events)` -> dict: {trail, hash_chain, summary}
  - events is array of strings. Each event gets {event, timestamp, hash}
  - hash = crypto.sha256(event + previous_hash). First uses "genesis".
  - trail = array of event dicts. hash_chain = array of hashes.
  - summary = {total_events, first_event, last_event}

- `export_findings_csv(findings)` -> string
  - CSV format: rule_id, entity_id, result, severity, message

- `format_finding(finding)` -> string
  - Pipeline: finding |> extract fields |> format as line
  - Uses match for severity formatting:
    ```
    let prefix = match finding.severity {
        models.Severity.Critical => "[CRIT]",
        models.Severity.High => "[HIGH]",
        models.Severity.Medium => "[MED ]",
        models.Severity.Low => "[LOW ]",
        _ => "[INFO]"
    }
    ```

- `write_report_to_file(report_text, filepath)` -> dict: {success, path, bytes_written}
  - MUST sanitize report_text with validators.sanitize_string() before file.write()

---

## Module 10: remediation.naab — Remediation Planning

Requires: `use uuid`
Imports: models.naab

**Exported Functions:**
- `find_best_remediation(finding, available_actions)` -> dict: {action, priority, estimated_effort, dependencies}
  - Scores each action based on:
    - Finding severity: Critical=highest priority
    - Action effort: lower effort scores higher
    - Dependency count: fewer dependencies scores higher
  - Uses match for priority assignment:
    ```
    let priority = match finding.severity {
        models.Severity.Critical => models.RemediationPriority.Immediate,
        models.Severity.High => models.RemediationPriority.High,
        models.Severity.Medium => models.RemediationPriority.Medium,
        _ => models.RemediationPriority.Low
    }
    ```
  - Returns best-scoring action

- `build_remediation_plan(findings, available_actions)` -> dict: {steps, total_effort, priority_order, blocked_by}
  - For each finding: find_best_remediation. Create RemediationStep.
  - Topological sort steps by blocked_by dependencies.
  - total_effort = sum of estimated_hours
  - priority_order = sorted by priority (Immediate first)
  - blocked_by = dict of step_id -> [blocking step_ids]

- `estimate_total_effort(steps)` -> dict: {hours, days, cost_estimate}
  - hours = sum of all step estimated_hours
  - days = hours / 8
  - cost_estimate = hours * 150 (hourly rate)

- `get_actionable_steps(steps)` -> array
  - Returns steps where ALL blocked_by steps are completed
  - Requires iterating steps and checking completed status of blockers

- `mark_step_completed(steps, step_id)` -> array
  - Finds step by id, sets completed=true, re-assigns in array (value semantics!)

---

## Module 11: interpreter.naab — Agent-Based Rule Interpretation

Requires: `use agent`, `use json`
Imports: models.naab

**Exported Functions:**
- `check_agent_available(config_name)` -> dict: {available, reason}
  - Uses agent.check(config_name)

- `generate_interpretation_prompt(rule, entity)` -> string
  - Builds structured text: rule name, description, conditions summary, entity attributes

- `interpret_rule(rule, entity, context)` -> dict: {interpretation, confidence, reasoning, alternatives}
  - try/catch around agent calls
  - Create agent with agent.create("rule_interpreter")
  - Turn 1: send rule context
  - Turn 2: send entity details for specific interpretation
  - Parse response for interpretation, confidence, reasoning, alternatives
  - On failure: return {interpretation: "unavailable", confidence: 0, reasoning: "agent error", alternatives: []}

- `batch_interpret(rules, entity)` -> array of dicts
  - Creates 3 agent handles
  - Uses agent.batch for parallel interpretation
  - Handles partial failures gracefully

- `analyze_rule_conflicts(rules)` -> dict: {conflicts, count, resolutions}
  - Python polyglot with `-> JSON`:
  - Finds rules that could produce contradictory results on the same entity
  - Two rules conflict if: same category, overlapping condition fields, different expected values
  - conflicts = array of {rule_a, rule_b, field, reason}
  - resolutions = array of suggested resolution strings

---

## Module 12: validators.naab — Additional Details

The `validate_condition_tree` function is the key recursive validator. Here's the exact
algorithm:

```
fn validate_condition_tree(conditions) {
    let errors = []
    let max_d = 0

    fn validate_node(node, current_depth) {
        if current_depth > max_d { max_d = current_depth }

        let op = node.get("op")
        // Leaf operators: Equals through Matches (0-7)
        if op <= models.ConditionOp.Matches {
            if node.get("field") == null || node.get("field") == "" {
                errors.push("missing field at depth " + string(current_depth))
            }
            if node.get("value") == null {
                errors.push("missing value at depth " + string(current_depth))
            }
        }
        // And/Or (8-9): require children
        if op == models.ConditionOp.And || op == models.ConditionOp.Or {
            let children = node.get("children") ?? []
            if len(children) == 0 {
                errors.push("AND/OR requires children at depth " + string(current_depth))
            }
            for child in children {
                validate_node(child, current_depth + 1)
            }
        }
        // Not (10): exactly 1 child
        if op == models.ConditionOp.Not {
            let children = node.get("children") ?? []
            if len(children) != 1 {
                errors.push("NOT requires exactly 1 child at depth " + string(current_depth))
            }
            for child in children {
                validate_node(child, current_depth + 1)
            }
        }
    }

    for cond in conditions {
        validate_node(cond, 0)
    }
    return {valid: len(errors) == 0, errors: errors, depth: max_d}
}
```

**Note:** The inner function `validate_node` captures `errors` and `max_d` from the outer
scope. After the loop, `max_d` must be read from the outer variable. Due to value semantics
in NAAb, be careful: if you reassign `max_d` inside `validate_node`, the outer variable
won't see it unless you use a mutable container (array or dict) pattern:

```naab
let state = {"max_d": 0, "errors": []}
// Inside validate_node: mutate state dict, re-assign state = state after changes
```

This is a VALUE SEMANTICS TRAP — the spec intentionally requires this pattern.

---

## Test Orchestrator (main.naab) — 120 Tests

### test_models() — 10 tests
1. Create Rule: create_rule with valid fields -> rule.id == "R1"
2. Create Entity: create_entity("E1", "ProdDB", EntityType.Database, {"encryption": "AES256"}, ["prod"], "admin") -> entity.name == "ProdDB"
3. Create Condition: create_condition(ConditionOp.Equals, "encryption", "AES256", []) -> op == ConditionOp.Equals
4. Create Finding: create_finding("R1", "E1", ComplianceResult.Fail, Severity.Critical, "Missing encryption", "no TLS") -> result == ComplianceResult.Fail
5. severity_name: `models.severity_name(models.Severity.Critical)` == "Critical"
6. op_name: `models.op_name(models.ConditionOp.GreaterThan)` == ">"
7. result_name: `models.result_name(models.ComplianceResult.Pass)` == "Pass"
8. entity_type_name: `models.entity_type_name(models.EntityType.Database)` == "Database"
9. validate_rule valid: valid rule -> {valid: true, errors: []}
10. validate_entity invalid: entity with empty name -> {valid: false, ...}

### test_validators() — 10 tests
1. sanitize strips angle brackets: `sanitize_string("<script>alert</script>")` -> result does not contain "<"
2. sanitize preserves JSON chars: `sanitize_string('{"key": "val"}')` -> contains `"`
3. sanitize_log_line strips ANSI: input with `\x1b[31m` -> result has no ANSI codes
4. validate_rule_syntax valid: {id: "R1", name: "Test", category: "encryption", severity: 0, conditions: [{}]} -> valid == true
5. validate_rule_syntax missing id: {id: "", ...} -> valid == false
6. validate_rule_syntax bad category: {category: "invalid_category"} -> valid == false, errors mentions "category"
7. validate_condition_tree simple leaf: [{op: Equals, field: "x", value: "y", children: []}] -> valid == true, depth == 0
8. validate_condition_tree AND with children: AND node with 2 leaf children -> valid == true, depth == 1
9. validate_condition_tree NOT with 2 children: NOT node with 2 children -> valid == false
10. validate_condition_tree deep: 3-level nested tree -> depth == 2

### test_conditions() — 10 tests
1. Equals match: condition(Equals, "role", "admin") against {"role": "admin"} -> passed == true
2. Equals mismatch: condition(Equals, "role", "admin") against {"role": "user"} -> passed == false
3. GreaterThan: condition(GT, "port", "1024") against {"port": "8080"} -> passed == true
4. LessThan: condition(LT, "age_days", "90") against {"age_days": "30"} -> passed == true
5. Contains: condition(Contains, "name", "prod") against {"name": "prod-db-01"} -> passed == true
6. And both pass: AND([Equals("role","admin"), Equals("active","true")]) against {"role":"admin","active":"true"} -> passed == true
7. And one fails: AND([Equals("role","admin"), Equals("active","true")]) against {"role":"user","active":"true"} -> passed == false
8. Or one passes: OR([Equals("env","prod"), Equals("env","staging")]) against {"env":"staging"} -> passed == true
9. Not inverts: NOT([Equals("disabled","true")]) against {"disabled":"false"} -> passed == true
10. count_conditions: AND([Equals, Equals, OR([Equals, Equals])]) -> count == 5

### test_rules() — 10 tests
1. parse valid JSON rule: `parse_rule_definition('{"id":"R1","name":"Encryption Check","description":"Verify encryption","severity":0,"category":"encryption","conditions":[{"op":"==","field":"encryption","value":"AES256","children":[]}],"dependencies":[],"tags":["security"]}')` -> valid == true, rule.id == "R1"
2. parse invalid JSON: `parse_rule_definition("not json{")` -> valid == false, errors contains "invalid JSON"
3. parse missing field: JSON without "name" field -> valid == false
4. parse_condition_tree: [{"op":"==","field":"x","value":"1","children":[]}, {"op":"AND","field":"","value":"","children":[{"op":"!=","field":"y","value":"2","children":[]}]}] -> array of 2 conditions, second has 1 child
5. filter by category: 3 rules (2 encryption, 1 audit) -> filter "encryption" returns 2
6. filter by severity: 4 rules (Critical, High, Medium, Low) -> filter Critical returns 1
7. filter active: 3 rules (2 Active, 1 Deprecated) -> returns 2
8. get_rule_by_id found: `get_rule_by_id(rules, "R2")` -> returns rule with id "R2"
9. update_rule_status: Active -> Deprecated, version increments by 1
10. merge_rule_tags: rule with ["a","b"], merge ["b","c"] -> tags contain ["a","b","c"] (deduplicated)

### test_entities() — 10 tests
1. register new: empty registry + entity -> success == true, registry length 1
2. register duplicate: same entity id twice -> second returns success == false
3. get_entity_by_id found: 3 entities, get "E2" -> entity.id == "E2"
4. get_entity_by_id not found: get "E99" -> null
5. get_entities_by_type: 2 Servers, 1 Database -> type Server returns 2
6. update_attribute: set entity "E1" attribute "tls" to "1.3" -> success == true, attribute updated
7. update_attribute not found: update "E99" -> success == false
8. filter_by_tag: 3 entities, 2 tagged "prod" -> returns 2
9. count_by_type: 2 Servers, 1 Database, 1 App -> {"Server": 2, "Database": 1, "Application": 1}
10. get_entity_attributes: entity with {"os": "linux", "version": "20.04"} -> returns dict with 2 keys

### test_evaluator() — 10 tests
1. evaluate passing rule: encryption rule against entity with "AES256" -> passed == true
2. evaluate failing rule: encryption rule against entity with "none" -> passed == false
3. evaluate inactive rule: Deprecated rule -> passed == true (skipped)
4. evaluate_entity: 3 rules, entity passes 2 fails 1 -> passed == 2, failed == 1
5. evaluate with AND condition: AND(encryption=AES256, tls=1.3) against matching entity -> passed == true
6. evaluate with nested OR: OR(env=prod, env=staging) against env=staging -> passed == true
7. batch_evaluate: 2 entities, 3 rules each -> total == 6
8. aggregate by severity: 5 findings (2 Critical, 2 High, 1 Medium) -> Critical count == 2
9. aggregate by entity: 4 findings for 2 entities -> each entity has correct count
10. critical findings: 5 findings (1 Critical, 2 High, 2 Medium) -> returns 3 (Critical + High)

### test_dependencies() — 10 tests
1. build graph: 3 rules (R1 deps [], R2 deps [R1], R3 deps [R1, R2]) -> graph has 3 entries
2. resolve no cycles: R1->[], R2->[R1], R3->[R1,R2] -> order starts with R1
3. resolve with cycle: R1->[R2], R2->[R1] -> cycles length > 0, unresolvable contains R1 and R2
4. detect_circular none: linear chain -> has_cycles == false
5. detect_circular simple: A->B->A -> has_cycles == true, cycles has [A, B]
6. detect_circular complex: A->B->C->A, D->[] -> affected_rules has A,B,C but not D
7. traverse linear: R1->R2->R3 -> traverse from R1 returns [R2, R3]
8. traverse tree: R1->[R2,R3], R2->[R4] -> traverse from R1 returns [R2, R3, R4]
9. dependency_depth: R1->R2->R3->R4 -> depth of R1 == 3
10. depth with memo: compute depth of R3 first (memo), then R1 reuses memo -> same result

### test_risk() — 10 tests
1. critical only: 3 Critical findings -> score >= 80
2. low only: 2 Low findings -> score <= 30
3. mixed: 1 Critical, 2 High, 3 Medium -> score in range 40-80
4. risk_level critical: score >= 80 -> risk_level == "critical"
5. risk_level minimal: score < 20 -> risk_level == "minimal"
6. portfolio: 3 entities with varying risk -> avg_risk_score is average of 3 scores
7. portfolio high_risk_count: 1 critical, 2 low -> high_risk_count == 1
8. risk matrix: 2 entity types, 3 severities -> matrix has correct cell counts
9. compliance_metrics: 10 evaluations, 7 pass -> compliance_rate == 0.7
10. rank entities: 3 entities -> sorted highest risk first

### test_reports() — 10 tests
1. report contains rule id: generate_compliance_report with 1 finding -> report_text contains rule_id
2. hash length: report hash is 64 chars (SHA-256)
3. audit trail: 3 events -> trail length 3, hash_chain length 3
4. hash chain integrity: hash_chain[1] != hash_chain[0]
5. hash chain deterministic: same events -> same hash_chain
6. CSV export: 3 findings -> CSV contains 4 lines (header + 3)
7. format_finding Critical: Severity.Critical -> starts with "[CRIT]"
8. format_finding Low: Severity.Low -> starts with "[LOW ]"
9. write sanitized: report with "<script>" -> no angle brackets in output
10. taint passes: write_report_to_file succeeds without taint violation

### test_remediation() — 10 tests
1. find_best Critical: Critical finding -> priority == Immediate
2. find_best Low: Low finding -> priority == Low or Medium
3. find_best scores effort: 2 actions, one with less effort -> returns lower effort one
4. build_plan: 3 findings -> steps length == 3
5. build_plan total_effort: 3 steps with hours 2,4,6 -> total_effort == 12
6. build_plan priority_order: Critical finding first in priority_order
7. estimate_effort: 24 hours total -> days == 3, cost_estimate == 3600
8. actionable steps: 3 steps, step C blocked by A -> initially only A and B are actionable
9. mark completed: mark step A completed -> completed == true, re-check: step C now actionable
10. value semantics: mark_step_completed returns new array, original unchanged

### test_interpreter() — 10 tests
1. agent available: `check_agent_available("rule_interpreter")` -> check available field exists
2. agent unavailable: `check_agent_available("nonexistent")` -> available == false
3. prompt contains rule name: generate_interpretation_prompt -> contains rule.name
4. prompt contains entity: generate_interpretation_prompt with entity -> contains entity.name
5. interpret returns contract keys: interpret_rule -> has "interpretation", "confidence", "reasoning", "alternatives"
6. interpret confidence type: confidence is number or string
7. interpret handles failure: bad API key -> {interpretation: "unavailable", ...}
8. batch returns array: batch_interpret with 2 rules -> result length == 2
9. batch partial failure: one agent fails -> at least one succeeds
10. conflict detection: 2 rules same category different values -> conflicts count > 0

### test_integration() — 10 tests
These tests exercise multi-module data flow:

1. End-to-end: create rule, create entity, evaluate, generate finding, compute risk -> risk score > 0
2. Rule parsing to evaluation: parse JSON rule definition, evaluate against entity -> result is pass or fail
3. Dependency order evaluation: 3 rules with dependencies, resolve order, evaluate in order -> all evaluated
4. Recursive condition tree: 3-level deep AND(OR(Equals, NotEquals), Equals) -> evaluation correct
5. Taint flow: polyglot output -> sanitize -> file.write -> no taint violation
6. Pipeline chain: entities |> filter by type |> evaluate |> aggregate findings |> format -> string result
7. Match expression coverage: severity_name + op_name + result_name + priority_name all return correct strings
8. Try/catch expression: parse_rule_definition with invalid JSON -> caught, returns {valid: false}
9. Value semantics chain: update entity attribute -> re-evaluate rule -> result changes
10. Remediation from findings: evaluate -> collect findings -> build remediation plan -> actionable steps exist

---

## Required Language Feature Usage

These features MUST appear in the codebase. The test suite verifies their correct
operation — omitting them will cause test failures.

### Match Expressions (minimum 8 uses)
- `severity_name()` — 5-way match + default
- `status_name()` — 4-way match + default
- `result_name()` — 4-way match + default
- `op_name()` — 11-way match + default
- `entity_type_name()` — 6-way match + default
- `priority_name()` — 5-way match + default
- `evaluate_rule()` — match for result message classification
- `format_finding()` — match for severity prefix
- `aggregate_findings()` — match for grouping key selection

### Try/Catch Expressions (minimum 3 uses)
- `parse_rule_definition()` — `let parsed = try { json.parse(text) } catch (e) { null }`
- `interpret_rule()` — try/catch around agent calls
- Any polyglot block that might fail parsing

### Recursive Functions (minimum 5)
- `evaluate_condition()` — tree evaluation
- `count_conditions()` — tree counting
- `flatten_conditions()` — tree flattening
- `max_depth()` — tree depth
- `traverse_dependency_tree()` — graph traversal
- `calculate_dependency_depth()` — depth with memoization
- `detect_circular_dependencies()` — DFS cycle detection
- `validate_condition_tree()` inner recursive helper

### Pipeline Operator (minimum 5 uses)
- `evaluate_entity()` — rules |> filter |> evaluate |> collect
- `count_entities_by_type()` — registry |> group |> count
- `merge_rule_tags()` — tags |> combine |> deduplicate
- `rank_entities_by_risk()` — results |> score |> sort
- `generate_sla_summary()` or `format_finding()` — data |> transform |> format
- Integration test 6 — pipeline chain test

### Value Semantics Correctness (critical)
- `update_entity_attribute()` — must re-assign entity in registry array
- `mark_step_completed()` — must re-assign step in steps array
- `register_entity()` — push and return modified registry
- `validate_condition_tree()` — mutable state via container pattern
- `evaluate_entity()` — collecting findings in loop

---

## Output Format

When run with no arguments, main.naab should output:
```
=== Compliance Rule Engine Test Suite ===

test_models: X/10
test_validators: X/10
test_conditions: X/10
test_rules: X/10
test_entities: X/10
test_evaluator: X/10
test_dependencies: X/10
test_risk: X/10
test_reports: X/10
test_remediation: X/10
test_interpreter: X/10
test_integration: X/10

Total: X/120
```

Each test must print its suite result. Exit with code 0 (governance handles exit codes for violations).

## What NOT to Do
- Do NOT write standalone .py, .js, .go files
- Do NOT hardcode results, use placeholders, or stub functions
- Do NOT leave TODO/FIXME/STUB comments
- Do NOT use hedging: "simplified", "basic", "for now", "mock"
- Do NOT swallow errors silently (empty catch blocks)
- Do NOT pad functions with dummy loops for complexity
- Do NOT use `return` inside Python polyglot blocks
- Do NOT use `dict["key"]` — use `dict.get("key")`
- Do NOT forget value semantics re-assignment
- Do NOT modify govern.json — it is signed
- Do NOT use if/else chains where match expressions are specified — the test suite
  calls the name functions and verifies match is used
