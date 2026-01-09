#!/bin/bash
# Test Block Enrichment - Process 50 blocks
# Quick test to validate enrichment approach

set -e

BLOCKS_DIR="/storage/emulated/0/Download/.naab/naab/blocks/library"
BUILD_DIR="/data/data/com.termux/files/home/naab-build"
ENRICH_TOOL="$BUILD_DIR/enrich_tool"
TEST_DIR="/data/data/com.termux/files/home/enrich_test_50"
LIMIT=50

# Statistics
TOTAL=0
SUCCESS=0
FAILED=0

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "==================================================================="
echo "  NAAb Block Enrichment - Test on 50 Blocks"
echo "==================================================================="
echo ""

# Create test directory
mkdir -p "$TEST_DIR"
cd "$TEST_DIR"

# Check enrich_tool exists
if [ ! -f "$ENRICH_TOOL" ]; then
    echo -e "${RED}ERROR: enrich_tool not found at $ENRICH_TOOL${NC}"
    exit 1
fi

echo "Collecting 50 sample blocks..."
echo ""

# Collect sample blocks from different languages
find "$BLOCKS_DIR" -name "*.json" -type f | head -$LIMIT > block_list.txt

echo "Processing $(wc -l < block_list.txt) blocks..."
echo ""

while IFS= read -r block_file; do
    TOTAL=$((TOTAL + 1))
    block_id=$(basename "$block_file" .json)

    # Copy to test directory
    cp "$block_file" "$TEST_DIR/"

    echo -n "[$TOTAL] $block_id ... "

    # Run enrichment
    if "$ENRICH_TOOL" "$TEST_DIR/$(basename "$block_file")" > /dev/null 2>&1; then
        echo -e "${GREEN}✓${NC}"
        SUCCESS=$((SUCCESS + 1))
    else
        echo -e "${RED}✗${NC}"
        FAILED=$((FAILED + 1))
        echo "FAILED: $block_file" >> failures.txt
    fi
done < block_list.txt

# Calculate rates
SUCCESS_RATE=$((SUCCESS * 100 / TOTAL))
FAILED_RATE=$((FAILED * 100 / TOTAL))

echo ""
echo "==================================================================="
echo "  Test Results"
echo "==================================================================="
echo ""
echo "Total Processed:  $TOTAL"
echo -e "${GREEN}Successful:       $SUCCESS ($SUCCESS_RATE%)${NC}"
echo -e "${RED}Failed:           $FAILED ($FAILED_RATE%)${NC}"
echo ""

# Estimate full run
ESTIMATED_SUCCESS=$((SUCCESS * 24172 / TOTAL))
echo "Estimated for full 24,172 blocks: ~$ESTIMATED_SUCCESS successful"

if [ $ESTIMATED_SUCCESS -ge 16000 ]; then
    echo -e "${GREEN}✓ Projected to meet target (16,000+)${NC}"
else
    echo -e "${YELLOW}⚠ Projected below target (need 16,000+)${NC}"
fi

echo ""
echo "Test directory: $TEST_DIR"
if [ -f failures.txt ]; then
    echo "Failed blocks:  $TEST_DIR/failures.txt"
fi
echo ""
