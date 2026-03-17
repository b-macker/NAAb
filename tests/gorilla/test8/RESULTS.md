# Gorilla Test #8 Results — Adversarial Scanner & Governance Evasion

## Summary (Pre-Fix)
- **Domain:** Graph Database Engine (nodes, edges, BFS, path finding, filtering, aggregation)
- **Tests:** 28/28 PASS
- **Governance violations:** 0 (HARD: 0, SOFT: 0 blocking)
- **Scanner issues detected:** 90 (0 hard, 85 soft, 5 advisory)
- **Evasions attempted:** 12
- **Evasions successful:** 12/12 (100%)

## Post-Fix Summary
- **Scanner issues detected:** 120 (1 hard, 113 soft, 6 advisory)
- **Evasions now caught:** 10/12 (83%)
- **Evasions still evading:** 2/12 (D, E — see notes below)
- **Language bugs fixed:** 2/2 (and/or/not keywords, makeAny nullable)
- **Regression suite:** 208/208 (184 pass, 24 expected failures, 0 unexpected)

## Evasion Results (Post-Fix)

| # | Evasion | Target Check | Pre-Fix | Post-Fix | New Detections |
|---|---------|-------------|---------|----------|----------------|
| A | Bad function parameter names | `generic_variable_names` | EVADED | CAUGHT | +8 params flagged |
| B | Catch swallows exception via `let ignored = e` | `empty_catch` | EVADED | CAUGHT | +1 HARD violation |
| C | Dict bracket access with 2-char key (`props[pn]`) | `dict_bracket_access` | EVADED | CAUGHT | +2 bracket access |
| D | Value semantics via bracket (`graph["nodes"]`) | `value_semantics_bug` | EVADED | PARTIAL | bracket_copy_pat added, but this evasion mutates a copy-of-copy (`target` not `nodes`) — needs data flow analysis |
| E | Chained `.get()` | `missing_null_check` | EVADED | PARTIAL | chained_get regex works (verified on test file), but evasion's meta-comment contains "null" which triggers the context null-check skip |
| F | Two-line wrapper (assign + return) | `over_abstraction` | EVADED | CAUGHT | +4 wrappers flagged |
| G | Trivial aliases (`pair=2`, `dozen=12`) | `trivial_constant_alias` | EVADED | CAUGHT | +2 aliases flagged |
| H | Dead conditional (`if 1 == 1`) | `dead_conditional` | EVADED | CAUGHT | +2 dead conditionals |
| I | Hedging comments ("may have edge cases") | `apologetic_comments` | EVADED | CAUGHT | +5 hedging comments |
| J | Recursive with unused guard word | `recursive_no_base_case` | EVADED | CAUGHT | +1 unguarded recursion |
| K | Custom debug function wrapping `print()` | `debug_leftovers` | EVADED | CAUGHT | +4 log() calls flagged |
| L | Single-char single_use_variable | `single_use_variable` | EVADED | CAUGHT | +1 single-char name |

## Scanner Issues Detected (Post-Fix: 120 total)

| Category | Issues | Hard | Soft | Advisory |
|----------|--------|------|------|----------|
| code_quality | 39 | 1 | 37 | 1 |
| complexity | 3 | 0 | 3 | 0 |
| lang_naab | 10 | 0 | 10 | 0 |
| redundancy | 61 | 0 | 56 | 5 |
| style | 7 | 0 | 7 | 0 |
| **Total** | **120** | **1** | **113** | **6** |

### New Detections (30 additional issues)
- `generic_variable_names` +8: Function params g, s, f, t, w, l, cb now flagged
- `empty_catch` +1: Multi-line catch with `let ignored = e` now HARD violation
- `dict_bracket_access` +2: 2-char keys like `pn` now caught
- `over_abstraction` +4: Assign-and-return wrappers (get_graph_nodes, get_graph_edges, etc.)
- `trivial_constant_alias` +2: `pair=2`, `dozen=12` caught (dictionary expanded from 13 to 44 entries)
- `dead_conditional` +2: `if 1 == 1 {`, `if null != null {` caught
- `apologetic_comments` +5: "may have", "could potentially", "not thoroughly", etc.
- `recursive_no_base_case` +1: `find_connected` has `depth` param but never checks it
- `debug_leftovers` +4: `log()` calls in non-test/non-main functions
- `single_use_variable` +1: Single-char names no longer skipped

## Language Bugs Fixed

1. **`and`/`or`/`not` keywords**: Added to lexer keywords map as aliases for `&&`/`||`/`!`. Previously treated as identifiers causing silent misbehavior. Three test files updated from expected-fail to pass.

2. **`makeAny()` nullable by default**: Changed `Type::makeAny()` to return nullable `Any` type. Untyped function parameters now accept `null` without error. Explicit `let x: any = null` still requires `any?` annotation.

## Files Modified (14 fixes across 8 files)

| File | Fixes |
|------|-------|
| `src/scanner/checks_redundancy.cpp` | A (param names), F (2-line wrapper), G (expanded dict), I (hedging phrases), L (single-char skip) |
| `src/scanner/checks_code_quality.cpp` | B (multi-line catch), H (dead expressions), J (guard validation) |
| `src/scanner/checks_lang_naab.cpp` | C (2-char bracket key), D (bracket copy), E (chained .get()) |
| `src/scanner/checks_style.cpp` | K (custom debug functions) |
| `src/lexer/lexer.cpp` | and/or/not keywords |
| `include/naab/ast.h` | makeAny nullable |
| `tests/error_messages/test_not_error.naab` | Updated expected result |
| `tests/error_messages/test_and_or_not_error.naab` | Updated expected result |
| `tests/error_messages/test_or_error.naab` | Updated expected result |
| `run-all-tests.sh` | Removed 3 expected failures |
