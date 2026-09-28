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

**Windows follow-up.** build-windows stalled once (58 min, no logs) in the
phase that runs the new suite. The first explanation, that MinGW holds the
process open for abandoned regex threads, was tested and falsified: on the next
Windows run the suite passed 7/7 and B-01 took 1 s including process exit. The
stall is most likely the runner wedge `windows.yml` documents. Measured on that
run: `test_r22_fixes.sh` alone took about 6 of the 9m39s shell phase (the slow
`naab-gov scan` of an 11 MB file).

**Fixed (2a):** the timeout's timer thread now queues a CPython pending call
that raises `TimeoutError` in the running block, and re-queues itself while the
timeout stands, so `except Exception: pass` in a loop cannot swallow it.
Queuing alone was measured to do nothing on CPython 3.11: `Py_AddPendingCall`
computes the eval breaker on the *calling* thread, where "can handle pending
calls" is false, so the loop is never told. A brief GIL acquire from the timer
thread makes the running thread re-take the GIL and recompute the breaker on
its own thread (skipped on Android, where `PyGILState_Ensure` on a foreign
thread is the bionic CFI crash). Measured: the 20 s busy loop stops at 3.06 s
in both engines. **Limit:** a block blocked inside C (`time.sleep`, a socket
read) is interrupted only when the call returns, since CPython runs the
interrupt between bytecodes (`time.sleep(15)` took 15 s on both builds).

**Fixed later (2e): Python on a worker thread.** CPython runs pending calls on
its main thread only, so the 2a interrupt never reached Python running
elsewhere. The recorded limit also named the wrong path: Python in a
tree-walker parallel polyglot group runs on the MAIN thread by design
(`polyglot.cpp`), and a probe built on that path passed on the old build too.
The real worker path is an `async fn` on the VM: measured 20.8 s against
`--timeout 3`, and 40 s when the loop caught every Exception. Threads running
Python now register (with a sequence number) and a worker is sent
`PyThreadState_SetAsyncExc`, re-sent until the execution that was running
when the deadline fired has left Python. Re-sending alone was not enough:
an async exception lands at the next eval-breaker check, which inside
`try: <loop> except Exception: pass` is almost always inside the try. The
exception is therefore `naab.ExecutionTimeout`, a `BaseException` subclass
like KeyboardInterrupt, which `except Exception` does not catch. Both cases now
stop at 3 s.

**2f: timeouts were all script-wide.** Every timer that fired set the
process-wide flag, and every arm or clear reset it. So an async executor that
honoured its own per-task budget would have stopped the whole script, and a
worker merely arming a timer could erase the script's timeout after it fired.
Only the outermost timeout of a run is script-wide now; nested and per-task
timeouts set only their thread's flag.

### 2c. One block cancelled the script's `--timeout` (found while fixing 2a)

There is one timer per thread. The CLI and REST wrap the run in a
`ScopedTimeout`, and the JS, shell, subprocess and C++ executors wrap every
block in another. Each scope re-armed the timer with its own budget and its
destructor **cleared** it, so after a single `<<javascript>>` expression the
script had no timeout at all. Measured, `--timeout 3`: a `while true` after
`let v = <<javascript 1 + 1 >>` ran until killed from outside, in both
engines. `codegen.run` did the same by calling `setExecutionTimeout` /
`clearTimeout` directly. A second layer: the cancel counter was process-wide,
so a scope on another thread (the tree-walker runs this JS off the main
thread) cancelled the main thread's timer, and a REST request could cancel a
concurrent request's.

**Fixed:** `ScopedTimeout` nests. A scope can only tighten the deadline in
force, never extend it, and restores the outer deadline on exit. The cancel
counter is per thread. `0` now means "no limit of its own", which also fixes
the zero-second timer in section 3 (it killed every subprocess on start under
the `UNRESTRICTED` preset).

### 2d. `http.*` outlived `--timeout` (was section 5, unmeasured)

Measured once an address that drops SYNs was found (`8.8.8.8:81`):
`http.get(url, {}, 0)` was still connecting 20 s into a `--timeout 3` run.
curl never returns to the interpreter mid-transfer, and `timeout_ms = 0` is
libcurl's "never". **Fixed:** a progress callback aborts the transfer when the
timeout fires (curl calls it at least once a second, including while
connecting), a non-positive `timeout_ms` falls back to the 30 s default, and
curl's own timeouts are capped at the time left before the script's deadline.
The cap is what holds on Windows: there the progress callback was not called
during connect, and the first CI run stopped at curl's 10 s connect timeout
instead of 3 s. With the callback disabled locally, the cap alone stops the
request at 3 s.

Regression suite for 2a, 2c, 2d and 2e: `tests/security/test_timeout_reach.sh`,
13 arms. The W arms are 2e: on the build before it, W-01/vm and W-02/vm fail
(20 s, 40 s) while their tree-walk twins pass, so the tree-walk arms are
coverage and the VM arms are the proof.

## 3. Real but not reachable today

- **Async executors drop `timeout`** (fixed). Every `*AsyncExecutor::executeAsync()` in
  `polyglot_async_executor.cpp` captured `timeout` and never read it; the
  nested-thread version was removed over an Android CFI crash. The single
  production caller (parallel polyglot groups, `polyglot.cpp`) passes none.
  Each task now runs under a LOCAL `ScopedTimeout` on its pool worker (2f), so
  an overrun stops that task only. `ShellWithTimeout` stops at 52 ms against
  a 50 ms budget. `PythonTimeout` uses `time.sleep`, so it is reported as a
  timeout when the sleep returns, not before (the 2a limit).
- **`AsyncCallbackPool` deadlocks** (fixed) once submissions exceed `max_concurrent_`.
  `AsyncCallbackWrapper::executeAsync()` uses `std::launch::deferred` (commented
  as a workaround for thread exhaustion), so a callback runs only when its
  future's `.get()` is called; `submit()` blocks until an earlier callback is
  done, which cannot happen before the caller reaches `.get()`. Also explains
  `CancelDuringExecution` and `ExecuteRaceFirstWins`. Only tests construct a
  pool. It is also RACY, not only deadlock-prone: `PoolThreadSafety`
  segfaulted in 2 of 6 isolated runs (measured on the string-interpolation
  branch, which does not touch this code). With deferred launch a callback
  runs on whichever thread calls `.get()`, while another thread's `submit()`
  runs `cleanupCompleted()` and erases wrappers it sees as done -- a wrapper
  can be freed while its callback is still returning through it. A timed-out
  callback was worse: its thread was detached still running `callback_()`, a
  member of a wrapper the pool was free to destroy. **Fixed:** the wrapper's
  mutable state (callback, flags, name, timeout) lives in a `shared_ptr` held
  by every thread that runs it, and `executeAsync()` starts the work
  immediately. All five pool and race tests pass, and the pool tests passed
  30 of 30 repeated runs.
- **Zero means two things** (fixed with 2c). `PermissionLevel::UNRESTRICTED`
  sets `max_cpu_seconds = 0` ("no limit"); the shell, JS and generic-subprocess
  executors passed it to `ScopedTimeout(0)`, whose timer fired immediately.
  `ScopedTimeout(0)` now arms nothing. The persistent-process executor
  converts it to a millisecond budget of its own and is unchanged.
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

## 5. CI

`naab_unit_tests` now runs in CI (`ci.yml`, Build & Test) through
`tests/unit/run_unit_tests.sh`. It started with 30 known failures in
`tests/unit/known_failures.txt`, each with its reason from this document; the
runner fails if an unlisted test fails, if a listed test no longer exists, or
if a listed `fails` entry starts passing, so the list has to shrink when
something is fixed.

Now 580 pass and 9 remain listed, none of them hangs: two C++ snippets written
against the pre-NaabVal ABI, the two `executeBlocking` shell tests that the
fail-closed sandbox refuses (the callback thread has none, a fixture issue),
and the five FFI-validator stub tests. The 14 stale expectations that were
one-line fixes were rewritten to the rule each now encodes, not to whatever
the code returned: DIV-001 for division, mandatory `catch`, ISS-036
first-definition-wins for struct registration (it no longer throws, but a
conflicting definition must not replace the first), the oracle's
`math.abs`-returns-float fact, and newline/`|>` tokens. The module-count test
no longer hard-codes a number: it checks every listed module resolves and the
original set is present.

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
