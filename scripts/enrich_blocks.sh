#!/bin/bash
# Batch Block Enrichment Script
# Processes all blocks in the library and enriches them with C-ABI wrappers

set -e

# Configuration
BLOCKS_DIR="/storage/emulated/0/Download/.naab/naab/blocks/library"
BUILD_DIR="/data/data/com.termux/files/home/naab-build"
ENRICH_TOOL="$BUILD_DIR/enrich_tool"
REPORT_FILE="/storage/emulated/0/Download/.naab/naab_language/enrichment_report.txt"
LOG_FILE="/storage/emulated/0/Download/.naab/naab_language/enrichment_log.txt"

# Statistics
TOTAL_BLOCKS=0
SUCCESS_COUNT=0
FAILED_COUNT=0
SKIPPED_COUNT=0

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "==================================================================="
echo "  NAAb Block Enrichment - Batch Processing"
echo "==================================================================="
echo ""
echo "Blocks directory: $BLOCKS_DIR"
echo "Build directory:  $BUILD_DIR"
echo "Enrich tool:      $ENRICH_TOOL"
echo "Report file:      $REPORT_FILE"
echo ""

# Check if blocks directory exists
if [ ! -d "$BLOCKS_DIR" ]; then
    echo -e "${RED}ERROR: Blocks directory not found: $BLOCKS_DIR${NC}"
    exit 1
fi

# Build the enrich_tool if it doesn't exist
if [ ! -f "$ENRICH_TOOL" ]; then
    echo "Enrich tool not found. Building..."
    cd "$BUILD_DIR"
    cmake /storage/emulated/0/Download/.naab/naab_language
    make enrich_tool -j4

    if [ ! -f "$ENRICH_TOOL" ]; then
        echo -e "${RED}ERROR: Failed to build enrich_tool${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓ Enrich tool built successfully${NC}"
    echo ""
fi

# Initialize log files
echo "=== NAAb Block Enrichment Report ===" > "$REPORT_FILE"
echo "Started: $(date)" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"

echo "=== NAAb Block Enrichment Log ===" > "$LOG_FILE"
echo "Started: $(date)" >> "$LOG_FILE"
echo "" >> "$LOG_FILE"

# Function to enrich a single block
enrich_block() {
    local block_file="$1"
    local block_id=$(basename "$block_file" .json)

    echo -n "[$((TOTAL_BLOCKS + 1))] Processing $block_id... "

    # Run enrich_tool
    if "$ENRICH_TOOL" "$block_file" "$block_file" >> "$LOG_FILE" 2>&1; then
        echo -e "${GREEN}✓${NC}"
        SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
        echo "SUCCESS: $block_file" >> "$LOG_FILE"
    else
        echo -e "${RED}✗${NC}"
        FAILED_COUNT=$((FAILED_COUNT + 1))
        echo "FAILED: $block_file" >> "$LOG_FILE"
    fi

    TOTAL_BLOCKS=$((TOTAL_BLOCKS + 1))

    # Progress update every 100 blocks
    if [ $((TOTAL_BLOCKS % 100)) -eq 0 ]; then
        local success_rate=$((SUCCESS_COUNT * 100 / TOTAL_BLOCKS))
        echo ""
        echo "--- Progress: $TOTAL_BLOCKS blocks processed ---"
        echo "  Success: $SUCCESS_COUNT ($success_rate%)"
        echo "  Failed:  $FAILED_COUNT"
        echo ""
    fi
}

# Find and process all block JSON files
echo "Scanning for block JSON files..."
echo ""

# Process C++ blocks
if [ -d "$BLOCKS_DIR/c++" ]; then
    echo "Processing C++ blocks..."
    while IFS= read -r -d '' file; do
        enrich_block "$file"
    done < <(find "$BLOCKS_DIR/c++" -name "*.json" -type f -print0)
fi

# Process Python blocks
if [ -d "$BLOCKS_DIR/python" ]; then
    echo ""
    echo "Processing Python blocks..."
    while IFS= read -r -d '' file; do
        enrich_block "$file"
    done < <(find "$BLOCKS_DIR/python" -name "*.json" -type f -print0)
fi

# Process JavaScript blocks
if [ -d "$BLOCKS_DIR/javascript" ]; then
    echo ""
    echo "Processing JavaScript blocks..."
    while IFS= read -r -d '' file; do
        enrich_block "$file"
    done < <(find "$BLOCKS_DIR/javascript" -name "*.json" -type f -print0)
fi

# Process other language blocks
for lang_dir in "$BLOCKS_DIR"/*; do
    if [ -d "$lang_dir" ]; then
        lang=$(basename "$lang_dir")
        if [ "$lang" != "c++" ] && [ "$lang" != "python" ] && [ "$lang" != "javascript" ]; then
            echo ""
            echo "Processing $lang blocks..."
            while IFS= read -r -d '' file; do
                enrich_block "$file"
            done < <(find "$lang_dir" -name "*.json" -type f -print0)
        fi
    fi
done

# Calculate final statistics
SUCCESS_RATE=0
FAILED_RATE=0
if [ $TOTAL_BLOCKS -gt 0 ]; then
    SUCCESS_RATE=$((SUCCESS_COUNT * 100 / TOTAL_BLOCKS))
    FAILED_RATE=$((FAILED_COUNT * 100 / TOTAL_BLOCKS))
fi

# Generate final report
echo ""
echo "==================================================================="
echo "  Enrichment Complete"
echo "==================================================================="
echo ""
echo "Total Blocks Processed: $TOTAL_BLOCKS"
echo -e "${GREEN}Successful:             $SUCCESS_COUNT ($SUCCESS_RATE%)${NC}"
echo -e "${RED}Failed:                 $FAILED_COUNT ($FAILED_RATE%)${NC}"
echo -e "${YELLOW}Skipped:                $SKIPPED_COUNT${NC}"
echo ""
echo "Report saved to: $REPORT_FILE"
echo "Log saved to:    $LOG_FILE"
echo ""

# Write final report
{
    echo ""
    echo "=== Final Statistics ==="
    echo ""
    echo "Total Blocks Processed: $TOTAL_BLOCKS"
    echo "Successful:             $SUCCESS_COUNT ($SUCCESS_RATE%)"
    echo "Failed:                 $FAILED_COUNT ($FAILED_RATE%)"
    echo "Skipped:                $SKIPPED_COUNT"
    echo ""
    echo "Finished: $(date)"
    echo ""
    echo "=== Success Criteria ==="
    echo ""
    echo "Target: 16,000+ blocks validated (66% of 24,172)"
    if [ $SUCCESS_COUNT -ge 16000 ]; then
        echo "Status: ✓ TARGET MET"
    else
        echo "Status: ✗ BELOW TARGET (need $((16000 - SUCCESS_COUNT)) more)"
    fi
    echo ""
    echo "C++ Blocks Target: 60%+ success rate"
    echo "Python/JS Blocks Target: 99%+ success rate"
    echo ""
} >> "$REPORT_FILE"

# Exit with appropriate code
if [ $SUCCESS_COUNT -ge 16000 ]; then
    exit 0
else
    exit 1
fi
