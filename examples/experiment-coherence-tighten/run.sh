#!/usr/bin/env bash
# ============================================================
# experiment-coherence-tighten — run the mid-run OA threshold experiment
#
#   ./run.sh              live run against Gemini API (requires GEMINI_API_KEY)
#   ./run.sh --analyze    re-analyze existing telemetry without running
#
# What this does:
#   1. Sets up a work directory with govern.json (OA threshold 0.30)
#   2. Pre-stages the tightened config (OA threshold 0.95) and signs both
#   3. Runs the .naab experiment: 5 baseline turns, mid-run tighten, 3 post turns
#   4. Analyzes telemetry for CONFIG_ADJUSTMENT and OA gate transitions
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR/../.."
NAAB="$REPO/build/naab-lang"
SRC="$SCRIPT_DIR/src"
RESULTS="$SCRIPT_DIR/results"

# -- Parse args --
MODE=run
for _a in "$@"; do
    case "$_a" in
        --analyze) MODE=analyze ;;
        "") ;;
        *) echo "usage: run.sh [--analyze]" >&2; exit 2 ;;
    esac
done

# -- Preflight --
if [ ! -x "$NAAB" ]; then
    echo "ERROR: naab-lang not built. Run: cd $REPO/build && cmake .. && make naab-lang -j4" >&2
    exit 1
fi

if [ "$MODE" = run ] && [ -z "${GK1:-}" ]; then
    # Try sourcing bashrc for the key
    [ -f ~/.bashrc ] && source ~/.bashrc 2>/dev/null
    if [ -z "${GK1:-}" ]; then
        echo "ERROR: GK1 not set" >&2
        exit 1
    fi
fi

# -- Temp directory --
if [ -d /data/data/com.termux ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
WDIR="${_SYSTMP}/coherence-tighten-$$"
cleanup() {
    teardown_isolated_trust 2>/dev/null
    [ -n "${KEEP_TMP:-}" ] && { echo "Artifacts kept: $WDIR"; return; }
    rm -rf "$WDIR"
}
trap cleanup EXIT
mkdir -p "$WDIR" "$RESULTS"

# -- Isolated trust store + key generation --
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
"$NAAB" --keygen "$WDIR/key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$WDIR/key.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$WDIR/key.pem"

sign_gov() {
    local dir="$1"
    (cd "$dir" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1)
}

if [ "$MODE" = analyze ]; then
    echo "=== Analyzing existing telemetry ==="
    TFILE="$RESULTS/telemetry.jsonl"
    if [ ! -f "$TFILE" ]; then
        echo "ERROR: No telemetry file at $TFILE" >&2
        exit 1
    fi
    # Fall through to analysis below
else
    # -- Stage configs --
    cp "$SRC/experiment.naab" "$WDIR/"
    cp "$SRC/govern.json" "$WDIR/govern.json"
    sign_gov "$WDIR"

    # Stage the tightened config in a subdir for the polyglot block to copy from
    mkdir -p "$WDIR/tightened"
    cp "$SRC/tightened-govern.json" "$WDIR/tightened/tightened-govern.json"
    # Sign the tightened config so the copy carries a valid signature
    cp "$SRC/tightened-govern.json" "$WDIR/tightened/govern.json"
    sign_gov "$WDIR/tightened"
    cp "$WDIR/tightened/govern.json.sig" "$WDIR/tightened/tightened-govern.json.sig"

    echo "=== Experiment: Mid-run OA Threshold Tightening ==="
    echo "  Work dir: $WDIR"
    echo "  OA threshold: 0.30 -> 0.999 (mid-run)"
    echo "  Model: gemini-2.5-flash"
    echo ""

    # -- Run --
    export EXPERIMENT_WDIR="$WDIR"
    export EXPERIMENT_SRC="$WDIR/tightened"

    (cd "$WDIR" && timeout 600s "$NAAB" experiment.naab \
        --governance-dashboard 2>&1) | tee "$RESULTS/run-output.txt"
    EXIT=$?
    echo ""
    echo "Exit code: $EXIT"

    # Copy telemetry
    [ -f "$WDIR/telemetry.jsonl" ] && cp "$WDIR/telemetry.jsonl" "$RESULTS/"
    [ -f "$WDIR/transcript.jsonl" ] && cp "$WDIR/transcript.jsonl" "$RESULTS/"

    TFILE="$RESULTS/telemetry.jsonl"
fi

# -- Analysis --
echo ""
echo "=== Telemetry Analysis ==="

if [ ! -f "$TFILE" ]; then
    echo "No telemetry file found — experiment may have failed before any agent turns."
    exit 1
fi

echo ""
echo "--- CONFIG_ADJUSTMENT events ---"
grep '"CONFIG_ADJUSTMENT"' "$TFILE" 2>/dev/null | python3 -c "
import sys, json
for line in sys.stdin:
    try:
        e = json.loads(line)
        print(f\"  turn={e.get('turn','?')}  accepted={e.get('accepted','?')}\")
        for n in e.get('notices', []):
            print(f\"    notice: {n}\")
        for v in e.get('violations', []):
            print(f\"    violation: {v}\")
    except: pass
" 2>/dev/null || echo "  (none or parse error)"

echo ""
echo "--- OUTPUT_ADMISSIBILITY_EVAL events ---"
grep '"OUTPUT_ADMISSIBILITY_EVAL"' "$TFILE" 2>/dev/null | python3 -c "
import sys, json
for line in sys.stdin:
    try:
        e = json.loads(line)
        print(f\"  turn={e.get('turn','?')}  result={e.get('result','?')}  coherence={e.get('coherence','?')}  threshold={e.get('threshold','?')}  agent={e.get('config_name','?')}\")
    except: pass
" 2>/dev/null || echo "  (none or parse error)"

echo ""
echo "--- CDD_TURN coherence trajectory ---"
grep '"CDD_TURN"' "$TFILE" 2>/dev/null | python3 -c "
import sys, json
for line in sys.stdin:
    try:
        e = json.loads(line)
        if e.get('analyzed') == 'true' or e.get('analyzed') == True:
            sig = e.get('signals_detail', '')
            pen = e.get('penalties_detail', '')
            print(f\"  turn={e.get('turn','?')}  coherence={e.get('coherence','?')}  level={e.get('level','?')}  signals={sig[:60]}\")
    except: pass
" 2>/dev/null || echo "  (none or parse error)"

echo ""
echo "--- GOVERNANCE_LEVEL_CHANGE events ---"
grep '"GOVERNANCE_LEVEL_CHANGE"' "$TFILE" 2>/dev/null | python3 -c "
import sys, json
for line in sys.stdin:
    try:
        e = json.loads(line)
        print(f\"  {e.get('from_level','?')} -> {e.get('to_level','?')} at turn {e.get('turn','?')}\")
    except: pass
" 2>/dev/null || echo "  (none)"

echo ""
echo "=== Done ==="
