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

---
## Investigating

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
