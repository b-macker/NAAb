#!/usr/bin/env bash
# ============================================================
# hivemind — run the 4-agent hive-mind chat relay
#
# Usage:
#   ./run.sh "your question or task here"
#   ./run.sh "Build a REST API for a todo app in Python"
#
# Requires: GK1 env var (Gemini API key) — source ~/.bashrc first
# Expected: ~8 API calls, 30-90 seconds depending on response sizes
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR/../.."
NAAB="$REPO/build/naab-lang"

if [ $# -lt 1 ]; then
    echo "Usage: $0 \"your question or task\""
    echo "Example: $0 \"Build a REST API for a todo app in Python\""
    exit 2
fi

PROMPT="$*"

# Source bashrc for API keys if GK1 not already set
if [ -z "${GK1:-}" ]; then
    source ~/.bashrc 2>/dev/null || true
fi

if [ -z "${GK1:-}" ]; then
    echo "ERROR: GK1 not set. Export a Gemini API key as GK1."
    exit 1
fi

if [ ! -x "$NAAB" ]; then
    echo "ERROR: naab-lang not found at $NAAB"
    echo "Build first: cd $REPO/build && cmake .. && make naab-lang -j4"
    exit 1
fi

# Termux tmp
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
WDIR="${_SYSTMP}/hivemind-$$"
mkdir -p "$WDIR"

# Trust store isolation
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

cleanup() {
    cp "$WDIR"/telemetry.jsonl "$HOME/hm-telem-$(date +%s).jsonl" 2>/dev/null
    cp "$WDIR"/transcript.jsonl "$HOME/hm-trans-$(date +%s).jsonl" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$WDIR"
}
trap cleanup EXIT

# Generate signing key and sign governance
"$NAAB" --keygen "$WDIR/key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$WDIR/key.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$WDIR/key.pem"

# Copy source files into work dir
cp "$SCRIPT_DIR/src/hivemind.naab" "$WDIR/"
cp "$SCRIPT_DIR/src/govern.json" "$WDIR/"

# Inject inter-call spacing for live API
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

echo "=== hivemind ($PROMPT) ==="
echo ""

# Run with the prompt as args
(cd "$WDIR" && timeout 600s "$NAAB" "hivemind.naab" $PROMPT) 2>"$WDIR/stderr.txt"
RC=$?

if [ "$RC" -ne 0 ]; then
    echo ""
    echo "--- stderr (last 20 lines) ---"
    tail -20 "$WDIR/stderr.txt" 2>/dev/null
fi

exit $RC
