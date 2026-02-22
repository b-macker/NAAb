#!/bin/bash
# Performance test for NAAb REPL
# Compares original O(n) vs optimized O(1) implementation

echo "========================================"
echo "NAAb REPL Performance Test"
echo "========================================"
echo ""

# Test with different statement counts
for count in 10 50 100 200; do
    echo "Testing with $count statements..."
    echo ""

    # Generate test input
    TEST_FILE="$(mktemp "/tmp/repl_test_${count}_XXXXXX.txt")"
    > "$TEST_FILE"
    for i in $(seq 1 $count); do
        echo "let x$i = $i" >> "$TEST_FILE"
    done
    echo ":stats" >> "$TEST_FILE"
    echo "exit" >> "$TEST_FILE"

    # Test original REPL
    echo "  Original REPL (O(n) re-execution):"
    START=$(date +%s%N)
    ./naab-repl < "$TEST_FILE" 2>&1 | grep -A 5 "Performance Statistics" | tail -4
    END=$(date +%s%N)
    ORIGINAL_TIME=$(( (END - START) / 1000000 ))
    echo "  Total time: ${ORIGINAL_TIME}ms"
    echo ""

    # Test optimized REPL
    echo "  Optimized REPL (O(1) incremental):"
    START=$(date +%s%N)
    ./naab-repl-opt < "$TEST_FILE" 2>&1 | grep -A 5 "Performance Statistics" | tail -4
    END=$(date +%s%N)
    OPTIMIZED_TIME=$(( (END - START) / 1000000 ))
    echo "  Total time: ${OPTIMIZED_TIME}ms"
    echo ""

    # Calculate speedup
    if [ $OPTIMIZED_TIME -gt 0 ]; then
        SPEEDUP=$(awk "BEGIN {printf \"%.2f\", $ORIGINAL_TIME / $OPTIMIZED_TIME}")
        echo "  Speedup: ${SPEEDUP}x faster"
    fi

    echo "----------------------------------------"
    echo ""

    # Cleanup
    rm "$TEST_FILE"
done

echo "Test complete!"
