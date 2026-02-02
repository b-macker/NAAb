#!/bin/bash
# NAAb Benchmarking Suite Runner
# Runs all benchmarks and saves results

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
NAAB_BIN="../build/naab-lang"
RESULTS_DIR="results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_FILE="$RESULTS_DIR/benchmark_$TIMESTAMP.txt"

# Create results directory
mkdir -p "$RESULTS_DIR"

echo -e "${BLUE}╔═══════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║         NAAb Performance Benchmark Suite          ║${NC}"
echo -e "${BLUE}╚═══════════════════════════════════════════════════╝${NC}"
echo ""
echo "Date: $(date)"
echo "Git Commit: $(git rev-parse --short HEAD 2>/dev/null || echo 'N/A')"
echo "Results will be saved to: $RESULTS_FILE"
echo ""

# Start results file
{
    echo "NAAb Benchmark Results"
    echo "======================"
    echo "Date: $(date)"
    echo "Git Commit: $(git rev-parse HEAD 2>/dev/null || echo 'N/A')"
    echo ""
} > "$RESULTS_FILE"

# Function to run a benchmark
run_benchmark() {
    local category=$1
    local name=$2
    local file=$3

    echo -e "${YELLOW}▶ Running: $category/$name${NC}"

    # Run benchmark and capture output
    if $NAAB_BIN run "$file" >> "$RESULTS_FILE" 2>&1; then
        echo -e "${GREEN}  ✓ Completed${NC}"
    else
        echo -e "${RED}  ✗ Failed${NC}"
        echo "ERROR: Benchmark $name failed" >> "$RESULTS_FILE"
    fi

    echo "" >> "$RESULTS_FILE"
}

# Run micro-benchmarks
echo -e "${BLUE}━━━ MICRO-BENCHMARKS ━━━${NC}"
{
    echo "═══════════════════════════════════════"
    echo "MICRO-BENCHMARKS"
    echo "═══════════════════════════════════════"
    echo ""
} >> "$RESULTS_FILE"

for bench in micro/*.naab; do
    if [ -f "$bench" ]; then
        name=$(basename "$bench" .naab)
        run_benchmark "micro" "$name" "$bench"
    fi
done

# Run macro-benchmarks
echo ""
echo -e "${BLUE}━━━ MACRO-BENCHMARKS ━━━${NC}"
{
    echo "═══════════════════════════════════════"
    echo "MACRO-BENCHMARKS"
    echo "═══════════════════════════════════════"
    echo ""
} >> "$RESULTS_FILE"

for bench in macro/*.naab; do
    if [ -f "$bench" ]; then
        name=$(basename "$bench" .naab)
        run_benchmark "macro" "$name" "$bench"
    fi
done

# Summary
echo ""
echo -e "${BLUE}╔═══════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║              Benchmark Run Complete               ║${NC}"
echo -e "${BLUE}╚═══════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${GREEN}Results saved to: $RESULTS_FILE${NC}"
echo ""
echo "To view results:"
echo "  cat $RESULTS_FILE"
echo ""
echo "To compare with baseline:"
echo "  diff $RESULTS_DIR/baseline.txt $RESULTS_FILE"
echo ""

# Create baseline if it doesn't exist
if [ ! -f "$RESULTS_DIR/baseline.txt" ]; then
    echo -e "${YELLOW}Creating baseline from this run...${NC}"
    cp "$RESULTS_FILE" "$RESULTS_DIR/baseline.txt"
    echo -e "${GREEN}Baseline created: $RESULTS_DIR/baseline.txt${NC}"
fi

echo -e "${GREEN}✓ Benchmarking complete!${NC}"
