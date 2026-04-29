# NAAb Project Setup Guide

How to bootstrap a new NAAb project for LLM-governed code generation.

## Prerequisites

- NAAb built and on PATH (`naab-lang --help` works)
- OpenSSL available (for Ed25519 key generation)

## 1. One-Time Key Setup

Generate an Ed25519 signing keypair. This only needs to be done once per machine.

```bash
# Generate keypair — private key at specified path, public key auto-installed
naab-lang --keygen ~/.naab/keys/signing.pem
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
naab-lang --list-keys

# Trust another user's public key
naab-lang --trust-key /path/to/their-key.pub

# Override signing key for one command
naab-lang --signing-key /path/to/other.pem --sign-governance
```

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
naab-lang --sign-governance

# Verify it worked
ls -la govern.json.sig
```

You must re-sign after every `govern.json` edit. If the signature is stale, NAAb will block execution.

### Drift Baselines (Optional)

Drift detection locks down function signatures and main{} body hashes:

```bash
# Save drift baseline after code is written
naab-lang --drift-baseline-save src/main.naab

# Sign the baseline
naab-lang --sign-baseline
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
naab-lang src/main.naab [command] [args]

# With timeout override (seconds)
naab-lang run --timeout 60 src/main.naab [command]

# With governance dashboard (summary to stderr)
naab-lang --governance-dashboard src/main.naab [command]

# Skip governance (development only — never for LLM testing)
naab-lang --no-governance src/main.naab [command]
```

## 9. Post-Session Analysis

After the LLM finishes, analyze the session:

```bash
# Run context-review against session chat logs
cd ~/.naab/projects/context-review
naab-lang run --timeout 300 main.naab -- ~/.gemini/tmp/naab-N/chats/

# Filter by category
naab-lang run --timeout 300 main.naab -- --category stuck,hollow ~/.gemini/tmp/naab-N/chats/
```

### Re-signing After Changes

If you edited `govern.json` during the session:
```bash
cd /path/to/project
naab-lang --sign-governance
```

## Quick Reference

```bash
# Full bootstrap sequence
mkdir my-project && cd my-project
mkdir -p src data
cp ~/.naab/language/CLAUDE-TEMPLATE.md ./CLAUDE.md
# Edit govern.json, CLAUDE.md, prompt.md, seed data/
naab-lang --sign-governance
naab-lang src/main.naab test
```
