#!/bin/bash
# NAAb Governance Demo — 7 scenes showing AI code governance in action
#
# Usage:
#   bash demo/governance_demo.sh              # Run live
#   bash demo/governance_demo.sh --fast       # Skip pauses (CI mode)
#   bash demo/governance_demo.sh --record     # Record with asciinema
#
# Recording output:
#   demo/demo.cast         — asciinema v2 format (upload to asciinema.org)
#
# To upload to asciinema.org (embeddable player for website/GitHub):
#   asciinema upload demo/demo.cast
#   → gives you a URL like https://asciinema.org/a/123456
#
# To embed in GitHub README (after uploading):
#   [![demo](https://asciinema.org/a/YOUR_ID.svg)](https://asciinema.org/a/YOUR_ID)
#
# To generate animated SVG (no upload needed, works offline in browsers):
#   bash demo/governance_demo.sh --svg
#   → produces demo/demo.svg (embed directly in HTML/README)

set -e

# --- Config ---
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${SCRIPT_DIR}/../build/naab-lang"
DEMO_DIR="$SCRIPT_DIR"
FAST=false
RECORD=false
SVG=false

for arg in "$@"; do
    case "$arg" in
        --fast)   FAST=true ;;
        --record) RECORD=true ;;
        --svg)    SVG=true ;;
    esac
done

# --- Handle SVG recording mode (termtosvg) ---
if $SVG; then
    SVG_FILE="${DEMO_DIR}/demo.svg"
    if ! command -v termtosvg &>/dev/null; then
        echo "Error: termtosvg not installed. Run: pip install termtosvg"
        exit 1
    fi
    echo "Recording animated SVG to ${SVG_FILE} ..."
    exec termtosvg "$SVG_FILE" \
        --screen-geometry 90x35 \
        -c "bash $0"
    # exec replaces this process — never reaches here
fi

# --- Handle asciinema recording mode ---
if $RECORD; then
    CAST_FILE="${DEMO_DIR}/demo.cast"
    if ! command -v asciinema &>/dev/null; then
        echo "Error: asciinema not installed. Run: pip install asciinema"
        exit 1
    fi
    echo "Recording to ${CAST_FILE} ..."
    echo "Press Ctrl+D or let the demo finish to stop recording."
    echo ""
    exec asciinema rec "$CAST_FILE" \
        --title "NAAb Governance Demo" \
        --cols 90 --rows 35 \
        --overwrite \
        -c "bash $0"
    # exec replaces this process — never reaches here
fi

# --- Colors ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# --- Helpers ---
pause() { $FAST || sleep "$1"; }

header() {
    local num="$1"; shift
    echo ""
    echo -e "${CYAN}${BOLD}══════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}${BOLD}  Scene ${num}: $*${NC}"
    echo -e "${CYAN}${BOLD}══════════════════════════════════════════════════════${NC}"
    echo ""
    pause 1
}

show_code() {
    echo -e "${DIM}─── $1 ───${NC}"
    echo -e "${YELLOW}"
    cat "$1"
    echo -e "${NC}"
    pause 1
}

run_expect_fail() {
    echo -e "${DIM}$ naab-lang $1${NC}"
    echo ""
    if "$NAAB" "$1" 2>&1; then
        echo -e "${RED}${BOLD}  UNEXPECTED: No governance error!${NC}"
        exit 1
    fi
    echo ""
    echo -e "${RED}${BOLD}  ✗ Blocked by governance${NC}"
    pause 2
}

run_expect_pass() {
    echo -e "${DIM}$ naab-lang $1${NC}"
    echo ""
    if ! env USER_QUERY="hello world" API_KEY="from-env" "$NAAB" "$1" 2>&1; then
        echo -e "${RED}${BOLD}  UNEXPECTED: Governance error on fixed code!${NC}"
        exit 1
    fi
    echo ""
    echo -e "${GREEN}${BOLD}  ✓ All governance checks passed${NC}"
    pause 2
}

# --- Verify naab-lang exists ---
if [[ ! -x "$NAAB" ]]; then
    echo -e "${RED}Error: naab-lang not found at $NAAB${NC}"
    echo "Build first: cd build && cmake .. && make naab-lang -j4"
    exit 1
fi

# --- Title ---
echo ""
echo -e "${BOLD}${CYAN}┌──────────────────────────────────────────────────────────┐${NC}"
echo -e "${BOLD}${CYAN}│                                                          │${NC}"
echo -e "${BOLD}${CYAN}│   NAAb Governance Demo                                   │${NC}"
echo -e "${BOLD}${CYAN}│   Catching AI-generated code mistakes at runtime          │${NC}"
echo -e "${BOLD}${CYAN}│                                                          │${NC}"
echo -e "${BOLD}${CYAN}│   6 mistakes. 1 govern.json. 0 bad code gets through.    │${NC}"
echo -e "${BOLD}${CYAN}│                                                          │${NC}"
echo -e "${BOLD}${CYAN}└──────────────────────────────────────────────────────────┘${NC}"
echo ""
pause 3

# --- Scene 1: Hallucinated API ---
header 1 "AI uses JavaScript .push() in Python"
echo -e "  ${DIM}AI models frequently confuse APIs across languages.${NC}"
echo -e "  ${DIM}NAAb knows 86+ cross-language confusion patterns.${NC}"
echo ""
show_code "$DEMO_DIR/scene1_hallucination.naab"
run_expect_fail "$DEMO_DIR/scene1_hallucination.naab"

# --- Scene 2: Unknown Import ---
header 2 "AI hallucinates a Python package"
echo -e "  ${DIM}AI invents packages that don't exist.${NC}"
echo -e "  ${DIM}NAAb validates imports against 180+ known modules.${NC}"
echo ""
show_code "$DEMO_DIR/scene2_bad_import.naab"
run_expect_fail "$DEMO_DIR/scene2_bad_import.naab"

# --- Scene 3: Stub Function ---
header 3 "AI ships a stub as 'complete'"
echo -e "  ${DIM}AI writes validate_user_input() that always returns true.${NC}"
echo -e "  ${DIM}NAAb detects 80+ oversimplification patterns.${NC}"
echo ""
show_code "$DEMO_DIR/scene3_stub.naab"
run_expect_fail "$DEMO_DIR/scene3_stub.naab"

# --- Scene 4: Hardcoded Secret ---
header 4 "AI hardcodes an API key"
echo -e "  ${DIM}AI puts secrets directly in source code.${NC}"
echo -e "  ${DIM}NAAb detects 18 secret patterns with entropy analysis.${NC}"
echo ""
show_code "$DEMO_DIR/scene4_secret.naab"
run_expect_fail "$DEMO_DIR/scene4_secret.naab"

# --- Scene 5: Incomplete Logic ---
header 5 "AI swallows errors with empty catch"
echo -e "  ${DIM}AI generates try/catch that silently ignores failures.${NC}"
echo -e "  ${DIM}NAAb detects empty error handlers and dummy conditionals.${NC}"
echo ""
show_code "$DEMO_DIR/scene5_incomplete.naab"
run_expect_fail "$DEMO_DIR/scene5_incomplete.naab"

# --- Scene 6: Taint Tracking ---
header 6 "AI passes user input to shell"
echo -e "  ${DIM}AI reads from environment and passes directly to shell.${NC}"
echo -e "  ${DIM}NAAb tracks data flow: source -> variable -> sink.${NC}"
echo ""
show_code "$DEMO_DIR/scene6_taint.naab"
run_expect_fail "$DEMO_DIR/scene6_taint.naab"

# --- Scene 7: Fixed Version ---
header 7 "All issues fixed — governance passes"
echo -e "  ${DIM}Same logic, correct code. Governance is silent when code is right.${NC}"
echo ""
show_code "$DEMO_DIR/scene7_fixed.naab"
run_expect_pass "$DEMO_DIR/scene7_fixed.naab"

# --- Summary ---
echo ""
echo -e "${CYAN}${BOLD}══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}${BOLD}  Summary${NC}"
echo -e "${CYAN}${BOLD}══════════════════════════════════════════════════════${NC}"
echo ""
echo -e "  ${GREEN}✓${NC} Cross-language API hallucination    ${RED}→ caught${NC}"
echo -e "  ${GREEN}✓${NC} Fake Python package import          ${RED}→ caught${NC}"
echo -e "  ${GREEN}✓${NC} Stub function (return true)         ${RED}→ caught${NC}"
echo -e "  ${GREEN}✓${NC} Hardcoded API key                   ${RED}→ caught${NC}"
echo -e "  ${GREEN}✓${NC} Empty error handler                 ${RED}→ caught${NC}"
echo -e "  ${GREEN}✓${NC} Unsanitized data in shell           ${RED}→ caught${NC}"
echo -e "  ${GREEN}✓${NC} Fixed version                       ${GREEN}→ passed${NC}"
echo ""
echo -e "  ${BOLD}One govern.json. Zero bad code gets through.${NC}"
echo ""
echo -e "  ${DIM}Learn more: https://github.com/b-macker/NAAb${NC}"
echo ""
