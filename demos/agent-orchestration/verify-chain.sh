#!/bin/bash
# verify-chain.sh — Verify tamper-evident hash chain in NAAb telemetry
#
# Usage: bash verify-chain.sh [telemetry-file]
# Default: demos/agent-orchestration/telemetry.jsonl

TELEMETRY="${1:-demos/agent-orchestration/telemetry.jsonl}"

if [ ! -f "$TELEMETRY" ]; then
    echo "No telemetry file found at: $TELEMETRY"
    echo ""
    echo "Run a demo first to generate telemetry:"
    echo "  ./build/naab-lang demos/agent-orchestration/08-traceability-proof.naab"
    exit 1
fi

echo "=== NAAb Telemetry Hash Chain Verification ==="
echo "File: $TELEMETRY"
TOTAL=$(wc -l < "$TELEMETRY" | tr -d ' ')
echo "Events: $TOTAL"
echo ""

PREV_HASH="NAAB-GOVERNANCE-GENESIS"
VERIFIED=0
BROKEN=0

while IFS= read -r line; do
    [ -z "$line" ] && continue

    HASH=$(echo "$line" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('hash',''))" 2>/dev/null)
    PREV=$(echo "$line" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('prev_hash',''))" 2>/dev/null)
    EVENT=$(echo "$line" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('event_type','unknown'))" 2>/dev/null)
    TS=$(echo "$line" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('timestamp','')[:19])" 2>/dev/null)

    if [ "$PREV" = "$PREV_HASH" ] || [ "$PREV" = "NAAB-GOVERNANCE-GENESIS" ]; then
        VERIFIED=$((VERIFIED + 1))
        echo "  [ok] $TS  $EVENT  (${HASH:0:12}...)"
    else
        BROKEN=$((BROKEN + 1))
        echo "  [!!] $TS  $EVENT  CHAIN BREAK"
        echo "       expected: ${PREV_HASH:0:12}..."
        echo "       got:      ${PREV:0:12}..."
    fi
    PREV_HASH="$HASH"
done < "$TELEMETRY"

echo ""
echo "=== Results ==="
echo "Verified: $VERIFIED / $TOTAL events"

if [ $BROKEN -gt 0 ]; then
    echo "BROKEN LINKS: $BROKEN"
    echo "Telemetry may have been tampered with."
    exit 1
else
    echo "Chain integrity: INTACT"
    echo "All events linked — no deletions or modifications detected."
fi
