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

### CMP-001: string<->number ordering comparison (fixed 2026-07)

- **Was**: `"a" < 1` — VM raised `Type error: Cannot compare string < int`;
  tree-walker coerced the string via `toFloat()` (yielding 0, so
  `"2" > 1 == false`) and returned a bool.
- **Decision**: unify on the VM's strict type error — the TW coercion
  silently produced nonsense orderings.
- **Fix**: `src/interpreter/expressions.cpp` — `isOrderComparable()` guard
  on Lt/Le/Gt/Ge (both-numeric-incl-bool or both-string, else type error).
- **Regression**: `tests/differential/corpus/known_fork_mixed_compare.naab`
  (now passes identically on both engines) + oracle vectors.

### BOOL-001: bool operands in arithmetic (fixed 2026-07)

- **Was**: `true + 1` — VM raised `Type error: Cannot add bool and int`;
  tree-walker treats bool as numeric (1/0) and returns `2`.
- **Decision**: unify on the tree-walker — bool-as-numeric is its documented
  contract (div/mod error text says "int, float, or bool").
- **Fix**: `src/vm/vm.cpp` — bool→int normalization in OP_ADD/SUB/MUL/DIV/
  MOD/NEG and OP_LT/LE/GT/GE slow paths. String concat (`"s" + true`) and
  string repetition counts are excluded and keep their prior behavior.
  Equality (`true == 1` is `false`) is intentionally unchanged in both.
- **Regression**: `tests/differential/corpus/known_fork_bool_arith.naab`
  + oracle vectors.

### FEAT-001: tree-walker lagged VM on string/array conveniences (fixed 2026-07)

- **Was**: the VM supported `for c in "abc"` (string iteration), `s[0]` /
  `s[-1]` (string subscript with negative wrap), `arr[-1]` (negative list
  index wrap), and `arr.pop()`; the tree-walker rejected all of them.
- **Decision**: implement in the tree-walker — the VM behavior is the
  intended surface (the main VM test corpus relies on it).
- **Fix**: `src/interpreter/interpreter.cpp` (string iteration in for),
  `src/interpreter/expressions.cpp` (string subscript incl. the
  `s["message"]` thrown-string idiom; negative list index wrap),
  `src/interpreter/call_dispatch.cpp` (`arr.pop()` in both method paths).
- **Regression**: `tests/vm/test_vm_core.naab`, `test_vm_collections.naab`,
  `test_vm_dict_array.naab`, `test_vm_exceptions.naab` now pass identically
  on both engines via the differential corpus.

### TRY-001: `break`/`continue` leaked a try's exception handler (fixed 2026-07)

The VM compiler emitted `OP_POPN` for locals on `break`/`continue` but never
`OP_TRY_END`, so jumping out of a `try` body leaked its handler permanently. A
later `throw` with no enclosing try resolved against the dead handler, re-entered
the stale catch block and fell through it, re-executing statements that had
already run. With nested trys the exception was delivered to the wrong catch —
one nesting level too deep — while the enclosing handler never saw it.

Two consequences beyond the divergence: an exception that should terminate the
run was silently absorbed, and any side effect between the stale `catch_ip` and
the leak point fired twice.

The tree-walker was correct throughout and is the oracle in the regression test.
`return` out of a try was never affected — both `OP_RETURN` paths pop handlers by
`frame_index` at runtime.

Fixed by tracking `CompilerState::open_try_depths` and emitting one `OP_TRY_END`
per try opened inside the innermost loop. The bound matters: closing a handler
belonging to a try that ENCLOSES the loop would deliver a later exception
nowhere, which is worse than the leak — `tests/vm/test_try_handler_leak.sh`
E1-E3 exist to pin that.

## Accepted divergences (allowlisted)

### ERR-TEXT-001: Error message wording differs between engines

- The tree-walker emits long-form errors with `Help:` / `Example:` sections;
  the VM emits terse one-liners (e.g. division by zero). The **category**
  (first line up to the first `:`) matches; only the explanatory text forks.
- **Status**: accepted. The differential harness compares error *categories*,
  not full error text, so this divergence is absorbed structurally rather
  than through an allowlist entry.

### REC-001: Recursion ceilings are engine-specific (accepted 2026-07-25)

- **Is**: the VM fails calls beyond `FRAMES_MAX` (1024 heap-allocated frames,
  so ~1022 usable NAAb depth) with `Stack overflow (call depth exceeded)`.
  The tree-walker recurses natively and fails when the thread's real stack
  headroom runs out (`Recursion error: Program nesting exceeded the available
  stack` — the #96 guard in `eval()`/`executeStmt()`), which lands at a
  build-dependent depth above the VM ceiling (~2-3k in Release, lower under
  sanitizers whose frames are larger). The logical `MAX_CALL_STACK_DEPTH`
  (10000) still applies as an upper bound in the tree-walker.
- **Decision**: accepted — the tree-walker's ceiling is a physical resource
  measurement, not a fixed count, so exact parity is impossible. Both
  engines fail *cleanly* with a catchable error whose category matches the
  differential harness's recursion/stack classification. Programs must stay
  under the VM's 1024-frame ceiling to be portable.
- **Fix**: `src/interpreter/interpreter.cpp` (`nativeStackLow()` headroom
  guard, thread-aware via pthread/Win32 stack bounds). Previously the
  tree-walker crashed with a native stack overflow (ASan SIGSEGV, nightly
  fuzz run 29310717112) because `MAX_CALL_STACK_DEPTH` exceeded what the
  8MB stack physically holds and argument evaluation recursed ahead of the
  call-depth guard.
- **Regression test / detector**: `tests/robustness/test_stack_guard.sh`
  (both engines: deep recursion = clean nonzero exit, no signal; depth-200
  succeeds).

### ASYNC-001: `async fn` is concurrent on the VM, synchronous on the tree-walker (accepted 2026-10-03)

- **Is**: the VM runs each `async fn` call on its own thread and returns a
  `future` (`vm.cpp`, `callValue`). The tree-walker runs the body at the
  call site and returns its value: `typeof(f())` is `future` on the VM and
  `int` on the tree-walker, and four 1 s calls take ~1 s against ~4 s.
  `await` on a non-future passes the value through, so both engines compute
  the same results; ordering of side effects inside the bodies can differ.
- **Why**: the tree-walker has a real async branch (`call_dispatch.cpp`,
  `callFunction()`, `if (func->is_async)`) that is unreachable. The
  declaration visitor never copies `is_async` onto the `FunctionValue`, and
  the direct by-name call path in `visit(CallExpr)` has a generator branch
  but no async one. Making it reachable (both one-line fixes, measured
  2026-10-03) exposed three defects in the branch itself:
  1. module imports do not reach the worker interpreter -- `use time` at file
     scope gives `Import error: Module not found` inside the async fn;
  2. a segfault (3 of 3 runs) when several worker interpreters start at once:
     shared tree-walker state is not thread-safe;
  3. a `GovernanceHardError` in the worker calls `_exit(3)`, which on the REST
     path (tree-walker) would end the daemon, not the request -- the defect
     #223 removed from `rest_api.cpp`. `await` already rethrows the error
     uncaught (`visit(AwaitExpr)`), so `throw;` is the VM-shaped replacement.
- **Decision**: accepted until the branch is fixed. Enabling it as-is turns a
  correct sequential engine into one that crashes. Do not set `is_async` on
  tree-walker functions without fixing 1-3 in the same change.
- **Regression test / detector**: `tests/vm/test_async_engine_semantics.sh`
  (AS-01 result parity, AS-02 VM concurrency, AS-03 pins the tree-walker
  sequential -- it goes red the moment the branch becomes reachable).

<!-- Add new entries above. Template:
### ID: title (status date)
- **Was/Is**: behavior in each engine
- **Decision**: unified-on-X | accepted-because-Y
- **Fix/Allowlist**: file refs or divergences.json id
- **Regression test / detector**: path
-->
