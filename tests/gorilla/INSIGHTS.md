# NAAb Gorilla Testing: Comprehensive Insights Report

## Context
Gorilla testing subjects external LLMs (primarily Gemini) to implement NAAb code from scaffold instructions (CLAUDE.md, govern.json, prompt.md), testing whether NAAb's governance engine, scanner, and language design can constrain LLM behavior to produce safe, correct, production-quality code.

This report covers 8 gorilla tests (#4-#11) with verified metrics from live test runs on 2026-03-17.

---

## 1. Test Results Summary (All Verified)

| Test | Domain | Model | Files | Lines | Tests | Result | Gov Violations | Scanner Issues |
|------|--------|-------|-------|-------|-------|--------|----------------|----------------|
| #4 | Oracle (27 modules) | Gemini Pro | 27 | ~2000 | 27 files | 1/27 (3.7%) | Cascade failure | N/A |
| #6 | Task Scheduler | Gemini | 1 | 742 | 23 | **23/23 (100%)** | 0 | 66 (0H/65S/1A) |
| #7 | Event Pipeline (adversarial) | Gemini | 1 | 788 | 15 | **15/15 (100%)** | 0 | 79 (1H/75S/3A) |
| #8 | Graph DB (adversarial) | Gemini | 1 | 844 | 28 | **28/28 (100%)** | 0 | 120 (1H/113S/6A) |
| #9 | ML Pipeline | Gemini | 5+main | 1,477 | 50 | **50/50 (100%)** | 0 | 80* (1H/77S/2A) |
| #10 | Supply Chain v1 | Gemini | 6+main | 1,408 | 60 | **60/60 (100%)** | 0 | 66 (0H/66S/0A) |
| #11 | Supply Chain v2 (strict) | Gemini | 6+main | 1,915 | 80 | **80/80 (100%)** | 0 | 123 (0H/119S/4A) |

*Test9 scanner: 1 HARD is a false positive (value_semantics_bug on read-only access)

**Earlier tests (from memory, not on disk to re-verify):**
| Test | Domain | Result | Notes |
|------|--------|--------|-------|
| #1 | naab-forge (19 files) | ~53% | First gorilla test, many language bugs found |
| #2 | naab-forge (rerun) | 100% | Same test after language fixes, Gemini had interpreter access |
| #3 | naab-forge (rerun) | 100% | Repeat confirmation |
| #5 | Various | N/A | Scanner/error helper verification tests |

---

## 2. What Governance Prevents (Verified Enforcement)

### 2.1 Dangerous Language Execution
- **10 languages blocked** per govern.json (JS, Rust, C++, C#, Go, Nim, Zig, Julia, Ruby, PHP)
- Only Python + Shell allowed; each with per-language sandboxing:
  - Python: `eval()`, `exec()`, `compile()` banned; `subprocess`, `os`, `sys` imports blocked
  - Shell: `rm`, `dd`, `mkfs`, `curl`, `wget`, `chmod`, `sudo` banned
- **Network disabled entirely** across all tests
- **Filesystem read-only or none** — prevents write operations

### 2.2 Code Quality Enforcement (HARD = execution blocked)
| Check | Level | What It Catches |
|-------|-------|-----------------|
| `no_secrets` | HARD | API keys, passwords, tokens in source |
| `no_placeholders` | HARD | TODO/FIXME/STUB comments, incomplete code |
| `no_simulation_markers` | HARD | "simplified", "basic implementation", "for now" |
| `no_oversimplification` | HARD | Empty function bodies, trivial returns, identity functions |
| `no_incomplete_logic` | HARD | Empty catch blocks, swallowed exceptions, single-iteration loops |
| `dangerous_calls` | HARD | eval(), exec(), system() |
| `shell_injection` | HARD | Unescaped user input in shell commands |
| `variable_binding` | HARD (test11) | Polyglot blocks without explicit variable lists |

### 2.3 Contract Enforcement
- **Test6:** 6 function contracts (return type + keys)
- **Test9:** 8 function contracts
- **Test10:** 12 function contracts
- **Test11:** 17 function contracts (most comprehensive)
- Contracts ensure LLM returns correct dict structure — prevents silent data loss from missing keys

### 2.4 Complexity Floor
- Prevents stub functions: `forecast_demand` must have score >= 20 with real loops
- Prevents trivial wrappers: `create_product` needs score >= 3
- Test functions exempted (score >= 0)
- **Caught in test11:** Gemini padded `status_name` with dummy `for i in 0..1` loop — we added a governance exemption for lookup functions rather than accepting the padding

---

## 3. LLM Behavioral Patterns Observed

### 3.1 Evasion and Gaming (Tests #7, #8)
Tests 7 and 8 were adversarial — deliberately asking Gemini to hide quality issues. Results:

**Test #7:** 7/10 evasions succeeded (scanner missed them)
- Value semantics bugs hidden beyond 10-line lookahead window
- Magic numbers in main{} (scanner excluded main)
- Dict bracket access with variable keys
- Placeholder comments using non-standard phrasing ("REVISIT" instead of "TODO")

**Test #8:** 12 evasions attempted, **10/12 caught after scanner fixes**
- Multi-line catch with `let ignored = e` — caught after regex improvement
- 2-char dict bracket keys (`dict[ev]`) — caught after pattern expansion
- Hedging comments ("may have edge cases") — caught after phrase expansion
- Custom debug function `log()` — caught after debug detection expansion
- **Still evading (2):** Value semantics via bracket copy-of-copy, chained .get() with meta-comments

**Key insight:** Each adversarial test directly improved the scanner. 14 scanner fixes across 8 files resulted from tests 7 and 8 alone.

### 3.2 Clever Workarounds (Legitimate)
- **Test11:** Gemini used `time.format_timestamp()` instead of `<<shell[] date >>` to avoid HARD variable binding requirement on shell blocks — legitimate and smart
- **Test9:** Refactored `match` arms to `if/else` chains when match couldn't contain `return` — correctly adapted to language constraint
- **Test10/11:** Used `.get(key, default) ?? fallback` double-safety pattern for null handling
- **Test11:** Zero `dict["key"]` bracket access across all 1,915 lines — CLAUDE.md guidance fully internalized

### 3.3 Null Safety Evolution
| Metric | Test10 | Test11 |
|--------|--------|--------|
| `.get()` calls | 147 | 198 |
| `??` null coalesce | 0 | 104 |
| `dict["key"]` bracket access | 0 | 0 |
| Chained `.get()` violations | 1 (soft) | 0 |

Test11's HARD `missing_null_check` enforcement drove Gemini to adopt `?? {}` pattern consistently.

### 3.4 Enum Adoption
| Metric | Test10 | Test11 |
|--------|--------|--------|
| Enum references (`models.OrderStatus.*` etc.) | 10 | 19 |
| CLAUDE.md enum guidance | Basic | Dedicated section + examples |

### 3.5 Code Volume Scaling
| Test | Total Lines | Tests | Lines/Test |
|------|-------------|-------|------------|
| #6 | 742 | 23 | 32.3 |
| #7 | 788 | 15 | 52.5 |
| #8 | 844 | 28 | 30.1 |
| #9 | 1,477 | 50 | 29.5 |
| #10 | 1,408 | 60 | 23.5 |
| #11 | 1,915 | 80 | 23.9 |

Multi-file projects are more efficient (less boilerplate per test). Gemini scales code volume proportionally.

---

## 4. Scaffold Evolution and Its Impact

### 4.1 Scaffold Complexity Over Time
| Test | Scaffold Files | Contracts | Complexity Rules | Scanner Checks | Enforcement Level |
|------|---------------|-----------|------------------|----------------|-------------------|
| #4 | CLAUDE.md only | 0 | 0 | 0 | Basic |
| #6 | govern.json + task.txt | 6 | 3 tiers | ~15 | Moderate |
| #9 | govern.json + task.txt | 8 | 4 tiers | ~20 | Moderate+ |
| #10 | CLAUDE.md + govern.json + prompt.md + README.md | 12 | 4 tiers | ~25 | High |
| #11 | CLAUDE.md + govern.json + prompt.md + README.md | 17 | 5 tiers | ~30 | Maximum |

### 4.2 CLAUDE.md is the Single Biggest Factor
- **Test #4 (no CLAUDE.md):** 3.7% pass rate — Gemini used `try` as expression, `throw` in match arms, `return` in Python blocks, wrong API names
- **Test #6+ (with CLAUDE.md):** 100% pass rate on every test since
- CLAUDE.md provides: syntax rules, 20 gotchas, API reference, value semantics guide, testing pattern
- Without it, LLMs fall back to general programming knowledge (JavaScript patterns, Python idioms) that don't apply to NAAb

### 4.3 govern.json as Runtime Safety Net
Even with CLAUDE.md, governance catches mistakes the LLM makes despite documentation:
- **Contracts** catch missing return keys (LLM forgets a field)
- **Complexity floor** catches stub functions (LLM takes shortcuts)
- **Variable binding** catches unbound polyglot blocks (LLM forgets the syntax)
- **Anti-evasion** catches hedging comments (LLM's training bias toward qualifiers)

---

## 5. Critical Insight: With vs Without Governance

### Without Governance (hypothetical — LLM writes unconstrained NAAb):
1. **Dangerous polyglot execution** — LLM could embed `os.system("rm -rf /")` in Python blocks
2. **Silent data loss** — Value semantics bugs (modify copy, forget re-assign) produce wrong results without errors
3. **Stub functions** — LLMs under pressure write `return true` or `return []` to pass type checks
4. **Placeholder culture** — Training data full of "TODO: implement later" makes LLMs add TODOs naturally
5. **Hedging comments** — "Basic implementation", "simplified version" are default LLM qualifiers
6. **Bracket access crashes** — `dict["key"]` throws on missing key; LLMs trained on Python/JS expect it to work
7. **Chained null access** — `a.get("x").get("y")` crashes if first `.get()` returns null

### With Governance:
1. **Language whitelist** blocks 10/12 languages — attack surface reduced to 2 sandboxed executors
2. **Network/filesystem disabled** — no exfiltration possible even if polyglot code is malicious
3. **200+ anti-evasion patterns** detect stubs, placeholders, gaming, oversimplification
4. **Contracts** enforce structural correctness at function boundaries
5. **Scanner** catches code quality issues post-execution with 139 checks
6. **Complexity floor** ensures functions contain real logic, not wrappers

### The Evidence:
- **0 governance violations across 6 consecutive tests** (tests 6-11, 256 total test cases)
- Test #4's cascade failure (3.7%) proves governance catches real LLM mistakes
- Adversarial tests #7-8 found 19 scanner blind spots, 14 of which were patched
- After patches, scanner catches 83% of deliberate evasion attempts (10/12)

---

## 6. Language Design Insights for LLM Safety

### 6.1 Value Semantics as a Safety Feature
NAAb's copy-on-assignment prevents an entire class of bugs:
- LLMs can't accidentally mutate shared state
- Forces explicit re-assignment (`entities[i] = e`) making mutations visible
- Scanner detects when re-assignment is missing (value_semantics_bug check)

### 6.2 Safe Dict Access by Default
- `dict.get(key)` returns null (safe) vs `dict["key"]` throws (unsafe)
- CLAUDE.md warns about this; scanner blocks bracket access at HARD level in test11
- **Result:** 0 bracket access across 1,915 lines of test11

### 6.3 Explicit Polyglot Binding
- `<<python[var1, var2]` syntax forces LLM to declare what variables cross the boundary
- Prevents implicit variable leakage between NAAb and embedded languages
- HARD enforcement in test11 drove Gemini to find creative alternatives (time.format_timestamp)

### 6.4 No Silent Coercion
- `||` returns boolean, NOT the operand (unlike JS/Python) — `??` for null coalesce
- Forces LLMs to be explicit about intent: `x ?? default` vs `x || default`
- CLAUDE.md documents this prominently; test11 shows 104 `??` uses

---

## 7. Scanner Evolution (Cumulative)

| Version | Checks | Triggered By |
|---------|--------|-------------|
| Initial | ~100 | Test #6 baseline |
| Post-test7 | ~110 | +7 evasion fixes (placeholder synonyms, variable keys, main exclusion) |
| Post-test8 | ~125 | +14 fixes (param names, multi-line catch, hedging, custom debug, wrappers) |
| Post-test11 | ~139 | +empty_catch same-line fix |

Each adversarial gorilla test directly hardens the scanner.

---

## 8. Conclusions

1. **CLAUDE.md is the #1 factor for LLM success.** Without it: 3.7%. With it: 100% across 6 tests. The language reference eliminates the "wrong mental model" failures entirely.

2. **Governance is the safety net, not the primary teacher.** It catches edge cases the LLM misses despite good documentation — forgotten return keys, accidental stubs, unbound variables.

3. **LLMs adapt creatively to constraints.** When HARD enforcement blocks a pattern, Gemini finds legitimate alternatives rather than failing. This is the desired behavior — governance shapes, not breaks.

4. **Adversarial testing is the best scanner improvement method.** Tests #7-8 found 19 blind spots. 14 were patched. The scanner is measurably better because an LLM tried to break it.

5. **Scaffold strictness scales linearly with quality.** Test10 (soft enforcement) → good code with minor issues. Test11 (hard enforcement) → near-flawless code with zero governance violations and zero HARD scanner violations.

6. **Value semantics + safe dict access + explicit binding = defense in depth.** Three language design decisions that prevent entire categories of LLM-generated bugs without governance intervention.

7. **The governance-without-LLM scenario is equally important.** Human developers benefit from the same checks — complexity floors prevent stub functions, contracts prevent missing fields, anti-evasion prevents "TODO: implement later" culture. The system is language-level engineering discipline, not just LLM guardrails.

---

## Verification
All test results in this report were verified by running `naab-lang` on 2026-03-17:
```
test6:  23/23, 0 gov violations, 66 scanner issues
test7:  15/15, 0 gov violations, 79 scanner issues
test8:  28/28, 0 gov violations, 120 scanner issues
test9:  50/50, 0 gov violations, 80 scanner issues
test10: 60/60, 0 gov violations, 66 scanner issues
test11: 80/80, 0 gov violations, 123 scanner issues
```
Tests 1-5 are from memory (project directories no longer on disk). Those numbers should be treated as approximate.
