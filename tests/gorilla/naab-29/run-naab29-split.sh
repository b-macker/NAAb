#!/usr/bin/env bash
# NAAb-29 Split Runner — runs each category in its own process to avoid OOM on Termux
# Usage: bash run-naab29-split.sh [start_cat]   (default: 1)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
START=${1:-1}

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

CAT_NAMES=(
    "" # 0 placeholder
    "EXECUTION"
    "ADMISSIBILITY"
    "AUTHORITY"
    "CONTINUITY"
    "INTERRUPTION"
    "LEGITIMACY"
    "RUNTIME"
    "REVALIDATION"
    "DEPENDENCY"
    "SURVIVABILITY"
    "EVOLVING REALITY"
)

TOTAL_PASS=0
TOTAL_FAIL=0
CAT_RESULTS=""

for cat in $(seq $START 11); do
    echo -e "\n${BOLD}${CYAN}━━━ Running Cat $cat: ${CAT_NAMES[$cat]} ━━━${NC}"
    bash "$SCRIPT_DIR/run-naab29.sh" --cat "$cat" 2>&1
    rc=$?
    if [ $rc -eq 0 ]; then
        CAT_RESULTS="${CAT_RESULTS}\n  ${GREEN}Cat $cat${NC}: ${CAT_NAMES[$cat]} — ${GREEN}PASS${NC}"
    else
        CAT_RESULTS="${CAT_RESULTS}\n  ${RED}Cat $cat${NC}: ${CAT_NAMES[$cat]} — ${RED}HAS FAILURES${NC}"
    fi
done

echo -e "\n${BOLD}${CYAN}══════════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  CATEGORY SUMMARY${NC}"
echo -e "${CAT_RESULTS}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════${NC}"
echo -e "\n  Full results in: $SCRIPT_DIR/results/"
