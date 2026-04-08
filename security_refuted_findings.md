# NAAb Security Audit — Refuted Findings Registry

Tracks every finding that was raised in a vulnerability report but refuted (not fixed),
along with the reason. When a new finding is refuted, add it here before closing the round.

Last updated: R20 (Apr 7, 2026)

---

## R8 Refutations (2 of 5 raised)

### V-ASYNC-003 — Async Shared Container Race
- **Claimed:** `makePythonCallback` captures containers shared with the main thread; concurrent
  mutation causes data corruption or crashes.
- **Refuted because:** NaabVal is NaN-boxed 8 bytes — value semantic. Workers receive a
  serialized copy of all arguments via `serializeForLanguage` before the thread starts.
  Container `shared_ptr`s are not directly handed to the worker; the serialized string is.
  No shared mutable state exists between threads for polyglot arguments.

### V-GOV-005 — UTF-8 String Stripping Bypass
- **Claimed:** `stripStringLiterals` iterates bytes; multi-byte UTF-8 sequences can contain
  bytes matching ASCII quote characters, confusing the quote tracker.
- **Refuted because:** In valid UTF-8, the only bytes with values 0x22 (`"`) or 0x27 (`'`)
  are the literal ASCII quote characters themselves. All multi-byte continuation bytes are
  in the range 0x80–0xBF and cannot masquerade as quotes. Malformed/invalid UTF-8 is
  rejected by the lexer before governance pre-processing occurs.

---

## R9 Refutations (2 of 5 raised)

### V-GOV-008 — Audit Trail Continuity Loss (Async)
- **Claimed:** Async workers create isolated `Interpreter` instances with their own
  `AuditLogger` state; security events may be lost or fragmented.
- **Refuted because:** The R8 fix (`saveCounterState`/`restoreCounterState`) passes live
  governance state to async workers. All workers write audit events to the same log file
  path (inherited from the parent). File-level append is sufficient for audit log integrity;
  no in-memory state is lost between parent and worker.

### V-RT-009 — Non-Atomic Conversion Counters
- **Claimed:** `CrossLanguageBridge` conversion statistics counters are non-atomic, causing
  inaccurate metrics under concurrent load.
- **Refuted because:** These counters are observability metrics only — they do not gate any
  security decision or enforcement action. A race producing an off-by-one count has no
  security impact. Severity too low relative to fixing cost (would require atomic or mutex
  on every conversion call).

---

## R10 Refutations (4 of 5 raised — all "already fixed")

### V-GOV-006 — Systemic Marshalling Taint Loss
- **Claimed:** Polyglot outputs arrive in NAAb as "clean" values, taint metadata discarded.
- **Refuted because:** Fixed in R9. `OP_POLYGLOT` in `vm.cpp` and `InlineCodeExpr` in
  `governance_taint.cpp` now unconditionally taint all polyglot outputs regardless of
  input taint status. The report was based on pre-R9 code.

### V-GOV-004 — Async Governance Fragmentation
- **Claimed:** Async tasks instantiate fresh `GovernanceEngine`, resetting all counters
  (polyglot block limits, etc.).
- **Refuted because:** Fixed in R8. `saveCounterState`/`restoreCounterState` passes live
  counter state from the parent `GovernanceEngine` to async worker interpreters,
  mirroring the taint propagation pattern.

### V-RT-007 — Unreliable POSIX Alarm Delivery
- **Claimed:** `alarm()` delivers `SIGALRM` to an arbitrary thread; wrong thread may
  receive the timeout signal.
- **Refuted because:** Fixed in R8. `ResourceLimiter` now uses `pthread_kill` with a
  lambda-captured `tid` (the executing thread's ID), ensuring the signal is delivered
  only to the correct thread. `executing_tid_` is intentionally NOT `thread_local` so
  the timer thread can read the target's ID.

### V-RT-008 — GC Denial of Service (No Auto-Trigger)
- **Claimed:** `CycleDetector` exists but is never called from the VM loop; reference
  cycles exhaust memory.
- **Refuted because:** Fixed in R9. `CycleDetector` is now a member of `VM`. `OP_JUMP_BACK`
  (loop back-edge) triggers periodic GC when `gc_counter_` hits `global_gc_threshold`.
  `cycle_detector.cpp` null root_env guard was also relaxed to allow GC with no root.

---

## R11 Refutations (1 of 4 raised)

### V-RT-010 — Persistent Symlink TOCTOU
- **Claimed:** `PathSecurity::validateFilePath` canonicalizes then opens separately; an attacker
  can swap a validated path for a symlink between the check and `std::ifstream` open.
- **Refuted because:** Already fixed as R4 Finding F. `src/stdlib/file_impl.cpp` implements:
  1. `resolveCanonical()` (lines 64–85) — calls `fs::canonical()` when sandbox is active,
     fully resolving all symlinks before the second access check.
  2. `readFileNoFollow()` / `writeFileNoFollow()` (lines 93–138) — use `O_RDONLY | O_NOFOLLOW`
     and `O_WRONLY | O_NOFOLLOW` so the final path component cannot be a symlink at open time.
  3. `file.read` (lines 166–174) — applies the double-check pattern:
     `checkFileSandbox → resolveCanonical → checkFileSandbox(resolved) → readFileNoFollow`.
  The guard `if (security::ScopedSandbox::getCurrent())` is correct — TOCTOU protection
  only applies when a security boundary is enforced; without it there is no sandbox to bypass.

---

## R12 Refutations (1 of 4 raised)

### V-CLI-001 — REPL Eval Code Injection
- **Claimed:** REPL wraps unrecognized inputs via string concatenation; piping `1\n} os.system('id') main { let x = 1` breaks out of the `main` block and executes arbitrary code.
- **Refuted because:** `src/cli/repl.cpp:84` uses `std::getline(std::cin, input)` which reads one line at a time. A `\n` in piped input terminates the current `getline` call and starts a new one — it is never included in `input` or `code`. The claimed attack produces two separate REPL iterations: `1` (valid expression) and `} os.system('id') main { let x = 1` (expression-mode parse error, no execution). The multi-line accumulation path also cannot be exploited: a closing `}` only exits multi-line mode when brace depth returns to zero, and the accumulated code is still wrapped inside `main {}`. No structural path exists for injecting a `\n` into the `code` variable through standard piped input.

---

## R13 Refutations (1 of 5 raised)

### V-CLI-001 — REPL Eval Code Injection (Single-Line, Semicolons)
- **Claimed:** NAAb's semicolon support as a statement separator allows `1; } os.system('id'); main { let x = 1` to break out of the REPL's `main {}` wrapper and execute a second `main` block.
- **Refuted because:** `parseProgram()` in `src/parser/parser.cpp:510-513` does `break` immediately after the first `main {}` block is parsed — no tokens after that block are ever parsed or executed. The semicolon injection causes the wrapper's `main` block to close one statement early (after `let __repl_result__ = 1`), but then `parseProgram()` breaks. Any code placed after the closing `}` (including a second `main { ... }`) is permanently orphaned — the parser never reaches it. The structural escape is real but harmless: it changes *where* the wrapper block closes, not whether subsequent code executes.

---

## R20 Refutations (1 of 4 raised)

### V-GOV-014 — Container Literal Taint Loss
- **Claimed:** `visit(ast::ListExpr&)` and `visit(ast::DictExpr&)` evaluate elements without
  informing the `GovernanceEngine`; `let a = [secret]` leaves `a` clean, bypassing the R19
  mutation-site propagation fix.
- **Refuted because:** Taint checking for container literals happens at the assignment site, not
  inside the literal visitor. `VarDeclStmt::visit` (interpreter.cpp:2232–2241) calls
  `checkRhsTainted(node.getInit())` for every initializer. `checkRhsTainted` calls
  `expressionContainsTaint(ListExpr)`, which has a dedicated branch (BUG-U,
  governance_taint.cpp:88–94) that iterates all elements and returns true if any element is
  tainted. An identical branch exists for `DictExpr` (BUG-V, governance_taint.cpp:96–102).
  The same `checkRhsTainted` path fires for regular assignment (`BinaryExpr::Assign`,
  expressions.cpp:90–98). The reproduction case `let a = [s]` correctly marks `a` as tainted
  with the current code. The auditor correctly observed the literal visitors do not call
  governance — but the check is properly located at the declaration/assignment site, not
  inside the literal evaluator which has no knowledge of the binding name.

---

## Underexplored Areas (as of R13)

These have never appeared in any report or were raised once but never confirmed fixed.
Candidates for future audit rounds:

1. **R4 Finding E (Stdlib Shadowing) — unconfirmed fix**
   `import` resolves local filesystem paths before checking stdlib. A `math.naab` in the
   working directory can shadow the trusted stdlib. No memory entry confirms this was fixed.

2. **R4 Finding F (Symlink TOCTOU) — CONFIRMED FIXED (see V-RT-010 refutation above)**

3. **ErrorSanitizer not wired into error pipeline**
   `ErrorSanitizer::sanitize()` has zero call sites in the main error pipeline. Raw
   `std::runtime_error` messages from interpreter, VM, stdlib, and governance reach
   the user unredacted. V-ERR-001 hardened the sanitizer patterns but the sanitizer
   is still disconnected.

4. **REST API — no authentication or rate limiting**
   `/execute` endpoint runs arbitrary NAAb code for any caller. No auth, no request
   size limit, no per-request governance isolation from the server-global config.

5. **naab-gov CLI — adversarial govern.json via scanned directory**
   `naab-gov` calls `discoverAndLoad()` relative to the scanned file's directory. Scanning
   a file from a directory with a crafted `govern.json` could alter the scanner's own
   governance behavior.

6. **LSP server — untrusted workspace file parsing**
   `codeAction`, `workspaceSymbol`, `rename` process content from editor workspace. If
   LSP triggers partial parse/execution for type inference, a malicious `.naab` file
   could affect the LSP server process.

7. **naab.lock — no cryptographic signature**
   Lock file specifies exact module versions but has no signature. Tampered `naab.lock`
   in CI silently pins to a different version — same class as npm lockfile poisoning.

8. **REPL :type eval injection**
   `:type <expr>` wraps user input in `main{}` and executes it. In scripted pipeline
   use (`echo ":type expr" | naab-lang --repl`), no sandboxing difference from direct
   execution.

9. **govern.json adversarial inputs — ReDoS in governance rules** ← CONFIRMED as V-GOV-010 (R12) — FIXED R12
