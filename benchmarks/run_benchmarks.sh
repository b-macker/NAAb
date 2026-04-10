#!/bin/bash
# NAAb Performance Benchmark Suite
# Run: bash benchmarks/run_benchmarks.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${SCRIPT_DIR}/../build/naab-lang"

if [ ! -x "$NAAB" ]; then
    echo "Error: naab-lang not found at $NAAB"
    echo "Build first: cd build && cmake .. && make naab-lang -j4"
    exit 1
fi

echo "NAAb Performance Benchmarks"
echo "==========================="
echo "Binary: $NAAB"
echo "Date: $(date -Iseconds 2>/dev/null || date)"
echo ""

for bench in "$SCRIPT_DIR"/bench_*.naab; do
    name=$(basename "$bench" .naab)
    echo "--- $name ---"

    # VM mode (default)
    echo -n "  VM:         "
    "$NAAB" -q "$bench" 2>/dev/null

    # Tree-walker mode
    echo -n "  Tree-walk:  "
    "$NAAB" -q --tree-walk "$bench" 2>/dev/null

    echo ""
done
