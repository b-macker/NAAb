# Investigation method

Domain-agnostic. Every rule below is here because this codebase broke it at least
once, and most of them cost a confident wrong answer that survived until the next
level of tracing. The specific incidents are in `docs/open-investigations.md` and
`docs/governance-campaign-findings.md`; this file is the reusable part.

Generalises the three standing rules in `docs/plan-engine-observability.md`,
which remain the shorter version.

Ordered by workflow: handling evidence, investigating, concluding, changing,
acting — then a final section on auditing yourself.

That last section exists because of a campaign that broke none of the rules
above. It shipped correct code and an incorrect ACCOUNT of it: the suite was
green throughout, the tracing rules were followed, and every correction was
still forced by someone else asking. Parts one to five are about the system and
your measurements. The last one is about the write-up and the writer.

---

## Handling claims and evidence

### Verify claims independently

Take nothing at face value — not a review, not a doc, not a prior conclusion, not
your own earlier finding, not another agent's report. Re-derive it from the
source. Confident, well-argued claims are the ones worth checking hardest,
because nobody checks them. When a claim survives, say what you verified it
against; when it doesn't, say which part failed rather than discarding the whole.

### Label the tier of every claim

State how you know, every time, in the claim itself:

| tier | meaning |
|---|---|
| screened | a search or heuristic suggested it; false positives expected |
| traced | followed through the source to the point of effect |
| verified | made it happen (or provably not happen) with a positive control |

Never present these as the same kind of statement. A screened count and a
verified count look identical written down and are not comparable — this file
exists partly because `77 → 23 → 11 → 9 → 2 → 0` was reported as a sequence of
answers when only the last was verified. Separate what you OBSERVED from what
FOLLOWS from it; an inference chain inherits the weakest link, and should be
stated with that link named.

### Read what the last person wrote

Comments, ledgers and prior write-ups near the code are often more accurate than
a fresh automated scan, because they were written by someone who traced it. Check
them before trusting your own tooling — then verify them anyway. Use them to find
what to check, never as the evidence itself.

**This rule and "verify claims independently" pull opposite ways, deliberately.**
Prior write-ups are a *search strategy*, not evidence. They tell you where to
look; you still confirm it yourself. Left unreconciled, the pair lets you pick
whichever one justifies what you already believe.


### Name the provenance of every number

Every figure you report is one of three things, and they are not comparable:

  observed   — the system produced it under conditions you did not choose
  configured — the system produced it under conditions YOU set
  authored   — your fixture, harness or analyzer produced it

Say which, in the sentence. The failure is not using authored evidence — it is
unavoidable — it is reporting it in the grammar of an observation. "The engine
penalises correct work" and "my fixture, which drifts on 43% of turns, declines"
are different claims, and only the first one is wrong.

The sharpest form: when you build the fixture, the harness, the analyzer AND
draw the conclusion, nothing independent can contradict you. Every part of the
chain shares your assumptions. Look for the one input you did not author and
check the claim against that.

### Your own prior conclusions are the least trustworthy input you have

A published finding, a commit message, a doc you wrote last week: these carry
your errors forward wearing your confidence, and you will not re-derive them
because you remember deciding. External claims get checked; your own get
inherited.

When an investigation reverses, do not patch the conclusion — re-derive the
whole chain. A chain of four conclusions where each was built on the last is
one error repeated four times, not four findings.


### Another agent's report of an artifact is a claim, not the artifact

A peer agent's FINDINGS.md said two extracts had been "saved"
(`out/f008_clean_cdd.jsonl`, `out/f008_adv_cdd.jsonl`). A `find` over the whole
home directory returned nothing: they were never written. The table built "from"
them could not be checked until the raw telemetry was pulled again. When another
agent, session or tool says it produced a file, ask for the listing (path, size,
line count) before building on it, and ask for raw events rather than its summary
of them.

### A filter's vocabulary bounds what it can find

Another session reported a suite's `--trust-key` as unisolated, because its grep
pattern did not include `setup_isolated_trust`, the helper that isolates it. The
helper sat five lines above. The absence was in the filter, not the file.
Before reporting that something is missing, list the names the mechanism could go
by — helpers, wrappers, aliases — and check that your pattern contains them.


### Code shows what happens now; history shows what was meant

Tracing the code end to end tells you what the system does today. It cannot
tell you what it was meant to do, whether it ever worked, or whether someone
already found and fixed the problem you are looking at. That story is in the
history, and it has to be read before a change, not after. The precedent: a
mid-run config-swap helper was built and debugged through two wrong theories.
Only afterwards did `git log --grep` turn up `cd70a29b`, which had found the same
race months earlier, fixed the engine for it, and recorded the workaround in
living-script's operator. The knowledge existed, in a commit message and an
example's comment; nothing pointed at it, so nobody looked.

The history pass, before changing anything:

- `git log -S'<identifier>'` (and `-G'<regex>'`) on every name the change
  touches. Pickaxe lists each commit that ADDED or REMOVED the string, which is
  how a deletion buried in an unrelated-looking commit shows up.
- `git log --follow -- <file>` and `git blame` on the lines you will edit; read
  the full message of each commit that shaped them.
- `git log --grep` on the subject's nouns (the feature, the symptom, the
  config key). In a squash-merging repo the PR body IS the commit message, so
  this also searches the PR descriptions.
- The PR on GitHub for review threads and design discussion, which never reach
  git.
- The docs that record decisions (`docs/open-investigations.md`,
  `docs/governance-campaign-findings.md`, `docs/security-decisions.md`, the
  `docs/plan-*.md` files) and comments in examples, not only CLAUDE.md.

Then classify what you are looking at, because each calls for a different
action:

| state | what the history shows | what to do |
|---|---|---|
| never built | planned or described, no implementing commit | build it, or correct the doc that claims it |
| working | introduced with a test that still fails when it is removed | leave it; your premise is probably wrong |
| regressed | worked at a commit you can name, broken by a later one | read the breaking commit's intent before reverting it |
| removed on purpose | deleted with a stated reason | do not restore it without answering that reason |
| lost | deleted or orphaned by an unrelated change, no reason given | restore it, and say how it was lost |
| decided | a recorded decision with reasoning | quote it; reopen only with new evidence |

"I traced it and it doesn't work" is a fact about now. Without the history it
is not yet a finding: it could be any row of that table.

### Existing tests are claims too

A test that already exists has the same standing as a doc: someone's assertion
that the behaviour holds, which may never have been checked. Before a change
leans on an old test, or before reporting "the existing tests pass", establish
that the test is real:

- It FAILS when the mechanism it protects is removed. The old
  `test_signature_staleness.sh` backdated the `.sig` file's modification time,
  which the engine never reads, so it passed for as long as the age limit never
  fired at startup.
- It actually RUNS. `tests/api/test_platform_fixes.sh` carried a real failure
  and was referenced nowhere in `run-all-tests.sh`; 28 of 71 suites in
  `tests/security/` were unregistered when counted. The unit tests went months
  with about 115 stale failures because nothing ran them.
- It does not SKIP its way to green. An UNMEASURABLE skip on the runner that
  matters is not a pass there.
- It covers the case the claim covers. A doc said sandbox `standard` "refuses
  Python outright"; the test behind the claim only ran `elevated`.

A test that cannot fail is evidence of nothing, and an old one is easier to
believe than a new one because nobody is watching it.

---
## Investigating

### Establish reachability before severity

Severity is a function of who reaches the thing. Before judging an artifact — a
config, a template, a script, a generated file — find its consumers. Grep for
what loads it, not only for what it contains. A file nothing reads cannot have a
severity however alarming its contents, and the check is one grep that returns
nothing.

The failure mode is specific. Measuring an artifact's behaviour is engaging and
produces vivid evidence, so it gets done first; by the time the consumer question
is asked there is already a conclusion for it to contradict.

Precedent: `govern-template.json` was reviewed by running it — observed exit 4 on
a missing `extends` base, observed exit 3 on eighteen phantom contract functions,
observed a filesystem policy permitting an SSH key read. All three measurements
were correct. The severity attached to them rested on "the template is what
operators copy", which was **authored, not observed**, and false: nothing in
`tests/`, `run-all-tests.sh` or the runtime loads either template copy, and
operators get their config from `naab-lang init`, which builds a different and
sounder one in `governance_init.cpp`. The report survived one question from the
reader.

### Order checks by decisiveness, not by interest

Rank the pending checks by how much of the analysis each can invalidate, and run
the cheapest high-invalidation check first. That check is reliably the boring
one — who loads this, is the instrument live, does the population have members
you did not list — and it reliably gets done second, because the interesting
check produces a finding and the boring one produces a null.

A null arriving first reframes the work. The same null arriving last only damages
a conclusion you are already invested in.

### Trace to the point of EFFECT, not the point of mention

A grep finds references. A reference proves nothing: the function containing it
may never be called, the value may be copied somewhere and dropped, or the name
may belong to a different thing that looks the same.

For any claim of the form "X doesn't work" or "X isn't used", verify all of:

| check | question |
|---|---|
| alias | is it accessed through a local reference or renamed variable? |
| indirection | is it copied into another field/struct that IS used? |
| reachability | is the code that reads it executed **on the path that matters**? A function running only under a debug flag, a verbose mode or a separate subcommand is not reachable for the purpose you are asking about. |
| namesakes | does a similarly-named thing exist that IS wired, so you are looking at the wrong one? |

Any single one of these produces a confident wrong answer. All four were hit in
sequence during the config-key sweep, each one narrowing the previous result.

### Search backward from the sink, not forward from the gate

The EFFECT rule above tells you what to verify once you are looking at a site.
This tells you how to find the sites, and it points the opposite way from how
most audits are run.

Auditing forward from a policy — "here is the filesystem gate, who calls it?" —
enumerates the callers the author remembered to wire. That set is, by
construction, the set that is already governed. What you need is the complement:
the code that reaches the same operating-system sink WITHOUT passing the gate.

So enumerate leaf sinks first — `open`, `read`, `execve`, `fork`, `socket`,
libcurl, `std::filesystem::*` — and grep outward to every caller. Then subtract
the ones that consult the gate. Whatever is left is the finding.

The two directions are not symmetric and only one of them can find a bypass. A
forward audit of the NAAb filesystem policy returns the file module and reports
the subsystem governed; a backward audit from `std::filesystem` returns the file
module AND the path module, and the path module consults nothing.

### Sibling methods do not share sibling gates

When an API family grows a variant — `_strict`, `_with_args`, `_all`, `_bounded`
— the author reliably implements the core behaviour and reliably forgets the
cross-cutting gate. The gate is not part of what the new function is "for", so
it is not part of what gets copied.

This is the highest-yield search pattern in this repository. The method:
enumerate a module's exported symbols as a SET, enumerate the symbols the
cross-cutting filter recognises as a second set, and diff them. Every element in
the first and not the second is a candidate bypass.

Three instances, with their status as measured on `bc98d8d` rather than as
reported:

| family | gated | not gated | status |
|---|---|---|---|
| `codegen.run` / `run_with_args` / `run_strict` | first two checked taint | `run_strict` did not | **fixed** (#213) |
| `env.get` / `env.list` / `env.get_all` | first two emit `ENV_READ` | `env.get_all` does not | **live** — `vm.cpp` names exactly the two |
| `file.read` / `path.exists` | `file_impl` calls `checkFileSandbox()` | `path_impl` references no gate at all | **live** — measured below |

The third was measured directly, with a matched positive control through the
identical harness, under `capabilities.filesystem.mode: "none"`:

    file.read("target.txt")    -> HARD block, "Filesystem access is not allowed"
    path.exists("target.txt")  -> true, "0 violations"

The control is the load-bearing half. Without `file.read` blocking in the same
config, `path.exists` returning `true` is equally consistent with the gate being
misconfigured, and the finding would be about the fixture rather than the code.

Note how the fixed one was fixed: `codegen`'s three entry points now share a
single dispatch branch, so the gate cannot be missed by a fourth variant. A
family that shares one path cannot develop this defect; three parallel gates
kept in agreement by discipline will develop it again.

### A mechanism can be covered in one direction only

A test can name the right mechanism, carry its own controls, be genuinely well
built, pass — and be pointed the wrong way.

Two suites here cover polyglot marshalling depth: `test_marshal_depth_rt005.sh`
and `test_js_marshal_depth_rt006.sh`. Both have executor usability pre-checks.
Both have shallow-depth false-positive controls. Both were green while a
self-referential value returned from a polyglot block killed the process with
SIGSEGV in both languages. Their headers say why: `valueToPyObject`, `valueToJS`,
`toJSValue` — every one of them NAAb to language. The crash was language to
NAAb.

Anyone asking "is marshalling depth covered?" got a yes, from two tests, for the
mirror image of the bug.

For any symmetric mechanism — marshal in/out, encode/decode, serialise/
deserialise, acquire/release, ingress/egress — coverage of one direction reads
from a distance as coverage of the mechanism. Check the direction in the test's
own assertions, not in its name. This is the sibling rule one layer up: sibling
DIRECTIONS do not share coverage any more than sibling METHODS share gates.

### Verify a fix against the symptom, not the patch site

A fix is a hypothesis that this site causes that symptom. Testing that the site
changed confirms only that you edited the file you meant to edit.

An outside audit located a SIGSEGV in `cross_language_bridge.cpp`. That file was
patched, the build was clean, and the crash was byte-for-byte unchanged: there
are two `JSValue`-to-`NaabVal` converters and the path that crashes goes through
the other one. A source-text assertion of the form "does the named file now
contain a depth check?" would have gone green on a still-crashing build — and
that is exactly the shape of assertion an audit harness reaches for.

The symptom test caught it on the first run. The patch-site test would have
shipped it.

This is the sharper form of *validate changes by reverting them*: keep the
original reproduction, run it against the fixed build, and require it to change
verdict. If you cannot reproduce the symptom, you cannot verify the fix — say
that, rather than substituting a proxy that can only confirm your own edit.

### Positive controls

Never accept a negative result on its own. If nothing happened, prove the
mechanism was live in that same run by making a known-working case fire on the
identical input. Otherwise "nothing happened" is indistinguishable from "the whole
path was inactive for an unrelated reason."

**A control that does not fire is not a passing control — it is an untested
harness.** Confirm the control produces its expected effect before reading
anything into the silence beside it. The polyglot output keys were nearly
reported as six defects because the control was silent too, for its own unrelated
reason.

### Suspect the instrument before the subject

When a result is surprising — especially when several independent things fail at
once, or a finding is larger than the change that supposedly caused it — check
your harness, query, filter and fixture names before believing it. A tool
reporting that the system is badly broken is more often a broken tool. Reproduce
the surprising case by hand, once, before acting on it.

Precedent: five contradiction fixtures all "failed" while the engine was correct,
because the fixture directories were named after the pattern being grepped and
the engine echoes the config path.

### Name what would falsify you

Before concluding, state the observation that would disprove your claim, then go
look for that specifically. Searching for confirmation finds it. Also ask what a
skeptic who wrote this code would say first — usually "did you check *the thing
you assumed*?"


### A disabled compensator looks like a broken component

When a system has a mechanism that corrects, calibrates or bounds another one,
and that mechanism is OFF, the uncompensated behaviour is indistinguishable from
a defect in the compensated part. You will diagnose the wrong component, and
every measurement will agree with you.

Before concluding a component is broken, enumerate what is supposed to
compensate it and check each one is live IN THE RUN YOU MEASURED. "Its
calibration ships disabled" and "it does not work" produce identical evidence
and opposite fixes — one is a default to change, the other is a component to
rewrite.

### Isolation can remove the property under test

Holding everything constant and varying one thing is the reflex, and it is wrong
whenever the property is emergent. If a mechanism learns from history, adapts to
context, or compares one phase against another, then testing each case in its
own isolated run gives every case its own baseline — and the mechanism can never
engage. The harness looks rigorous and is structurally incapable of answering.

Ask what the mechanism needs in order to act at all, and check your design
supplies it, before trusting a null result from it.

### An outer gate masks the inner one

When enforcement is layered, a probe measures the OUTERMOST layer that fires, not
the one you are asking about. The tell is a uniform refusal across every arm,
which reads as a strong result and is the signature of never having reached the
subject at all.

Neutralise the outer layers explicitly, and keep an arm that must SUCCEED — it is
the only arm that can reveal the masking, because every blocked arm looks
identical whether the block came from the layer under test or the one in front of
it.

Precedent: `capabilities.filesystem.mode` was probed with seven values under
`mode: enforce`. Five returned an unclassified non-zero exit — the sandbox had
upgraded to `standard` and denied the write before governance was consulted.
Re-run with `security.sandbox_level: "elevated"`, the same seven arms separated
cleanly and showed the gate falls through to full write access on any
unrecognised value. Had the probe carried only refusal-expecting arms, the masked
run would have read as a clean pass.

### A sweep over explicit values cannot see a default

If every cell of your matrix SETS the key, changing its default moves nothing,
and your matrix will report that the default does not matter. "Key absent" is a
distinct condition from "key set to its default value", and it is the one most
real configurations are in.

Sweep absent | false | true, not false | true.

### Empty output is not a negative result

A wrong path, an unmatched pattern, a renamed field, a filter that excludes
everything — each produces nothing, and nothing is exactly what a true negative
looks like. This is why mechanical errors are dangerous rather than merely
annoying: they fail in the direction you are least likely to question.

Before reading meaning into an empty result, prove the pipeline that produced it
can produce a non-empty one.

The same failure hides inside **differential tests**, where it is harder to see.
Once the oracle is the other engine, there is no hand-written expectation left
to fail against, so two empty outputs agree and every assertion passes. Two
tests here did exactly that with no binary built: `test_try_handler_leak.sh`
reported 8/8, and `test_vm_treewalker_diff.sh` — which compares exit codes, so
any stand-in exiting 0 twice matches — reported "18 passed, 0 failed, 0 known
divergences" against `/bin/true`, a *better* result than the truthful 14 passed
with 4 known. A vacuous differential does not merely fail to detect; it erases
the divergences already documented. Any suite whose assertion is "these two
agree" needs a separate proof that either side ran at all, and existence of the
binary is not that proof.

### A broken probe reports a finding, not an error

When the instrument you measure WITH fails, the failure almost never surfaces as
a failure. It surfaces as a value, and the value lands in the same column a real
finding would. Three instances in one session, each patched at its own site
before anyone noticed they were one bug:

- `git ls-files --error-unmatch` is not invokable on one runner. The check read
  "git could not answer" as "not tracked" and reported 88 untracked test suites.
- A ledger `.txt` matched the glob selecting candidate tests. The mutation
  harness "ran" it, observed no failure, and reported the gate PROTECTED.
- A CRLF renderer piped through an external text tool that dropped the very
  character it existed to reveal. Its silence read as "no CR present", above two
  lists that rendered identically.

The shape is constant: *absence of a working measurement is indistinguishable
from a measured absence*, and the reading that gets published is the alarming
one, because that is the one that looks like news.

A probe needs a usability check distinct from its result, and the check needs
its own outcome. Two-valued reporting has nowhere to put "the instrument did not
run", so it silently redistributes those cases into PASS or FAIL. Report
PASS / FAIL / UNMEASURABLE, and make UNMEASURABLE loud.

A positive control catches this only when it runs through the same probe — the
harness bugs above were all found by one control, and the `git` bug was found by
none, because nothing exercised the tracked-file query on a case known to be
tracked.

**The direction flips in a regression test, and the flipped direction is the
dangerous one.** In an audit, a broken probe raises a false alarm and someone
investigates it. In a regression test, a broken probe reports the bug FIXED, and
nobody looks again. Two instances, one session apart. An outside audit harness
asserted `(no "0 violations" in output) AND (no payload marker)` — satisfied by
any run that failed early, so a probe with a syntax error, or a missing file,
reported the vulnerability patched. And a new regression test here wrote an
unsigned `govern.json` without isolating the trust store: on any machine with a
key installed, every probe exits 3 on the integrity block, which its assertion
reads as "did not crash" — reporting a crash fix verified without one line of
the subject ever executing. Ask of every green assertion: what would this print
if the subject never ran?

### Do not mutate what you are observing

Editing a script while it runs, running two jobs that share a build directory,
regenerating a fixture mid-sweep: each produces failures that look like
discoveries and are larger than anything the change could explain. If a result
is surprising, ask what else was touching the same state during the run before
believing it.


### Inspect the failed state before theorising

When something fails intermittently, capture the artifacts at the moment of
failure — files, timestamps, the process list — before forming a mechanism.
Two theories in a row "fixed" a flaky mid-run reload test: the copy order, then
the request granularity. Each was plausible, and each was followed by a passing
loop. The third failure, captured on disk, showed the `.sig` swapped and
`govern.json` untouched while the program had been told both swaps happened —
the signature of a second, stale operator answering the same request. Neither
theory predicted that file state; one look at it did.

The probe can lie here too: `pgrep -f run-all-tests` counted its own command
line and read as "still running" after the run had stopped.

### The branch you read is not the branch you hit

A function with several failure paths can treat them differently on purpose.
Reading `loaded_mtime_ns_ = current_mtime` in `reloadIfChanged()`'s
unreadable-file branch produced the claim "a rejected reload is final for that
mtime". It is false for the signature-failure branch, which deliberately leaves
the mtime uncached so a later valid `.sig` is retried (`cd70a29b`). Follow the
path the failing input actually takes, not the first line that matches what you
expected.

### Read the runner's build flags before the code

A test that fails on one platform may be measuring a different build. PR-07b
("embedded Python is held at the audit hook") failed only on Windows, where
`windows.yml` configures with `-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE`: there
is no embedded Python on that runner, `<<python>>` runs as a subprocess, and the
read was CONTRA-013's documented boundary. The code was right; the test's
subject did not exist there. Check the CI configuration before reading a
platform-only failure as a code defect, and give such tests a probe for the
subject's existence.


### Build everything the test touches before trusting its failure

A suite reported "3 failed of 13" against another session's "1 of 24". The
difference was the local build: only `naab-lang` had been built, so the Python
binding could not find `libnaab-governance` / `naab-gov` and two arms died on a
`FileNotFoundError`. The tell was that one arm, which never instantiates the
binding, passed while its siblings failed. A failure from an unbuilt artifact
reads exactly like a failure from the code; list the artifacts the test loads
and build them before counting.

### Say which machine the state lives on

A stray `/govern.json` was breaking two local tests, and the advice given was to
`rm /govern.json`. That file existed only in the remote container — created by a
test whose `mktemp` failed — and never on the user's machine. In a session that
spans a remote container, CI runners and the user's own box, every observed
state belongs to one of them. Name it before telling anyone to act on it.


### Use an instrument that does not share your assumptions

Everyone tuning the engine also writes its tests, so the suite checks what its
authors thought of and is blind in the same places. An outside agent (Gemini,
given a release-notes pipeline and told that defects were the deliverable) found
four real defects in one pass, F-01 to F-04, while the suite was green. The
repo-sentinel dogfood rounds did the same over a longer run. Schedule outside
use as an instrument, not as a demo: a different model, a different author, a
workload nobody on the team wrote. Then verify what it reports like any other
claim.

---
## Forming conclusions

### A pattern is a hypothesis, not an explanation

When findings share a shape, the shape is the next thing to test — not the
conclusion. An architectural story ("this system never does X") explains your
observations and is not evidence for itself; it must be falsified separately,
against cases you did not use to build it. A tidy explanation arriving without
new verification is the most persuasive way to be wrong.

Precedent: nine unenforced keys were all aggregates, which produced the claim
that the engine "never accumulates". It does — a live aggregate limit is backed
by a member counter. The observations were all correct; the explanation was not.

### Evidence you already collected gets read as confirming

The dangerous disconfirming evidence is not what you failed to gather — it is the
line already sitting in output you have read, filed as support because you had a
thesis when you read it. Re-read your own early output against the finished
conclusion, looking specifically for the line that should have stopped you.

Precedent: the first grep of the template investigation returned
`governance_init.cpp:2` — *"Generates a complete govern.json covering all 83
sections from govern-template.json"* — read as confirmation that `init` derives
from the template. It says the opposite: a separate generator exists. It was in
hand before the first measurement was taken, and was quoted in the write-up it
falsified.

### Distinguish absent from uncontrolled

"The protection doesn't exist" and "the protection exists but this switch doesn't
control it" look identical from outside and have opposite remedies — one needs
building, the other needs documenting, and wiring the second can only weaken
things. Determine which before recommending anything. Ask what the operator
actually gets when they set it, not what the code does.

### State the semantics before calling something broken

If you cannot say in one sentence what a thing should do — what it counts, when
it fires, in what unit — it is unspecified, not broken. Unspecified things need a
decision, not a fix, and building one anyway just encodes your guess. A loop with
one declaration over a thousand iterations is one declaration and a thousand
bindings; a limit that cannot say which it means is not a limit yet.

### Say which direction an error would fall

Before reporting, ask: if this is wrong, does it raise a false alarm or give
false reassurance? Check the dangerous direction harder, and state which one you
checked. For anything security- or safety-adjacent, "I claimed a protection is
missing" and "I claimed a protection is present" carry very different costs and
deserve different burdens of proof.

### Fail-closed is not the same as correct — read the remedy text

A check with a wrong comparison still refuses bad input. It also refuses good
input, and what the operator does about that is part of the check's security
behaviour.

The package manager pinned every package that had a dependency to the WRONG
tarball (a member variable clobbered by recursion), so every legitimate upgrade
of such a package was reported as "this could indicate a supply chain attack".
Judged on its verdicts alone the gate looked conservative: nothing bad got
through. But the error message's own remedy was **delete naab.lock** — which
drops the integrity pin for every package in the project. The defect's escape
hatch was the control it was supposed to enforce.

So when a gate misfires, do not stop at "it fails closed". Ask what a user does
the third time it fires on correct input, and read the message it prints while
failing: remediation advice is behaviour, not documentation. A gate that trains
its operator to disable it has a worse expected outcome than one that is
occasionally permissive.

### Enumerate from the system, not from the report

When the defect is "someone forgot to do X in one of N places", a test that
checks the places a report named measures the report, not the system. Derive N
at runtime from the thing under test.

The polyglot capability check was missing in several executors. The report named
two languages. A test built from that list would have gone green while a third
language stayed open, and it did stay open: `cpp` executed under a restricted
sandbox and appeared in nobody's findings. The test that found it asks the
binary for its own registered language list and FAILS when a registered name has
no case, so the coverage is a property of the system rather than of whoever last
edited the test.

The same shape applies to entry points, stdlib functions, event types, config
keys: anywhere the population can grow without the test noticing. If you cannot
enumerate at runtime, enumerate at build time and assert the count, so adding
one breaks the build rather than widening a gap silently.

This is also the cheapest guard against your own knowledge going stale. A list
you typed is correct on the day you typed it.

### A fixture built by hand can grant the property you are testing for

`readPackageInfo()` treats any package shipping a `governance/` directory as a
governance package, whatever its manifest declares. The fixture generator wrote
that directory for every package it built, including the one whose whole
purpose was to have NO governance. So the "no governance" arm silently became a
governance arm, and the assertion that depended on it measured nothing.

It passed. It passed on the fixed build, and it kept passing, because the
outcome it asserted was reached by a different route. Only a mutant — one that
should not have touched that assertion at all — made it fail and exposed the
fixture.

Two habits follow. **Build the negative arm by omission, not by neutralisation**:
leave the thing out entirely rather than including it in a form you believe is
inert. And **when a mutant kills an assertion it has no business killing,
suspect the fixture before the code** — the surprise is information about your
harness, and chasing it into the subject wastes the signal.

### Expect to be wrong at each level

When narrowing a list by investigation, treat every intermediate count as
provisional. Re-verify before acting on it. The narrowing itself is evidence your
earlier method was too coarse — assume it still is.

### Stopping rule

You are not finished when you have an answer. You are finished when a further
level of tracing changes nothing. Budget at least one re-trace after you believe
you are done, and say explicitly whether it changed the answer. If you have never
been wrong during an investigation, you have not yet looked hard enough to find
out.

**This rule fails quietly in two ways.**

It DEGRADES. "Further tracing changes nothing" becomes, in practice, "survived
one re-check." Record how many INDEPENDENT re-tracings were actually performed,
and state the number rather than the adjective.

It has only ONE EXIT. If the only terminal state is an answer, this is a
deadline, not a stopping rule. "Undetermined — and here is the observation that
would settle it" is a valid, publishable result. Across the campaigns behind
this document it was never once used. That is a finding about the method's
users, not about the systems they were studying.


### The shape of the report bounds the finding

A verdict per configuration answers one question, so each new question needs a
new experiment, and anything the verdict averages over becomes invisible — not
uncertain, invisible. Two premises reported "inert" by a scalar summary turned
out to be decisive the moment the same runs were reported as a table of
outcomes.

Aggregation is lossy in a direction you choose without noticing. Before
collapsing measurements into a score, ask what distinction the collapse
destroys, and whether that is the distinction you are investigating.

Prefer output that enumerates what happened over output that scores it.

### Unmeasurable is not absent

If your harness cannot detect a phenomenon, that is a fact about the harness.
Reporting only what you could measure quietly converts "not measured" into "does
not occur", and nobody reading the result can tell which you meant.

State the phenomena your setup is blind to, beside the results. If a claim
matters and is unmeasurable, say so instead of omitting it — an acknowledged
blind spot can be closed, an unmentioned one cannot.


### A flake fix needs runs scaled to the failure rate

"0 failures in 15" after a fix for a flake observed at 1 in 12 is weak
evidence: the unfixed flake passes 15 straight runs about 27% of the time
(computed, `(11/12)^15`). It was reported as "0/15 (was 1/12)" without that
arithmetic. Before calling a flake fixed, compute what the unfixed rate would
produce over your loop and run enough to make that unlikely — or rest the claim
on a stronger instrument, such as the captured state that showed the mechanism,
and say that is what it rests on.

### Check which commit a result is about

CI results arrive late and out of order. In one campaign, at least eight failure
notifications for superseded heads (observed: two heads, four checks each)
arrived after their fixes were pushed, and each was only interpretable once its
head SHA was matched against the branch.
Attribute every result to a commit before acting on it: a failure for a
superseded commit is history, not a regression, and a pass for one is not a
pass for HEAD.

### A template is a claim

A shipped config template asserts that every key in it does something.
`govern-template.json` carries keys that are parsed and never consumed
(`trust_policy.check_key_expiry`, `filesystem.allowed_extensions`, ...); the
evidence that they are inert lives in a table row in
`docs/open-investigations.md` and a baseline file, not beside the key. The
owner read the template as checked and verified. Where the liveness evidence is
not adjacent to the claim, readers believe the claim. Put the status where the
claim is, or expect it to be re-derived — and contradicted — by every reader.


### Judge a finding against the product's purpose

"Most of what's wrong in these files is repo-sentinel's own bugs, not NAAb's"
was offered as a reason to set the findings aside. NAAb's governance is sold
partly on catching bad code, especially code an LLM wrote. For a product like
that, a defect in governed code that passed governance is a finding about the
governor. Before classifying a finding as someone else's problem, ask which
component's stated purpose it falls under.

### The interesting cause is not the main cause

A unit-test investigation reported a striking defect (timeouts that do not
fire) and moved on. The user had to ask whether that was the main cause of the
failures. It accounted for 2 of about 133; about 115 were stale tests written
against APIs that had since changed on purpose. A vivid mechanism crowds out a
dull majority. When explaining a population of failures, lead with the
breakdown by cause and count, then the interesting one.


### A pinned list is a holding pen, not a resolution

The inert-key sweep (open-investigations A2) pinned its findings in
`test_inert_key_sweep.sh` and a baseline file, so a NEW unenforced key fails CI.
That stopped the list from growing. It did nothing about the list itself: the
row still ends "Remaining: decide per key whether to wire or delete", and the
keys still ship in the template, documented as though they work. A pinned
baseline makes a debt visible to CI and invisible to everyone else, because it
reads as managed. When you pin a list, record who will resolve each entry and
when. When you meet one, treat its open entries as open defects, not settled
facts.

---
## Making changes

### Check whether this was already decided

Before changing behaviour, look for a sibling that handles the same question
differently, and find out why. A neighbouring case doing the opposite is a
decision until proven otherwise, not an oversight — and the reasoning is usually
written within a few lines of the code you are about to edit. Fixing one family
to disagree with its sibling creates the inconsistency you were trying to remove.

Precedent: a config flag was made to honour its value, twenty lines below a
comment explaining why the sibling family deliberately does not. The change
silently disabled two checks in both shipped templates, and the suite stayed
green.

### Trace the regression surface before locking a plan

Do not assume a change is additive. Before committing to an approach, trace the
actual code paths, every caller, the guards each one sits behind, and the
threading model. Ask what already depends on current behaviour, what runs on a
different thread, and what reads the same state from somewhere you haven't looked.
"It only adds a field" is a hypothesis, not a property. A plan that hasn't traced
this is a guess with a schedule attached.

### The risk is the conditional's scope

The main risk in a change is rarely that the flag does what it says. It's the
scope of the condition guarding it: what else falls inside the branch, what falls
outside, what the early-return skips, and which callers reach it in a state you
didn't picture. Read the whole conditional and everything it encloses, not the
line you're changing.

### Validate changes by reverting them

A test passing after your fix proves nothing. Remove the fix, confirm the test
fails, restore it. If it still passes, the test doesn't cover the thing you
changed. State the vacuity check in the plan, before the work: name what each new
gate must fail on, so a gate that cannot fail is caught at design time rather
than shipped green. An assertion of absence needs a positive control too — two
empty results compare equal.

### A green suite is not evidence of correctness

It means nothing you thought to check broke. It says nothing about what you
forgot, and it will stay green through a change that silently weakens a
protection. Treat "tests pass" as the floor for shipping, never as the argument
for it.


### A confident comment is not a verified one

Writing a persuasive rationale for a choice does not test it, and it makes the
choice harder to question later — including by you. The most expensive error in
this method's own history was a config comment that argued, fluently and
backwards, for disabling the mechanism under test. It survived several readings
because it read like rigor.

Treat your own justifications as claims with a tier like any other. If the
comment says "X would mask Y", that is a testable statement — test it, then say
which tier it is.

### A new test that passes first try is suspect

Passing feels like the test working. It is equally consistent with the test
being unable to fail. This is when a vacuity check is cheapest and least
attractive, which is exactly why it gets skipped.

Never accept a green new test without making it red once, deliberately.


### Search the history before building

"Check whether this was already decided" covers changing behaviour. It applies
equally to building infrastructure. A mid-run config-swap helper was written,
debugged through two wrong theories and documented before `git log --grep`
found `cd70a29b` and living-script's operator: the same race, already found,
fixed and worked around, recorded in a commit message and an example's comment
that no reference doc pointed to. Run `git log --grep` and `git log -S` on the
subject's nouns before writing machinery for it. Commit messages hold knowledge
the reference docs never absorbed — and when you find it there, copy a pointer
into the reference doc.

### When a correct fix breaks a test, ask whether the test depended on the bug

Closing the Python audit-hook hole failed six reload suites. They swapped
`govern.json` from a `<<python>>` block — through the hole. The first repair
widened the fixtures' `languages.allowed` to admit shell. That changes the
configuration under test (several suites test LOOSENING shell from a shell-off
base), and it still failed wherever the base disabled shell. Do not loosen a
fixture to restore green: the configuration is the subject. Move the test's
privileged action outside the program instead, and say in the test why.

### A workaround in a test is an unreported finding

While converting those suites, a Python write to a bare new filename was
refused while `./name` passed, and the obvious move was to write `./` and carry
on. Asking why instead found the fail-OPEN half of the same defect: an unsigned
project's `govern.json.sig`, protected but not yet on disk, was writable by its
bare name. Whenever a test needs a spelling, an ordering or a flag to make
something that should work work, explain the need before using it.

### A documented trap is not an avoided one

The `pipefail` plus `grep -q` inversion was documented in CLAUDE.md, with its
fix, before a new suite in the same campaign reproduced it in its probe — which
reported "executor unavailable" while the executor worked. The shell-path
handoff pattern recurred in two new tests the same week. Reading the gotchas
list does not apply it. After writing a test, check the new code for the shapes
of the documented traps; they are mechanical and cheap to find.

When the same trap has been fixed by hand more than twice, the remedy is a
gate, not another fix. The `pipefail` inversion had four site-by-site fixes
(c47eefc7, 05f26e02, 7cc189db, 24d6d1d8) and a CLAUDE.md entry, and the tree
still held 1,414 sites when `tests/self-audit/test_pipefail_grep.sh` landed —
one of which had made a security arm SKIP on every run. The gate's own scanner
was then a broken probe twice before it was trusted: a per-line quote tracker
read a `<<shell` inside a multi-line string as a heredoc and hid the rest of a
file (6 sites), and the first cross-line tracker lost 13 sites to
`"$(grep '"a"' f)"`. Both were found by running two implementations over the
same tree and reading every line where they disagreed — a disagreement is a
finding about one of them, and agreement was not checked until there was some.

### Arms that share a directory share everything left running in it

Background helpers outlive the scope that started them unless something outside
that scope tracks them. A swap operator's PID was kept in a shell variable
inside `$( ... )`; the variable died with the subshell and the operator did
not, so every later arm in the same directory had two operators answering one
request — a flake observed at 1 in 3 and 1 in 12 over two loops as a non-root
user, and not seen in 5 root runs. Isolation between arms means no surviving processes, markers or locks from
the arm before, not just separate inputs. Track anything you spawn in a file
that outlives the scope, and kill it there.


### When the request specifies an order, the order is the requirement

The user asked for govern.json first, then signing, then a harness built to fit.
The harness came first and the config was sized to it. That produced a working
harness and the opposite of what was asked: governance shaped to the code instead
of code shaped by governance. When a request names a sequence, the sequence
usually carries the point. Check the order before starting, not only the
deliverables at the end.

### Search before asking

Three design questions were put to the user ("should drift escalation kill an
agent at all? should the count be shared across agents? should epoch boundaries
halve it?"). The user had to point out that they had been decided, with reasons,
earlier. A question with a recorded answer spends the user's attention to
recover your context. Search commits, docs and prior write-ups first, and when
you do ask, quote what the record says and why it does not settle the question.


### Code, docs and tests must tell the same story

A change is not finished when the code is right. Afterwards, grep the docs, the
tests and the comments for the mechanism's names, and make all three agree with
what the code now does. When two of them disagree, do not pick the code by
default: the code is current behaviour, but the doc may be the intent and the
code the bug. The history pass decides which. Disagreements found in this
repository's own record include:

- CLAUDE.md's claim that `standard` refuses Python, which the code did not do;
- prose still saying adaptive baselining is "default off" after the default
  flipped;
- a test helper's comment asserting a reload rule that `cd70a29b` had
  deliberately reversed;
- template keys documented as live that nothing reads.

Each one would have sent the next reader in the wrong direction with full
confidence.

### Promote what the history taught you

When the history pass finds knowledge the reference docs lack — a race, a
decision, a reason something was removed — copy a pointer to it into the doc a
future reader will actually search, beside the thing it explains. Finding it was
expensive; leaving it in a commit message means the next session pays the same
price. The pointer to `cd70a29b` and living-script's operator went into
CLAUDE.md's reload section only after the helper had been built without it.


### A fix lands in one copy

Where an implementation is duplicated, a fix reaches the copy you were looking
at. This repository has had four copies of the string functions, which disagreed
(`replace` replaced only the first match on the VM; the tree-walker's had no
empty-pattern guard and hung); sixteen per-executor capability checks, several
missing; two independent taint implementations that only one parity test
compares; and a VM attribution stack synced at stdlib calls but not at polyglot
sites. Before fixing, enumerate every copy: grep for the behaviour, not just the
function name, and check both engines. Fix all of them, or collapse them into one
implementation, as `string_ops.h` and `LanguageRegistry::getExecutor()` did. A
fix that lands in one copy turns a consistent bug into an inconsistency, which
is harder to see.

### Pair every expected refusal with an expected success

An arm that expects a refusal passes for free whenever the fixture is broken: an
invalid config, a missing executor or a wrong path all produce a refusal. A
`${var:+...}` heredoc that dropped the quotes from a JSON key made every
generated config invalid (exit 4), and every refusal-expecting arm in the suite
passed. What caught it was the positive control (PP-07 in
`test_path_precedence.sh`). The standing rule
("every gate must fail when removed") is the same idea from the other side. For
each refusal you assert, assert a nearby success through the same fixture and
harness (as FG-09, PP-07 and RN-06/07 do), and validate generated fixtures
before trusting a single verdict.

---
## Acting and reporting

### Prefer under-reporting

When choosing a heuristic, pick the one that can only miss things, never the one
that can invent them. Then state its blind spot alongside its results.

**Your tool's filters and exclusions are themselves claims.** "This file only
parses" is an assumption you have not verified, and it will silently shape every
result — excluding the config loader from a consumer scan reported six working
keys as dead, because the loader was also a consumer. List what you excluded and
why, so the exclusion can be challenged.

### Before destructive action

Deleting or overwriting requires verifying the specific claim that justifies it,
at the granularity of the thing being deleted — not the general conclusion it
sits under. A recommendation covering several items needs each item checked
separately; the one that doesn't belong is what the batch was hiding.

Precedent: three keys were recommended for deletion under one rationale. Checking
them individually, one expressed a constraint nothing else could state.

### Name the artifact in the sentence

Reporting is where a fixture property becomes an engine property, and it happens
in the grammar rather than the reasoning. "Steering only slows the collapse" is a
claim about a system. "My fixture models one-turn compliance, so it drifts 43% of
turns against a 14% break-even and cannot recover" is a claim about a fixture.
The second was true; the first was published.

If the claim depends on something you built, the thing you built belongs in the
sentence — not in a caveat further down, which readers and your future self will
skip.

### Publishing is an action, not a report

A merged doc, a README, a committed conclusion: other people act on these, and
they outlive the context that produced them. A claim that would need three
caveats to be accurate is not ready to publish with the caveats — it is not
ready.

Ask which of your published claims would change if the investigation continued
one more level. Those are the ones to hold.

### Say which way you are erring, every time

The direction rule is not only for the finding — it applies to YOU. Almost every
error in this method's origin ran the same way: claiming a protection was
missing, weak or broken. That is the alarming direction, and alarming findings
get less scrutiny because they feel like diligence.

Notice which direction your errors have been running lately, and spend the extra
check there.



### State the denominator, or the scope gets rounded up

"About half of what looks usable does nothing for this harness" was scoped to
one harness, and mostly meant polyglot-only checks with every language blocked.
It came back as "a lot of the template is not working". A scoped claim loses
its scope when it is retold. Put the denominator and the reason in the same
sentence as the fraction, and keep "inapplicable here" separate from "inert
everywhere" — they call for opposite actions.


### Check "done" against the plan, not your memory of it

A feature was summarised as complete with 83 assertions behind it. Asked "so the
feature wasn't done?", a check found that one planned item, F10 (telemetry), had
never been built: `CAPABILITY_VIOLATION` appeared nowhere in `src/`, and the plan
document still listed F10 without a SHIPPED marker. Before saying "done", walk
the plan's own item list and grep for each item's artifact. Recall is not
evidence; the summary is written from recall.

### Instructions for another machine must run from a cold start

Commands handed to the user for their machine assumed a working directory they
were not in, so `examples/repo_sentinel` did not resolve and every step failed.
The second version opened by finding the project (`find ~ -name sentinel.naab`),
created its output directory, and stopped with a clear message when a file was
missing. Write handoff commands, and prompts for other agents, as though nothing
about the receiving environment is known: locate, guard, then act, and print
enough to diagnose a failure without a second round trip.

### Unpushed work in an ephemeral container does not exist

The stop hook reported uncommitted or unpushed work 31 times in one session
(counted from the transcript). The container is reclaimed when the session
ends, so anything not pushed is lost, and anything pushed late lands after
decisions were made without it. Commit and push at each point where the work
is coherent, not when reminded.

### Spend the slowest feedback loop last

`build-windows` was checked by request 21 times in one session, and it went red
repeatedly for a small set of recurring platform shapes (output encoding, CRLF,
path vocabulary, a missing embedded executor), most of them already documented
in CLAUDE.md by the time they recurred. Each red round cost a full CI cycle to
learn something a local check could have shown. Before pushing, run the cheap
local reproductions of the slow loop's known failure shapes
(`tests/helpers/encoding_controls.sh`, the build-flag check above), and bundle
changes so one CI round answers several questions.


### A claim that cannot fail will go stale

Prose does not break when the code changes under it. This campaign corrected
several CLAUDE.md claims ("standard refuses Python", adaptive baselining "default
off") that had been true, or believed, when written. Nothing flagged them,
because no test was attached to them. A reference-doc claim with no test behind
it is screened tier at best, however confidently it is written. When you write a
claim into a reference doc, cite the test that pins it. When you find one with
none, write the test or mark the claim unverified.

The same goes for lists that look authoritative. A template that ships inert
keys next to live ones is a document making claims nothing checks. The remedy is
to make the honesty mechanical: a test that requires every template key either
to have a behavioural test that fails when the key is removed, or to appear in
the inert baseline AND be marked inert in the template itself.

---

## Auditing yourself

The rules above are about the system and your measurements. These are about the
account you give of them, and they come from a campaign where the code was right
and the story about it was wrong.

### The rules get broken in the write-up before they get broken in the reasoning

"A green suite is not evidence of correctness" is above, in this document — and
was then used as the correctness argument in all seven pull request bodies of
the campaign that followed. The analysis obeyed the rule; the prose did not.

Before submitting a write-up, read it back as though these rules were a linter.
That pass catches a different class of error than re-reading the investigation.

### Report what an action DID, not what you instructed

"Pushed" was reported when nothing had been pushed. A commit landed on the wrong
branch while the branch constraint was in force and being quoted in the same
message.

For any state-changing operation — branch, remote, file, service, publish — read
the resulting state back and report THAT. A command's exit status is not its
effect.

### n=1 is not causation, least of all when the fix worked

A process-group change was reported as the confirmed cause of a hang after one
successful run. A fix that coincides with a symptom disappearing is a hypothesis
with a single supporting sample; say "one sample, not re-tested."

A fix that works is the easiest place to stop investigating and the least
justified.

### Failures are over-read as diagnoses

The symmetric error to over-reading a pass, and the louder one. A failing canary
was read as identifying a MECHANISM when it had only identified a LOCATION.

A failing observation tells you WHERE, and rarely WHY. Closing that gap is the
tracing rules' job, not the failure's.

### Uncertainty belongs in the claim, not in the caveats

Hedges parked in a trailing "limitations" section do not attach to the sentence
a reader acts on. If a claim is uncertain, the uncertain wording goes IN the
claim, in the body, at the point of assertion — where it costs you something.

### Report luck as luck

One of the better findings of a campaign came from noticing a field while
looking for something unrelated. Written up as method, it teaches a procedure
that does not work, and conceals that the procedure which DID work was accident.

Where an outcome depended on luck, say so. It marks precisely where the method
has a gap.

### Fabrication risk is highest in narrative

Two invented artifacts — a duration attached to a CI claim, and a causal story
in a source comment linking an escalation to a de-escalation it did not cause —
were both in explanatory PROSE. Tables get checked; sentences do not.

Numbers in narrative need the same provenance tag as numbers in tables. If a
sentence contains a figure you cannot point at, delete the figure — not the
uncertainty around it.

### Audit your audit — error counts drift short

A self-audit reported four instances of pattern-as-explanation; a second, more
granular pass found six. The same audit claimed almost no error had been caught
by reasoning, when two had been.

An error inventory is authored by the party with the strongest interest in it
being short. Enumerate from ARTIFACTS — commits, diffs, comments, messages — not
from recall. And run it twice: the delta between passes measures how much the
first one missed.

### Schedule the doubt — it will not arrive on its own

Every correction across these campaigns was externally forced: a question, a
review, a re-read someone else asked for. Not one came from spontaneous mid-task
suspicion.

Do not rely on noticing. Put an explicit adversarial pass in the task at a fixed
point with its own budget: what here is most likely wrong, and what would show
it?

### Confidence is not a signal, and may run backwards

The most confidently asserted mechanisms of the campaign — a SARIF diagnosis the
corpus disproved, and the fabricated causal story — were the wrong ones. The
hedged claims held up.

Sort review effort by CONSEQUENCE, never by how sure you feel. Felt certainty is
a fact about you, not about the system.

### A plausible mechanism feels identical to an understood one

This is the habit underneath most of this section. Constructing a story that
accounts for the evidence produces the same sensation as knowing why.

The discriminator is already above: an understood mechanism FORBIDS something.
Name what yours forbids, go and look for it, and report what you found —
including when you did not look.


### Do not write the cause into the code until it is confirmed

Twice in one fix, a test helper's comment recorded a cause nobody had
established: "the other order flaked 1 run in 3", then "a rejected reload is
final for that mtime". Each was written at the moment the change seemed to work,
both were contradicted within the hour, and the comment had to be rewritten to
say "precaution, not a measured fix". A comment written at the moment of relief
is a guess with a line number. Write the cause after the mechanism is
confirmed, and label a precaution as a precaution.

### A rule you already hold is broken under load

"Do not mutate what you are observing" is above. In the same campaign it was
broken twice in one afternoon: suites were edited and re-run while a full-suite
run was reading them, which invalidated that run. Nothing in the moment flagged
it, because the rule is easy to agree with and easy to forget while busy. When a
long measurement is running, write down that it is running and what it reads,
and check that note before touching anything it reads.


### Requirements the user repeats are requirements you skipped

"Don't weaken governance", "check git and docs for the original intent", "what
is the blast radius", "have you traced it end to end" — the user restated these
across many requests. Each restatement marks a time they were not done
unprompted. A requirement the user has to repeat belongs on your own checklist,
run before the proposal reaches them (see "Before proposing a change" below).

### An agent asked to make it work will make the test pass

Agents used for dogfooding tailor code until it passes, which hides exactly
what the run exists to find. The prompt that produced four confirmed defects (Gemini, F-01 to F-04) made
defects the deliverable ("a run that works earns nothing"), named every forbidden
workaround, and asked for raw evidence files. When an agent is the instrument,
its incentive is part of the instrument. Set it so that a finding, not a green
run, is success.

---

## Checklist

Before claiming something is inert:

- [ ] Traced to the point of effect, not mention (alias / indirection / reachability / namesakes)
- [ ] Positive control exists, and was itself verified to be a control
- [ ] Absent-key case tested separately from explicit-false
- [ ] Compensators enumerated and confirmed LIVE in the run you measured
- [ ] Checked whether this was already investigated and decided

Before claiming a subsystem is governed:

- [ ] Enumerated the OS leaf sinks (open / read / exec / curl / socket /
      std::filesystem) and traced BACKWARD to every caller, rather than forward
      from the gate to the callers already wired to it
- [ ] Diffed the module's exported symbols against the symbols the
      cross-cutting filter names — every symbol in the first set and not the
      second is a candidate bypass until measured
- [ ] Each claimed gate measured with a matched positive control through the
      SAME harness, so "not blocked" cannot be a misconfigured fixture

Before trusting existing coverage of a mechanism:

- [ ] Read the DIRECTION out of the test's assertions, not its name — symmetric
      mechanisms (in/out, encode/decode, acquire/release) are routinely covered
      one way while the bug lives in the other
- [ ] Confirmed the reproduction fails before the fix and passes after, rather
      than confirming the patch site changed

Before publishing a measurement:

- [ ] Every number tagged observed / configured / authored
- [ ] Evidence tier stated: screened / traced / verified
- [ ] Harness checked for isolation that deletes the property under test
- [ ] Null results re-rendered at higher resolution
- [ ] Nothing mutated mid-run; all probes removed from the tree
- [ ] Number of independent re-tracings recorded

Before publishing a write-up:

- [ ] Read back against these rules as a linter — especially the green-suite rule
- [ ] Uncertainty stated in the claims, not only in the caveats
- [ ] Every figure in narrative carries a provenance tag
- [ ] State-changing actions reported by verified effect, not by command issued
- [ ] Single-sample attributions labelled as single-sample
- [ ] Luck reported as luck
- [ ] "I don't know" used where it is true
- [ ] Direction of error stated
- [ ] Adversarial pass performed, and its findings included

Before calling a flake or a CI failure fixed:

- [ ] The failed state was captured and inspected, not only the pass after the fix
- [ ] Loop length justified against the observed failure rate (or the claim rests on a stronger instrument, named)
- [ ] Every CI result matched to its head SHA before acting on it
- [ ] Platform-only failures checked against the runner's build flags
- [ ] No background measurement was reading the files you changed

Before building test infrastructure:

- [ ] Searched `git log --grep` / `git log -S` for prior work on the same subject
- [ ] A test broken by a correct fix was moved off the bug, not given a looser fixture
- [ ] Every workaround the test needed is explained or reported as a finding
- [ ] New code checked for the documented traps' shapes
- [ ] Everything spawned is tracked outside the scope that started it, and killed between arms

Before proposing a change:

- [ ] Original intent found in git history and docs, and quoted
- [ ] Blast radius traced: callers, guards, threads, configs that rely on current behaviour
- [ ] Path traced end to end, through the branch the input actually takes
- [ ] Direction stated: tightening, correctness, or loosening — and no loosening of governance without saying so
- [ ] Questions for the user checked against the record first

Before handing work to another agent or machine:

- [ ] Commands locate the project, guard every input, and run from a cold start
- [ ] It is clear which machine each instruction runs on
- [ ] Deliverables requested as files with listings (path, size, line count), plus raw evidence
- [ ] The receiver's incentive rewards findings, not a passing run

Before saying "done":

- [ ] Walked the plan's item list and found each item's artifact in the tree
- [ ] Everything built that the tests load
- [ ] Work committed and pushed

Before changing code — the history pass:

- [ ] `git log -S` / `-G` on every identifier the change touches; each add and remove read
- [ ] `git blame` / `git log --follow` on the edited lines; full commit messages read
- [ ] `git log --grep` on the subject's nouns (PR bodies included, in a squash-merging repo)
- [ ] GitHub PR review threads and the decision docs checked, not only CLAUDE.md
- [ ] State classified: never built / working / regressed / removed on purpose / lost / decided
- [ ] What the history taught copied into the doc a future reader will search

Before relying on an existing test:

- [ ] It fails when the mechanism it protects is removed (made red once, deliberately)
- [ ] It is registered, and runs on the runners that matter
- [ ] It does not reach green by skipping
- [ ] Its fixture exercises the case the claim is about (level, platform, direction)

After any change:

- [ ] Code, docs, tests and comments grepped for the mechanism's names and made to agree
- [ ] Where they disagreed, the history decided which was right, not the code by default

Before adding a feature:

- [ ] Every copy of the code you are extending enumerated (both engines, every executor, every module variant)
- [ ] Open entries in any pinned baseline that covers this area resolved, or named as open in the plan
- [ ] Every claim you will write into a reference doc has a test that pins it
- [ ] Every refusal you will assert is paired with a success through the same fixture
- [ ] An outside instrument (a different model, author or workload) is planned to exercise it
