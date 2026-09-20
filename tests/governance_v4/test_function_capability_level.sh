#!/usr/bin/env bash
# ============================================================
# test_function_capability_level.sh — F4: the tier gets a level, and the
# advisory level actually accumulates
#
# F4 in docs/plan-function-effects.md is two claims. The first is that
# capabilities.functions needs a configurable enforcement level so a project can
# bootstrap at advisory and tighten to hard — that is the ratchet's own
# direction, so progression is enforced rather than encouraged.
#
# The second claim did NOT survive tracing, and this suite exists mostly because
# of it. The plan said "at advisory, one run reports EVERY undeclared effect
# with the exact declaration to add". It did not. enforce() prints an advisory's
# detail on the FIRST occurrence per RULE NAME, and every undeclared effect in a
# program shares the rule name "capabilities.functions" — so site one printed its
# remedy and every later site was silent. Measured before the fix: three
# violating functions, one reported, the other two producing no stderr at all
# (the occurrence-count line is behind advisory_escalation, which defaults off).
# That made the bootstrap pass a one-finding-per-run loop.
#
# FL-02 is the arm for that half; FL-06 is the one that matters most, because a
# summary listing every site is worthless if pasting what it prints does not
# actually silence it. Guidance that does not survive being followed is not
# guidance.
#
# FL-07/FL-08 are a pair and neither means anything alone: "level" is a reserved
# key ONLY when its value is a string. A function really named level() must
# still be a function entry, or reserving the name would silently drop it
# through to "default" and change its permissions — the exact failure this tier
# exists to prevent.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
W="${_SYSTMP}/fncap-level-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  F4: capabilities.functions.level, and advisory accumulates  |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if ! command -v python3 >/dev/null 2>&1; then
    for id in FL-01 FL-02 FL-03 FL-04 FL-05 FL-06 FL-07 FL-08 FL-09 FL-10; do
        skip "$id" "python3 unavailable (fixture generator)"
    done
    echo ""
    echo "  Total: 10 | Pass: 0 | Fail: 0 | Skip: 10"
    exit 0
fi

mkdir -p "$W"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT

# $1 = the capabilities.functions object, verbatim JSON.
# Nothing else varies, so the only thing under test is that object.
mkcfg() {
    python3 - "$W/govern.json" "$1" << 'PY'
import json, sys
json.dump({
    "version": "5.0", "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    "capabilities": {
        "filesystem": {"mode": "readwrite"},
        "functions": json.loads(sys.argv[2]),
    },
}, open(sys.argv[1], "w"), indent=1)
PY
    # A fixture that fails to parse makes NAAb exit 4, and exit-4 reads as
    # "refused" for every arm that expects a refusal — they would all pass for
    # free. Validate by BYTES, never by handing a path to a helper that may not
    # share this shell's path vocabulary.
    python3 -c "import json,sys; json.load(sys.stdin)" < "$W/govern.json" || {
        echo "FIXTURE BROKEN"; return 1; }
}

cat > "$W/three.naab" << 'EOF'
fn alpha()   { file.write("a.txt", "x") }
fn bravo()   { file.write("b.txt", "x") }
fn charlie() { file.write("c.txt", "x") }
main { alpha() bravo() charlie() print("DONE") }
EOF

cat > "$W/named.naab" << 'EOF'
fn level() { file.write("lv.txt", "x") }
main { level() print("DONE") }
EOF

RO='{"default": {"allowed_actions": ["FS_READ"]}}'

# run PROGRAM CONFIG [extra-args...] -> sets RC, OUT (stdout+stderr merged)
run() {
    local prog="$1"; shift
    local cfg="$1"; shift
    mkcfg "$cfg" >/dev/null || { RC=99; OUT="FIXTURE BROKEN"; return; }
    rm -f "$W"/*.txt
    OUT="$(cd "$W" && timeout 60s "$NAAB" "$@" "$prog" 2>&1)"
    RC=$?
}

# --- The advisory default ----------------------------------------------------

run three.naab "$RO"
if [ "$RC" -eq 0 ] && [[ "$OUT" == *DONE* ]]; then
    pass "FL-01" "advisory is the default: the run continues"
else
    fail "FL-01" "the default level blocked the run" "rc=$RC"
fi

# FL-02: the accumulate half. Before this, only 'alpha' was ever named.
missing=""
for f in alpha bravo charlie; do
    [[ "$OUT" == *"$f needs FS_WRITE"* ]] || missing="$missing $f"
done
if [ -z "$missing" ]; then
    pass "FL-02" "one run reports every undeclared effect, not just the first"
else
    fail "FL-02" "sites missing from the advisory summary" "absent:$missing"
fi

# --- Raising the level -------------------------------------------------------

run three.naab '{"level": "hard", "default": {"allowed_actions": ["FS_READ"]}}'
if [ "$RC" -eq 3 ] && [[ "$OUT" == *"Undeclared action in 'alpha'"* ]]; then
    pass "FL-03" "level hard blocks, and blocks on the first site"
else
    fail "FL-03" "hard did not block via the function gate" "rc=$RC"
fi

# FL-04: an exit code is not a block. The write must not have happened.
if [ ! -f "$W/a.txt" ]; then
    pass "FL-04" "the blocked effect did not occur (exit code is not the proof)"
else
    fail "FL-04" "hard reported a block but the write went through"
fi

run three.naab '{"level": "soft", "default": {"allowed_actions": ["FS_READ"]}}'
RC_SOFT=$RC
run three.naab '{"level": "soft", "default": {"allowed_actions": ["FS_READ"]}}' --governance-override
if [ "$RC_SOFT" -eq 3 ] && [ "$RC" -eq 0 ]; then
    pass "FL-05" "level soft blocks, and honours the override (not a hard/advisory binary)"
else
    fail "FL-05" "soft did not behave as soft" "no-override=$RC_SOFT override=$RC"
fi

# --- FL-06: the summary's own remedy has to work -----------------------------
# Pull the declaration line out of the advisory and feed it straight back.
run three.naab "$RO"
REMEDY="$(printf '%s\n' "$OUT" | grep -oE '"default": \{ "allowed_actions": \[[^]]*\] \}' | head -1)"
if [ -z "$REMEDY" ]; then
    fail "FL-06" "the summary printed no pasteable declaration" "none matched"
else
    run three.naab "{$REMEDY}"
    if [ "$RC" -eq 0 ] && [[ "$OUT" != *"Undeclared function effects"* ]] && [[ "$OUT" == *DONE* ]]; then
        pass "FL-06" "control: pasting the printed declaration silences it and the program still runs"
    else
        fail "FL-06" "applying the suggested remedy did not silence the advisory" "rc=$RC"
    fi
fi

# --- FL-07 / FL-08: "level" is reserved only as a STRING ---------------------
# No "default" here, so every other frame is unrestricted and narrows nothing —
# the only constraint in play is the entry named level.
run named.naab '{"level": {"allowed_actions": ["FS_READ"]}}'
if [ "$RC" -eq 0 ] && [[ "$OUT" == *"level needs FS_WRITE"* ]]; then
    pass "FL-07" "an object-valued \"level\" is a function entry, not the policy key"
else
    fail "FL-07" "a function named level() was swallowed by the reserved key" "rc=$RC"
fi

run named.naab '{"level": "hard", "default": {"allowed_actions": ["FS_READ"]}}'
if [ "$RC" -eq 3 ]; then
    pass "FL-08" "control: a string-valued \"level\" IS the policy key"
else
    fail "FL-08" "the reserved key did not take effect as a string" "rc=$RC"
fi

# --- FL-09: an unknown level disables, loudly (the A1c precedent) ------------
run three.naab '{"level": "warn", "default": {"allowed_actions": ["FS_READ"]}}'
if [ "$RC" -eq 0 ] && [[ "$OUT" == *"unknown enforcement level"* ]] \
   && [[ "$OUT" != *"Undeclared function effects"* ]]; then
    pass "FL-09" "an unrecognised level disables the gate and says so"
else
    fail "FL-09" "unknown level was silent or still enforcing" "rc=$RC"
fi

# --- FL-10: both engines ------------------------------------------------------
run three.naab '{"level": "hard", "default": {"allowed_actions": ["FS_READ"]}}' --tree-walk
if [ "$RC" -eq 3 ] && [[ "$OUT" == *"Undeclared action in 'alpha'"* ]]; then
    pass "FL-10" "the tree-walker enforces the level identically"
else
    fail "FL-10" "engines disagree on the function-capability level" "tree-walk rc=$RC"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
