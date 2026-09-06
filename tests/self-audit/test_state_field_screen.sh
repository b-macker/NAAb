#!/usr/bin/env bash
# ============================================================
# test_state_field_screen.sh — pin the A6 state-field screen as a baseline gate
#
# tools/screen_state_fields.py answers, for every DriftState/AgentTracker member:
# does anything WRITE it, and can the writer be REACHED? It found A6a
# (DriftState::pipeline_depth, never written, reported to scripts and to the
# chain-protected cdd_snapshot) and A6b (AgentTracker::lease_granted_turn,
# written 3x and read 0x).
#
# WHY THIS EXISTS AS A TEST. The tool shipped unregistered, and register B10 had
# just established the failure mode that creates: an unrun self-audit tool reads
# as coverage while providing none. Three of tests/gorilla's four source-text
# greps sat latent for exactly that reason. A screening pass nobody runs rots
# into a claim about the past.
#
# A6S-01  the tool's own self-test must pass (exit 0). That covers BOTH
#        directions: SELF_TEST_WRITTEN (known-written members must classify
#        written — the false-DEAD direction, which invented findings four times)
#        and SELF_TEST_UNWRITTEN (known-unwritten must classify unwritten — the
#        false-ALIVE direction, which HIDES them).
#
# A6S-02  the flagged set must match the pinned baseline exactly. A NEW unwritten
#        or unread field fails CI here instead of waiting to be found by
#        accident, which is what A6 observes has happened four times out of four.
#        Mirrors A2's inert_keys_baseline.txt.
#
# IDs are A6S-* rather than SF-*: tests/governance_v4/test_steering_efficacy.sh
# already prints SF-01..SF-04, and two suites emitting the same assertion id makes
# a suite log ambiguous to anything that greps by id — including the mutation
# harness pattern used elsewhere in this repo.
#
# CHANGING THE BASELINE IS A DECISION, NOT A CHORE. A row leaving it means a
# field was populated or removed — update this file, SELF_TEST_UNWRITTEN and the
# A6 register row in the same change. A row entering it is a new finding and
# needs the empirical check with a positive control that A6 requires before
# anything is acted on.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOL="$REPO/tools/screen_state_fields.py"
PASS=0; FAIL=0
ok()  { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }

if [ ! -f "$TOOL" ]; then
    echo "  FAIL [A6S-00] tools/screen_state_fields.py is missing"
    exit 1
fi

OUT="$(mktemp)"; trap 'rm -f "$OUT"' EXIT
# One invocation, reused by both assertions: the tool walks every source file
# twice per struct and takes ~1-2 minutes, so running it per-assertion would
# triple the suite's slowest test for no added coverage.
timeout 600 python3 "$TOOL" > "$OUT" 2>&1
TOOL_RC=$?

# --- A6S-01: the instrument's own controls, both directions ---
if [ "$TOOL_RC" -eq 0 ] && grep -q '^self-test failures: 0' "$OUT"; then
    ok "A6S-01" "screening tool self-test passes (both directions)"
else
    bad "A6S-01" "tool exited $TOOL_RC / self-test not clean"
    grep -E '^!! SELF-TEST FAIL' "$OUT" | sed 's/^/       /'
fi

# --- A6S-02: flagged set matches the pinned baseline ---
# Baseline as of 2026-09-06, from a 134-member screen (DriftState 112,
# AgentTracker 22). Both entries are registered as A6a/A6b and deliberately
# NOT fixed — populating pipeline_depth changes chain-protected evidence and
# needs the same call A4 and the A2 keys are waiting on.
EXPECTED="DriftState.pipeline_depth NEVER-WRITTEN
AgentTracker.lease_granted_turn WRITTEN-NEVER-READ"

ACTUAL="$(python3 - "$OUT" <<'PY'
import re, sys
struct, rows = None, []
for line in open(sys.argv[1], encoding="utf-8"):
    m = re.match(r'^(DriftState|AgentTracker)\s+\(', line)
    if m:
        struct = m.group(1); continue
    m = re.match(r'^([a-z_]\w*)\s+\d+\s+\d+\s+(NEVER WRITTEN|WRITTEN, NEVER READ|NO ACCESS)', line)
    if m and struct:
        kind = {"NEVER WRITTEN": "NEVER-WRITTEN",
                "WRITTEN, NEVER READ": "WRITTEN-NEVER-READ",
                "NO ACCESS": "NO-ACCESS"}[m.group(2)]
        rows.append(f"{struct}.{m.group(1)} {kind}")
print("\n".join(rows))
PY
)"

if [ "$ACTUAL" = "$EXPECTED" ]; then
    ok "A6S-02" "flagged set matches baseline (2 entries: A6a, A6b)"
else
    bad "A6S-02" "flagged set drifted from the pinned baseline"
    echo "       --- expected ---"; echo "$EXPECTED" | sed 's/^/       /'
    echo "       --- actual ---";   echo "${ACTUAL:-<none parsed>}" | sed 's/^/       /'
    echo "       A row LEAVING means a field was populated or removed: update this"
    echo "       baseline, SELF_TEST_UNWRITTEN and the A6 register row together."
    echo "       A row ENTERING is a new finding: it needs an empirical check with a"
    echo "       positive control before anything is acted on (A6's constraint)."
fi

echo ""
echo "state-field screen: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
