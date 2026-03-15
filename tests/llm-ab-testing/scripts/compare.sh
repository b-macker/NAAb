#!/bin/bash
# compare.sh — Compare A/B test results between no-governance and with-governance
#
# Usage: bash scripts/compare.sh <task-name>
# Example: bash scripts/compare.sh task1-simple-calculator

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AB_DIR="$(dirname "$SCRIPT_DIR")"

TASK="$1"
if [ -z "$TASK" ]; then
    echo "Usage: bash scripts/compare.sh <task-name>"
    exit 1
fi

A="$AB_DIR/results/$TASK/no-governance"
B="$AB_DIR/results/$TASK/with-governance"

echo "╔══════════════════════════════════════════════════════╗"
echo "║        A/B Test Comparison: $TASK"
echo "╠══════════════════════════════════════════════════════╣"
echo ""

# --- Code Size ---
echo "┌─── Code Size ───────────────────────────────────────┐"
if [ -f "$A/line-counts.json" ] && [ -f "$B/line-counts.json" ]; then
    A_TOTAL=$(jq .total_lines "$A/line-counts.json")
    B_TOTAL=$(jq .total_lines "$B/line-counts.json")
    A_CODE=$(jq .code_lines "$A/line-counts.json")
    B_CODE=$(jq .code_lines "$B/line-counts.json")
    printf "│  %-20s %8s %8s\n" "" "No Gov" "With Gov"
    printf "│  %-20s %8d %8d\n" "Total lines:" "$A_TOTAL" "$B_TOTAL"
    printf "│  %-20s %8d %8d\n" "Code lines:" "$A_CODE" "$B_CODE"
else
    echo "│  (line counts not available — run analyze.sh first)"
fi
echo "└────────────────────────────────────────────────────┘"
echo ""

# --- Quality Scanner ---
echo "┌─── Quality Scanner Issues ─────────────────────────┐"
if [ -f "$A/quality-report.json" ] && [ -f "$B/quality-report.json" ]; then
    A_HARD=$(jq '.summary.by_level.hard // 0' "$A/quality-report.json")
    A_SOFT=$(jq '.summary.by_level.soft // 0' "$A/quality-report.json")
    A_ADV=$(jq '.summary.by_level.advisory // 0' "$A/quality-report.json")
    A_TOT=$(jq '.summary.total_issues // 0' "$A/quality-report.json")

    B_HARD=$(jq '.summary.by_level.hard // 0' "$B/quality-report.json")
    B_SOFT=$(jq '.summary.by_level.soft // 0' "$B/quality-report.json")
    B_ADV=$(jq '.summary.by_level.advisory // 0' "$B/quality-report.json")
    B_TOT=$(jq '.summary.total_issues // 0' "$B/quality-report.json")

    printf "│  %-20s %8s %8s\n" "" "No Gov" "With Gov"
    printf "│  %-20s %8d %8d\n" "HARD violations:" "$A_HARD" "$B_HARD"
    printf "│  %-20s %8d %8d\n" "SOFT violations:" "$A_SOFT" "$B_SOFT"
    printf "│  %-20s %8d %8d\n" "ADVISORY:" "$A_ADV" "$B_ADV"
    printf "│  %-20s %8d %8d\n" "TOTAL:" "$A_TOT" "$B_TOT"

    if [ "$A_TOT" -gt 0 ] && [ "$B_TOT" -gt 0 ]; then
        IMPROVEMENT=$(( (A_TOT - B_TOT) * 100 / A_TOT ))
        printf "│  Improvement: %d%% fewer issues with governance\n" "$IMPROVEMENT"
    elif [ "$A_TOT" -gt 0 ] && [ "$B_TOT" -eq 0 ]; then
        echo "│  Improvement: 100% — zero issues with governance!"
    fi
else
    echo "│  (quality reports not available — run analyze.sh first)"
    if [ ! -f "$A/quality-report.json" ]; then echo "│  Missing: $A/quality-report.json"; fi
    if [ ! -f "$B/quality-report.json" ]; then echo "│  Missing: $B/quality-report.json"; fi
fi
echo "└────────────────────────────────────────────────────┘"
echo ""

# --- Governance Violations ---
echo "┌─── Governance Violations ──────────────────────────┐"
if [ -f "$A/governance-report.json" ] && [ -f "$B/governance-report.json" ]; then
    A_GOV_TOTAL=$(jq '[.results // [] | length] | add // 0' "$A/governance-report.json" 2>/dev/null || echo "0")
    A_GOV_FAIL=$(jq '[.results // [] | .[] | select(.passed == false)] | length' "$A/governance-report.json" 2>/dev/null || echo "0")
    A_GOV_PASS=$(jq '[.results // [] | .[] | select(.passed == true)] | length' "$A/governance-report.json" 2>/dev/null || echo "0")
    B_GOV_TOTAL=$(jq '[.results // [] | length] | add // 0' "$B/governance-report.json" 2>/dev/null || echo "0")
    B_GOV_FAIL=$(jq '[.results // [] | .[] | select(.passed == false)] | length' "$B/governance-report.json" 2>/dev/null || echo "0")
    B_GOV_PASS=$(jq '[.results // [] | .[] | select(.passed == true)] | length' "$B/governance-report.json" 2>/dev/null || echo "0")
    printf "│  %-20s %8s %8s\n" "" "No Gov" "With Gov"
    printf "│  %-20s %8s %8s\n" "Checks run:" "$A_GOV_TOTAL" "$B_GOV_TOTAL"
    printf "│  %-20s %8s %8s\n" "Passed:" "$A_GOV_PASS" "$B_GOV_PASS"
    printf "│  %-20s %8s %8s\n" "Failed:" "$A_GOV_FAIL" "$B_GOV_FAIL"
else
    echo "│  (governance reports not available)"
fi
echo "└────────────────────────────────────────────────────┘"
echo ""

# --- Execution Results ---
echo "┌─── Execution Results ────────────────────────────────┐"
if [ -f "$A/run-output.txt" ] && [ -f "$B/run-output.txt" ]; then
    A_PASS=$(grep -cE "PASS|passed" "$A/run-output.txt" 2>/dev/null || echo "0")
    A_FAIL=$(grep -cE "FAIL|failed" "$A/run-output.txt" 2>/dev/null || echo "0")
    B_PASS=$(grep -cE "PASS|passed" "$B/run-output.txt" 2>/dev/null || echo "0")
    B_FAIL=$(grep -cE "FAIL|failed" "$B/run-output.txt" 2>/dev/null || echo "0")
    # Trim whitespace
    A_PASS=$(echo "$A_PASS" | tr -d '[:space:]')
    A_FAIL=$(echo "$A_FAIL" | tr -d '[:space:]')
    B_PASS=$(echo "$B_PASS" | tr -d '[:space:]')
    B_FAIL=$(echo "$B_FAIL" | tr -d '[:space:]')

    printf "│  %-20s %8s %8s\n" "" "No Gov" "With Gov"
    printf "│  %-20s %8d %8d\n" "PASS lines:" "$A_PASS" "$B_PASS"
    printf "│  %-20s %8d %8d\n" "FAIL/Error lines:" "$A_FAIL" "$B_FAIL"
else
    echo "│  (run outputs not available)"
fi
echo "└────────────────────────────────────────────────────┘"
echo ""

# --- Generate JSON comparison ---
COMP_FILE="$AB_DIR/results/$TASK/comparison.json"
cat > "$COMP_FILE" << JSONEOF
{
  "task": "$TASK",
  "no_governance": {
    "total_lines": $(jq '.total_lines // 0' "$A/line-counts.json" 2>/dev/null || echo 0),
    "code_lines": $(jq '.code_lines // 0' "$A/line-counts.json" 2>/dev/null || echo 0),
    "quality_issues": $(jq '.summary.total_issues // 0' "$A/quality-report.json" 2>/dev/null || echo 0),
    "hard_violations": $(jq '.summary.by_level.hard // 0' "$A/quality-report.json" 2>/dev/null || echo 0),
    "soft_violations": $(jq '.summary.by_level.soft // 0' "$A/quality-report.json" 2>/dev/null || echo 0),
    "governance_checks": $(jq '[.results // [] | length] | add // 0' "$A/governance-report.json" 2>/dev/null || echo 0),
    "governance_failures": $(jq '[.results // [] | .[] | select(.passed == false)] | length' "$A/governance-report.json" 2>/dev/null || echo 0)
  },
  "with_governance": {
    "total_lines": $(jq '.total_lines // 0' "$B/line-counts.json" 2>/dev/null || echo 0),
    "code_lines": $(jq '.code_lines // 0' "$B/line-counts.json" 2>/dev/null || echo 0),
    "quality_issues": $(jq '.summary.total_issues // 0' "$B/quality-report.json" 2>/dev/null || echo 0),
    "hard_violations": $(jq '.summary.by_level.hard // 0' "$B/quality-report.json" 2>/dev/null || echo 0),
    "soft_violations": $(jq '.summary.by_level.soft // 0' "$B/quality-report.json" 2>/dev/null || echo 0),
    "governance_checks": $(jq '[.results // [] | length] | add // 0' "$B/governance-report.json" 2>/dev/null || echo 0),
    "governance_failures": $(jq '[.results // [] | .[] | select(.passed == false)] | length' "$B/governance-report.json" 2>/dev/null || echo 0)
  }
}
JSONEOF

echo "Comparison saved to: $COMP_FILE"
