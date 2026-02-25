# Chapter 21: Governance and LLM Code Quality

NAAb includes a built-in governance engine that enforces project-level policies on polyglot code blocks. By placing a `govern.json` file in your project directory, you can control which languages are allowed, what APIs can be called, enforce code quality standards, and — uniquely — detect common LLM code generation failures like oversimplified stubs, incomplete error handling, and hallucinated APIs.

## 21.1 What is Governance?

Governance is **policy-as-code** for your polyglot blocks. Every polyglot block (`<<python ... >>`, `<<javascript ... >>`, etc.) is checked against your governance rules before execution. If a rule is violated, execution is blocked or a warning is emitted, depending on the enforcement level.

### 21.1.1 Three Enforcement Levels

NAAb uses a three-tier enforcement model inspired by HashiCorp Sentinel:

| Level | Behavior | Override |
|-------|----------|----------|
| **HARD** | Block execution. Cannot be overridden. | None |
| **SOFT** | Block execution. Can be overridden with `--governance-override`. | `--governance-override` |
| **ADVISORY** | Warn only. Execution continues. | N/A (always continues) |

### 21.1.2 Three Governance Modes

| Mode | Behavior |
|------|----------|
| **enforce** | Normal enforcement (default). Rules are checked and enforced. |
| **audit** | Dry-run. All rules are checked but nothing is blocked. Useful for testing new rules. |
| **off** | Governance disabled entirely. |

### 21.1.3 Zero Overhead

When no `govern.json` file exists, the governance engine is completely inactive. There is no performance impact on programs that don't use governance.

## 21.2 Quick Start

### Step 1: Create `govern.json`

Place a `govern.json` file in the same directory as your `.naab` file (or any parent directory):

```json
{
  "version": "3.0",
  "mode": "enforce",

  "languages": {
    "allowed": ["python", "javascript"]
  },

  "code_quality": {
    "no_secrets": { "level": "hard" },
    "no_oversimplification": { "level": "soft" },
    "no_incomplete_logic": { "level": "soft" },
    "no_hallucinated_apis": { "level": "advisory" }
  }
}
```

### Step 2: Run Your Program

```bash
naab-lang my_program.naab
```

If governance rules are loaded, you'll see:

```
[governance] Loaded: /path/to/govern.json (mode: enforce)
```

### Step 3: Read the Summary

After execution, a summary is printed showing which checks passed, which were warned, and which were blocked:

```
=== Governance Summary ===
  PASS  code_quality.no_secrets [HARD]
  PASS  code_quality.no_oversimplification [SOFT]
  WARN  code_quality.no_hallucinated_apis [ADVISORY]
    ".push() is JavaScript — in Python, use .append()"
```

## 21.3 The `govern.json` Reference

The governance configuration file has 13 sections. All sections are optional — only configure what you need.

### 21.3.1 Top-Level Fields

```json
{
  "version": "3.0",
  "mode": "enforce",
  "description": "My project governance rules"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `version` | string | Config version. Use `"3.0"`. |
| `mode` | string | `"enforce"`, `"audit"`, or `"off"`. |
| `description` | string | Human-readable description. |

### 21.3.2 Languages

Control which languages can be used and set per-language rules:

```json
{
  "languages": {
    "allowed": ["python", "javascript", "rust"],
    "blocked": ["shell"],
    "per_language": {
      "python": {
        "timeout": 15,
        "max_lines": 100,
        "banned_functions": ["eval(", "exec(", "compile("],
        "imports": {
          "mode": "blocklist",
          "blocked": ["subprocess", "ctypes", "os.system"]
        }
      },
      "javascript": {
        "timeout": 10,
        "banned_functions": ["eval(", "Function("],
        "imports": {
          "mode": "blocklist",
          "blocked": ["child_process", "fs"]
        }
      }
    }
  }
}
```

### 21.3.3 Capabilities

Control what polyglot blocks can access:

```json
{
  "capabilities": {
    "network": { "enabled": false },
    "filesystem": { "mode": "read" },
    "shell": { "enabled": false }
  }
}
```

Capabilities also support the legacy flat format: `"network": false`.

### 21.3.4 Limits

Set resource limits for execution:

```json
{
  "limits": {
    "timeout": {
      "global": 30,
      "per_block": 15
    },
    "execution": {
      "call_depth": 50,
      "polyglot_blocks": 20
    },
    "data": {
      "array_size": 10000,
      "string_length": 100000
    },
    "code": {
      "max_lines_per_block": 200,
      "max_nesting_depth": 8
    }
  }
}
```

### 21.3.5 Requirements

Enforce structural requirements:

```json
{
  "requirements": {
    "main_block": {
      "level": "soft",
      "message": "Programs should have a main block"
    }
  }
}
```

### 21.3.6 Restrictions

Set security restrictions for dangerous operations:

```json
{
  "restrictions": {
    "dangerous_calls": { "level": "hard" },
    "shell_injection": { "level": "hard" },
    "privilege_escalation": { "level": "hard" },
    "imports": {
      "level": "soft",
      "mode": "blocklist",
      "blocked": {
        "python": ["subprocess", "ctypes", "pickle"],
        "javascript": ["child_process", "fs", "net"]
      }
    }
  }
}
```

### 21.3.7 Code Quality

The largest section. Each check can be set as a simple string (`"hard"`, `"soft"`, `"advisory"`) or as an object with detailed configuration:

```json
{
  "code_quality": {
    "no_secrets": { "level": "hard" },
    "no_placeholders": "soft",
    "no_simulation_markers": { "level": "hard" },
    "no_oversimplification": { "level": "soft" },
    "no_incomplete_logic": { "level": "soft" },
    "no_hallucinated_apis": { "level": "advisory" }
  }
}
```

See sections 21.4 and 21.5 for the full list of code quality checks.

### 21.3.8 Custom Rules

Define your own regex-based governance rules:

```json
{
  "custom_rules": [
    {
      "id": "CUSTOM-001",
      "name": "no_eval_python",
      "description": "Prevent eval() in Python",
      "pattern": "\\beval\\s*\\(",
      "languages": ["python"],
      "level": "hard",
      "message": "eval() is forbidden — use ast.literal_eval() or json.loads()",
      "enabled": true
    }
  ]
}
```

See section 21.8 for the full custom rules reference.

### 21.3.9 Output

Control how governance results are displayed:

```json
{
  "output": {
    "summary": {
      "enabled": true,
      "format": "detailed",
      "show_passing": true
    },
    "errors": {
      "verbose": true,
      "show_help": true,
      "max_errors_per_rule": 3
    }
  }
}
```

### 21.3.10 Audit

Configure audit logging:

```json
{
  "audit": {
    "level": "basic",
    "log_events": {
      "checks_passed": true,
      "checks_failed": true
    }
  }
}
```

### 21.3.11 Meta

Schema validation settings:

```json
{
  "meta": {
    "schema_validation": {
      "warn_unknown_keys": true,
      "suggest_corrections": true
    }
  }
}
```

## 21.4 Code Quality Checks

NAAb provides 20 built-in code quality checks. Each can be enabled individually and set to any enforcement level.

### Standard Checks

| Check | Default Level | Description |
|-------|---------------|-------------|
| `no_secrets` | hard | Detects API keys, passwords, tokens (entropy-based + pattern matching) |
| `no_placeholders` | soft | Detects TODO, FIXME, STUB, PLACEHOLDER markers |
| `no_hardcoded_results` | advisory | Detects fabricated return values like `return {"status": "ok"}` |
| `no_pii` | advisory | Detects SSNs, credit card numbers, email addresses |
| `no_temporary_code` | soft | Detects temporary/throwaway code patterns |
| `no_simulation_markers` | hard | Detects "Simulated", "Mock", "Fake" markers in code |
| `no_mock_data` | advisory | Detects hardcoded mock/test data |
| `no_apologetic_language` | advisory | Detects "sorry", "unfortunately" in code comments |
| `no_dead_code` | advisory | Detects commented-out code blocks |
| `no_debug_artifacts` | soft | Detects console.log, print statements used for debugging |
| `no_unsafe_deserialization` | hard | Detects pickle.loads, yaml.load without SafeLoader |
| `no_sql_injection` | hard | Detects string concatenation in SQL queries |
| `no_path_traversal` | hard | Detects `../../` path traversal patterns |
| `no_hardcoded_urls` | advisory | Detects hardcoded URLs (configurable allowlist) |
| `no_hardcoded_ips` | advisory | Detects hardcoded IP addresses |
| `max_complexity` | advisory | Enforces max lines, nesting depth, cyclomatic complexity |
| `encoding` | advisory | Blocks null bytes, Unicode BiDi attacks, homoglyphs |

### Example: Configuring `no_secrets`

```json
"no_secrets": {
  "level": "hard",
  "entropy_check": {
    "enabled": true,
    "threshold": 4.5,
    "min_length": 20
  }
}
```

This enables Shannon entropy checking — strings with high randomness and sufficient length are flagged as potential secrets, even without matching known API key patterns.

## 21.5 LLM Anti-Drift Checks

These three checks specifically target common failures in LLM-generated code. They are designed to catch the patterns that make AI-generated code "look right" but behave incorrectly.

### 21.5.1 `no_oversimplification`

**Purpose:** Detect stub, trivial, and placeholder implementations where an LLM produced the minimal possible code instead of real logic.

**What it catches:**

- Empty function bodies (`def process(data): pass`)
- Trivial returns (`def validate(x): return True`)
- Identity/passthrough functions (`def transform(x): return x`)
- Not-implemented markers (`raise NotImplementedError`)
- Comment-only bodies (`# implementation here`)
- Fabricated results (`return {"status": "ok"}`)

**Example violation:**

```naab
main {
    let result = <<python
def validate_user(user):
    return True  // <- Governance blocks this

def process_data(data):
    pass         // <- Governance blocks this
>>
}
```

**Error output:**

```
Error: Governance error: Oversimplified code: "def process_data(data):
    pass" [HARD-MANDATORY]

  Rule (govern.json): code_quality.no_oversimplification

  Help:
  - This looks like a stub or trivial implementation.
  - LLMs often produce minimal code that passes syntax checks but lacks real logic.

  Example:
    X Blocked:  def validate(data): return True
    V Allowed:  def validate(data):
                    if not isinstance(data, dict): raise TypeError(...)
```

**Configuration:**

```json
"no_oversimplification": {
  "level": "soft",
  "check_empty_bodies": true,
  "check_trivial_returns": true,
  "check_identity_functions": true,
  "check_not_implemented": true,
  "check_comment_only_bodies": true,
  "check_fabricated_results": true,
  "case_sensitive": false,
  "min_function_lines": 2,
  "custom_patterns": []
}
```

Each sub-check can be toggled independently. For example, if you want to allow `NotImplementedError` in development but block empty bodies:

```json
"no_oversimplification": {
  "level": "hard",
  "check_not_implemented": false,
  "check_empty_bodies": true
}
```

### 21.5.2 `no_incomplete_logic`

**Purpose:** Detect logic gaps, missing error handling, degenerate control flow, and shortcuts that indicate the LLM took a shortcut instead of implementing complete logic.

**What it catches:**

- Empty/swallowed error handling (`except: pass`, empty catch blocks)
- Generic/vague error messages (`raise Exception("error")`)
- Bare raises (`raise` without exception type)
- Degenerate loops (loops that return/break on first iteration)
- Always-true/false conditions (`if True:`, `if False:`)
- Placeholder error messages (`"Something went wrong"`)

**Example violation:**

```naab
main {
    let result = <<python
try:
    data = open("file.txt").read()
except:
    pass  // <- Governance blocks this: swallowed exception
>>
}
```

**Error output:**

```
Error: Governance error: Incomplete logic: "except:
    pass" [HARD-MANDATORY]

  Rule (govern.json): code_quality.no_incomplete_logic

  Help:
  - This code has logic gaps that indicate shortcuts or lazy implementation.
  - Common issues: empty catch blocks, generic error messages, degenerate loops.

  Example:
    X Blocked:  except Exception: pass  # swallows all errors
    V Allowed:  except ValueError as e:
                    logger.error(f"Validation failed: {e}")
                    raise
```

**Configuration:**

```json
"no_incomplete_logic": {
  "level": "soft",
  "check_empty_catch": true,
  "check_swallowed_exceptions": true,
  "check_generic_errors": true,
  "check_vague_error_messages": true,
  "check_single_iteration_loops": true,
  "check_bare_raise": true,
  "check_always_true_false": true,
  "check_missing_validation": true,
  "case_sensitive": false,
  "custom_patterns": []
}
```

### 21.5.3 `no_hallucinated_apis`

**Purpose:** Detect calls to non-existent APIs, wrong method names, and cross-language confusion. LLMs frequently mix up APIs between Python and JavaScript — this check catches those mistakes and suggests the correct alternative.

**What it catches:**

**Python blocks using JavaScript APIs:**
- `.push()` instead of `.append()`
- `.length` instead of `len()`
- `console.log()` instead of `print()`
- `JSON.stringify()` instead of `json.dumps()`
- `Math.floor()` instead of `math.floor()`
- `===` instead of `==`
- `null` instead of `None`

**JavaScript blocks using Python APIs:**
- `print()` instead of `console.log()`
- `len()` instead of `.length`
- `.append()` instead of `.push()`
- `.strip()` instead of `.trim()`
- `True`/`False`/`None` instead of `true`/`false`/`null`
- `and`/`or`/`not` instead of `&&`/`||`/`!`

**Cross-language syntax confusion:**
- `//` comments in Python (should be `#`)
- `#` comments in JavaScript (should be `//`)

**Example violation:**

```naab
main {
    let result = <<python
data = [1, 2, 3]
data.push(4)       // <- ".push() is JavaScript — in Python, use .append()"
length = data.length  // <- ".length is JavaScript — in Python, use len()"
>>
}
```

**Error output:**

```
Error: Governance error: Hallucinated API in python block: ".length" [SOFT-MANDATORY]

  Rule (govern.json): code_quality.no_hallucinated_apis

  Help:
  - .length is JavaScript — in Python, use len()
```

**Configuration:**

```json
"no_hallucinated_apis": {
  "level": "advisory",
  "check_cross_language": true,
  "check_made_up_functions": true,
  "check_wrong_syntax": true,
  "case_sensitive": true,
  "custom_patterns": []
}
```

The check is language-aware: it automatically selects the appropriate pattern set based on the polyglot block's language header (`<<python`, `<<javascript`, etc.).

## 21.6 Security Checks

Beyond code quality, governance includes dedicated security checks:

| Check | Default Level | Description |
|-------|---------------|-------------|
| `shell_injection` | hard | Detects shell injection patterns (backticks, `$(...)`, `os.system`) |
| `code_injection` | hard | Detects dynamic code execution (`eval`, `exec`, `Function()`) |
| `privilege_escalation` | hard | Detects `sudo`, `chmod 777`, `os.setuid` |
| `data_exfiltration` | hard | Detects base64 encoding + network send patterns |
| `resource_abuse` | soft | Detects fork bombs, crypto mining patterns |
| `info_disclosure` | advisory | Detects information leakage patterns |
| `crypto_weakness` | advisory | Detects weak crypto (MD5, SHA1, DES, ECB mode) |

These are configured in the `restrictions` section of `govern.json`.

## 21.7 Per-Language Rules

Each language can have its own set of rules for fine-grained control:

```json
{
  "languages": {
    "per_language": {
      "python": {
        "timeout": 15,
        "max_lines": 100,
        "banned_functions": ["eval(", "exec("],
        "imports": {
          "mode": "blocklist",
          "blocked": ["subprocess", "ctypes"]
        },
        "style": {
          "no_star_imports": { "level": "advisory" }
        }
      },
      "javascript": {
        "timeout": 10,
        "max_lines": 80,
        "banned_functions": ["eval(", "Function("],
        "style": {
          "strict_mode": { "level": "soft" },
          "no_var": { "level": "advisory" },
          "no_console_log": { "level": "advisory" }
        }
      },
      "shell": {
        "timeout": 5,
        "banned_commands": ["rm -rf", "mkfs"],
        "style": {
          "require_set_e": { "level": "soft" },
          "no_curl_pipe_sh": { "level": "hard" }
        }
      }
    }
  }
}
```

## 21.8 Custom Rules

For project-specific policies, define custom regex-based rules:

```json
{
  "custom_rules": [
    {
      "id": "PROJ-001",
      "name": "no_hardcoded_urls",
      "description": "All URLs must come from config",
      "pattern": "https?://(?!localhost|127\\.0\\.0\\.1)",
      "languages": ["python", "javascript"],
      "level": "soft",
      "message": "Hardcoded URLs are not allowed. Use config.get_url() instead.",
      "help": "Move URLs to your configuration file",
      "good_example": "url = config.get_url('api')",
      "bad_example": "url = 'https://api.example.com'",
      "enabled": true,
      "case_sensitive": false
    },
    {
      "id": "PROJ-002",
      "name": "no_print_debug",
      "description": "No debug print statements",
      "pattern": "print\\(.*[Dd]ebug",
      "languages": [],
      "level": "advisory",
      "message": "Debug print statements should be removed",
      "enabled": true
    }
  ]
}
```

### Custom Rule Fields

| Field | Required | Description |
|-------|----------|-------------|
| `id` | Yes | Unique rule identifier (shown in errors) |
| `name` | Yes | Short name for the rule |
| `description` | No | Human-readable description |
| `pattern` | Yes | Regex pattern to match violations |
| `languages` | No | Languages to apply to (empty = all languages) |
| `level` | No | Enforcement level (default: `"hard"`) |
| `message` | No | Error message shown on violation |
| `help` | No | Help text explaining how to fix |
| `good_example` | No | Example of correct code |
| `bad_example` | No | Example of incorrect code |
| `enabled` | No | Whether rule is active (default: `true`) |
| `case_sensitive` | No | Case-sensitive matching (default: `false`) |

## 21.9 CLI Flags and Reports

### Governance CLI Flags

| Flag | Description |
|------|-------------|
| `--governance-override` | Override SOFT-level governance rules (HARD rules cannot be overridden) |
| `--governance-report <path>` | Write JSON governance report to file |
| `--governance-sarif <path>` | Write SARIF report (for IDE/CI integration) |
| `--governance-junit <path>` | Write JUnit XML report (for CI pipelines) |

### Examples

```bash
# Run with governance and override soft rules
naab-lang --governance-override my_program.naab

# Generate a JSON report for review
naab-lang --governance-report report.json my_program.naab

# Generate SARIF for GitHub Code Scanning
naab-lang --governance-sarif results.sarif my_program.naab

# Generate JUnit for CI (Jenkins, GitLab CI, etc.)
naab-lang --governance-junit results.xml my_program.naab
```

### SARIF Integration

SARIF (Static Analysis Results Interchange Format) reports can be uploaded to GitHub Code Scanning:

```yaml
# .github/workflows/governance.yml
- name: Run NAAb Governance
  run: naab-lang --governance-sarif results.sarif my_program.naab

- name: Upload SARIF
  uses: github/codeql-action/upload-sarif@v3
  with:
    sarif_file: results.sarif
```

## 21.10 Best Practices

### For CI/CD Pipelines

Use `enforce` mode with `--governance-sarif` or `--governance-junit` for machine-readable results:

```json
{
  "version": "3.0",
  "mode": "enforce",
  "code_quality": {
    "no_secrets": { "level": "hard" },
    "no_sql_injection": { "level": "hard" },
    "no_path_traversal": { "level": "hard" },
    "no_unsafe_deserialization": { "level": "hard" }
  }
}
```

### For LLM-Generated Code Review

Enable all three LLM anti-drift checks to catch the most common AI code failures:

```json
{
  "version": "3.0",
  "mode": "enforce",
  "code_quality": {
    "no_oversimplification": { "level": "hard" },
    "no_incomplete_logic": { "level": "hard" },
    "no_hallucinated_apis": { "level": "soft" },
    "no_simulation_markers": { "level": "hard" },
    "no_placeholders": { "level": "soft" }
  }
}
```

### For Team Projects

Start with `audit` mode to understand what would be flagged, then switch to `enforce`:

```json
{
  "version": "3.0",
  "mode": "audit",
  "description": "Team governance rules (audit mode for rollout)",
  "languages": {
    "allowed": ["python", "javascript"]
  },
  "code_quality": {
    "no_secrets": { "level": "hard" },
    "no_oversimplification": { "level": "soft" },
    "no_incomplete_logic": { "level": "advisory" }
  }
}
```

### Gradual Adoption

1. Start with `"mode": "audit"` to see what gets flagged without blocking anything
2. Enable security checks first (`no_secrets`, `no_sql_injection`, `no_unsafe_deserialization`)
3. Add LLM checks at `"advisory"` level, then promote to `"soft"` or `"hard"` as confidence grows
4. Use `custom_rules` for project-specific patterns
5. Generate reports with `--governance-sarif` for CI integration
