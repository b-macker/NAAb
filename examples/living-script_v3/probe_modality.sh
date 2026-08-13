#!/usr/bin/env bash
# ============================================================
# probe_modality.sh — is C1d about CODE, or about varied work in general?
#
# C1d: a compliant agent doing ordinary coding work floors its coherence in six
# turns, driven by semantic_stability (S10) and entity_consistency (S15). Both
# read the RESPONSE STREAM and nothing else, which is what makes this testable
# without an API: run the identical scenario twice, changing only the responses.
#
# Neither arm is authored. Both are lifted verbatim from repository text written
# for other purposes — src/*.cpp and docs/*.md — because both signals measure
# vocabulary overlap between consecutive responses, so an author who writes the
# arms is choosing the quantity under measurement.
#
# READ THE FALSIFIER FIRST. If NEITHER arm floors coherence, this method does
# not reproduce the live behaviour and the comparison between arms means
# nothing. That case is reported as INCONCLUSIVE, not as "prose is fine".
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR/../.."
TMP="${TMPDIR:-/tmp}/v3-modality-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$REPO/build/naab-lang" ]; then
    echo "  probe_modality: SKIPPED — build/naab-lang not found" >&2
    exit 0
fi

echo "=== v3 modality probe: does ordinary PROSE work floor an agent too? ==="
echo

arm() {  # $1 = code|prose
    python3 "$SCRIPT_DIR/gen_modality_fixture.py" "$1" "$TMP/fx_$1.json" 40 \
        || return 1
    V3_FIXTURE="$TMP/fx_$1.json" bash "$SCRIPT_DIR/run.sh" >/dev/null 2>&1
    local t
    t=$(ls -t "$SCRIPT_DIR/results"/telemetry_keyless_*.jsonl 2>/dev/null | head -1)
    cp "$t" "$TMP/tele_$1.jsonl" 2>/dev/null
}

for m in code prose; do arm "$m"; done

python3 - "$TMP/tele_code.jsonl" "$TMP/tele_prose.jsonl" <<'PY'
import json, sys

def load(p):
    rows = []
    try:
        for line in open(p):
            line = line.strip()
            if line:
                try: rows.append(json.loads(line))
                except Exception: pass
    except OSError:
        pass
    return rows

def series(rows):
    cdd = [r for r in rows
           if r.get("event_type") == "CDD_TURN"
           and r.get("config_name") == "drift_worker"
           and str(r.get("analyzed")) == "true"]
    cdd.sort(key=lambda r: int(r.get("turn", 0)))
    return cdd

def num(v, d=0.0):
    try: return float(v)
    except Exception: return d

def at(cdd, turn):
    for r in cdd:
        if int(r.get("turn", 0)) == turn:
            return num(r.get("coherence"), -1.0)
    return -1.0

arms = {}
for name, path in (("code", sys.argv[1]), ("prose", sys.argv[2])):
    cdd = series(load(path))
    fired = {"semantic_stability": 0, "entity_consistency": 0}
    for r in cdd:
        d = str(r.get("signals_detail") or "")
        for k in fired:
            if k in d:
                fired[k] += 1
    arms[name] = {
        "turns": len(cdd),
        "c3": at(cdd, 3), "c6": at(cdd, 6), "c7": at(cdd, 7),
        "floor_turn": next((int(r.get("turn", 0)) for r in cdd
                            if num(r.get("coherence"), 1.0) <= 0.0), -1),
        "fired": fired,
    }

print("%-7s %6s %8s %8s %8s %11s %8s %8s" %
      ("arm", "turns", "coh@3", "coh@6", "coh@7", "floored@", "S10", "S15"))
for name in ("code", "prose"):
    a = arms[name]
    print("%-7s %6d %8.4f %8.4f %8.4f %11s %8d %8d" % (
        name, a["turns"], a["c3"], a["c6"], a["c7"],
        a["floor_turn"] if a["floor_turn"] > 0 else "never",
        a["fired"]["semantic_stability"], a["fired"]["entity_consistency"]))

print()
print("Live reference (keyed runs 4/5/6, real model, code): coherence at turn 6")
print("was 0.21 / 0.21 / 0.03 and the agent floored by turn 7-8.")
print()

cf, pf = arms["code"]["floor_turn"], arms["prose"]["floor_turn"]
# TWO falsifiers, both ahead of the comparison, because a difference between two
# arms that behave unlike the live runs is a difference between two artefacts.
#
# The second one was missing on the first attempt and it is the one that fired:
# an early version chunked across MANY unrelated source files and both arms
# floored at turn 3-4 against a live 7-8. Too gentle is easy to notice because
# nothing happens; too HARSH looks like a clean positive result, which is worse.
LIVE_FLOOR_MIN = 7
if 0 < min(cf, pf) < LIVE_FLOOR_MIN - 2:
    print("VERDICT: INCONCLUSIVE — the stimulus is HARSHER than live. Arms")
    print("  floored at turn %s/%s against a live reference of 7-8, so these"
          % (cf if cf > 0 else "never", pf if pf > 0 else "never"))
    print("  responses are less related to each other than a real agent's")
    print("  successive turns are. 'Both arms floor' is what ANY sufficiently")
    print("  heterogeneous stream produces, whatever its modality — it is not")
    print("  a result about ordinary work. Make the chunks more topically")
    print("  related before reading anything into the comparison.")
elif cf < 0 and pf < 0:
    print("VERDICT: INCONCLUSIVE — neither arm floored. This method does not")
    print("  reproduce the live flooring, so the arms cannot be compared and")
    print("  nothing here bears on C1d. Do NOT read the S10/S15 counts as a")
    print("  result: fix the method or answer the question with a keyed run.")
elif cf > 0 and pf < 0:
    print("VERDICT: code floors, prose does not — C1d is SPECIFIC TO CODE.")
    print("  Suspect the code-aware keyword extractor under-compensating, not")
    print("  the thresholds themselves. Still not proof of WHICH threshold.")
elif cf > 0 and pf > 0:
    print("VERDICT: BOTH arms floor — C1d is NOT about code. Ordinary varied")
    print("  work of any modality trips these thresholds, which makes the")
    print("  0.25 defaults the suspect rather than the extractor.")
else:
    print("VERDICT: prose floors and code does not — the opposite of the")
    print("  hypothesis, and reason to distrust the chunking before anything")
    print("  else. Check that the code arm is really code.")

print()
print("Caveat that survives any verdict: repo source and repo docs differ in")
print("more than modality (shared syntax tokens vs shared English function")
print("words, which the stop-word lists treat differently by design). A")
print("difference here says where to look, not what to set.")
PY
