# Investigation method

Domain-agnostic. Every rule below is here because this codebase broke it at least
once, and most of them cost a confident wrong answer that survived until the next
level of tracing. The specific incidents are in `docs/open-investigations.md` and
`docs/governance-campaign-findings.md`; this file is the reusable part.

Generalises the three standing rules in `docs/plan-engine-observability.md`,
which remain the shorter version.

Ordered by workflow: handling evidence, investigating, concluding, changing,
acting.

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
