#!/bin/bash
# analyze.sh — Run NAAb scanner and governance checks on generated code
#
# Usage: bash scripts/analyze.sh <task-name>
# Example: bash scripts/analyze.sh task1-simple-calculator
#
# Expects generated.naab files in:
#   results/<task>/no-governance/generated.naab
#   results/<task>/with-governance/generated.naab

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AB_DIR="$(dirname "$SCRIPT_DIR")"
NAAB_LANG="${NAAB_LANG:-$HOME/.naab/language/build/naab-lang}"

TASK="$1"
if [ -z "$TASK" ]; then
    echo "Usage: bash scripts/analyze.sh <task-name>"
    echo "Example: bash scripts/analyze.sh task1-simple-calculator"
    exit 1
fi

RESULT_DIR="$AB_DIR/results/$TASK"

for config in no-governance with-governance; do
    CODE="$RESULT_DIR/$config/generated.naab"
    OUT_DIR="$RESULT_DIR/$config"

    if [ ! -f "$CODE" ]; then
        echo "SKIP: $CODE not found"
        continue
    fi

    echo "=== Analyzing: $config ==="

    # Copy the appropriate govern.json next to the generated code
    cp "$AB_DIR/configs/$config/govern.json" "$OUT_DIR/govern.json"

    # All commands run from OUT_DIR so scanner saves reports there
    cd "$OUT_DIR"

    # Run the code (captures scanner output + governance report)
    echo "--- Running code ---"
    "$NAAB_LANG" generated.naab > run-output.txt 2>&1 || true

    # Run standalone scan (from OUT_DIR so reports land here)
    echo "--- Running scanner ---"
    "$NAAB_LANG" --scan generated.naab naab > scan-output.txt 2>&1 || true

    # Run with governance report flag
    echo "--- Running governance report ---"
    "$NAAB_LANG" generated.naab --governance-report governance-report.json > governance-output.txt 2>&1 || true

    cd "$AB_DIR"

    # Count lines of code (excluding blank lines and comments)
    TOTAL_LINES=$(wc -l < "$CODE")
    CODE_LINES=$(grep -v '^\s*$' "$CODE" | grep -v '^\s*//' | wc -l)
    echo "  Total lines: $TOTAL_LINES"
    echo "  Code lines: $CODE_LINES"

    # Save line counts
    echo "{\"total_lines\": $TOTAL_LINES, \"code_lines\": $CODE_LINES}" > "$OUT_DIR/line-counts.json"

    echo "  Done: $config"
    echo ""
done

echo "Analysis complete for $TASK"
echo "Results in: $RESULT_DIR"
