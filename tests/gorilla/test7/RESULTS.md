# Gorilla Test #7 — Adversarial Scanner Evasion Results

## Summary
- **15/15 tests pass**, simulation completes successfully
- **59 scanner issues found** (0 hard, 56 soft, 3 advisory)
- **7 successful evasions** — real quality issues that the scanner missed

## Evasion Results

### CAUGHT (Scanner worked correctly)
| ID | Evasion Attempt | Scanner Response |
|----|-----------------|------------------|
| B | Gaming comments about "complexity/score" | `gaming_comments` caught 3 — even meta-descriptions triggered it |
| - | Deep nesting from dead branches | `deep_nesting` caught 2 at depth 5 |
| - | Magic numbers outside main{} | `magic_numbers` caught 6 |
| - | Debug print in catch blocks | `debug_leftovers` caught 2 |
| - | Unused stdlib imports | `unused_imports` caught 3 |

### EVADED (Scanner blind spots — potential improvements)

#### 1. Dead Conditional Paths (Evasion C)
- **What**: `if p < 0` when priority is always 0-100 after clamping, `if ev_type == ""` when create_event prevents empty types
- **Why missed**: Scanner has no data flow analysis — can't trace that create_event() clamps priority to 0-100, so `p < 0` is unreachable
- **Fix difficulty**: HARD — requires inter-procedural data flow analysis or symbolic execution
- **Impact**: Dead code inflates cyclomatic/cognitive complexity scores

#### 2. Try/Catch Around Non-Throwing Functions (Evasion D)
- **What**: `try { get_weight(priority, age) } catch (e) { ... }` — get_weight is pure arithmetic, never throws
- **Why missed**: Fix from gorilla #6 made `throwable_pat` recognize ALL function calls as potentially throwing
- **Fix difficulty**: MEDIUM — could track which functions contain `throw` statements and only allow try/catch around those
- **Impact**: False sense of error handling, wasted try/catch overhead

#### 3. Value Semantics Bug Beyond 10-Line Window (Evasion E)
- **What**: `let payload = ev.get("payload")` then `payload["processed"] = true` 11+ lines later without re-assigning
- **Why missed**: `value_semantics_bug` checker only looks 10 lines ahead after `.get()`
- **Fix difficulty**: EASY — increase lookahead window or use scope-based tracking
- **Impact**: Silent data loss — mutations to value-copy are lost

#### 4. Magic Numbers in main{} (Evasion F)
- **What**: Hardcoded 88, 42, 73, 15, 25, 95, 55, 30, 67, 82 etc. in simulation
- **Why missed**: `magic_numbers` checker explicitly skips lines inside `main{}`
- **Fix difficulty**: EASY — remove main{} exclusion or make it configurable
- **Impact**: Hardcoded values that should be named constants

#### 5. Short Generic Variable Names (Evasion G)
- **What**: `x`, `n`, `acc`, `cnt`, `idx`, `curr`, `prev` — all generic/meaningless
- **Why missed**: `generic_variable_names` bad_names set doesn't include single-letter names or common abbreviations
- **Fix difficulty**: EASY — add single-letter names (except `i`, `j`, `k` for loops) and abbreviations to the set
- **Impact**: Poor code readability

#### 6. Non-Standard Placeholder Comments (Evasion H/I)
- **What**: `// REVISIT: enhance with ML-based scoring later`
- **Why missed**: `placeholder_code` only checks `TODO: implement/add/fix/complete`, not REVISIT/ENHANCE/EXTEND/REFINE
- **Fix difficulty**: EASY — add REVISIT, ENHANCE, EXTEND, REFINE to the TODO pattern
- **Impact**: Placeholder intent hidden behind synonym

#### 7. Dict Bracket Access with Variable Keys (Evasion J)
- **What**: `event[field_name]` where `field_name` is a variable — throws on missing key
- **Why missed**: `dict_bracket_access` regex only matches quoted string literal keys: `\w+\["[^"]+"\]`
- **Fix difficulty**: MEDIUM — need to also match `\w+\[\w+\]` pattern (variable keys)
- **Impact**: Uncaught potential runtime crashes from missing dict keys

## Bug Severity Assessment

| Priority | Evasion | Fix | Impact |
|----------|---------|-----|--------|
| HIGH | #3 Value semantics 10-line limit | Increase window | Silent data loss |
| HIGH | #7 Dict bracket variable keys | Extend regex | Runtime crashes |
| MEDIUM | #2 Try/catch false negatives | Track throw-capability | False safety |
| MEDIUM | #5 Short generic names | Extend bad_names | Readability |
| LOW | #6 Placeholder synonyms | Extend TODO pattern | Minor |
| LOW | #4 Magic numbers in main | Config option | Style |
| WONTFIX | #1 Dead conditional paths | Need data flow analysis | Complexity gaming |
