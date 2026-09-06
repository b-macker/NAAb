#!/usr/bin/env bash
# ============================================================
# hivemind_governed — 6-specialist hivemind with the governance
# the Hivemind-6 audit found missing.
#
# Usage:
#   ./run.sh "your question or task here"
#
# Requires: the gravity CLI (`agy`) reachable through proot-distro,
# same as examples/hivemind. Nothing here needs an API key — the
# governance this example adds is engine-side (taint sinks, behavioural
# sequence detection) plus script-side content gates, none of which
# depend on the agent module.
#
# Expected: 15 calls (6 initial + 6 review + 3 final synthesis).
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR/../.."
NAAB="$REPO/build/naab-lang"

if [ $# -lt 1 ]; then
    echo "Usage: $0 \"your question or task\""
    echo "Example: $0 \"Where is the trust boundary in an LLM pipeline?\""
    exit 2
fi

TASK="$*"

if [ ! -x "$NAAB" ]; then
    echo "ERROR: naab-lang not found at $NAAB"
    echo "Build first: mkdir -p $REPO/build && cd $REPO/build && cmake .. && make naab-lang -j4"
    exit 1
fi

# Run from src/ so govern.json, the structured log and the telemetry files all
# land together. The engine picks up govern.json from the script's directory.
cd "$SCRIPT_DIR/src" || exit 1

echo "── hivemind_governed ─────────────────────────────────────"
echo "  task:      $TASK"
echo "  governance: $SCRIPT_DIR/src/govern.json (mode: enforce)"
echo "  log:        $SCRIPT_DIR/src/hivemind-log.jsonl"
echo "  telemetry:  $SCRIPT_DIR/src/hivemind-governed-telemetry.jsonl"
echo "──────────────────────────────────────────────────────────"
echo ""

"$NAAB" hivemind-governed.naab "$TASK"
rc=$?

echo ""
case "$rc" in
    0) echo "── run complete (exit 0)" ;;
    3) echo "── HARD governance block (exit 3). This is the engine refusing, not a crash."
       echo "   Check the taint and behavioural-sequence sections of govern.json, and"
       echo "   read the rationale fields there before widening anything." ;;
    *) echo "── exited $rc" ;;
esac

# What the gates actually did this run. A gate that leaves no evidence is
# indistinguishable from one that never ran, which is the failure this example
# was built to document — so surface it rather than making the operator grep.
LOG="$SCRIPT_DIR/src/hivemind-log.jsonl"
if [ -f "$LOG" ]; then
    echo ""
    echo "── content gate summary ──────────────────────────────────"
    python3 - "$LOG" <<'PY'
import json, sys, collections
gates = []
decisions = collections.Counter()
for line in open(sys.argv[1]):
    line = line.strip()
    if not line:
        continue
    try:
        e = json.loads(line)
    except Exception:
        continue
    if e.get("event") == "CONTENT_GATE":
        gates.append(e)
    if e.get("event") == "GRAVITY_DECISION":
        decisions[e.get("decision", "?")] += 1

if not gates:
    print("  no CONTENT_GATE events — the gate did not run this session")
else:
    quarantined = sum(1 for g in gates if g.get("quarantine"))
    redacted = sum(g.get("secrets_redacted", 0) for g in gates)
    observed = sum(g.get("injection_observed_count", 0) for g in gates)
    print(f"  responses gated:        {len(gates)}")
    print(f"  quarantined (injection): {quarantined}")
    print(f"  credentials redacted:    {redacted}")
    print(f"  markers seen but NOT acted on (quoted/fenced): {observed}")
for d, n in sorted(decisions.items()):
    print(f"  decision {d}: {n}")
PY
    echo "──────────────────────────────────────────────────────────"
fi

exit $rc
