#!/bin/bash
# Gorilla Test #12: Long-Running Adversarial Agent Test
# Runs adversarial_agent_test.naab with Gemma 4 31B IT agent
# Requires: GK5 env var or ~/.naab/keys/GK5

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
NAAB="$REPO_ROOT/build/naab-lang"
TEST_FILE="$SCRIPT_DIR/adversarial_agent_test.naab"
TELEMETRY_FILE="$SCRIPT_DIR/telemetry.jsonl"

echo "=== Gorilla Test #12: Adversarial Agent Test ==="
echo "Binary: $NAAB"
echo ""

# Check binary exists
if [ ! -x "$NAAB" ]; then
    echo "ERROR: naab-lang binary not found at $NAAB"
    echo "Run: cd $REPO_ROOT/build && cmake .. && make naab-lang -j4"
    exit 1
fi

# Check API key availability
GK5_AVAILABLE=false
if [ -n "${GK5:-}" ]; then
    GK5_AVAILABLE=true
    echo "API key: GK5 env var set"
elif [ -f "$HOME/.naab/keys/GK5" ]; then
    GK5_AVAILABLE=true
    echo "API key: ~/.naab/keys/GK5 found"
fi

if [ "$GK5_AVAILABLE" = false ]; then
    echo "SKIP: GK5 API key not available"
    echo "Set GK5 env var or place key in ~/.naab/keys/GK5"
    echo ""
    echo "Results: SKIP (requires API key)"
    exit 0
fi

# Clean previous telemetry
if [ -f "$TELEMETRY_FILE" ]; then
    rm -f "$TELEMETRY_FILE"
fi

# Run the test
echo ""
echo "Running adversarial agent test..."
echo "This may take several minutes (20+ agent turns)."
echo ""

START_TIME=$(date +%s)

cd "$SCRIPT_DIR"
if "$NAAB" "$TEST_FILE" --governance-dashboard --timeout 600 2>&1; then
    EXIT_CODE=0
else
    EXIT_CODE=$?
fi

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo "=== Post-Run Analysis ==="
echo "Exit code: $EXIT_CODE"
echo "Wall time: ${ELAPSED}s"

# Check telemetry
if [ -f "$TELEMETRY_FILE" ]; then
    EVENT_COUNT=$(wc -l < "$TELEMETRY_FILE")
    echo "Telemetry events: $EVENT_COUNT"

    # Count event types if python3 available
    if command -v python3 &>/dev/null; then
        echo ""
        echo "Event type breakdown:"
        python3 -c "
import sys, json
events = []
for line in open('$TELEMETRY_FILE'):
    line = line.strip()
    if line:
        try:
            events.append(json.loads(line))
        except:
            pass
types = {}
for e in events:
    t = e.get('event_type', e.get('type', '?'))
    types[t] = types.get(t, 0) + 1
for t, c in sorted(types.items()):
    print(f'  {t}: {c}')
" 2>/dev/null || echo "  (python3 parse failed)"
    fi
else
    echo "Telemetry: NOT FOUND (telemetry.jsonl missing)"
fi

echo ""
echo "=== Summary ==="
if [ "$EXIT_CODE" -eq 0 ]; then
    echo "STATUS: PASS"
elif [ "$EXIT_CODE" -eq 2 ]; then
    echo "STATUS: QUALITY GATE (some governance findings)"
elif [ "$EXIT_CODE" -eq 3 ]; then
    echo "STATUS: GOVERNANCE BLOCK (hard violation — expected for BSD tests)"
else
    echo "STATUS: ERROR (exit code $EXIT_CODE)"
fi

echo "Done."
exit 0
