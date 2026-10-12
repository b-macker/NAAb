#!/usr/bin/env bash
# ============================================================
# test_mcp_stdio.sh -- `naab-lang mcp`: governed execution over MCP (stdio)
#
# Design and the reasoning behind each arm: docs/plan-mcp-service.md.
# Every call runs in a fresh worker process; a per-caller ledger of
# behavioural-sequence events seeds each worker, so a sequence split across
# calls is still caught. The arms (MC-00..MC-19) are in mcp_stdio_arms.py;
# each refusal there is paired with a success through the same fixture.
#
# Linux only: the arms read /proc to find worker processes, and the server's
# worker spawning is POSIX in this slice. Elsewhere the suite reports
# UNMEASURABLE rather than passing.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  naab-lang mcp: governed execution over MCP (stdio)          |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if [ ! -x "$NAAB" ]; then
    fail "MC-pre" "naab-lang not built at $NAAB"
    exit 1
fi
if [ "$(uname -s)" != "Linux" ] || [ ! -d /proc/self ]; then
    skip "MC-*" "UNMEASURABLE: this slice is POSIX and the arms read /proc"
    exit 0
fi
command -v python3 >/dev/null 2>&1 || { skip "MC-*" "UNMEASURABLE: python3 not found"; exit 0; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/mcp-stdio-XXXXXX")"
[ -n "$TEST_TMP" ] && [ -d "$TEST_TMP" ] || { echo "FATAL: no work dir" >&2; exit 1; }
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT

# ── MC-00..MC-19 through the protocol ──────────────────────────────────────
ARMS_OUT="$TEST_TMP/arms.out"
timeout 600 python3 "$SCRIPT_DIR/mcp_stdio_arms.py" "$NAAB" "$TEST_TMP/arms" > "$ARMS_OUT" 2>&1
ARMS_RC=$?
while IFS= read -r line; do
    case "$line" in
        "PASS ["*) arm="${line#PASS [}"; arm="${arm%%]*}"; pass "$arm" "${line#*] }" ;;
        "FAIL ["*) arm="${line#FAIL [}"; arm="${arm%%]*}"; fail "$arm" "${line#*] }" ;;
        "SKIP ["*) arm="${line#SKIP [}"; arm="${arm%%]*}"; skip "$arm" "${line#*] }" ;;
        "       -> "*) echo -e "       ${RED}${line#       }${NC}" ;;
        "INFO "*) echo "  $line" ;;
        "ARMS:"*) : ;;
        *) [ -n "$line" ] && echo "    $line" ;;
    esac
done < "$ARMS_OUT"
if ! grep -q '^ARMS:' "$ARMS_OUT"; then
    fail "MC-run" "the arms script did not finish (rc=$ARMS_RC)" "$(tail -5 "$ARMS_OUT" | tr '\n' ' ')"
fi

echo ""
echo "  Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable"
[ "$FAIL_COUNT" -eq 0 ]
