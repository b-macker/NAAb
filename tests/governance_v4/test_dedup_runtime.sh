#!/usr/bin/env bash
# ============================================================
# test_dedup_runtime.sh — deduplicate_checks collapsed whole runs
#
# THE DEFECT
#
# The dedup key is rule_name|file|line. enforce() fills file/line from
# current_check_file_/current_check_line_, which the STATIC scanners set;
# runtime checks (BSD, CDD, admission, output admissibility) leave them empty.
# Measured: file="" line=0 on every runtime row -- empty, not stale, which was
# checked because a stale location would have been worse than none.
#
# So for runtime results the key degenerated to the rule name alone and the
# whole run collapsed to its first occurrence. Measured before the fix, dedup
# on: a BSD pattern that fired 3 times produced 1 RuleViolation, and 13 checks
# became 3 rows. A rule that fired once and a rule that fired constantly were
# indistinguishable in the record.
#
# Deduping a source SITE is the feature. Deduping an event STREAM is not, and
# the two shared a code path only because a runtime result and an unlocated
# static result look alike.
#
# Note there was NO test for deduplicate_checks anywhere in the suite before
# this file, which is why it survived.
#
#   DC-01  with dedup ON, a runtime rule firing N times produces N rows
#   DC-02  CONTROL: static duplicates are STILL collapsed. Without this,
#          DC-01 passes just as well on a build where dedup was deleted
#   DC-03  the summary reports runtime_exempt, so "deduplicated" cannot be
#          read as "everything repeated was collapsed"
#   DC-04  runtime counts are identical with dedup ON and OFF
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab-dedup-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
W="$TEST_TMP/w"; mkdir -p "$W"

count_rule() {  # $1=telemetry $2=rule substring -> occurrences
python3 - "$1" "$2" <<'PYEOF'
import json, sys
n = 0
for line in open(sys.argv[1]):
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") in ("RuleViolation", "GovernanceCheck") and \
       sys.argv[2] in (d.get("rule_name") or ""): n += 1
print(n)
PYEOF
}
summary_field() {  # $1=telemetry $2=field
python3 - "$1" "$2" <<'PYEOF'
import json, sys
v = "ABSENT"
for line in open(sys.argv[1]):
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") == "GovernanceCheckSummary": v = d.get(sys.argv[2], "MISSING")
print(v)
PYEOF
}

# --- runtime arm: a BSD pattern that fires repeatedly, no source site ---
RUNTIME_PROG='use file
main {
    file.write("a.txt","x")
    let a = file.read("a.txt")
    let b = file.read("a.txt")
    let c = file.read("a.txt")
    let d = file.read("a.txt")
    print("DONE")
}'
runtime_cfg() {  # $1 = true|false
cat <<EOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": $1 },
  "behavioral_sequences": { "enabled": true, "patterns": [
    { "name": "repeat_read", "sequence": ["file.read","file.read"], "max_gap": 20, "level": "advisory" } ] } }
EOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  deduplicate_checks collapsed runtime events to one row       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

for mode in true false; do
    runtime_cfg "$mode" > "$W/govern.json"
    printf '%s\n' "$RUNTIME_PROG" > "$W/t.naab"
    rm -f "$W/tele.jsonl"
    (cd "$W" && timeout 60s "$NAAB" t.naab >/dev/null 2>&1) || true
    if [ ! -f "$W/tele.jsonl" ]; then
        fail "DC-0x" "runtime arm (dedup=$mode) produced no telemetry"; continue
    fi
    n="$(count_rule "$W/tele.jsonl" behavioral_sequences.repeat_read)"
    if [ "$mode" = "true" ]; then
        N_ON="$n"; EXEMPT="$(summary_field "$W/tele.jsonl" runtime_exempt)"
    else
        N_OFF="$n"
    fi
done

echo -e "${CYAN}--- DC-01/03/04: runtime events are no longer collapsed ---${NC}"
if [ "${N_ON:-0}" -ge 3 ]; then
    pass "DC-01" "dedup ON: the runtime rule produced ${N_ON} rows (was 1 before the fix)"
else
    fail "DC-01" "runtime rows still collapsed" "got ${N_ON:-none}, expected >=3"
fi
if [ "${EXEMPT:-MISSING}" != "MISSING" ] && [ "${EXEMPT:-ABSENT}" != "ABSENT" ] && [ "${EXEMPT:-0}" -gt 0 ]; then
    pass "DC-03" "summary reports runtime_exempt=${EXEMPT}"
else
    fail "DC-03" "runtime_exempt missing from the summary" "got '${EXEMPT:-none}' — the exemption would be invisible"
fi
if [ "${N_ON:-0}" = "${N_OFF:-x}" ]; then
    pass "DC-04" "runtime counts identical with dedup ON and OFF (${N_ON})"
else
    fail "DC-04" "dedup still changes runtime counts" "on=${N_ON:-none} off=${N_OFF:-none}"
fi

# --- static arm: a source scan, which DOES carry file/line ---
echo -e "${CYAN}--- DC-02: static duplicates are still collapsed ---${NC}"
cat > "$W/govern.json" <<'EOF'
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": true },
  "restrictions": { "dangerous_calls": { "enabled": true, "level": "advisory" } } }
EOF
cat > "$W/t.naab" <<'EOF'
main {
    <<python
x = eval("1+1")
y = eval("2+2")
>>
    print("DONE")
}
EOF
rm -f "$W/tele.jsonl"
(cd "$W" && timeout 90s "$NAAB" t.naab >/dev/null 2>&1) || true
if [ ! -f "$W/tele.jsonl" ]; then
    fail "DC-02" "static arm produced no telemetry"
else
    DEDUPED="$(summary_field "$W/tele.jsonl" deduplicated)"
    STATIC_ROWS="$(count_rule "$W/tele.jsonl" restrictions.dangerous_calls)"
    if [ "$STATIC_ROWS" -lt 1 ]; then
        fail "DC-02" "the static check never fired" "the arm cannot show collapse either way — fixture failure, not a result"
    elif [ "${DEDUPED:-0}" != "ABSENT" ] && [ "${DEDUPED:-0}" != "MISSING" ] && [ "${DEDUPED:-0}" -gt 0 ]; then
        pass "DC-02" "CONTROL: static duplicates still collapsed (deduplicated=${DEDUPED})"
    else
        fail "DC-02" "static dedup no longer collapses anything" "deduplicated=${DEDUPED:-none} — the fix disabled the feature instead of scoping it"
    fi
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Passed: ${GREEN}$PASS_COUNT${NC}  Failed: ${RED}$FAIL_COUNT${NC}  Skipped: ${YELLOW}$SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}$FAILURES"
    echo -e "${CYAN}+==============================================================+${NC}"
    exit 1
fi
echo -e "${GREEN}ALL PASSED${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
exit 0
