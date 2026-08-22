#!/usr/bin/env bash
# ============================================================
# test_dict_size_limit.sh — limits.data.dict_size actually enforces
#
# GovernanceEngine::checkDictSize() was written, correct, and never called:
# defined once, declared once, invoked nowhere. An operator could set a HARD
# dictionary limit, watch it survive validation and the ratchet, and get no
# enforcement at all.
#
# WHAT WAS NOT TRUE, AND WHY IT MATTERS HERE
#
# The first reading of this was "dictionaries are bounded by nothing". They are
# not: both engines already ran the ARRAY limit over dict literals, deliberately
# — `checkArraySize(dict.size())` in the tree-walker and `checkArraySize(count)`
# at OP_DICT, each with a comment saying so. So the fix could not be a swap.
# Replacing the array check with the dict check would have LOOSENED every config
# that sets only array_size, which bounds dicts today.
#
# The guard is therefore ADDITIVE, and DS-05 is the assertion that protects that
# property: array_size must still bound a dict after this change. Without it, a
# future "cleanup" that removes the now-apparently-redundant array check would
# silently drop dict protection for every config that never set dict_size.
#
# DS-02 exists because the two engines enforce this in different files. A limit
# wired in one and not the other is worse than one wired in neither: it depends
# on --tree-walk, which is not something an operator reasons about when setting
# a size limit.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/dictsize-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP/trust" "$TEST_TMP/w"

if [ ! -x "$NAAB" ]; then
    skip "DS-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  limits.data.dict_size: configured, and actually enforced?     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# 12-entry dict literal
python3 - "$TEST_TMP/w/t.naab" <<'PY'
import sys
entries = ", ".join('"k%d": %d' % (i, i) for i in range(12))
open(sys.argv[1], "w").write('main {\n    let d = {%s}\n    print("COMPLETED")\n}\n' % entries)
PY

# $1 = govern.json body, $2 = extra naab flags. echoes exit code.
run() {
    printf '%s\n' "$1" > "$TEST_TMP/w/govern.json"
    (cd "$TEST_TMP/w" && NAAB_TRUST_STORE_DIR="$TEST_TMP/trust" \
        timeout 60s "$NAAB" $2 t.naab >/dev/null 2>&1)
    echo $?
}

CFG_LOW='{"version":"5.0","mode":"enforce","limits":{"data":{"dict_size":5}}}'
CFG_HIGH='{"version":"5.0","mode":"enforce","limits":{"data":{"dict_size":50}}}'
CFG_NONE='{"version":"5.0","mode":"enforce"}'
CFG_ARR='{"version":"5.0","mode":"enforce","limits":{"data":{"array_size":5}}}'

# --- DS-01: the limit blocks (VM) ----------------------------------------
if [ "$(run "$CFG_LOW" "")" = "3" ]; then
    pass "DS-01" "dict_size 5 blocks a 12-entry dict literal (VM, exit 3)"
else
    fail "DS-01" "dict_size did not enforce on the VM" \
         "the key is accepted and does nothing — checkDictSize has no call site"
fi

# --- DS-02: the tree-walker agrees ---------------------------------------
if [ "$(run "$CFG_LOW" "--tree-walk")" = "3" ]; then
    pass "DS-02" "same limit blocks on the tree-walker (engine parity)"
else
    fail "DS-02" "dict_size enforces on one engine only" \
         "enforcement would depend on --tree-walk, which no operator reasons about"
fi

# --- DS-03: unset does not block (control) -------------------------------
if [ "$(run "$CFG_NONE" "")" = "0" ]; then
    pass "DS-03" "unset dict_size does not block (control)"
else
    fail "DS-03" "a config with no dict limit blocked anyway" \
         "the check is firing on something other than the configured limit"
fi

# --- DS-04: a limit ABOVE the size does not block (control) ---------------
# Without this, DS-01 is satisfied by a check that fires whenever the key is
# merely present, rather than one that compares against the entry count.
if [ "$(run "$CFG_HIGH" "")" = "0" ]; then
    pass "DS-04" "dict_size 50 allows a 12-entry dict (the comparison is real)"
else
    fail "DS-04" "a limit above the actual size still blocked" \
         "presence of the key is being treated as the trigger, not its value"
fi

# --- DS-05: array_size STILL bounds dicts (the additive property) ---------
# Load-bearing. The dict check was added ALONGSIDE the pre-existing array check,
# because both engines already bounded dict literals with array_size. If a later
# change swaps rather than adds, every config that sets only array_size silently
# loses dict protection — and no other assertion here would notice.
if [ "$(run "$CFG_ARR" "")" = "3" ]; then
    pass "DS-05" "array_size still bounds a dict literal (nothing was loosened)"
else
    fail "DS-05" "array_size no longer bounds dicts" \
         "the dict check replaced the array check instead of adding to it; configs setting only array_size just lost dict protection"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
