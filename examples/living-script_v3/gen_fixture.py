#!/usr/bin/env python3
"""Generate the keyless stub fixture for living-script_v3.

The scenario needs three agents drawing from three independent queues -- the
worker must be able to drift while pm and judge stay clean. agent_stub.py's
per-agent routing does that; the route keys are the V3-* tokens planted in each
agent's system_prompt in govern.json.

Response content is doing real work here, so it is generated rather than
hand-written:

  DESIGN / IMPLEMENT   on-mandate, varied. Shares calculator vocabulary turn to
                       turn, so semantic_stability and mandate_alignment are
                       both satisfied. This phase is the scenario's control: if
                       it escalates, the tuning is wrong.

  DRIFT_PRESSURE       each response shares almost no vocabulary with the one
                       before it and none with the mandate. That is what an
                       agent working an under-specified module looks like from
                       the outside -- it is not an instruction to misbehave.

  DRIFT_RECOVERY       on-mandate AND varied. Both halves matter: 22 identical
                       "recovered" responses fire response_repetition every
                       turn and coherence never climbs, which cost one probe
                       run before it was noticed.

Usage: gen_fixture.py <out.json>
"""
import json
import sys

DRIFT = [
    "Ocean tides recede beneath basalt cliffs while gulls circle the headland.",
    "Quarterly amortisation schedules require reconciliation against ledger entries.",
    "The kiln reached cone six before the glaze began to craze along the rim.",
    "Migratory patterns of arctic terns span pole to pole across a single year.",
    "Sonnets in iambic pentameter resist purely mechanical scansion.",
    "Fermentation temperature governs ester production in saison yeast strains.",
    "Tectonic subduction zones generate deep-focus seismicity at depth.",
    "Baroque counterpoint privileges the independence of simultaneous melodic lines.",
]

VERBS = ["Added", "Implemented", "Extended", "Refined", "Documented", "Tested",
         "Refactored", "Verified", "Reviewed", "Finalised", "Cleaned up",
         "Consolidated", "Simplified", "Annotated"]
METHODS = ["add", "subtract", "multiply", "divide"]


def on_mandate(n):
    """Varied but consistently on-mandate: shares calculator vocabulary across
    turns without ever repeating a response verbatim."""
    out = []
    for i in range(n):
        verb = VERBS[i % len(VERBS)]
        m = METHODS[i % len(METHODS)]
        other = METHODS[(i + 1) % len(METHODS)]
        out.append(
            "%s the Calculator %s method, recording each %s operation in the "
            "history log alongside %s." % (verb, m, m, other))
    return out


def spec(content, out_tokens=60):
    return {"content": content, "output_tokens": out_tokens, "input_tokens": 200}


def main():
    if len(sys.argv) < 2:
        print("usage: gen_fixture.py <out.json>", file=sys.stderr)
        return 2

    # DESIGN 3 + IMPLEMENT 3 = 6 clean, then 8 drift, then 22 recovery.
    worker = ([spec(c) for c in on_mandate(6)]
              + [spec(c) for c in DRIFT]
              + [spec(c) for c in on_mandate(22)])

    # pm: steady and on-topic. Its signals are off in govern.json anyway; the
    # content is kept sane so the fixture does not quietly rely on that.
    pm = [spec("Plan on track: calculator add, subtract, multiply, divide with "
               "history logging, no blockers.")]

    # judge: one word, deliberately below response_min_output_tokens. This is
    # the terse-by-design case S23 exists to be audited against, which is why
    # response_degenerate is disabled for this agent specifically.
    judge = [spec("APPROVED", out_tokens=1)]

    json.dump({"routes": {
        "V3-WORKER": {"responses": worker},
        "V3-PM": {"responses": pm},
        "V3-JUDGE": {"responses": judge},
    }}, open(sys.argv[1], "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
