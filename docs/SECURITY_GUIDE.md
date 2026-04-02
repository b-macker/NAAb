# NAAb Security Guide

**Version:** 0.7.0  
**Applies to:** NAAb CLI (`naab-lang`) and embedded use via the public API

---

## Table of Contents

1. [Overview: Two Security Layers](#1-overview-two-security-layers)
2. [Sandbox Levels](#2-sandbox-levels)
3. [Capability Reference](#3-capability-reference)
4. [Per-Module Sandbox Restrictions](#4-per-module-sandbox-restrictions)
5. [CLI Security Flags](#5-cli-security-flags)
6. [govern.json Security Rules](#6-governjson-security-rules)
7. [Exit Codes](#7-exit-codes)
8. [Audit Logging](#8-audit-logging)
9. [Common Patterns](#9-common-patterns)

---

## 1. Overview: Two Security Layers

NAAb enforces security through two independent, complementary layers:

### Layer 1 — Sandbox (runtime capability enforcement)

The sandbox controls what a running script can access at the OS level:
filesystem paths, network connections, environment variables, and process execution.
It operates at the interpreter level and cannot be bypassed from within NAAb code.

Controlled by: `--sandbox-level <level>` CLI flag (or `SandboxConfig` in the embedding API).

### Layer 2 — Governance Engine (policy enforcement)

The governance engine applies developer-defined policies from `govern.json`:
banned functions, blocked imports, taint flow analysis, quality gates, and
hard-block rules. It runs before and during script execution.

Controlled by: `govern.json` (discovered from the script's directory upward) and
governance CLI flags (`--governance-override`, `--no-governance`, etc.).

**These layers are independent.** You can run a script with the sandbox fully
restricted but governance disabled (`--no-governance`), or vice versa. For
maximum security, use both.

---

## 2. Sandbox Levels

The `--sandbox-level` flag selects a preset capability profile. Four levels are available:

| Level | CLI Value | Use Case |
|-------|-----------|----------|
| Restricted | `restricted` | Untrusted scripts, maximum isolation |
| Standard | `standard` | Enterprise default, polyglot blocks |
| Elevated | `elevated` | Trusted scripts needing network or env access |
| Unrestricted | `unrestricted` | Development only — no restrictions |

### Capability grants per level

| Capability | `restricted` | `standard` | `elevated` | `unrestricted` |
|-----------|:---:|:---:|:---:|:---:|
| FS_READ | **no** | yes | yes | yes (UNSAFE) |
| FS_WRITE | no | yes | yes | yes (UNSAFE) |
| FS_CREATE_DIR | no | no | yes | yes (UNSAFE) |
| FS_DELETE | no | no | no | yes (UNSAFE) |
| FS_EXECUTE | no | no | no | yes (UNSAFE) |
| NET_CONNECT | no | no | yes | yes (UNSAFE) |
| NET_LISTEN | no | no | no | yes (UNSAFE) |
| NET_RAW | no | no | no | yes (UNSAFE) |
| SYS_EXEC | no | no | yes | yes (UNSAFE) |
| SYS_ENV | no | no | yes | yes (UNSAFE) |
| SYS_TIME | no | no | yes | yes (UNSAFE) |
| BLOCK_LOAD | no | no | yes | yes (UNSAFE) |
| BLOCK_CALL | no | yes | yes | yes (UNSAFE) |
| network_enabled | false | false | true | true |
| allow_fork | false | false | true | true |
| allow_exec | false | false | true | true |

### Resource limits per level

| Limit | `restricted` | `standard` | `elevated` | `unrestricted` |
|-------|:-----------:|:----------:|:----------:|:--------------:|
| Memory | 128 MB | 2048 MB | 1024 MB | unlimited |
| CPU time | 10 s | 30 s | 60 s | unlimited |
| Max file size | 10 MB | 100 MB | 1000 MB | unlimited |

### Allowed filesystem paths per level

| Level | Allowed read paths | Allowed write paths |
|-------|--------------------|---------------------|
| `restricted` | none | none |
| `standard` | `$CWD`, `$TMPDIR` | `$CWD`, `$TMPDIR` |
| `elevated` | all | all |
| `unrestricted` | all (UNSAFE) | all (UNSAFE) |

> **Note on `standard` vs `fromPermissionLevel(STANDARD)`:**  
> The CLI `--sandbox-level standard` uses `createEnterpriseConfig()`, which is optimized
> for polyglot block execution: it grants FS_READ, FS_WRITE, BLOCK_CALL, 2 GB memory, and
> allows paths in `$CWD` + `$TMPDIR`. It does **not** grant SYS_ENV or BLOCK_LOAD.
> This differs from the internal `fromPermissionLevel(STANDARD)` preset (which does grant
> SYS_ENV and BLOCK_LOAD). The CLI `standard` level is intentionally more conservative.

---

## 3. Capability Reference

All 16 capabilities defined in `naab::security::Capability`:

| Capability | Category | Description |
|-----------|----------|-------------|
| `FS_READ` | Filesystem | Read files from allowed paths |
| `FS_WRITE` | Filesystem | Write or modify files |
| `FS_EXECUTE` | Filesystem | Execute files as programs |
| `FS_DELETE` | Filesystem | Delete files |
| `FS_CREATE_DIR` | Filesystem | Create directories |
| `NET_CONNECT` | Network | Outbound TCP/HTTP connections |
| `NET_LISTEN` | Network | Bind and listen on ports |
| `NET_RAW` | Network | Raw socket access (ICMP, etc.) |
| `SYS_EXEC` | System | Execute external processes |
| `SYS_ENV` | System | Read and write environment variables |
| `SYS_TIME` | System | Access system clock and time APIs |
| `BLOCK_LOAD` | Inter-block | Import/load other NAAb blocks |
| `BLOCK_CALL` | Inter-block | Call functions in other blocks |
| `RES_UNLIMITED_MEM` | Resources | Bypass memory limit enforcement |
| `RES_UNLIMITED_CPU` | Resources | Bypass CPU time limit enforcement |
| `UNSAFE` | Special | Grants all capabilities unconditionally |

> `UNSAFE` is granted only under `--sandbox-level unrestricted`. Any code running with
> `UNSAFE` has full access to the host system. Never use `unrestricted` in production.

### Path canonicalization

Before any filesystem capability check, NAAb calls `normalizePath()` which invokes
`realpath()` to resolve symlinks and `..` components. This means:

- `../../etc/os-release` → `/etc/os-release` (then checked against allowed paths)
- A symlink in `$TMPDIR` pointing to `/etc/passwd` → `/etc/passwd` (blocked under restricted)
- Deep traversal `../../../../proc/version` → `/proc/version` (blocked under restricted)

**Path traversal and symlink attacks are blocked at all levels below `unrestricted`.**

---

## 4. Per-Module Sandbox Restrictions

Three stdlib modules are sandbox-controlled. All other modules (string, math, array, etc.)
are always available regardless of sandbox level.

### `file` module

| Operation | `restricted` | `standard` | `elevated` | `unrestricted` |
|-----------|:-----------:|:----------:|:----------:|:--------------:|
| `file.read` | blocked | cwd/tmp only | any path | any path |
| `file.write` | blocked | cwd/tmp only | any path | any path |
| `file.append` | blocked | cwd/tmp only | any path | any path |
| `file.delete` | blocked | blocked | any path | any path |
| `file.create_dir` | blocked | blocked | any path | any path |
| `file.list_dir` | blocked | cwd/tmp only | any path | any path |
| `file.exists` | blocked | cwd/tmp only | any path | any path |
| `file.copy`, `file.move` | blocked | cwd/tmp only | any path | any path |

Under `restricted`, all `file` operations throw a `SandboxViolationException` (exit code 1).

### `http` module

| Operation | `restricted` | `standard` | `elevated` | `unrestricted` |
|-----------|:-----------:|:----------:|:----------:|:--------------:|
| `http.get` | blocked | blocked | allowed | allowed |
| `http.post` | blocked | blocked | allowed | allowed |
| `http.put` | blocked | blocked | allowed | allowed |
| `http.delete` | blocked | blocked | allowed | allowed |
| `http.head` | blocked | blocked | allowed | allowed |
| `http.patch` | blocked | blocked | allowed | allowed |

Network is disabled (`network_enabled = false`) under both `restricted` and `standard`.
All HTTP operations are blocked at both levels regardless of the URL.

**`file://` URLs are always rejected**, even under `unrestricted`, as a defense-in-depth
measure against SSRF-style attacks via the HTTP client.

### `env` module

| Operation | `restricted` | `standard` | `elevated` | `unrestricted` |
|-----------|:-----------:|:----------:|:----------:|:--------------:|
| `env.get` | blocked | blocked | allowed | allowed |
| `env.set_var` | blocked | blocked | allowed | allowed |
| `env.has` | blocked | blocked | allowed | allowed |
| `env.get_all` | blocked | blocked | allowed | allowed |
| `env.delete_var` | blocked | blocked | allowed | allowed |
| `env.load_dotenv` | blocked | blocked | allowed | allowed |

`SYS_ENV` is not granted by `createEnterpriseConfig()` (CLI `standard`). All `env` operations
require `--sandbox-level elevated` or higher.

---

## 5. CLI Security Flags

### Sandbox

| Flag | Description |
|------|-------------|
| `--sandbox-level <level>` | Set sandbox preset: `restricted`, `standard`, `elevated`, `unrestricted`. Default: `standard`. |

### Governance

| Flag | Description |
|------|-------------|
| `--no-governance` | Disable governance engine entirely. `govern.json` is not loaded or checked. |
| `--governance-override` | Override SOFT governance blocks (allow execution to continue). Has no effect on HARD blocks. |
| `--governance-report` | Print a governance summary report to stderr after execution. |
| `--governance-sarif` | Output governance findings in SARIF format (for CI/CD integration). |
| `--governance-junit` | Output governance findings in JUnit XML format. |
| `--governance-baseline-save` | Save the current governance findings as a baseline file for future regression detection. |
| `--env <name>` | Apply the named environment override from `govern.json`'s `environments` map. |
| `--agent-id <name>` | Set agent identity for role-based governance checks (`agent_roles` in `govern.json`). |

---

## 6. govern.json Security Rules

`govern.json` is discovered by searching from the script's directory upward to the
filesystem root. The first `govern.json` found is used. Place it at your project root
to cover all scripts in the project.

### Minimal example

```json
{
  "mode": "HARD",
  "banned_functions": ["eval", "exec"],
  "blocked_imports": ["os", "subprocess"]
}
```

### Full structure reference

```json
{
  "mode": "HARD",

  "banned_functions": ["eval", "exec", "shell"],

  "blocked_imports": ["os", "subprocess", "ctypes"],

  "quality_gate": {
    "max_severity_high": 0,
    "max_severity_medium": 3,
    "max_total_issues": 10
  },

  "governance_baseline": "baseline.json",

  "environments": {
    "production": {
      "mode": "HARD",
      "banned_functions": ["eval", "exec"]
    },
    "development": {
      "mode": "ADVISORY"
    }
  },

  "agent_roles": {
    "data-pipeline": {
      "allowed_languages": ["python", "naab"],
      "blocked_paths": ["/etc", "/proc"]
    }
  },

  "telemetry": {
    "output": "governance-telemetry.jsonl",
    "include_runtime_versions": true
  }
}
```

### Enforcement modes

| Mode | Behavior |
|------|----------|
| `HARD` | Block execution and exit 3. Cannot be overridden. |
| `SOFT` | Block execution, but allow `--governance-override` to proceed with a warning. |
| `ADVISORY` | Warn but do not block execution. |

### Rule types

**`banned_functions`** — List of function names that must not appear in any NAAb or
polyglot block. Checked statically before execution begins.

**`blocked_imports`** — List of module names that must not be imported. Applies to
`use <module>` in NAAb and to `import` statements detected in polyglot blocks.

**`quality_gate`** — Aggregate thresholds for governance findings. If the script's
findings exceed any threshold, execution fails with exit code 2.

**`governance_baseline`** — Path to a JSON file of known-acceptable findings. Only
findings that are *new* relative to the baseline cause failures. Use
`--governance-baseline-save` to create or update the baseline.

**`environments`** — Named overlays applied with `--env <name>`. Keys override the
top-level `govern.json` values for the named environment.

**`agent_roles`** — Per-agent-id restrictions applied when `--agent-id` is specified.
Each role can restrict `allowed_languages`, `allowed_paths`, and `blocked_paths`.

**`telemetry`** — Configure JSONL telemetry output for governance findings, polyglot
execution events, and runtime version tracking.

### Taint analysis

The governance engine performs taint analysis across NAAb and polyglot blocks:

- Sources: `env.get()`, `file.read()`, HTTP response bodies, function arguments marked `@tainted`
- Sinks: `file.write()`, `http.post()`, `eval()`, shell interpolation in polyglot blocks
- Taint propagates through assignments, string concatenation, and function calls
- A taint reaching a sink without sanitization produces a governance finding

CWE and OWASP mappings are included in findings for SARIF/JUnit output.

---

## 7. Exit Codes

| Code | Meaning | Triggered by |
|------|---------|--------------|
| `0` | Success | Script completed normally |
| `1` | Runtime error or sandbox violation | Uncaught exception, sandbox block, `SandboxViolationException` |
| `2` | Quality gate failure | `quality_gate` thresholds exceeded in `govern.json` |
| `3` | Governance hard block | HARD-mode governance rule triggered (e.g., banned function used) |
| `4` | Configuration error | Invalid CLI flags, malformed `govern.json`, bad `--sandbox-level` value |

Exit code `1` covers both runtime errors and sandbox violations. If a script throws
an uncaught exception *and* also triggers a sandbox violation, exit code is still `1`.
Exit code `3` always takes priority over `2` if both conditions are met.

---

## 8. Audit Logging

Every sandbox violation is logged via `Sandbox::logViolation()` before the
`SandboxViolationException` is thrown. Violation records include:

- **operation**: the capability that was attempted (e.g., `FS_WRITE`, `NET_CONNECT`)
- **resource**: the specific target (e.g., file path, host:port)
- **reason**: why it was denied (e.g., `path not in allowed_write_paths`)

Audit logs are written to the audit logger (`naab/audit_logger.h`). By default,
violations are also printed to stderr so they appear in CI/CD logs.

Governance findings are separate from audit logs and are reported via the governance
report (`--governance-report`), SARIF (`--governance-sarif`), or telemetry output.

---

## 9. Common Patterns

### Run untrusted code safely

```bash
naab-lang --no-governance --sandbox-level restricted untrusted_script.naab
```

- `--no-governance`: Skip policy checks (the sandbox is sufficient for isolation)
- `--sandbox-level restricted`: Maximum isolation — no file writes, no network, no env

For additional safety in CI, pipe stdout/stderr and check exit code:

```bash
naab-lang --no-governance --sandbox-level restricted script.naab 2>&1
if [ $? -ne 0 ]; then echo "Script failed or violated sandbox"; fi
```

### Restrict file access to the project directory

Use `standard` level: only `$CWD` and the system temp directory are accessible.

```bash
naab-lang --sandbox-level standard script.naab
```

To further restrict to a specific directory, use the embedding API:

```cpp
naab::security::SandboxConfig cfg =
    naab::security::SandboxConfig::fromPermissionLevel(
        naab::security::PermissionLevel::STANDARD);
cfg.allowed_read_paths = {"/path/to/project"};
cfg.allowed_write_paths = {"/path/to/project/output"};
```

### Disable network access

Both `restricted` and `standard` have `network_enabled = false`. Use either level to
block all `http.*` operations:

```bash
naab-lang --sandbox-level standard script.naab   # file access OK, network blocked
naab-lang --sandbox-level restricted script.naab # file access also blocked
```

### Apply strict governance in CI

```bash
naab-lang \
  --sandbox-level standard \
  --governance-sarif \
  --env production \
  script.naab
```

- `--env production`: apply the production governance overlay (typically mode=HARD)
- `--governance-sarif`: upload SARIF to your security dashboard

### Detect governance regressions

```bash
# Save current baseline after initial review
naab-lang --governance-baseline-save script.naab

# Future runs only fail on NEW findings
naab-lang script.naab
```

### Emergency override for SOFT blocks

```bash
naab-lang --governance-override script.naab
```

Does **not** override HARD blocks (exit 3). Only bypasses SOFT enforcement.
Always audit the override in your CI/CD logs.

---

## See Also

- `docs/API_REFERENCE.md` — Complete stdlib function reference
- `include/naab/public/naab_sandbox.h` — Embedding API for sandbox configuration
- `include/naab/sandbox.h` — Full internal sandbox API (`SandboxConfig`, `Capability`, `PermissionLevel`)
- `src/runtime/sandbox.cpp` — Sandbox enforcement implementation and `fromPermissionLevel()` presets
