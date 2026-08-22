# Investigation method

Domain-agnostic. Every rule below is here because this codebase broke it at least
once, and most of them cost a confident wrong answer that survived until the next
level of tracing. The specific incidents are in `docs/open-investigations.md` and
`docs/governance-campaign-findings.md`; this file is the reusable part.

Supersedes nothing — it generalises the three standing rules recorded in
`docs/plan-engine-observability.md`, which remain the shorter version.

---

## Trace to the point of EFFECT, not the point of mention

A grep finds references. A reference proves nothing: the function containing it
may never be called, the value may be copied somewhere and dropped, or the name
may belong to a different thing that looks the same.

For any claim of the form "X doesn't work" or "X isn't used", verify all of:

| check | question |
|---|---|
| alias | is it accessed through a local reference or renamed variable? |
| indirection | is it copied into another field/struct that IS used? |
| reachability | is the code that reads it ever actually executed? |
| namesakes | does a similarly-named thing exist that IS wired, so you're looking at the wrong one? |

Any single one of these produces a confident wrong answer. All four were hit
in sequence during the config-key sweep, each one narrowing the previous result.

## Verify claims independently

Take nothing at face value — not a review, not a doc, not a prior conclusion, not
your own earlier finding, not another agent's report. Re-derive it from the
source. Confident, well-argued claims are the ones worth checking hardest,
because nobody checks them. When a claim survives, say what you verified it
against; when it doesn't, say which part failed rather than discarding the whole.

## Positive controls

Never accept a negative result on its own. If nothing happened, prove the
mechanism was live in that same run by making a known-working case fire on the
identical input. Otherwise "nothing happened" is indistinguishable from "the whole
path was inactive for an unrelated reason."

## Trace the regression surface before locking a plan

Do not assume a change is additive. Before committing to an approach, trace the
actual code paths, every caller, the guards each one sits behind, and the
threading model. Ask what already depends on current behaviour, what runs on a
different thread, and what reads the same state from somewhere you haven't looked.
"It only adds a field" is a hypothesis, not a property. A plan that hasn't traced
this is a guess with a schedule attached.

## The risk is the conditional's scope

The main risk in a change is rarely that the flag does what it says. It's the
scope of the condition guarding it: what else falls inside the branch, what falls
outside, what the early-return skips, and which callers reach it in a state you
didn't picture. Read the whole conditional and everything it encloses, not the
line you're changing.

## Validate changes by reverting them

A test passing after your fix proves nothing. Remove the fix, confirm the test
fails, restore it. If it still passes, the test doesn't cover the thing you
changed. State the vacuity check in the plan, before the work: name what each new
gate must fail on, so a gate that cannot fail is caught at design time rather than
shipped green. An assertion of absence needs a positive control too — two empty
results compare equal.

## Distinguish absent from uncontrolled

"The protection doesn't exist" and "the protection exists but this switch doesn't
control it" look identical from outside and have opposite remedies — one needs
building, the other needs documenting, and wiring the second can only weaken
things. Determine which before recommending anything.

## Expect to be wrong at each level

When narrowing a list by investigation, treat every intermediate count as
provisional. Re-verify before acting on it. The narrowing itself is evidence your
earlier method was too coarse — assume it still is.

## Read what the last person wrote

Comments, ledgers and prior write-ups near the code are often more accurate than a
fresh automated scan, because they were written by someone who traced it. Check
them before trusting your own tooling — then verify them anyway. Use them to find
what to check, never as the evidence itself.

**This rule and "verify claims independently" pull opposite ways, deliberately.**
The resolution: prior write-ups are a *search strategy*, not evidence. They tell
you where to look; you still confirm it yourself. Left unreconciled, the pair lets
you pick whichever one justifies what you already believe.

## Prefer under-reporting

When choosing a heuristic, pick the one that can only miss things, never the one
that can invent them. Then state its blind spot alongside its results.

## Before destructive action

Deleting or overwriting requires verifying the specific claim that justifies it,
not the general conclusion it sits under.

---

## The thing all of these are for

A green test suite means nothing you thought to check broke. It does not mean the
code is correct. In this repository every real defect has been found by reading a
code path end to end, and every correction came from re-reading something already
concluded — including a fix that was silently disabling two checks in both shipped
templates while the full suite stayed green.
