# Findings: build-windows CI — skipped canary suite, orphan naab-lang, slow leak suite

Investigation of three symptoms on the `build-windows` job (`.github/workflows/windows.yml`).
Investigation only: nothing here changes code. Proposed fixes are listed per item; any
that are implemented live in a separate PR.

**Evidence base.** Unless stated otherwise, CI evidence is from master `fecb13e`:
Windows job `111252413961` and Linux CI "Build & Test" job `111252414146`
([Windows Build run 37140050538](https://github.com/b-macker/NAAb/actions/runs/37140050538),
[CI run 37140050613](https://github.com/b-macker/NAAb/actions/runs/37140050613)).
The `test-timing-windows` artifact could not be downloaded from the investigating
container (its network policy denies `productionresultssa15.blob.core.windows.net`).
The per-suite timing tables used here are the ones `tools/testtiming/run_timed.sh`
prints into the job log. The job-log API returned the last 5000 of 7617 lines. Those
start 3 s into the shell step, so they cover every suite discussed here.

**Tiers** (from `docs/investigation-method.md`):
- *observed*: read off a CI log.
- *verified*: reproduced in the investigating container (Linux, bash 5.2.21, Release
  build of `fecb13e`), with a control.
- *traced*: followed through config or source to the point of effect, not run on the
  Windows runner.
- *screened*: a heuristic or documentation suggests it; unconfirmed.

---

## 1. `test_prescan_canaries.sh` never runs on Windows

**Finding (traced):** `git` is not on the PATH inside the MSYS2 shell. In
`run-all-tests.sh:2970`, `git diff --quiet -- src/ include/` then exits 127, and the
gate reads any non-zero status as "dirty". So it prints a misleading reason:

```
17:36:11  test_prescan_canaries.sh: SKIPPED (uncommitted changes in src/include)   (observed)
```

It is not line endings, file modes or submodules. Those three only matter once git can
run at all (see "Second layer" below).

Evidence, in decreasing order of decisiveness:

- **CV-04 in the same run (observed).** `test_coverage_visibility.sh` prints
  `UNMEASURABLE — git cannot answer here` at 17:41:10. Its probe runs from the repo root:
  `git rev-parse --is-inside-work-tree` plus
  `git ls-files --error-unmatch run-all-tests.sh`. Neither command is sensitive to line
  endings, file modes or submodule state. All submodules are under `external/` anyway,
  outside the gate's pathspec. Same shell, same PATH, same run: git cannot answer at all.
- **Configuration (traced):**
  - `msys2/setup-msys2` at the pinned SHA `66cd2cc` defaults to `path-type: minimal`.
  - MSYS2's `/etc/profile` minimal branch appends only `System32`, `Windows`,
    `System32/Wbem` and `WindowsPowerShell/v1.0` from Windows.
  - The checkout step's git is `C:\Program Files\Git\bin\git.exe` (visible in the job
    log). It is not on that PATH.
  - Neither MSYS2's `base` metapackage nor the workflow's `install:` list contains `git`.
- **Mechanism (verified locally).** With git made unresolvable (`hash -p /nonexistent/git git`):
  - the gate exits 127 and prints exactly the message above;
  - the CV-04 probe also exits 127, which CV-04 reports as UNMEASURABLE;
  - positive control: with git present, on the clean tree, the gate's command exits 0.
- **History.**
  - `0304af3a` (2026-06-18) is the canary suite's own `git rev-parse` guard firing on
    Windows. That commit turned it into `SKIPPED (no git)` and gave the reason
    *"Prescan canaries require git checkout with full history."*
    - That reason does not hold. A `--depth 1` clone supports every git operation the
      suite uses (`rev-parse`, `diff`, `checkout --`, `hash-object`). Verified: edit a
      file, `diff --quiet` exits 1, `checkout --` restores it, `diff --quiet` exits 0.
  - `561964a8` (2026-06-21, #46) added the outer registration gate. It now fires first,
    with the wrong reason.
  - Net effect: the canary injections have never executed on Windows.
- **Weakest link.** git's own stderr is never visible on the runner: every git call that
  runs there redirects it to `/dev/null`. The 127 is inferred from configuration plus the
  local reproduction.
  - **Would falsify this:** a `command -v git` on the runner, inside the MSYS2 shell,
    printing a path.

**Second layer (screened, matters only if git is installed).**
- `actions/runner-images`' `Install-Git.ps1` installs Git for Windows with no CRLF
  option. I believe the installer default is `core.autocrlf=true`, which writes a CRLF
  working tree; this repo has no `eol` rules in `.gitattributes`.
- An MSYS2 `git` reads its own `/etc/gitconfig`, not Git for Windows' system config. It
  would likely see every `src/` file as modified, so the gate would still skip, now
  truthfully.
- MSYS2 bash running CRLF scripts proves nothing here: MSYS2's bash carries
  `0005-bash-4.3-msys2-fix-lineendings.patch`.
- Settle it with `git config --show-origin core.autocrlf` and a CR count on one
  `src/` file, both on the runner.

**Proposed fixes.** The June commit decided "skip on Windows", so whether the canaries
should *run* there is your call.

- **(a) Recommended regardless:** split the gate by exit status, so a missing git reads
  as UNMEASURABLE, the way CV-04 already does:
  - 0: run the suite;
  - 1: dirty working tree;
  - anything else: git unusable.

  This costs nothing on Windows. It also tests the hypothesis above: the runner will
  print the real exit status.
- **(b) To actually run the canaries on Windows:**
  - add `git` to the setup-msys2 `install:` list;
  - probably also `git config --global core.autocrlf true` inside MSYS2 (second layer);
  - treat it as the suite's first-ever Windows run. It takes 69 s on Linux CI and its
    Windows cost is unmeasured (see item 3 for why spawn-heavy shell is expensive there).
- **Avoid** `path-type: inherit`. It imports the whole Windows PATH, and #274 relies on
  the minimal ordering for `/usr/bin/timeout`.
- **Interacts with** the DG-06 change in
  [b-macker/NAAb#277](https://github.com/b-macker/NAAb/pull/277), which stopped
  requiring the canary to be listed because Windows never lists it.

---

## 2. The orphan `naab-lang` comes from `tests/security/test_entry_point_parity.sh`

**Finding:** `test_entry_point_parity.sh:110` starts the REST server without `exec`:

```bash
( cd "$WDIR" && "$NAAB" api "$SRV_PORT" > "$WDIR/server.log" 2>&1 ) &
SRV_PID=$!
```

So `$!` is the subshell. `stop_server` and the EXIT trap run `kill -9 "$SRV_PID"`,
which kills the subshell, and the server survives. `tests/api/test_platform_fixes.sh:381`
documents exactly this shape and uses `exec` to avoid it.

**Linux (verified).**
- Locally, every run leaves four `naab-lang api <port>` processes reparented to PID 1:
  2 runs of 2. Each orphan's PID is the killed subshell's PID + 2.
- Linux CI on the same SHA reports exactly four
  `Terminate orphan process: … (naab-lang)` (observed). `rest_probe` starts a server
  four times: EP-00, 01, 02, 04.
- Positive control: with `exec` added, 0 survivors and 5/5 passed. One run.

**Windows (observed, plus traced).**
- `SKIP [EP-00] cli=0 rest=0 -- cannot drive both doors, UNMEASURABLE` at 17:32:03.91.
  - `rest_probe` runs before that check, so exactly one server was started before the
    early exit.
- The suite's own cleanup then prints
  `rm: cannot remove '/tmp/tmp.9iHjUzuNw6/naab.db': Device or resource busy`.
  - Only `naab-lang api` opens the cwd-relative `naab.db`: `NAAB_DATABASE_PATH` at
    `src/cli/main.cpp:3925`. `validate`/`stats` use `~/.naab/blocks.db`, and the CLI
    arm's `run` opens neither.
  - So the server was alive at the suite's exit. On its own that could be a process
    still dying, since `rm` runs right after `kill -9`.
- The job ends with exactly one orphaned `naab-lang`. That held in both Windows jobs
  sampled: master run 37140050538 and PR run 37140109521.
- Matched control on the same runner:
  - `test_rest_hard_block_survives.sh` (`exec` form, R-01..R-03 pass) and
    `test_platform_fixes.sh` Fix 12 (direct `&` and `exec` form) ran real servers;
  - the total is still one orphan, so killing the server's own PID does work under MSYS2.
- **Not verified:** the orphan's command line on the runner. **Would falsify this:** a
  job-end process listing showing anything other than `naab-lang.exe api <port>`
  created around 17:32:03.

**Proposed fix:**
- `exec "$NAAB" api` at line 110, and `wait` after the `kill -9`.
- A regression arm that fails when a stopped server still answers `/health`.

**Side finding (traced, cause not established):** EP-00 is UNMEASURABLE on Windows on
every run, so this suite measures nothing there.
- Candidate cause (screened): the documented path-vocabulary trap (CLAUDE.md, "fourth
  shape"). `$WDIR` is an MSYS `/tmp/...` path written into NAAb *source*, which the
  native `naab-lang.exe` then opens.
- No argv conversion happens for a path inside a file.
- EP-00 took 0.45 s, which is unexplained for a server start. It does not change the
  orphan finding.

**Not investigated:** the Linux job's fifth orphan, `python3` (pid 21718).

---

## 3. `test_error_msg_leaks.sh`: 137.7 s on Windows, under 6.6 s on Linux

**Finding:** the cost is process creation, not grep.
- The suite runs 874 checks. For 855 of them it runs a 10-process
  `$(grep | grep -v × 9)` pipeline.
- Measured locally with `strace -f`: **8,590 `execve` and 9,464 forks**.
- Under MSYS2, a process start costs on the order of 10 ms: Cygwin emulates `fork()`.

| | Windows (observed) | Linux CI (observed) | This container (verified) |
|---|---:|---:|---:|
| `test_error_msg_leaks.sh` | 137.7 s, `874 passed` | not in top 25 (< 6.6 s) | 4.84 s |
| `test_state_field_screen.sh` (36 `execve`, CPU-bound Python) | 68.1 s | 40.9 s | 67.4 s |
| `test_r22_fixes.sh` | 56.9 s | 17.3 s | — |

- The leak suite is the outlier: more than 20× slower on Windows.
  - Suites dominated by `naab-lang` or Python work are 1.3–3.3× slower.
  - The lowest-spawn suite measured, `test_state_field_screen.sh`, is 1.7× slower.
- Locally, the grep work is 0.06–0.3 s of the 4.8 s. A spawn-only skeleton of the same
  pipeline shape, on empty input, takes 4.5 s.
- Implied Windows cost: roughly 130 s over ~9.5k spawns, about **13–14 ms per
  fork+exec**. That is an estimate from the arithmetic above, not measured on the runner.
- History: no design reason for one pipeline per pattern. The suite grew by appending
  patterns (now 45) and files (now 19), and the cost multiplies.

**Proposed fix: one first-stage grep per file, with the same filters.**

- **Design.**
  - `grep -n -f` runs once per file with all 45 patterns.
  - The 9 exclusion filters are applied once per file. They are line-wise, so filtering
    the union and then selecting a pattern's lines gives the same result as selecting
    first.
  - Per-pattern attribution happens in bash `[[ =~ ]]`: no spawn.
- **Equivalence (verified, prototype outside the repo).**
  - Byte-identical output to the current suite on the clean tree: 874 passed, exit 0.
  - Byte-identical on a copy with planted leaks: 7 FAIL, exit 1. The plant exercises
    `head -3` truncation, two patterns on one line, a `.*` pattern, the
    `static const char*` and `== "` exclusions, and the second (sanitizer-iteration) loop.
- **Cost.** **232 `execve` and 270 forks; 0.21 s locally**, against 4.84 s.
  The Windows estimate is a few seconds (inference).
- **Safe only while** no pattern contains a metacharacter whose meaning differs between
  basic and extended regex (`+ ? | ( ) { }`). None of the 45 do, checked mechanically.
  A future pattern with one needs care.
- The suite has no positive control today. If the refactored matcher silently matched
  nothing, the suite would pass; it should carry a planted-literal control.
- **Pre-existing quirk, kept for equivalence:**
  - `grep -v '^\s*//'` can never match: it is applied to `grep -n` output, whose lines
    begin with the line number. So comment lines are not excluded.
  - That errs toward false alarms, never toward missed leaks.

---

## Incidental (found by luck while scanning the Windows log, not part of the request)

`tests/security/test_r11_fixes.sh` T5 (lines 226–243) never sends its request on Windows:

```
17:39:08.358  tests/security/test_r11_fixes.sh: line 233: /mingw64/bin/curl: Argument list too long
17:39:08.359  PASS: V-API-001: oversized body blocked (HTTP , not 200)
```

- The 100 KB body is passed on curl's command line with `-d`, which exceeds the Windows
  command-line limit, so curl never runs.
- `BIG_STATUS` is empty, and the `!= "200"` branch records a PASS: a broken probe
  reported as a pass.
- Proposed fix: send the body on stdin (`--data-binary @-`), and treat an empty status
  as a failure of the probe rather than a rejection.

---

## Method notes

- **Re-tracing.**
  - Item 2 was re-run independently twice on Linux, then cross-checked against both CI
    platforms' orphan counts.
  - Item 3's equivalence was checked on two independent inputs.
  - Item 1 rests on one independent route (configuration) plus one corroborating
    observation (CV-04). Its weakest link is named above.
- **Adversarial pass, and what it changed.**
  - It found the `rm`-right-after-`kill -9` race. So the "busy `naab.db`" line is now
    used only as evidence that the server was alive at suite exit, and survival to job
    end rests on the orphan count.
  - It confirmed no pattern uses metacharacters that differ between basic and extended
    regex.
- **Direction of error.**
  - Item 1 reports a missing check (the canary). That direction is the one this
    project's history shows is most often over-claimed, but the user observed it
    independently.
  - Items 2 and 3 make no claim about governance strength.
