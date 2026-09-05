#!/usr/bin/env bash
# ============================================================
# test_bsd_preexec_double_record.sh — B9(a): the pre-check consumed the event
#                                     AND then let it be recorded again
#
# `checkPreExecution` tests a candidate action with `wouldMatch` and, on a
# match, calls `recordEvent` to consume it. When the enforcement does not block,
# the action then RUNS, and the normal post-call `emitEvent` records the same
# action a second time.
#
# The duplicate does two things. It leaves a phantom event in the sequence
# buffer, so every pattern's max_gap budget is spent on an action that happened
# once. And because `recordEvent` advances AND RESETS the pattern, the duplicate
# arrives at step 0 — which is why B9's headline symptom existed at all: an
# 8-turn ADVISORY run produced ZERO BSD_MATCH events from the completed path.
# The pre-check was not merely silent, it was stealing the match from the path
# that reports.
#
# MEASURED, six actions against a two-step same-type ADVISORY pattern:
#   file.read (final step IS pre-checked)         5 matches  <- 2,3,4,5,6
#   crypto.base64_decode (never pre-checked)      3 matches  <- 2,4,6
# Three is the arithmetic. The fix suppresses the duplicate record inside
# emitEvent, and file.read now also yields 3.
#
#   B9A-01  the fix: a pre-checked type yields the same count as the oracle
#   B9A-02  ORACLE/POSITIVE CONTROL: a never-pre-checked type yields 3 on an
#           identical fixture shape. Without it, "3" cannot be distinguished
#           from a fixture that simply matches less often, and B9A-01 would
#           pass for a build that broke detection outright.
#   B9A-03  blocking still blocks — the suppression must not disarm the
#           pre-execution gate it lives next to.
#   B9A-04  a catchable DETECT block does not leave the consumed flag set: a
#           later, unrelated pattern still fires. NOTE this holds today by
#           construction rather than by the guard that appears to enforce it —
#           enforce() THROWS on every blocking path, so the assignment after it
#           is never reached on a block. Inverting that assignment to an
#           unconditional `true` leaves this test UNCHANGED. It is kept as a
#           forward guard for a future enforce() that returns instead.
#
# Negative control for B9A-01: disabling the suppression in emitEvent restores
# 5 while B9A-02 stays at 3.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${PREEXECDR_TMP:-${_SYSTMP}/preexecdr-$$}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

command -v python3 >/dev/null 2>&1 || { echo "python3 not available — skipping"; exit 0; }

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"

ACTIONS=6

# $1=tag $2=step-name $3=level $4=naab body line
build_arm() {
    local d="$TEST_TMP/$1"; mkdir -p "$d"; echo "hello" > "$d/data.txt"
    cat > "$d/govern.json" <<GEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true,
    "patterns": [ { "name": "probe", "level": "$3", "max_gap": 10,
                    "sequence": ["$2", "$2"] } ] } }
GEOF
    cat > "$d/t.naab" <<NEOF
use file
use crypto
main {
    let i = 0
    while i < $ACTIONS { i = i + 1
        $4
    }
    print("DONE")
}
NEOF
    (cd "$d" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

# -> "<match_count> <exit_code>"
run_arm() {
    local d="$TEST_TMP/$1"
    (cd "$d" && timeout 90s "$NAAB" t.naab >/dev/null 2>&1); local rc=$?
    local n
    n=$(python3 - "$d/tele.jsonl" <<'PY'
import json, sys, os
p = sys.argv[1]; n = 0
if os.path.exists(p):
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        if e.get("event_type") == "BSD_MATCH": n += 1
print(n)
PY
)
    echo "$n $rc"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  BSD pre-check: one action, recorded twice                   |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

build_arm prechecked  "file.read"            "advisory" 'let c = file.read("data.txt")'
build_arm oracle      "crypto.base64_decode" "advisory" 'let c = crypto.base64_decode("aGVsbG8=")'
build_arm blocking    "file.read"            "hard"     'let c = file.read("data.txt")'

read -r PRE_N PRE_RC <<<"$(run_arm prechecked)"
read -r ORA_N ORA_RC <<<"$(run_arm oracle)"
read -r BLK_N BLK_RC <<<"$(run_arm blocking)"

EXPECT=$((ACTIONS / 2))
printf "  %-46s matches=%s exit=%s\n" "pre-checked type (file.read), advisory" "$PRE_N" "$PRE_RC"
printf "  %-46s matches=%s exit=%s\n" "oracle: never pre-checked (decode)"     "$ORA_N" "$ORA_RC"
printf "  %-46s matches=%s exit=%s\n" "pre-checked type at HARD"               "$BLK_N" "$BLK_RC"
echo ""

if [ "${ORA_N:-0}" -ne "$EXPECT" ]; then
    fail "B9A-02" "oracle did not produce the expected $EXPECT matches from $ACTIONS actions" \
        "got $ORA_N — the fixture shape is wrong, so B9A-01 proves nothing"
    skip "B9A-01" "oracle failed — no baseline to compare against"
else
    pass "B9A-02" "oracle: $ACTIONS actions on a never-pre-checked type give $ORA_N matches"
    if [ "${PRE_N:-0}" -eq "$ORA_N" ]; then
        pass "B9A-01" "pre-checked type agrees with the oracle ($PRE_N) — no duplicate record"
    else
        fail "B9A-01" "pre-checked type gives $PRE_N where the oracle gives $ORA_N" \
            "the pre-check is still consuming the event AND letting emitEvent record it again"
    fi
fi

if [ "${BLK_RC:-0}" -eq 3 ]; then
    pass "B9A-03" "the pre-execution gate still blocks at HARD (exit 3)"
else
    fail "B9A-03" "HARD did not block" \
        "exit=$BLK_RC (expected 3) — suppressing the duplicate must not disarm the gate"
fi

# B9A-04: a catchable DETECT block must not leave the consumed flag set.
D4="$TEST_TMP/detect"; mkdir -p "$D4"; echo "hello" > "$D4/data.txt"
cat > "$D4/govern.json" <<'GEOF'
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true,
    "patterns": [ { "name": "rr_detect", "level": "detect", "max_gap": 10,
                    "sequence": ["file.read","file.read"] },
                  { "name": "dd_after", "level": "advisory", "max_gap": 10,
                    "sequence": ["crypto.base64_decode","crypto.base64_decode"] } ] } }
GEOF
cat > "$D4/t.naab" <<'NEOF'
use file
use crypto
main {
    let a = file.read("data.txt")
    try { let b = file.read("data.txt") print("NOT BLOCKED") }
    catch (e) { print("CAUGHT") }
    let d1 = crypto.base64_decode("aGVsbG8=")
    let d2 = crypto.base64_decode("aGVsbG8=")
    print("DONE")
}
NEOF
(cd "$D4" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
D4_OUT=$(cd "$D4" && timeout 90s "$NAAB" t.naab 2>&1)
D4_PATTERNS=$(python3 - "$D4/tele.jsonl" <<'PY'
import json, sys, os
p = sys.argv[1]; names = set()
if os.path.exists(p):
    for ln in open(p):
        try: e = json.loads(ln)
        except Exception: continue
        if e.get("event_type") == "BSD_MATCH": names.add(e.get("pattern_name"))
print(",".join(sorted(names)) or "NONE")
PY
)
if ! echo "$D4_OUT" | grep -q "CAUGHT"; then
    fail "B9A-04" "the DETECT block was not raised or not catchable" \
        "output=[$(echo "$D4_OUT" | tr '\n' ' ' | head -c 120)] — the arm never reached its own premise"
elif echo "$D4_PATTERNS" | grep -q "dd_after"; then
    pass "B9A-04" "an unrelated pattern still fires after a caught DETECT block ($D4_PATTERNS)"
else
    fail "B9A-04" "the later pattern never fired after a caught DETECT block" \
        "patterns=[$D4_PATTERNS] — the consumed flag was left set and swallowed a real event"
fi

echo ""
if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}All $PASS_COUNT checks passed${NC} (${SKIP_COUNT} skipped)"
else
    echo -e "${RED}$FAIL_COUNT failed${NC}, $PASS_COUNT passed"
    echo -e "$FAILURES"
    exit 1
fi
