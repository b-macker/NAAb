# Plan — code map roadmap (`tools/codemap`)

**Status:** planned 2026-10-21, nothing below is built. Agreed with the owner:
finish the A64 governance coverage sweep first, then build all ten items in
the order below. Each item's premise was checked on 2026-10-21 against the tree
at `18039b76` (master `dd7744fd` plus #329's docs/tooling), and the check is
named beside it; two premises changed shape on that check and say how.

**Where this sits.** CLAUDE.md "Code map" describes what is built. This file is
the queue: what comes next, why, in what order, and when each item is done. A
session picking up code-map work reads this first, then the item's own history
(`git log --grep`, `codemap history`) before touching code.

## Rules for every item

- **History pass first** (investigation-gates G1): `git log -S`/`--grep` on the
  names touched, CLAUDE.md's Code map section, and this file.
- **The map informs; `docs/investigation-method.md` decides.** A new answer is a
  lead with its tier and blind spots printed, never a verdict. Anything that can
  read as reassurance (a gate edge, an "executed" credit, "citation still
  holds") gets the strictest check, because that is the direction that hides a
  defect.
- **Tests:** each arm beside a control that must come out the other way; every
  arm made red once by a mutant, the mutants listed in the PR. A new answer that
  cannot fail is not a feature.
- **Nothing builds or edits `src/` while a coverage run is mapping** (it compares
  sources against the instrumented build).
- **Landing:** one commit per item, each with its own tests. Items ride the open
  PR on this session's branch until the owner merges; the branch then restarts
  from master. Docs (CLAUDE.md Code map, the skill) are updated in the same
  commit as the item, and this file's status table with it.

## Status

| # | Item | Status |
|---|---|---|
| 0 | A64 sweep: fresh coverage run, `untested` per governance file, classify each | done 2026-10-22: all 47 classified (`docs/sweeps/A64.md`); findings A65-A62; its proposed tests and removals are open |
| 1 | Coverage for the binaries `coverage run` does not instrument | **built** 2026-10-22 (helpers from `build-cov`, gtest and C API units; C-22..C-24b); sweep re-read against `sweep2` done: one function moved to executed, by the gtest unit |
| 2 | `impact` prints the records that name each changed function | built 2026-10-22 (`githist.symbol_names()` shared with `history`; C-25/C-25c) |
| 3 | "Recorded at another commit" only when coverage-relevant files changed | built 2026-10-22 (`coverage_commit_change()`; premise narrowed, below; C-26/b/c) |
| 4 | `cite-check DOC`: do `path:line` citations still point where they say | built 2026-10-22 (`citecheck.py`; 14 arms, 11 mutants); first run over three docs below |
| 5 | Sweep worklist: every item ends with a recorded outcome | built 2026-10-22 (`sweep init`/`status`; C-28); A64 not yet loaded into it |
| 6 | Pre-dispatch checks other than the filesystem gate: enumerate, then model | **built** 2026-10-22 -- and the enumeration found a route defect (A51 corrected, fixed) |
| 7 | `untested` grouped by shared callers | built 2026-10-22 (`untested --group`; C-27) |
| 8 | Functions defined in several files stored per file | planned |
| 9 | PlantUML jar: reconcile the two digests, pin one | reconciled 2026-10-22 (two builds, a compile time apart; Maven's is signed); the pin awaits the owner |
| 10 | How sessions use the map, from their transcripts | built 2026-10-22 (`usage.py`; 7 arms, 8 mutants); first run is authored usage |

Order: 0, then 1 (it changes which A64 readings can be trusted), then 2 and 3
(small), 4, 5 and 7 (sweep tooling), 6, 8, 9, 10.

## 0. The A64 sweep (classified 2026-10-22)

`build-cov` rebuilt and a whole-suite run (`sweep`) started 2026-10-21 at
`18039b76`. Steps and the settling rule are A64 in `docs/open-investigations.md`.
Results are recorded per function in A64. **After item 1 lands, every A64
"no unit" reading is re-read against a run that includes the newly instrumented
binaries** -- an independent re-trace of the sweep's premise, recorded in A64.

## 1. Coverage for the binaries `coverage run` does not instrument

**Premise, corrected on checking (traced, `CMakeLists.txt:79-88`, `dead_gate.py:48`).**
CLAUDE.md says libnaab, naab-lsp, naab-verify-audit and the GoogleTest unit tests
"run uninstrumented". They are not compiled without coverage: `NAAB_COVERAGE`
adds `--coverage` to every target through `add_compile_options`; only the
`_exit` wrap is limited to naab-lang and naab-gov. The gap is in the RUN:
`coverage run` builds a worktree copy whose helper binaries
(`dead_gate.KEEP_BINARIES`: naab-gov, naab-verify-audit, naab-lsp, libnaab.a,
libnaab.so) are copied from the ordinary `build/`, and only naab-lang and
naab-gov are then replaced from `build-cov`. The GoogleTest suite is not a
`run-all-tests.sh` unit at all (screened: no match for `unit_tests` in it).

**Change.** `coverage run` copies every KEEP_BINARIES entry from `build-cov`
when it is built there (and says which came from where); the CLAUDE.md build
line names the extra targets. A codemap-only unit runs the GoogleTest binary
from `build-cov` with its own `GCOV_PREFIX`, named `gtest:naab_unit_tests` so a
reader sees where execution came from (one unit per binary first; per-test
attribution only if a reading needs it). `run-all-tests.sh` is not changed.

**Also the C API (added 2026-10-22, A62).** `libnaab-governance` and its test
program `test_governance_c_api` are not in KEEP_BINARIES and not a suite unit,
so `naab_gov_check()`'s path reads "no unit" however well CI now tests it (the
A64 sweep found A62 through exactly that reading). Instrument the library from
`build-cov` and run the test program as a codemap-only unit
(`capi:test_governance_c_api`), the same way as the GoogleTest binary.

**Direction of error.** Today: false "executed by NO unit" (alarming). After:
more "executed" credits (reassuring), so the unit name must show it came from a
unit test, which is execution of a C++ API, not of the language.

**Done when.** A fixture function reachable only through a second binary reads
executed after the change and "no unit" before (control: a function no binary
reaches still reads "no unit"); the same for a gtest-only function; mutants for
"copy from build/", "no gtest unit". CLAUDE.md's blind-spot list corrected.

**Re-read (2026-10-22, observed).** `sweep2`, a complete run at `1027eece`
with the helpers instrumented and the gtest and C API units, reads 45 of the six
governance files' functions as executed by no unit, against the sweep record's
47: `preprocessCode` no longer exists (A62 removed it), and
`AgentModule::hasFunction` is now executed -- by `gtest:naab_unit_tests` only (19
calls), the unit this item added. All 45 are already classified in the sweep
record (matched by function name; lambdas carry line numbers that moved). So the
item changed one reading, in the reassuring direction, and it is honest: the
unit name says the execution came from a C++ test. Nothing new to classify.

## 2. `impact` prints the records that name each changed function

**Premise (traced).** `codemap history FN` already lists every tracked `.md`
line naming a function as a whole word, with its record id or heading
(`githist.py`). `impact` lists changed functions, callers, entry points and
units, and no records. An agent changing code must run `history` per function
to see the gotchas and decisions about it -- the step ("check whether this was
already decided") the method records as skipped most.

**Change.** `impact` prints, under each changed function, the records naming it
(file, record id or heading), from the same search `history` uses -- one copy of
that search, not two. `--no-records` to suppress.

**Done when.** Fixture: a `.md` naming a changed function is listed; controls: a
function no doc names lists none; a longer identifier containing the name does
not count (whole word, as `history`). Mutant: impact without the record search.

## 3. "Recorded at another commit" only when it matters

**Premise (observed in #329).** `codemap status` asks for a coverage refresh
after ANY commit, including a docs-only one. A warning that fires on noise trains
a reader to skip it.

**Change.** Between the run's commit and HEAD, the reason is kept when any file
OUTSIDE `*.md` and `docs/` changed (conservative: tests, the runner, CMake and
examples all change what a run would record). When git cannot compare the two
(the run's commit is missing from a shallow clone, or git is unavailable) the
reason stays, saying why.

**Premise corrected when built (2026-10-22, traced).** "`*.md` and `docs/`" was
too wide in three places, each a file a run reads: `docs/` holds `.naab`
programs and JSON that suites read (`test_nested_unknown_keys.sh` and
`test_template_coverage.sh` read `docs/govern-template.json`); `tests/` and
`examples/` hold `.md` fixtures runs read (seven `CLAUDE.md`, seven
`prompt.md`); and project_context reads `CLAUDE.md`, `gemini.md` and
`.github/copilot-instructions.md` BY NAME, walking up from a project
directory to `/` (`walkUpward()`, `src/runtime/project_context.cpp`) -- the
repo's own 282 KB `CLAUDE.md` is skipped today only by the 100 KB default
`max_file_size_kb`. Documentation is therefore Markdown outside `tests/` and
`examples/` and not named like those three. Measured (observed, master's last
200 commits): 25 changed only Markdown, 6 of them a `CLAUDE.md`, so the rule
still drops the reason on 19 of 25.

**Done when.** Fixture commits: a `.md`-only commit drops the reason; a `.cpp`
commit keeps it; an unknown commit keeps it with "cannot compare". Mutant for
each.

## 4. `cite-check DOC`

**Premise (observed).** NAAb's docs cite `path:line` (and `~line N`) beside a
named function throughout; lines drift with every edit. The 2026-10-19 status
reconcile of `open-investigations.md` changed 41 rows by hand. Borrowed from
alibaba/open-code-review's comment resolver: locate by quoted text, never trust
the number, and decline when zero or several places match.

**Change.** For each citation, take the backticked identifiers in the same
sentence or table cell, and report one of: **HOLDS** (an identifier is within a
few lines of the cited line at HEAD), **MOVED** (it is not there but occurs once
in the file: the new line is printed), **GONE** (not in the file), **AMBIGUOUS**
(several places), **FILE GONE**, or **UNCHECKABLE** (no identifier beside the
citation) -- with counts, so an unchecked citation is never silently passed.
Screened tier, printed. It never edits a doc.

**Direction of error.** HOLDS is the reassuring answer and only means the name
is near the line, not that the sentence is still true.

**Done when.** A fixture doc with one citation per outcome, each beside a
control; a run over CLAUDE.md, `open-investigations.md` and
`docs/message-map.md` reported (counts per outcome), not acted on.

**First run (observed, `1027eece`, reported, not acted on).** 148 citations:
CLAUDE.md 10 (HOLDS 3, MOVED 1, GONE 1, AMBIGUOUS 5), `open-investigations.md`
126 (HOLDS 26, MOVED 24, GONE 9, AMBIGUOUS 50, UNCHECKABLE 17),
`message-map.md` 12 (HOLDS 4, MOVED 2, GONE 1, AMBIGUOUS 2, UNCHECKABLE 3); no
FILE GONE. The doubt below ("HOLDS is the weakest") held: the first
version's hand-checked samples found two false HOLDS (a directory span's
`governance_v4` confirmed a stale `run-all-tests.sh:2197`, and `code_quality`,
on 137 lines of `governance_checks.cpp`, confirmed a citation whose
`PYTHON_EVAL_CHECKS` had moved 230 lines), and one rare name in a long table
cell decided five citations at once. Each became a rule and an arm (CC-07,
CC-08, CC-12). After them: 5 of 5 sampled HOLDS and 5 of 5 sampled MOVED point
at the named line (one MOVED lands on a comment describing the code that
moved). The run is a list of leads for the docs' owners, not a correction.

## 5. Sweep worklist

**Premise.** A sweep of N items is tracked by hand in a doc row; nothing shows
how many are still open. Borrowed: open-code-review counts files reviewed against
files selected so an agent cannot quietly stop early.

**Change.** `codemap sweep init NAME --untested PATH...` writes
`docs/sweeps/NAME.tsv` (committed, so it survives sessions): one row per
function -- name, file, then empty `outcome`, `evidence`, `decided_by`.
`codemap sweep status NAME` counts rows by outcome, lists the open ones,
counts a row with an outcome but no evidence apart from the done ones, and
flags rows whose function no longer exists in the index. Outcomes use the
method's vocabulary (never built / working / regressed / removed on purpose /
lost / decided) plus the action (test / restore / remove / none).
A64's results are loaded into it once it exists.

**Done when.** Fixture: init from a coverage run, two outcomes recorded, status
shows the rest open; control: a renamed function is flagged, not counted as
done.

## 6. Pre-dispatch checks other than the filesystem gate

**Premise, changed on checking (screened: a grep for the known check names in
both dispatchers, `vm.cpp:116`, `vm.cpp:1860`, `call_dispatch.cpp:1431`; the
dispatchers were not read end to end).** The only other check that grep found
before a stdlib call dispatches is the behavioural-sequence pre-check (`checkPreExecution`, from the
`bsd_stdlib_events.h` table, VM only -- the tree-walker records no stdlib BSD
event, B11). It is a PATTERN detector, not an access gate: it blocks only when
a configured sequence completes. Modelling it as a `[predispatch]` gate edge
would make `reach` report sinks as gated that no policy guards -- false
reassurance. So this item starts by stating the semantics, not by adding edges.

**Change.** Enumerate every check both engines make before dispatching a stdlib
call (by reading both dispatchers; the map's own `callees` as a lead). Gates that
guard a sink become `[predispatch]` edges under the existing verification (every
caller of the table also calls the gate, else nothing is added). Conditional
detectors get a separate `[precheck]` edge that `reach` prints as context and
never counts as gating. The enumeration is pinned by a test, so a new
pre-dispatch check added later is noticed.

**Done when.** The enumeration is written into CLAUDE.md with its file:lines;
fixture: a `[precheck]` edge never stops `reach`; the CM-21b-style control
(dispatcher without the check adds nothing) for each new kind.

**Done (2026-10-22).** The enumeration is not a reading; it is code.
`tools/codemap/predispatch_enum.py` parses `vm.cpp` and `call_dispatch.cpp`
through the extractor and lists every check each dispatcher calls under a
stdlib-marker guard, plus every sink call that runs a stdlib function there
(the routes, counted). `tools/codemap/predispatch_checks.txt` classifies the 64
rows; `tests/self-audit/test_predispatch_enumeration.sh` (10 arms, 6 of them
controls that each kind of difference is reported; 4 mutants of the enumerator
killed) fails on anything unclassified or stale and on a class that disagrees
with `store.py`'s model. CLAUDE.md's pre-dispatch gotcha carries the per-route
table with file:lines (8 citations, all HOLD under `cite-check`). Model:
`[predispatch]` for the `http` module gate, `[precheck]` for six detectors,
credited to the function that owns the route (`_route_owners`), with fixture
arms CM-29..CM-31 and the CM-21b-style controls CM-29b/CM-30b.

What the premise check missed, and the enumeration found:
- **Two more dispatchers.** `VM::callValue` (a module or builtin called as a
  function) runs its own `file`/`http` gates, taint and dangerous-call checks;
  the tree-walker's member route runs `math` constants. The first rule ("a
  dispatcher calls the filesystem allowlist") could not see either -- a filter's
  vocabulary bounds what it finds. The rule is now: calls the allowlist, or
  calls a check or a sink under a marker guard.
- **A route with fewer gates.** OP_GET_MEMBER's zero-argument call ran no
  filesystem or network gate. A51 had recorded them as needing an argument; the
  MODE gate needs none, and `env.load_dotenv` (no parentheses) under
  `filesystem.mode: "none"` was refused only by the sandbox, catchably. Fixed by
  one helper, `VM::stdlibCallGates`, for both routes (A51's correction).
- **Detectors are credited to the route's owner.** Moving the gates into a
  helper made the helper the allowlist's caller, and the model then lost every
  detector `VM::run` runs itself; `_route_owners` resolves a same-file helper
  called on a stdlib route to its caller (CM-31 is the arm, killed by removing it).

Still screened: the enumerator reads guards, not control flow, so a check
reached by fall-through rather than under a marker guard is not listed, and two
routes under the same guard are one row (the count tells them apart).

## 7. `untested` grouped by shared callers

**Change.** `untested --group` clusters the functions it lists by shared direct
callers (and class), so sibling functions are investigated together -- sibling
methods are where gates go missing (the method's "sibling methods do not share
sibling gates"). Borrowed: open-code-review bundles related files into one
review unit.

**Done when.** Fixture: two untested siblings with a common caller form one
group; control: an unrelated untested function stands alone.

## 8. Functions defined in several files, stored per file

**Premise (observed, `codemap stats`, 6 collisions at `18039b76`).** The index
keys a definition by USR, so `main()` (24 files), `main(int, char**)` (7;
CLAUDE.md still says 8, from `f2f892ca`),
`makeInt`, `printUsage`, `OrchestraModule::getName` (in `orchestra.h` and
`stdlib_new_modules.h`) and `StackFrame::toString` (in `error_context.cpp` and
`interpreter.cpp`) are stored once and their call edges merged.

**Change.** Definitions with the same USR in different files get separate
entries, addressed `FILE:NAME` as `reach` already accepts; `stats` keeps listing
them. The two NAAb ones are then classified: linked into one binary (a real ODR
violation, which is undefined behaviour -- an `open-investigations` row) or into
separate ones (harmless).

**Done when.** Fixture: two `main`s in two files keep their own callees; control:
an ordinary function is unchanged. `gov_main.cpp`'s `main` stops reading
UNMEASURED in coverage (the CM-22 case).

## 9. PlantUML jar: reconcile the two digests, pin one

**Premise (`tools/codemap/plantuml.sha256`).** Nothing is pinned, so
`uml --render` refuses to run. On 2026-10-16 the GitHub release jar's sha1 and
the sha1 Maven Central publishes for the same version disagreed, and Maven
Central rate-limited the comparison.

**Change.** Fetch both jars again and Maven's PGP signature; compute both
sha256; check the signature against PlantUML's published key. Equal digests:
pin. Different: find out why (different artefacts under one version?) and bring
the owner the evidence and a recommendation. **The pin itself is the owner's
decision** -- `--render` runs `java -jar` on a downloaded file.

**Done 2026-10-22 up to the decision (observed, this container).** Different
artefacts, but two BUILDS of one release, not two releases: 5,812 identical
entries apart from `version/CompilationInfo.class` and `version/Version.class`,
3 bytes each -- an embedded compile time 47 minutes apart (Maven's 16:46:13Z,
GitHub's 15:59:17Z, 2026-09-05). Maven Central's jar (from Google's mirror;
repo1 answered 429 again) matches Maven's published sha1 and md5 and carries a
GOOD signature by the "PlantUML JAR Signing Key" (`C08C18EE...0213`), the key
that also signed 1.2023.7, 1.2024.8 and 1.2026.6; against the GitHub jar the
same signature is BAD, as it must be for different bytes. Not established: that
the key is PlantUML's own (fetched from a keyserver by id; plantuml.com is
blocked here), and GitHub's own asset digests (API 403). Evidence and both
sha256s are in `tools/codemap/plantuml.sha256`. **Recommendation:** pin Maven
Central's jar (sha256 `8a171e89...910e3`), the one a signature covers.

## 10. How sessions use the map

**Premise.** Nothing measures whether sessions run the queries the skill names,
or which answers they repeat, abandon or check by grep. Borrowed:
open-code-review shaped its toolset from tool-call traces.

**Change.** `tools/codemap/usage.py TRANSCRIPT...` reads Claude Code transcript
JSONL and reports, per subcommand: calls, errors, immediate repeats, and whether
a grep of the same name followed (screened). It prints subcommand names and
counts only, never command text or content.

**Limit, stated in its output.** This container's transcripts are few and mostly
written by the session that built the tool -- authored usage, not observed. The
report is a lead for later, not a basis for removing anything.

## Where this plan is most likely wrong

Written as the doubt pass, before any item is built; each is checked when its
item starts.

- **Item 1 may not credit libnaab's callers.** The Python binding loads
  `libnaab.so` into python3; its counters are written by libgcov at that
  process's normal exit, and the `_exit` wrap is deliberately not linked into a
  shared library (`CMakeLists.txt:86-88`). A binding test that ends through
  `_exit` would write nothing. Measure it with a fixture before claiming the
  library is covered.
- **Item 6's enumeration was a grep for names already known.** A pre-dispatch
  check under a name nobody thought to search is exactly what it would miss;
  the item starts by reading both dispatchers end to end.
- **Item 4's HOLDS is the reassuring answer and the weakest.** A name near a
  line says nothing about whether the sentence around it is still true. If the
  first run reports mostly HOLDS, that is a property of the check, not of the
  docs.
- **Items 5 and 7 can become the thing they measure.** A worklist makes "every
  row has an outcome" the goal; an outcome of "decided" with no evidence column
  filled is a skipped item, which is why item 5 counts those rows apart.
- **Item 10's data is authored.** Almost every recorded codemap call in this
  container came from the session that built the tool. Nothing is removed on its
  evidence.
