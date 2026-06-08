# Project: Security Incident Triage Pipeline

Build a security incident triage system in 4 NAAb source files. The system reads
hardcoded incident data, classifies incidents by severity, generates analysis using
`codegen.run_with_args` with Python, runs concurrent analysis via async functions,
and sanitizes all output before printing.

## Architecture

4 source files in `src/`:

```
src/
├── main.naab          # Entry point — orchestrates the pipeline
├── models.naab        # Factory functions for incident and report dicts
├── validators.naab    # Sanitization functions (taint tracking compliance)
└── analyzer.naab      # Analysis engine using codegen + async
```

## Module Specifications

### models.naab — Data Factories

No imports needed (pure NAAb).

**Exported functions:**
- `make_incident(id, timestamp, source, message, severity)` — returns dict with keys: id, timestamp, source, message, severity
- `make_report(incident_id, category, findings, score)` — returns dict with keys: incident_id, category, findings, score

Example:
```naab
export fn make_incident(id, timestamp, source, message, severity) {
    return {
        "id": id,
        "timestamp": timestamp,
        "source": source,
        "message": message,
        "severity": severity
    }
}
```

### validators.naab — Sanitization

Requires: `use string`

**Exported functions:**
- `sanitize_string(s)` — strips `<` and `>` characters, trims whitespace, handles null input. Returns safe string.
- `validate_report(report)` — checks that report dict has keys: incident_id, category, findings, score. Returns bool.
- `sanitize_output(text)` — prepares text for safe output: trims, strips null bytes. Returns safe string.

These function names MUST start with `sanitize_` or `validate_` for taint tracking to
recognize them as sanitizers. They must contain real validation logic (not just pass-through).

### analyzer.naab — Analysis Engine

Requires: `use json`, `use string`, `use codegen`
Imports: `import "models.naab" as models`, `import "validators.naab" as validators`

**Exported functions:**

- `analyze_incident(incident)` — takes an incident dict, returns analysis dict with keys:
  `category`, `severity_score`, `keywords_found`, `recommendation`
  - Uses `codegen.run_with_args("python", code, bindings)` to compute keyword analysis
  - Has try/catch for error recovery on runtime errors
  - Classification logic: check message content for patterns like "auth", "login" -> "authentication",
    "port", "scan" -> "network_intrusion", "malware", "virus" -> "malware", default -> "general"

- `analyze_batch_async(incidents)` — takes array of incidents, returns array of results
  - Uses `async fn` to process incidents concurrently
  - Each async function calls `analyze_incident` inside a polyglot block or directly
  - Collects results via `await`

### main.naab — Entry Point

Requires: `use json`, `use string`, `use codegen`
Imports: all 3 modules

**What it does:**
1. Creates 3-5 test incidents with realistic security data:
   - Auth failure: "Failed login attempt from IP 192.168.1.100 - brute force detected"
   - Port scan: "Port scan detected on ports 22,80,443 from external host"
   - Malware: "Malware signature matched: trojan.gen.2 in /tmp/uploads/payload.exe"
   - (optional more)

2. Runs sequential analysis on first incident via `analyzer.analyze_incident()`

3. Runs async batch analysis on remaining incidents via `analyzer.analyze_batch_async()`

4. Sanitizes all results with `validators.sanitize_output()` before printing

5. Prints structured output showing category, severity, and recommendations for each

6. Uses try/catch for error recovery (catching runtime errors only, NOT governance errors)

## Codegen Usage

Use `codegen.run_with_args` for Python analysis. Example:

```naab
use codegen

fn analyze_incident(incident) {
    let msg = incident.get("message")
    let sev = incident.get("severity")
    
    // Use codegen to do keyword frequency analysis
    let keyword_result = codegen.run_with_args("python",
        "words = msg_text.lower().split()\nkeyword_count = len([w for w in words if len(w) > 3])\nprint(keyword_count)",
        {"msg_text": msg})
    
    // ... rest of classification logic
}
```

**Binding rules:**
- Keys MUST be valid identifiers: `[A-Za-z_][A-Za-z0-9_]*`
- Do NOT use keys like `"key;bad"`, `"key bad"`, `"$(cmd)"`
- String values in shell bindings are safely single-quoted automatically
- Governance scans the FULL assembled code including binding values

## Async Usage

```naab
async fn process_incident(incident) {
    let result = analyze_incident(incident)
    return result
}

// In main:
let future1 = process_incident(incidents[1])
let future2 = process_incident(incidents[2])
let r1 = await future1
let r2 = await future2
```

## Governance Rules (govern.json)

- **Allowed languages:** python, javascript, shell
- **Codegen:** enabled for python, shell, javascript
- **Taint tracking:** HARD — all polyglot output must be sanitized before file.write
- **Network:** disabled
- **Shell:** enabled but rm/curl/wget/chmod/sudo/dd/kill blocked
- **Dangerous calls:** HARD blocked (no `import os`, `os.system`, `subprocess`)
- **No secrets/placeholders/simulation markers:** HARD
- **Sandbox:** elevated (allows absolute path reads)

## What NOT to Do

- Do NOT use `codegen.run()` without args — always use `codegen.run_with_args()`
- Do NOT use `import os` / `os.system()` / `subprocess` in Python polyglot or codegen
- Do NOT write to files without sanitizing first (taint tracking will block)
- Do NOT use blocked languages (Go, Rust, C++, etc.)
- Do NOT use `agent.send()` — no API keys available
- Do NOT declare variables at top level — all `let`/`const` inside `main {}` or functions
- Do NOT use `return` in Python polyglot blocks
- Do NOT use `dict["key"]` — use `dict.get("key")` for safe access
- Do NOT modify govern.json — it is signed
- Do NOT use `import os` in Python — governance blocks dangerous calls
- Do NOT leave TODO/FIXME/STUB comments
- Do NOT use hedging comments ("simplified", "basic", "for now")

## Expected Output

When `src/main.naab` runs successfully, it should print structured output like:
```
=== Security Incident Triage Pipeline ===

--- Sequential Analysis ---
Incident INC-001: category=authentication, severity_score=8, keywords=5
  Recommendation: Review authentication logs and enforce MFA

--- Async Batch Analysis ---
Incident INC-002: category=network_intrusion, severity_score=7, keywords=4
  Recommendation: Block source IP and review firewall rules
Incident INC-003: category=malware, severity_score=9, keywords=3
  Recommendation: Quarantine affected system and run full scan

Pipeline complete: 3 incidents analyzed
```

The exact format is flexible, but the output must contain:
- The word "category" or "Category"
- The word "severity" or "Severity"
- Analysis results for each incident
