# NAAb Project Setup Guide

How to bootstrap a new NAAb project for LLM-governed code generation.

## Prerequisites

- NAAb built and on PATH (`naab --help` works)
- OpenSSL available (for Ed25519 key generation)

## 1. One-Time Key Setup

Generate an Ed25519 signing keypair. This only needs to be done once per machine.

```bash
# Generate keypair — private key at specified path, public key auto-installed
naab --keygen ~/.naab/keys/signing.pem
```

Output:
```
Ed25519 keypair generated:
  Private key: ~/.naab/keys/signing.pem (keep secret — never commit!)
  Public key:  ~/.naab/trusted-keys/<fingerprint>.pub
  Fingerprint: e211d4b5d470bece
```

Set the signing key for all sessions:
```bash
# Add to ~/.bashrc or ~/.zshrc
export NAAB_SIGNING_KEY=~/.naab/keys/signing.pem
```

### Key Locations

| What | Path | Permissions |
|------|------|-------------|
| Private key | `~/.naab/keys/signing.pem` | 0600 (owner-only) |
| Public keys | `~/.naab/trusted-keys/*.pub` | Read-only |
| Signature files | `<project>/govern.json.sig` | Committed with project |

### Key Management Commands

```bash
# List trusted key fingerprints
naab --list-keys

# Trust another user's public key
naab --trust-key /path/to/their-key.pub

# Override signing key for one command
naab --signing-key /path/to/other.pem --sign-governance
```

### Key Mismatch Troubleshooting

If you see "INTEGRITY BLOCK: signature does not match any trusted key", the signing key
and the trusted public key are from different keypairs. This happens when:
- You regenerated keys with `--keygen` but didn't re-sign govern.json
- The trust store has a key from a different machine or user
- Someone deleted and recreated the trust store

Fix: ensure the private key in `NAAB_SIGNING_KEY` and the public key in
`~/.naab/trusted-keys/` are from the same `--keygen` invocation. Then re-sign:

```bash
naab --sign-governance        # signs with NAAB_SIGNING_KEY
naab --list-keys              # verify fingerprint matches
```

If the trust store is emptied entirely (no `.pub` files in `~/.naab/trusted-keys/`),
the runtime falls back to unsigned mode (backward compatibility) — governance loads
normally but signatures are not verified. This only blocks when trusted keys exist
but the `.sig` file is missing or invalid.

### Why Signing Matters

When an LLM (Gemini, Claude, etc.) runs NAAb code, governance rules in `govern.json` control what it can do — blocked imports, banned functions, taint tracking, complexity floors. Without signing, the LLM could modify `govern.json` to weaken or disable these rules. Ed25519 signatures make `govern.json` tamper-evident: any modification invalidates the signature, and NAAb refuses to run with an invalid or missing signature when trusted keys are installed.

## 2. Create Project Directory

```bash
mkdir my-project && cd my-project
mkdir -p src data
```

### Project Layout

```
my-project/
├── CLAUDE.md          # Language reference + project spec (LLM reads this)
├── govern.json        # Governance policy (enforced at runtime)
├── govern.json.sig    # Ed25519 signature (prevents LLM tampering)
├── prompt.md          # Initial prompt for the LLM
├── conductor/         # Phase plans (optional, for multi-phase builds)
├── src/
│   ├── main.naab      # Entry point with main {} block
│   ├── models.naab    # Structs and enums
│   ├── config.naab    # Constants and configuration
│   ├── storage.naab   # File I/O and persistence
│   ├── tests.naab     # Test suite
│   └── ...            # Feature modules
└── data/
    ├── input/         # Seed data (JSON files)
    ├── reports/       # Generated output
    └── ...
```

## 3. Write govern.json

Minimal template covering the most important sections:

```json
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Project Name — One-line description",

  "languages": {
    "allowed": ["python", "javascript"],
    "blocked": ["shell", "go", "rust", "nim", "c++", "c#", "ruby", "php", "zig", "julia"],
    "require_explicit": true,
    "per_language": {
      "python": {
        "timeout": 15,
        "max_lines": 100,
        "banned_functions": ["eval(", "exec(", "compile("],
        "imports": {
          "mode": "blocklist",
          "blocked": ["subprocess", "os", "ctypes", "pickle", "shutil", "socket"]
        }
      },
      "javascript": {
        "timeout": 10,
        "max_lines": 80,
        "banned_functions": ["eval(", "Function("],
        "no_var": true
      }
    }
  },

  "capabilities": {
    "filesystem": { "allowed_paths": ["./data/"] },
    "network": false,
    "shell": false
  },

  "taint_tracking": {
    "enabled": true,
    "sources": ["polyglot_output"],
    "sinks": ["file.write", "file.append"],
    "sanitizers": ["validate_", "sanitize_", "int(", "float(", "string("]
  },

  "contracts": {
    "level": "soft",
    "validate_inputs": true,
    "functions": {}
  },

  "code_quality": {
    "no_stub_functions": { "enabled": true, "level": "hard" },
    "no_todo_comments": { "enabled": true, "level": "soft" },
    "no_oversimplification": { "enabled": true, "level": "hard" },
    "no_incomplete_logic": { "enabled": true, "level": "soft" },
    "no_secrets": { "enabled": true, "level": "hard" },
    "no_simulation_markers": { "enabled": true, "level": "hard" },
    "no_placeholders": { "enabled": true, "level": "soft" }
  },

  "quality_gate": {
    "enabled": true,
    "conditions": [
      { "metric": "hard_violations", "operator": ">", "threshold": 0 },
      { "metric": "soft_violations", "operator": ">", "threshold": 3 }
    ]
  }
}
```

### Adding Function Contracts

Add entries to `contracts.functions` for each function you want enforced:

```json
"contracts": {
  "level": "soft",
  "validate_inputs": true,
  "functions": {
    "compute_stats": {
      "description": "Compute statistics for a dataset",
      "params": ["data:array"],
      "return_type": "dict",
      "return_keys": ["mean", "median", "stddev", "min", "max"]
    },
    "classify_item": {
      "description": "Classify an item into a category",
      "params": ["item:dict"],
      "return_type": "string",
      "return_one_of": ["critical", "warning", "normal", "info"]
    }
  }
}
```

### Agent Tool Execution

To enable agents to call NAAb functions as tools, add tool configuration to your agent config:

```json
{
  "agents": {
    "researcher": {
      "provider": "gemini",
      "model": "gemini-2.5-flash",
      "api_key_env": "GEMINI_API_KEY",
      "system_prompt": "You are a research assistant.",
      "max_turns": 20,
      "tools": ["search_database", "calculate_stats"],
      "tools_enabled": true,
      "max_tool_calls_per_turn": 5,
      "max_tool_loop_turns": 10,
      "tool_result_max_chars": 4096,
      "tool_timeout_seconds": 10,
      "allowed_actions": ["AGENT_SEND", "TOOL_EXEC"],
      "standing_lease_turns": 10,
      "risk_budget": 20,
      "output_contract": {
        "format": "json",
        "required_fields": ["answer", "confidence"],
        "field_types": { "confidence": "number" }
      }
    }
  }
}
```

In your NAAb script, register the tool functions before sending:

```naab
use agent

fn search_database(query) {
    // ... real implementation
    return {"results": found}
}

main {
    agent.register_tool("search_database", search_database, {
        "description": "Search the knowledge base",
        "parameters": {
            "query": {"type": "string", "description": "Search query"}
        }
    })

    let h = agent.create("researcher")
    let r = agent.send(h, "Find articles about governance")
    // r.tool_calls_made shows how many tools were called
}
```

**Dual-gate enforcement**: A tool only executes if it appears in BOTH the govern.json `tools` array AND is registered via `agent.register_tool()`. Tool config fields are ratchet-enforced — they can be tightened mid-run but never loosened.

### Agent Governance Features

Add these sections to govern.json for advanced agent governance:

```json
{
  "advisory_escalation": {
    "enabled": true,
    "soft_after": 3,
    "weight_multiplier": 2.0
  },
  "governance_health": {
    "enabled": true,
    "check_after_turns": 10,
    "governance_entropy_warning": 0.5
  }
}
```

- **Standing Lease** (`standing_lease_turns`/`standing_lease_seconds`): TTL on agent authorization. 0 = unlimited.
- **Output Contracts** (`output_contract` in agent config): validate LLM response structure against a schema.
- **Advisory Escalation**: repeated advisories escalate — 2nd+ occurrence gets weight multiplied, N-th becomes SOFT block.
- **Governance Pulse**: `governance.health()` (requires `use governance`) returns health verdict and instrumentation status.
- **Evidence Epoch**: monotonic counter incremented on governance state changes. Prior-epoch evidence discounted.

### Policy Inheritance

govern.json supports inheritance for policy distribution across teams:

```json
{
  "extends": "./policies/base-security.json",
  "meta": {
    "inheritance": {
      "merge_arrays": "append",
      "max_depth": 5
    }
  }
}
```

Child overrides parent. Array fields (blocked_commands, custom_rules, taint sources/sinks/sanitizers, env_vars lists) merge via `merge_arrays` mode. Parent must pass signature verification.

### Dynamic Code Execution

Enable governed runtime code generation:

```json
{
  "codegen": {
    "enabled": true,
    "max_lines_per_call": 100,
    "max_calls_per_run": 50,
    "allow_tainted_code": false
  }
}
```

Use `codegen.run(lang, code)` or `codegen.run_strict(lang, code)` in NAAb scripts. Same 39+ governance checks as static polyglot blocks.

### Telemetry Forwarding

Forward JSONL telemetry events to external systems:

```json
{
  "telemetry": {
    "enabled": true,
    "output_file": "telemetry.jsonl",
    "forwarding": {
      "webhook_url": "https://your-siem.example.com/ingest",
      "batch_size": 10,
      "flush_interval_ms": 5000
    }
  }
}
```

### Taint Tracking

Taint tracking ensures polyglot output is sanitized before reaching file writes:

- **Sources**: What gets tainted (e.g., `polyglot_output` — all `<<python ... >>` results)
- **Sinks**: Where tainted data is blocked (e.g., `file.write`, `file.append`)
- **Sanitizers**: Function name prefixes that clear taint (e.g., `validate_`, `sanitize_`)

The LLM must write real sanitizer functions (not identity functions — governance detects and blocks `fn validate_x(d) { return d }`).

## 4. Sign Governance

After writing or editing `govern.json`:

```bash
# Sign govern.json — creates govern.json.sig
naab --sign-governance

# Verify it worked
ls -la govern.json.sig
```

You must re-sign after every `govern.json` edit. If the signature is stale, NAAb will block execution.

### Drift Baselines (Optional)

Drift detection locks down function signatures and main{} body hashes:

```bash
# Save drift baseline after code is written
naab --drift-baseline-save src/main.naab

# Sign the baseline
naab --sign-baseline
```

## 5. Write CLAUDE.md

Copy `CLAUDE-TEMPLATE.md` from the NAAb repo as your base:

```bash
cp ~/.naab/language/CLAUDE-TEMPLATE.md ./CLAUDE.md
```

Then append project-specific sections at the bottom:

- **Project description** — what it does, why
- **Module layout** — which files to build, in what order, with dependencies
- **Function contracts** — mirror govern.json contracts with human descriptions
- **Data file schema** — what's in `data/`, key names, types
- **CLI commands** — what commands the tool supports
- **Struct/enum definitions** — full NAAb struct/enum code
- **Test plan** — what each test function validates
- **What NOT to do** — project-specific constraints and common mistakes

## 6. Write prompt.md

This is the initial prompt given to the LLM. Keep it focused and actionable:

```markdown
# Project Name — One-Line Description

Build [what] in NAAb that [does what].

## Setup
1. Read `CLAUDE.md` — NAAb language reference and project spec
2. Read `govern.json` — contracts, taint tracking, quality gates
3. Read data files in `data/` — this is your input data

## What It Should Do
1. [Feature one]
2. [Feature two]
3. ...

## CLI
```
tool command1          Description
tool command2 <arg>    Description
tool test              Run test suite
```

## Testing
- Each test function returns [passed, total]
- Main aggregates and prints results
- All contracts must pass
```

### Tips for Effective Prompts
- Tell the LLM to read files before coding (prevents hallucinating schemas)
- List features in dependency order (build phase 1 before phase 2)
- Specify CLI commands explicitly — the LLM won't invent good ones
- Don't over-specify implementation — let the LLM choose data structures

## 7. Seed Data

Put input data as JSON files in `data/`. The LLM reads these to understand the schema — don't describe the schema in prose when you can show it.

```bash
# Example: service health data
cat > data/services/api_gateway.json << 'EOF'
{
  "service": "api-gateway",
  "checks": [
    {"timestamp": "2026-04-28T08:00:00Z", "status": "healthy", "response_ms": 45},
    {"timestamp": "2026-04-28T08:05:00Z", "status": "degraded", "response_ms": 1200}
  ]
}
EOF
```

## 8. Run

```bash
# Execute the project
naab src/main.naab [command] [args]

# With timeout override (seconds)
naab run --timeout 60 src/main.naab [command]

# With governance dashboard (summary to stderr)
naab --governance-dashboard src/main.naab [command]

# Skip governance (development only — never for LLM testing)
naab --no-governance src/main.naab [command]
```

## 9. Post-Session Analysis

After the LLM finishes, analyze the session:

```bash
# Run context-review against session chat logs
cd ~/.naab/projects/context-review
naab run --timeout 300 main.naab -- ~/.gemini/tmp/naab-N/chats/

# Filter by category
naab run --timeout 300 main.naab -- --category stuck,hollow ~/.gemini/tmp/naab-N/chats/
```

### Re-signing After Changes

If you edited `govern.json` during the session:
```bash
cd /path/to/project
naab --sign-governance
```

## Quick Reference

```bash
# Full bootstrap sequence
mkdir my-project && cd my-project
mkdir -p src data
cp ~/.naab/language/CLAUDE-TEMPLATE.md ./CLAUDE.md
# Edit govern.json, CLAUDE.md, prompt.md, seed data/
naab --sign-governance
naab src/main.naab test
```
