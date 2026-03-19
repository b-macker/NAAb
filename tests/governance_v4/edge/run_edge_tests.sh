#!/bin/bash
# Edge Test Suite Runner
cd "$(dirname "$0")/../../.."
PASS=0
TOTAL=0
FAILED_FILES=""

for f in tests/governance_v4/edge/test_edge_*.naab; do
    result=$(./build/naab-lang --governance-override "$f" 2>&1 | grep "^Edge")
    if [ -z "$result" ]; then
        echo "ERROR: $(basename $f) — no summary line"
        FAILED_FILES="$FAILED_FILES $(basename $f)"
        continue
    fi
    echo "$result"
    p=$(echo "$result" | sed 's/.*: \([0-9]*\)\/.*/\1/')
    t=$(echo "$result" | sed 's/.*\/\([0-9]*\) .*/\1/')
    PASS=$((PASS + p))
    TOTAL=$((TOTAL + t))
    if [ "$p" != "$t" ]; then
        FAILED_FILES="$FAILED_FILES $(basename $f)"
    fi
done

echo ""
echo "=============================="
echo "EDGE SUITE: $PASS/$TOTAL passed"
if [ -n "$FAILED_FILES" ]; then
    echo "FAILURES:$FAILED_FILES"
fi
echo "=============================="
