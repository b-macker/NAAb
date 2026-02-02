#!/bin/bash
# Test formatter idempotence: format(format(x)) == format(x)

set -e

NAAB_FMT="../../build/naab-lang fmt"
TEST_DIR="."

echo "=== Formatter Idempotence Test ==="
echo ""

# Test each .naab file
for file in ${TEST_DIR}/*.naab; do
    if [ -f "$file" ]; then
        echo "Testing: $file"

        # Create temporary copies
        cp "$file" "$file.tmp1"
        cp "$file" "$file.tmp2"

        # Format once
        ${NAAB_FMT} "$file.tmp1" 2>&1 || {
            echo "  ❌ First format failed"
            rm -f "$file.tmp1" "$file.tmp2"
            continue
        }

        # Format twice
        ${NAAB_FMT} "$file.tmp1" 2>&1 || {
            echo "  ❌ Second format failed"
            rm -f "$file.tmp1" "$file.tmp2"
            continue
        }

        # Compare first and second format
        if diff -q "$file.tmp1" "$file.tmp2" > /dev/null 2>&1; then
            echo "  ✅ Idempotent"
        else
            echo "  ❌ NOT idempotent (output changed on second format)"
            diff "$file.tmp1" "$file.tmp2" || true
        fi

        # Cleanup
        rm -f "$file.tmp1" "$file.tmp2"
    fi
done

echo ""
echo "=== Test Complete ==="
