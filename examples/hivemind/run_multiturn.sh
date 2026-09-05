#!/usr/bin/env bash
# multiturn — 3-agent x 8-turn experiment
# Usage: ./run_multiturn.sh "topic to discuss"
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR/../.."
NAAB="$REPO/build/naab-lang"

if [ $# -lt 1 ]; then
    echo "Usage: $0 \"topic to discuss\""
    exit 2
fi

PROMPT="$*"

if [ -z "${GK1:-}" ]; then
    source ~/.bashrc 2>/dev/null || true
fi

if [ -z "${GK1:-}" ]; then
    echo "ERROR: GK1 not set."
    exit 1
fi

if [ ! -x "$NAAB" ]; then
    echo "ERROR: naab-lang not found at $NAAB"
    exit 1
fi

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
WDIR="${_SYSTMP}/multiturn-$$"
mkdir -p "$WDIR"

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

OUTDIR="$HOME/multiturn_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"

cleanup() {
    cp "$WDIR"/telemetry.jsonl "$OUTDIR/" 2>/dev/null
    cp "$WDIR"/transcript.jsonl "$OUTDIR/" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$WDIR"
}
trap cleanup EXIT

# Generate signing key and sign governance
"$NAAB" --keygen "$WDIR/key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$WDIR/key.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$WDIR/key.pem"

# Copy source files — use multiturn config as govern.json
cp "$SCRIPT_DIR/src/multiturn.naab" "$WDIR/"
cp "$SCRIPT_DIR/src/multiturn_govern.json" "$WDIR/govern.json"

# Inject inter-call spacing
python3 - "$WDIR/govern.json" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
for a in d["agents"].values():
    if "rate_limit" not in a:
        a["rate_limit"] = {"requests_per_minute": 0, "delay_between_calls_ms": 1500}
json.dump(d, open(p, "w"), indent=2)
PY

# Sign governance
(cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1)

echo "=== multiturn ($PROMPT) ==="
echo "Output dir: $OUTDIR"
echo ""

(cd "$WDIR" && timeout 600s "$NAAB" "multiturn.naab" $PROMPT) 2>"$WDIR/stderr.txt"
RC=$?

if [ "$RC" -ne 0 ]; then
    echo ""
    echo "--- stderr (last 20 lines) ---"
    tail -20 "$WDIR/stderr.txt" 2>/dev/null
fi

cp "$WDIR/stderr.txt" "$OUTDIR/" 2>/dev/null
echo ""
echo "Telemetry: $OUTDIR/telemetry.jsonl"
echo "Transcript: $OUTDIR/transcript.jsonl"
echo "Exit code: $RC"

exit $RC
