#!/usr/bin/env bash
# ============================================================
# test_level_key_silent_disable.sh — an unknown "level" disables the check
#
# parseEnforcementLevel maps five strings (hard, soft, advisory, detect,
# approval_required) to {enabled, level} and falls through to {false, HARD} for
# anything else. So "level": "off", "none", "warn" or a plain typo does not fail
# the load, does not clamp to a default, and does not warn — it turns the check
# OFF, silently.
#
# That is the same silence as the ignored "enabled" flag, and it sits in the
# very key the warning for that flag tells operators to reach for: "this check is
# enabled by the presence of its block" invites writing a level, and getting the
# level wrong then disables what they were trying to configure.
#
# The value is deliberately still not honoured differently. Rejecting unknown
# levels, and defaulting them to enabled, are both TIGHTENINGS that could fail
# configs which load today — the same trade already settled for the enabled flag
# at the restrictions.* block. Only the silence is fixed.
#
# LK-03 IS THE CONTROL. Asserting "a warning appeared" is satisfied by an engine
# that warns on every level it sees, which would be worse than the defect. A
# VALID level must stay silent, or LK-01 proves nothing.
#
# LK-04 pins the de-duplication. Several checks (no_secrets, no_placeholders,
# no_hardcoded_results) reach parseEnforcementLevel by TWO parse paths, so one
# bad value printed per call site emits the same line twice. The message does not
# name the key, so the second line carries no information — but two DIFFERENT bad
# values must still produce two warnings, which is what separates de-duplication
# from simply suppressing everything after the first.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/lvlkey-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP/trust" "$TEST_TMP/w"

if [ ! -x "$NAAB" ]; then
    skip "LK-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Enforcement level: an unknown value disables, silently?      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

printf 'main { print("ok") }\n' > "$TEST_TMP/w/t.naab"

# echoes the number of unknown-level warnings for a given config
warns() {
    printf '%s\n' "$1" > "$TEST_TMP/w/govern.json"
    (cd "$TEST_TMP/w" && NAAB_TRUST_STORE_DIR="$TEST_TMP/trust" \
        timeout 60s "$NAAB" t.naab 2>&1) | grep -c "unknown enforcement level"
}

N_BAD=$(warns '{"version":"5.0","mode":"enforce","code_quality":{"no_secrets":{"level":"off"}}}')
N_TYPO=$(warns '{"version":"5.0","mode":"enforce","code_quality":{"no_secrets":{"level":"HARD"}}}')
N_GOOD=$(warns '{"version":"5.0","mode":"enforce","code_quality":{"no_secrets":{"level":"hard"}}}')
N_TWO=$(warns '{"version":"5.0","mode":"enforce","code_quality":{"no_secrets":{"level":"off"},"no_pii":{"level":"nope"}}}')

# --- LK-01: an unknown level is reported ---------------------------------
if [ "${N_BAD:-0}" -ge 1 ]; then
    pass "LK-01" "\"level\": \"off\" is reported rather than silently disabling"
else
    fail "LK-01" "an unknown level disabled the check with no diagnostic" \
         "this is the silent-switch failure the enabled flag already had"
fi

# --- LK-02: case matters, and a near-miss is still unknown ----------------
if [ "${N_TYPO:-0}" -ge 1 ]; then
    pass "LK-02" "a near-miss (\"HARD\") is reported, not quietly accepted"
else
    fail "LK-02" "\"HARD\" was not reported" \
         "levels are case-sensitive, so a capitalised one disables the check"
fi

# --- LK-03: a VALID level stays silent (control) -------------------------
if [ "${N_GOOD:-0}" -eq 0 ]; then
    pass "LK-03" "a valid level warns about nothing (control)"
else
    fail "LK-03" "a valid level produced a warning" \
         "warning on every level is worse than the defect; LK-01 would prove nothing"
fi

# --- LK-04: de-duplicated per value, not suppressed after the first ------
if [ "${N_BAD:-0}" -eq 1 ] && [ "${N_TWO:-0}" -eq 2 ]; then
    pass "LK-04" "one line per distinct bad value (1 for one, 2 for two)"
elif [ "${N_BAD:-0}" -gt 1 ]; then
    fail "LK-04" "the same bad value warned $N_BAD times" \
         "checks parsed by two paths emit duplicate lines that name no key"
else
    fail "LK-04" "two distinct bad values produced $N_TWO warnings, expected 2" \
         "de-duplication is suppressing distinct values, not just repeats"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
