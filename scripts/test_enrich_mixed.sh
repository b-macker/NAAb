#!/bin/bash
# Test Block Enrichment - Mixed Languages
# Test 50 blocks from C++, Python, and JavaScript

set -e

BLOCKS_DIR="/storage/emulated/0/Download/.naab/naab/blocks/library"
BUILD_DIR="/data/data/com.termux/files/home/naab-build"
ENRICH_TOOL="$BUILD_DIR/enrich_tool"
TEST_DIR="/data/data/com.termux/files/home/enrich_test_mixed"

# Statistics
TOTAL=0
SUCCESS=0
FAILED=0
CPP_SUCCESS=0
CPP_TOTAL=0
PY_SUCCESS=0
PY_TOTAL=0
JS_SUCCESS=0
JS_TOTAL=0

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "==================================================================="
echo "  NAAb Block Enrichment - Mixed Language Test"
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

echo "Collecting sample blocks..."
echo ""

# Get 30 C++ blocks, 15 Python blocks, 5 JavaScript blocks
> block_list.txt
find "$BLOCKS_DIR/c++" -name "*.json" -type f | head -30 >> block_list.txt || true
find "$BLOCKS_DIR/python" -name "*.json" -type f | head -15 >> block_list.txt || true
find "$BLOCKS_DIR/javascript" -name "*.json" -type f | head -5 >> block_list.txt || true

echo "Processing $(wc -l < block_list.txt) blocks..."
echo ""

while IFS= read -r block_file; do
    TOTAL=$((TOTAL + 1))
    block_id=$(basename "$block_file" .json)

    # Determine language
    lang=""
    if [[ "$block_file" == *"/c++/"* ]]; then
        lang="C++"
        CPP_TOTAL=$((CPP_TOTAL + 1))
    elif [[ "$block_file" == *"/python/"* ]]; then
        lang="PY "
        PY_TOTAL=$((PY_TOTAL + 1))
    elif [[ "$block_file" == *"/javascript/"* ]]; then
        lang="JS "
        JS_TOTAL=$((JS_TOTAL + 1))
    fi

    # Copy to test directory
    cp "$block_file" "$TEST_DIR/"

    echo -n "[$TOTAL] [$lang] $block_id ... "

    # Run enrichment
    if "$ENRICH_TOOL" "$TEST_DIR/$(basename "$block_file")" > /dev/null 2>&1; then
        echo -e "${GREEN}✓${NC}"
        SUCCESS=$((SUCCESS + 1))

        if [[ "$block_file" == *"/c++/"* ]]; then
            CPP_SUCCESS=$((CPP_SUCCESS + 1))
        elif [[ "$block_file" == *"/python/"* ]]; then
            PY_SUCCESS=$((PY_SUCCESS + 1))
        elif [[ "$block_file" == *"/javascript/"* ]]; then
            JS_SUCCESS=$((JS_SUCCESS + 1))
        fi
    else
        echo -e "${RED}✗${NC}"
        FAILED=$((FAILED + 1))
        echo "FAILED: $block_file" >> failures.txt
    fi
done < block_list.txt

# Calculate rates
SUCCESS_RATE=$((SUCCESS * 100 / TOTAL))
FAILED_RATE=$((FAILED * 100 / TOTAL))

if [ $CPP_TOTAL -gt 0 ]; then
    CPP_RATE=$((CPP_SUCCESS * 100 / CPP_TOTAL))
else
    CPP_RATE=0
fi

if [ $PY_TOTAL -gt 0 ]; then
    PY_RATE=$((PY_SUCCESS * 100 / PY_TOTAL))
else
    PY_RATE=0
fi

if [ $JS_TOTAL -gt 0 ]; then
    JS_RATE=$((JS_SUCCESS * 100 / JS_TOTAL))
else
    JS_RATE=0
fi

echo ""
echo "==================================================================="
echo "  Test Results - By Language"
echo "==================================================================="
echo ""
echo -e "C++ Blocks:        $CPP_SUCCESS / $CPP_TOTAL ($CPP_RATE%)"
echo -e "Python Blocks:     $PY_SUCCESS / $PY_TOTAL ($PY_RATE%)"
echo -e "JavaScript Blocks: $JS_SUCCESS / $JS_TOTAL ($JS_RATE%)"
echo ""
echo "==================================================================="
echo "  Overall Results"
echo "==================================================================="
echo ""
echo "Total Processed:  $TOTAL"
echo -e "${GREEN}Successful:       $SUCCESS ($SUCCESS_RATE%)${NC}"
echo -e "${RED}Failed:           $FAILED ($FAILED_RATE%)${NC}"
echo ""

# Estimate for full dataset
# C++: 23,906 blocks
# Python: 237 blocks
# Others: ~29 blocks
ESTIMATED_CPP=$((CPP_SUCCESS * 23906 / CPP_TOTAL))
ESTIMATED_PY=$((PY_SUCCESS * 237 / PY_TOTAL))
ESTIMATED_TOTAL=$((ESTIMATED_CPP + ESTIMATED_PY + 29))

echo "Estimated for full 24,172 blocks:"
echo "  C++:     ~$ESTIMATED_CPP / 23,906"
echo "  Python:  ~$ESTIMATED_PY / 237"
echo "  Total:   ~$ESTIMATED_TOTAL / 24,172"
echo ""

if [ $ESTIMATED_TOTAL -ge 16000 ]; then
    echo -e "${GREEN}✓ Projected to meet target (16,000+)${NC}"
else
    echo -e "${YELLOW}⚠ Projected below target (need 16,000+)${NC}"
    echo "  Gap: $((16000 - ESTIMATED_TOTAL)) blocks"
fi

echo ""
echo "Test directory: $TEST_DIR"
if [ -f failures.txt ]; then
    echo "Failed blocks:  $TEST_DIR/failures.txt"
fi
echo ""
