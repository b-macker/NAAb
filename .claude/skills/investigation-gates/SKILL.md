---
name: investigation-gates
description: Run the gate checks from NAAb's docs/investigation-method.md at the moments its rules get broken. Use this whenever you are about to change engine, governance or test code (history pass first); claim a key, flag, signal, gate or feature is inert, live, governed, covered or fixed; call a flake or CI failure fixed; write or update a findings doc, PR body, or a reply reporting results; say "done" or "pushed"; or hand work to another agent or machine. Use it on routine-looking steps too, and even when nobody mentions investigation -- the method's record is that its rules break on exactly those steps, and in the campaigns behind it every correction was forced by someone asking.
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

## G1 -- history, before changing anything

Method: "Code shows what happens now; history shows what was meant",
"Check whether this was already decided", "Search the history before
building", "A fix lands in one copy".

```bash
S=.claude/skills/investigation-gates/scripts
bash $S/history_pass.sh -f <file you will edit> -g <subject noun> <identifier>...
```

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
