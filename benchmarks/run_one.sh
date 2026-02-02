#!/bin/bash
# Run a single benchmark

if [ $# -eq 0 ]; then
    echo "Usage: ./run_one.sh <benchmark.naab>"
    exit 1
fi

BENCHMARK=$1
NAAB_BIN="./build/naab-lang"

echo "Running benchmark: $BENCHMARK"
echo "===================="

$NAAB_BIN run "$BENCHMARK"

echo ""
echo "Done"
