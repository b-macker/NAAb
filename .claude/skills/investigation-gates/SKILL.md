---
name: investigation-gates
description: Run the gate checks from NAAb's docs/investigation-method.md at the moments its rules get broken. Use this whenever you are about to change engine, governance or test code (history pass first); claim a key, flag, signal, gate or feature is inert, live, governed, covered or fixed; call a flake or CI failure fixed; write or update a findings doc, PR body, or a reply reporting results; say "done" or "pushed"; hand work to another agent or machine; or rely on a code-map (tools/codemap) answer for any of these. Use it on routine-looking steps too, and even when nobody mentions investigation -- the method's record is that its rules break on exactly those steps, and in the campaigns behind it every correction was forced by someone asking.
---

# Investigation gates

The rules live in `docs/investigation-method.md`. That file is the source of
truth; this skill does not copy it. What this skill adds is the part that has
to happen at a specific MOMENT -- a checklist walked, a script run, a draft
linted -- because the method's own finding is that doubt does not arrive on
its own ("Schedule the doubt").

If `docs/investigation-method.md` is not present, you are not in the NAAb
repository or it has moved. Say so and stop: do not reconstruct the method
from memory, which is the least trustworthy input available.

## How to use it

1. Find the gate(s) you are at in the table below. Several can apply at once.
2. Read the named method sections. Locate them by heading, not line number
   (`grep -n '^### ' docs/investigation-method.md`); the file grows.
3. Run the gate's script if it has one.
4. Walk the named checklist block from the method's `## Checklist` section
   (block names below are exact, so `grep -n -F` finds them) and mark every item **done**, **not done**, or **n/a, because ...**.
   An item you skipped is a finding about the work, not a formality.
5. Report in the shape under "Report shape".

| Moment | Gate | Script | Checklist blocks |
|---|---|---|---|
| About to edit code, build test infrastructure, or propose a change | G1 history | `history_pass.sh` | Before changing code — the history pass; Before proposing a change; Before building test infrastructure; Before adding a feature |
| About to say something is inert, live, governed, covered, or unreachable | G2 claim | -- | Before claiming something is inert; Before claiming a subsystem is governed; Before trusting existing coverage of a mechanism; Before relying on an existing test |
| About to call a flake or CI failure fixed | G3 flake | `flake_runs.py` | Before calling a flake or a CI failure fixed |
| About to publish: findings doc, PR body, results-bearing commit message, a reply reporting results | G4 publish | `writeup_lint.py` | Before publishing a measurement; Before publishing a write-up; After any change |
| About to say "done", "pushed", "committed", "merged" | G5 done | -- | Before saying "done" |
| About to give instructions or a prompt to another agent or machine | G6 handoff | -- | Before handing work to another agent or machine |

Scripts are in `.claude/skills/investigation-gates/scripts/`. Their own
positive controls are `tests/selftest.sh` beside them; run it once in a new
environment before trusting a script's silence there.

### The code map informs; the method decides

`tools/codemap` (CLAUDE.md, "Code map") answers several gate questions in one
command instead of a grep campaign, and each gate below names the query that
fits it. Its answers are LEADS, and say so: every call edge carries its tier
(`[ast]` is a direct call; `[value]`, `[dispatch]`, `[thread]` and the rest are
screened), every answer prints what the map cannot see, and coverage says
"executed", never "verified". So a code-map answer can send you to a place but
never closes a checklist item on its own: trace or run what it points at, and
say in the report that the lead came from the map. A code-map count in a
write-up is screened tier, with the commit it was measured at.

The session-start hook prints `codemap status`: whether an index exists, how
fresh it is, which coverage run a read would use, and the command that
refreshes either one when it is not current. Refresh before you rely on it:

- A stale INDEX costs seconds (`codemap index`). Refresh it rather than read
  around it.
- COVERAGE costs 45 minutes or more: the suite runs against the `build-cov`
  build, then the counters are mapped. When a claim would rest on coverage and
  the line says `refresh:` (the run is not complete, was recorded at a commit
  followed by changes a run can see -- anything but Markdown documentation --
  or files changed after its build), either run the line's commands in
  the background, if the session has the time, or do the gate by hand and say
  in the report that coverage was not used. A reading from a run the line flags
  is not evidence.
- A run stopped after its test phase keeps its per-unit counters in
  `.cache/codemap/coverage/NAME/gcda` until a mapping succeeds (one was stopped
  at an agent's 2-hour background limit while mapping). Re-map them with
  `codemap coverage ingest --name NAME --units-dir
  .cache/codemap/coverage/NAME/gcda`. A new `coverage run` under the same name
  deletes them first.

When the line says UNAVAILABLE, NO INDEX or UNMEASURABLE, or the index is
older than the code you are reading and cannot be refreshed here, do the gate
by hand. A gate is never skipped because the tool is down, and an absence the
map reports ("0 callers", "executed by NO unit") is the map's absence until a
probe confirms it. Work already queued for the map is recorded in
`docs/open-investigations.md` (search it for `codemap`), and the map's own
roadmap is `docs/plan-codemap-roadmap.md`.

## G1 -- history, before changing anything

Method: "Code shows what happens now; history shows what was meant",
"Check whether this was already decided", "Search the history before
building", "A fix lands in one copy".

```bash
S=.claude/skills/investigation-gates/scripts
bash $S/history_pass.sh -f <file you will edit> -g <subject noun> <identifier>...
```

- **For one function, `python3 tools/codemap/codemap.py history FN`** (or
  `--range FILE:A-B`, or `--name NAME` for one that no longer exists) adds what
  this script does not: `git log -L` over the function's own lines (mapped
  through uncommitted edits onto HEAD), blame, the lines each commit added or
  removed that name it (a removal reads REMOVED here), the PRs, and the
  tracked `.md` records citing it with their record id or heading. It follows
  the same two rules as this script -- a shallow clone is UNMEASURABLE, and an
  empty pickaxe for a name HEAD still holds is a broken probe. Use this script
  for nouns, regexes and whole files.
- **Blast radius** ("Trace the regression surface before locking a plan"):
  `codemap callers FN --depth 2` before the edit -- it follows calls made
  inside lambdas, through callbacks and on other threads, each edge marked
  with its tier -- and `codemap impact` after it: the functions the
  uncommitted change touches, under each the tracked `.md` records that name
  it (the gotchas and decisions about it -- "check whether this was already
  decided" starts there; the search is by name, so a decision recorded
  without the function's name is not found), their callers up to the entry points, and the units that
  EXECUTE them. Run those units first; they enter the code, which is not the
  same as checking it.
- **Every copy** ("A fix lands in one copy"): `codemap types BASE` lists every
  subclass (every executor class) and `codemap explain FN` its overrides. A
  copy of a BEHAVIOUR under another name -- the VM's and the tree-walker's
  versions of one rule rarely share a name -- is not found by name, so grep
  the behaviour as well.
- **Shallow clones lie.** Clones in remote containers are often shallow (the
  one this skill was built in was depth 1). There, `git log -S` attributes
  every string in the tree to the boundary commit -- measured on this repo at
  depth 1, it named the newest commit (#296) as the origin of
  `loaded_mtime_ns_`, which `cd70a29b` and `596dd798` actually touch. The script detects this and reports UNMEASURABLE; deepen
  with `git fetch --depth=5000 origin master` and rerun. Never report
  "no history" from a shallow clone.
- **Unscoped pickaxe is slow here**: one `-S` over all of NAAb took about
  105 s in one measured run; `-p src -p include` took about 0.5 s. Scope when
  you can, and remember a scoped NONE covers only those paths -- a removal
  recorded only in `docs/` is outside it. An unscoped run with several
  identifiers outlasts the default 2-minute command timeout and looks like a
  hang: give it a 10-minute timeout or run it in the background.
- The script runs queries; it does not classify. Read each commit in full,
  then classify the subject as never built / working / regressed / removed on
  purpose / lost / decided, quoting the commit or doc line that puts it there.
- Before adding or fixing behaviour, enumerate every copy (both engines,
  every executor, every module variant) -- grep the behaviour, not just the
  function name.

## G2 -- before claiming inert, live, governed or covered

Method: "Establish reachability before severity", "Order checks by
decisiveness", "Trace to the point of EFFECT", "Search backward from the
sink", "Sibling methods do not share sibling gates", "A mechanism can be
covered in one direction only", "Positive controls", "An outer gate masks the
inner one", "A sweep over explicit values cannot see a default".

Run first the cheapest check that could invalidate the most -- usually: who
loads this, and is the instrument live? Then:

- Inert: trace alias / indirection / reachability / namesakes; test the key
  ABSENT separately from set-to-default; confirm compensators are live in the
  run you measured; check `docs/settings-liveness.md`, the inert baselines and
  `docs/open-investigations.md` -- some "inert" keys have real consumers in
  `governance_config.cpp`.
- Governed: start from OS sinks and trace backward; diff a module's exported
  symbols against the ones the filter names; measure each gate with a
  matched positive control through the SAME harness.
- Covered: read the direction from the test's assertions, not its name; the
  reproduction must fail before the fix and pass after. Check the test is
  registered, runs on the runner that matters, and does not pass by SKIP or
  against a dead interpreter (`tools/testrunner/dead_gate.py`).

Code-map leads for this gate (each a starting point for the checks above):

- Inert or unreachable: `codemap callers SYMBOL` and `codemap writers FIELD`
  (who sets the state a key feeds). Zero static callers is screened: a NAAb
  stdlib name, a callback or a function pointer can still reach it, and the
  answer lists those blind spots -- read them before writing "unreachable".
- Governed: `codemap reach SINK --gate GATE` walks backward from the sink to
  the entry points that reach it without passing a gate, and lists the gated
  functions where it stopped. Those are the reassuring answer: a function
  counts as gated when it calls the gate ANYWHERE in its body, so each needs a
  trace of the branch the input actually takes.
- Covered: `codemap tested FN` lists the units that EXECUTE it -- read those
  units' assertions, because entering a function is not checking it; `codemap
  untested --under PATH` lists functions no unit enters. A PARTIAL or
  completeness-UNKNOWN coverage run says so on every read, and "executed by
  NO unit" from it is not evidence that nothing tests the function. Neither
  is a reading from a run `codemap status` flags for refresh (above).

## G3 -- before calling a flake or CI failure fixed

```bash
python3 $S/flake_runs.py --observed <fails>/<runs> [--observed ...] --runs <clean runs after>
```

It gives the chance an UNFIXED flake passes your loop anyway, at the point
estimate and at a conservative lower bound, and the loop length needed. If
the loop is too short, the claim rests on the captured failed state or it is
not made. Match every CI result to its head SHA; check platform-only failures
against the runner's build flags before the code.

## G4 -- before publishing

Draft first, then:

```bash
python3 $S/writeup_lint.py <draft.md>       # or: ... - < draft (stdin)
```

A number from the code map (callers, units, untested counts) goes in with its
tier (screened) and the commit it was measured at.

The lint is screened tier: regexes that flag places to re-read (green suite
used as argument, "pushed" without read-back, single-sample causes, absence
claims without a tier, figures without provenance, confidence words) and
document-level gaps (no blind spots, no error direction, no re-tracing count,
no adversarial pass). An empty report means no pattern matched, not that the
draft obeys the method. Then walk the two publishing checklist blocks by hand.

The adversarial pass happens here, at a fixed point, with its own effort:
name the claim most likely to be wrong and the observation that would show
it, go and look, and put what you found in the write-up.

## G5 -- before saying "done" or "pushed"

Report the state you read back, not the command you ran:

```bash
git status --short; git log --oneline -1
git log --oneline @{upstream}..HEAD     # empty = nothing unpushed
git ls-remote origin <branch>           # the SHA the remote actually has
```

Then walk the plan's item list and find each item's artifact in the tree.

## G6 -- before handing work off

Commands must locate the project, guard every input, and run from a cold
start; say which machine each runs on; ask for files with listings (path,
size, line count) plus raw evidence; set the receiver's incentive so a
finding, not a passing run, is success.

## Report shape

Every result-bearing message or document carries, in the sentences
themselves rather than a trailing caveats section:

- **Tier** on each claim: screened / traced / verified.
- **Provenance** on each number: observed / configured / authored.
- **PASS / FAIL / UNMEASURABLE** per check, with UNMEASURABLE stated loudly.
- **The falsifier**: what observation would show the claim wrong, and whether
  you looked.
- **Re-tracings**: how many independent ones, and whether the last changed
  anything.
- **Direction of error**: false alarm or false reassurance, and which one you
  checked harder.
- **"Undetermined -- and here is the observation that would settle it"** is a
  valid result. The method records it was never once used; use it when true.
