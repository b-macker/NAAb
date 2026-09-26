# Unit-test suite: triage and findings

`naab_unit_tests` (GoogleTest, `tests/unit/`, 19 files, 589 tests) was declared
in `CMakeLists.txt` and built by nothing: no workflow and no script referenced
it, and `gtest_discover_tests` was commented out for every platform over a
Termux-only problem. Five of the nineteen files had stopped compiling after the
`shared_ptr<Value>` -> `NaabVal` migration, and nobody could have noticed.

This records what the suite said once it ran again, with every failure traced to
a cause and every real defect checked for reachability **against the shipped
binary**, not against the unit test that surfaced it. Provenance of each number
is stated: *measured* (run here, on the commit named), or *code* (read, not run).

Measured on `ea50e0f` (master `7f1f1cf` + compile fixes), Linux, Debug build.

---

## 1. Summary

| Cause | Failures | Real defect? |
|---|---|---|
| Top-level statements, now invalid outside `main {}` | 92 | No, stale test |
| Language rules the tests predate (DIV-001, `catch` required, block-vs-dict) | 4 | No, stale test |
| Lexer tokens renamed/added, stdlib count 13->25, `math.*` return types, ISS-036 | 10 | No, stale test |
| C++ snippets written against the pre-`NaabVal` ABI | 2 | No, stale test |
| No sandbox policy established in the fixture (fail-closed denial) | 12 | No, fixed in fixture |
| SafeRegex timeout cannot interrupt | 1 (hang) | **Yes, reachable** |
| Async polyglot executors ignore `timeout` | 1 | Yes, contract only |
| `AsyncCallbackPool` deadlock (`std::launch::deferred`) | 4 (2 hang) | Yes, test-only callers |
| `max_cpu_seconds = 0` arms a 0-second kill timer | 6 | Yes, unreachable today |
| FFI callback type validator is a stub | 5 | Unimplemented, no callers |

108 of the 137 failures and hangs are tests that fell behind deliberate
changes, and 12 more were a fixture that never established a sandbox. The
count is not the point: one hanging test out of 589, followed into
the real binary, led to the finding in section 2.

---

## 2. Script work that outruns `--timeout` (reachable, highest priority)

The global execution timeout (`ResourceLimiter`, `--timeout N`) is a flag that
the interpreter *polls*. Anything that runs inside the process without polling
it cannot be stopped; the timeout is only noticed once that work returns.

Measured, `--timeout 3`:

| Workload | Stopped after | |
|---|---|---|
| NAAb `while true` loop, VM and tree-walker | 3.1 s | control, works |
| `async fn` infinite loop, VM and tree-walker | 3.1 s, exit 1 | works |
| `<<shell sleep 20>>` | 3.1 s | works (subprocess killed) |
| `<<javascript>>` 20 s busy loop | 3.1 s | works (`JS_SetInterruptHandler`) |
| `<<python>>` 20 s busy loop | **20.1 s** | **not interrupted** |
| `regex.matches` on the input below | **30.5 s** | **not interrupted** |

### 2a. Embedded Python has no interrupt path

QuickJS registers an interrupt handler that polls the timeout
(`js_executor.cpp:42`). The embedded CPython executor has no equivalent: no
`Py_AddPendingCall`, `PyErr_SetInterruptEx`, trace hook or async exception.
A `<<python>>` block that never returns is never stopped by `--timeout`.

Fix direction: when the timer fires, have it inject an exception into CPython
(`PyErr_SetInterruptEx` or a pending call that raises), and map it back to the
existing timeout error. Needs a test that asserts the *wall time*, not only the
error text.

### 2b. SafeRegex: validator gap plus a timeout that cannot fire

Two defects that combine:

1. **The nested-quantifier check is one regex**,
   `\([^)]*[*+?][^)]*\)[*+?{]`, which requires the quantified group's `)` to be
   immediately followed by a quantifier. Redundant parentheses defeat it:
   `(a+)+b` is rejected, `((a+))+b` (the same catastrophic pattern) passes.
2. **`executeWithTimeout()` cannot abandon the work.** It runs the match under
   `std::async(std::launch::async)`, detects the timeout with `wait_for`, and
   throws. Unwinding destroys the future, and a `std::async` future's destructor
   blocks until the task finishes. The timeout exception is therefore only
   delivered after the regex completes.

Measured, `regex.matches("aaa...a!", "((a+))+b")`, default limits (1 s budget):

| Input length | Time | Result |
|---|---|---|
| 18 | 0.1 s | `false` |
| 22 | 1.0 s | `false` |
| 24 | 3.7 s | timeout exception, after completion |
| 27 | 30.0 s | timeout exception, after completion |
| 27 with `--timeout 5` | 30.5 s | global timeout, after completion |

Each extra character roughly doubles the time. One line of NAAb can hold the
interpreter past every configured limit, which in the REST daemon means a
worker.

---

### Status

**Fixed (2b):** `executeWithTimeout()` now runs the work on a detached thread
that owns copies of its inputs, and returns at the deadline instead of waiting;
timed-out workers are capped at 4, beyond which new regex work is refused. The
nested-quantifier check is a scanner that tracks groups, so a quantifier at any
depth counts toward the enclosing group. Against the old check the same table
of 18 patterns scored 10/18: it missed 5 catastrophic patterns and falsely
rejected 3 ordinary ones, including `(?:ab)+`. Measured after the fix:
`((a+))+b` is rejected up front; `(a|a)+b` on a 31-char input (minutes of raw
`std::regex` work) stops at 1.0 s. Regression suite:
`tests/security/test_regex_timeout_bound.sh`, which fails 4 of 7 against the
old implementation (its 3 controls pass on both builds).

**Open (2a):** embedded Python.

## 3. Real but not reachable today

- **Async executors drop `timeout`.** Every `*AsyncExecutor::executeAsync()` in
  `polyglot_async_executor.cpp` captures `timeout` and never reads it; the
  nested-thread version was removed over an Android CFI crash. The single
  production caller (parallel polyglot groups, `polyglot.cpp`) passes none.
- **`AsyncCallbackPool` deadlocks** once submissions exceed `max_concurrent_`.
  `AsyncCallbackWrapper::executeAsync()` uses `std::launch::deferred` (commented
  as a workaround for thread exhaustion), so a callback runs only when its
  future's `.get()` is called; `submit()` blocks until an earlier callback is
  done, which cannot happen before the caller reaches `.get()`. Also explains
  `CancelDuringExecution` and `ExecuteRaceFirstWins`. Only tests construct a
  pool.
- **Zero means two things.** `PermissionLevel::UNRESTRICTED` sets
  `max_cpu_seconds = 0` ("no limit"); the shell, JS, generic-subprocess and
  persistent-process executors pass it to `ScopedTimeout(0)`, whose timer fires
  immediately. The CLI and REST API always overwrite the value from
  `--timeout`, so no measured path reaches it. Clamp at `ScopedTimeout` anyway.
- **`naab::Context` cannot run polyglot.** Executor registration
  (`initialize_executors()`) lives in `src/cli/main.cpp`, not in `libnaab`, so an
  embedder gets "No executor found" for every polyglot block. An API gap rather
  than a crash; it also makes the zero-timeout defect unreachable through the
  embedding API.
- **FFI callback validator** (`ffi_callback_validator.cpp`) accepts every type:
  `getTypeName()` returns `"type"` for all inputs and mismatches are logged,
  not rejected. Documented as deferred, citing a plan document that is not in the
  repository. No callers.

---

## 4. Checked and cleared

- **Stale-test attributions were verified per test, not by sampling.** All 96
  parser/interpreter failures hit the top-level parse gate. Because an outer
  gate masks the inner one, each suite was rerun with its source wrapped in
  `main { }`: interpreter 44 -> 2, parser 52 -> 2, and no previously passing
  test broke. The four left behind the gate are language rules (DIV-001
  division is double; `catch` is mandatory; `{` in statement position opens a
  block). Nothing real was hiding behind it.
- **Async functions and `--timeout`**: covered in both engines (exit 1 at about 3 s).
  An `rc=0` in the first measurement was the probe (`PIPESTATUS` read outside
  the subshell), not the binary.
- **Other `std::async` sites** (`vm.cpp`, `call_dispatch.cpp`) never abandon
  their futures on a timeout, so they do not share SafeRegex's defect.

## 5. Unmeasured

- **`http.*` with `timeout_ms = 0`.** The script-supplied value goes straight
  to `CURLOPT_TIMEOUT_MS`, which libcurl documents as "never time out", and
  nothing clamps it to `--timeout`. Could not be measured here: SSRF protection
  refuses loopback, and no external slow endpoint was available. *Code*, not
  *measured*.

## 6. Lessons that generalise

- **An unbuilt test target reads as coverage.** Same failure as
  `naab-verify-audit` (found broken only when a workflow first built it).
- **A uniform failure is a gate, not a verdict.** 96 tests failing on one parse
  error said nothing about the interpreter behind it until the gate was
  removed.
- **Follow the odd one out into the real binary.** The finding in section 2
  came from one hang in 589 tests, and only became real when reproduced in
  `naab-lang` with measured wall time.
- **Probes broke three times in this investigation** (a missing `use regex`, JS
  code evaluated as an expression, `PIPESTATUS` read outside the subshell). Each
  one produced a uniform or implausible result, and each was caught by asking
  why every arm agreed.
