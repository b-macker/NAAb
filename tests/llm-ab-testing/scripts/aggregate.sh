#!/bin/bash
# aggregate.sh — Generate aggregate report across all completed A/B tasks
#
# Usage: bash scripts/aggregate.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AB_DIR="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="$AB_DIR/results"

echo "╔══════════════════════════════════════════════════════╗"
echo "║     Aggregate A/B Test Report — LLM Governance      ║"
echo "╠══════════════════════════════════════════════════════╣"
echo ""

# Collect all comparison.json files
COMPARISONS=()
for dir in "$RESULTS_DIR"/*/; do
    comp="$dir/comparison.json"
    if [ -f "$comp" ]; then
        COMPARISONS+=("$comp")
    fi
done

if [ ${#COMPARISONS[@]} -eq 0 ]; then
    echo "No comparison files found. Run analyze.sh + compare.sh on tasks first."
    exit 1
fi

echo "Tasks analyzed: ${#COMPARISONS[@]}"
echo ""

# Per-task summary
echo "┌─── Per-Task Summary ─────────────────────────────────┐"
printf "│  %-25s %8s %8s %8s\n" "Task" "NoGov" "WithGov" "Diff"
echo "│  ─────────────────────────────────────────────────────"
for comp in "${COMPARISONS[@]}"; do
    TASK=$(jq -r .task "$comp")
    A_TOT=$(jq '.no_governance.quality_issues // 0' "$comp")
    B_TOT=$(jq '.with_governance.quality_issues // 0' "$comp")
    DIFF=$((A_TOT - B_TOT))
    printf "│  %-25s %8d %8d %+8d\n" "$TASK" "$A_TOT" "$B_TOT" "$DIFF"
done
echo "└────────────────────────────────────────────────────┘"
echo ""

# Aggregate averages
echo "┌─── Aggregate Averages ─────────────────────────────┐"

# Use jq to calculate averages across all comparison files
A_AVG_ISSUES=$(jq -s '[.[].no_governance.quality_issues] | add / length' "${COMPARISONS[@]}")
B_AVG_ISSUES=$(jq -s '[.[].with_governance.quality_issues] | add / length' "${COMPARISONS[@]}")
A_AVG_HARD=$(jq -s '[.[].no_governance.hard_violations] | add / length' "${COMPARISONS[@]}")
B_AVG_HARD=$(jq -s '[.[].with_governance.hard_violations] | add / length' "${COMPARISONS[@]}")
A_AVG_LINES=$(jq -s '[.[].no_governance.code_lines] | add / length' "${COMPARISONS[@]}")
B_AVG_LINES=$(jq -s '[.[].with_governance.code_lines] | add / length' "${COMPARISONS[@]}")

printf "│  %-20s %10s %10s\n" "" "No Gov" "With Gov"
printf "│  %-20s %10.1f %10.1f\n" "Avg quality issues:" "$A_AVG_ISSUES" "$B_AVG_ISSUES"
printf "│  %-20s %10.1f %10.1f\n" "Avg HARD violations:" "$A_AVG_HARD" "$B_AVG_HARD"
printf "│  %-20s %10.1f %10.1f\n" "Avg code lines:" "$A_AVG_LINES" "$B_AVG_LINES"

# Calculate improvement percentage
if [ "$(echo "$A_AVG_ISSUES > 0" | bc)" -eq 1 ]; then
    IMPROVEMENT=$(echo "scale=1; ($A_AVG_ISSUES - $B_AVG_ISSUES) * 100 / $A_AVG_ISSUES" | bc)
    printf "│  Quality improvement: %s%% fewer issues with governance\n" "$IMPROVEMENT"
fi

echo "└────────────────────────────────────────────────────┘"
echo ""

# Save aggregate JSON
AGG_FILE="$RESULTS_DIR/aggregate.json"
jq -s '{
    tasks_count: length,
    no_governance: {
        avg_quality_issues: ([.[].no_governance.quality_issues] | add / length),
        avg_hard_violations: ([.[].no_governance.hard_violations] | add / length),
        avg_soft_violations: ([.[].no_governance.soft_violations] | add / length),
        avg_code_lines: ([.[].no_governance.code_lines] | add / length),
        total_quality_issues: ([.[].no_governance.quality_issues] | add),
        total_hard_violations: ([.[].no_governance.hard_violations] | add)
    },
    with_governance: {
        avg_quality_issues: ([.[].with_governance.quality_issues] | add / length),
        avg_hard_violations: ([.[].with_governance.hard_violations] | add / length),
        avg_soft_violations: ([.[].with_governance.soft_violations] | add / length),
        avg_code_lines: ([.[].with_governance.code_lines] | add / length),
        total_quality_issues: ([.[].with_governance.quality_issues] | add),
        total_hard_violations: ([.[].with_governance.hard_violations] | add)
    },
    tasks: [.[]]
}' "${COMPARISONS[@]}" > "$AGG_FILE"

echo "Aggregate report saved to: $AGG_FILE"
