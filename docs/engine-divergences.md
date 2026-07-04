# Engine Divergence Registry

NAAb executes programs on two engines: the bytecode VM (default) and the
tree-walking interpreter (`--tree-walk`). Their arithmetic and semantics
logic is duplicated, so behavior can fork. This document is the registry of
rationale for every divergence that has been found, and records whether it
was **unified** (fixed) or **accepted** (allowlisted in
`tests/differential/divergences.json` / `tests/fuzzing/known_findings.txt`).

Rule: every entry in `tests/differential/divergences.json` and every accepted
signature in `tests/fuzzing/known_findings.txt` MUST link to a section here.

## Unified divergences (fixed)

### DIV-001: Integer division semantics (fixed 2026-07)

- **Was**: VM computed `7 / 2 == 3` (truncating int division); tree-walker
  computed `7 / 2 == 3.5` (always-double division).
- **Decision**: unify on the tree-walker semantics — `/` always produces a
  double. Rationale: the tree-walker is the semantic reference; the language
  docs and `NEEDS_TREE_WALK` tests already relied on `3.5`.
- **Fix**: `src/vm/vm.cpp` (`OP_DIV` int path) and the constant folder in
  `src/vm/compiler.cpp` now promote int/int division to double.
- **Regression test**: `tests/bugs/test_int_min_div_mod.naab`.

### DIV-002: `INT_MIN / -1` undefined behavior (fixed 2026-07)

- **Was**: C++ UB in the VM's int division path (signed overflow).
- **Fix**: subsumed by DIV-001 — division is now performed in doubles, so
  `INT_MIN / -1 == 2147483648.0`, well-defined in both engines.

### MOD-001: `INT_MIN % -1` undefined behavior (fixed 2026-07)

- **Was**: C++ UB in both engines (`a % b` with `a == INT_MIN, b == -1`
  overflows in the hardware remainder computation).
- **Decision**: both engines return `0` — the mathematically correct
  truncated-modulo result (`a - trunc(a/b)*b == 0`). Chosen over throwing
  because no information is lost and it matches `%`'s contract.
- **Fix**: guards in `src/vm/vm.cpp` (`OP_MOD`), `src/vm/compiler.cpp`
  (constant folder), `src/interpreter/expressions.cpp` (`BinaryOp::Mod`).
- **Regression test**: `tests/bugs/test_int_min_div_mod.naab`.

## Accepted divergences (allowlisted)

### ERR-TEXT-001: Error message wording differs between engines

- The tree-walker emits long-form errors with `Help:` / `Example:` sections;
  the VM emits terse one-liners (e.g. division by zero). The **category**
  (first line up to the first `:`) matches; only the explanatory text forks.
- **Status**: accepted. The differential harness compares error *categories*,
  not full error text, so this divergence is absorbed structurally rather
  than through an allowlist entry.

<!-- Add new entries above. Template:
### ID: title (status date)
- **Was/Is**: behavior in each engine
- **Decision**: unified-on-X | accepted-because-Y
- **Fix/Allowlist**: file refs or divergences.json id
- **Regression test / detector**: path
-->
