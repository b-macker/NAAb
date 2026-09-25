#!/usr/bin/env bash
# ============================================================
# test_capability_telemetry.sh — F10: CAPABILITY_VIOLATION reaches the chain
#
# Every other governance mechanism emits a JSONL event. Without one, a
# function-capability violation is visible on stderr and in
# --governance-report, and INVISIBLE to anything auditing the run afterwards:
# not in the tamper-evident chain, not forwardable to a SIEM, not replayable
# from preserved evidence. For a tier whose purpose is a confused-deputy
# defence, being absent from the evidence chain is the wrong hole to leave.
#
# Two arms are load-bearing for reasons that are not obvious:
#
# FT-02 — the event must carry blocking_frame SEPARATELY from function. Under
# intersection the frame that ATTEMPTED the call is usually not the one that
# narrowed the set, so an event recording only `function` cannot reconstruct
# why the call was refused. This is the same provenance the error message
# carries, and the audit trail needs it for the same reason.
#
# FT-06 — the emit has to happen BEFORE enforce(). At hard, enforce() throws
# and nothing after it runs, so an emit placed after it records every advisory
# violation and none of the blocking ones — exactly inverted from what an audit
# cares about most.
#
# FT-07/FT-08 are the success-expecting controls. Every other arm asserts an
# event IS present, and those pass for free if the implementation emitted
# unconditionally; these two require silence when the tier is unconfigured and
# when the action is properly declared.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

# The old form probed `[ -d <termux tmp> ]` first. That probe is unsound off
# Android: the project's own runners created that directory on Linux, so the
# branch was taken on a box where the path is root-owned 755 -- and a non-root
# user got "mktemp: Permission denied". TMPDIR is what Termux actually sets,
# so consulting it needs no Termux-specific branch at all.
_SYSTMP="${TMPDIR:-/tmp}"
W="${_SYSTMP}/cap-telem-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  F10: CAPABILITY_VIOLATION is emitted and chained            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

ALL_IDS="FT-01 FT-02 FT-03 FT-04 FT-05 FT-06 FT-07 FT-08"
if ! command -v python3 >/dev/null 2>&1; then
    for id in $ALL_IDS; do skip "$id" "python3 unavailable (fixture generator + JSONL reader)"; done
    echo ""; echo "  Total: 8 | Pass: 0 | Fail: 0 | Skip: 8"; exit 0
fi

mkdir -p "$W"; cleanup() { rm -rf "$W"; }; trap cleanup EXIT

# $1 = capabilities.functions object (or "" to omit the section)
# Fixtures never read govern.json and never set allowed_paths: both are
# auto-added to blocked_paths, so such a fixture dies on an unrelated rule
# before reaching the call under test — and then every arm expecting an
# ABSENT event passes for free.
mkcfg() {
    python3 - "$W/govern.json" "$1" << 'PY'
import json, sys
cfg = {
    "version": "5.0", "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    "telemetry": {"enabled": True, "output_file": "tel.jsonl", "tamper_evidence": True},
    "capabilities": {"filesystem": {"mode": "readwrite"}},
}
if sys.argv[2]:
    cfg["capabilities"]["functions"] = json.loads(sys.argv[2])
json.dump(cfg, open(sys.argv[1], "w"), indent=1)
PY
    python3 -c "import json,sys; json.load(sys.stdin)" < "$W/govern.json" >/dev/null || {
        echo "FIXTURE BROKEN"; return 1; }
}

# helper() attempts the write; caller() is what narrows it. Two frames, so the
# event's provenance fields are distinguishable — with one frame, recording the
# wrong field would still look right.
cat > "$W/t.naab" << 'EOF'
fn helper() { file.write("o.txt", "x") }
fn caller() { helper() }
main { caller() print("DONE") }
EOF

# run CFG [args] -> RC; events land in $W/tel.jsonl
run() {
    local cfg="$1"; shift
    mkcfg "$cfg" >/dev/null || { RC=99; return; }
    rm -f "$W/tel.jsonl" "$W/o.txt"
    (cd "$W" && timeout 60s "$NAAB" "$@" t.naab >/dev/null 2>&1); RC=$?
}

# field NAME -> value of that field on the first CAPABILITY_VIOLATION, or ""
field() {
    python3 - "$W/tel.jsonl" "$1" << 'PY'
import json, sys
try: lines = open(sys.argv[1], encoding="utf-8", errors="replace").readlines()
except OSError: sys.exit(0)
for l in lines:
    try: e = json.loads(l)
    except Exception: continue
    if e.get("event_type") == "CAPABILITY_VIOLATION":
        sys.stdout.write(str(e.get(sys.argv[2], ""))); break
PY
}
count() {
    python3 - "$W/tel.jsonl" << 'PY'
import json, sys
n = 0
try: lines = open(sys.argv[1], encoding="utf-8", errors="replace").readlines()
except OSError: lines = []
for l in lines:
    try: e = json.loads(l)
    except Exception: continue
    if e.get("event_type") == "CAPABILITY_VIOLATION": n += 1
print(n)
PY
}

RO='{"default": {"allowed_actions": ["FS_READ"]}}'

for eng in "" "--tree-walk"; do
    tag="${eng:-VM}"
    run "$RO" $eng

    if [ "$(count)" -ge 1 ]; then
        [ -z "$eng" ] && pass "FT-01" "the event is emitted on a violation [$tag]" \
                      || pass "FT-01b" "the event is emitted on a violation [$tag]"
    else
        fail "FT-01" "no CAPABILITY_VIOLATION event [$tag]" "count=$(count) rc=$RC"
    fi
done

# --- FT-02: provenance — the narrowing frame is recorded separately ----------
run "$RO"
FN=$(field function); BF=$(field blocking_frame)
if [ "$FN" = "helper" ] && [ "$BF" = "caller" ]; then
    pass "FT-02" "function='helper' and blocking_frame='caller' are recorded separately"
else
    fail "FT-02" "intersection provenance not reconstructible from the event" "function='$FN' blocking_frame='$BF'"
fi

# --- FT-03: the whole stack, so an auditor can see the shape -----------------
CS=$(field call_stack)
case "$CS" in
    *caller*helper*) pass "FT-03" "call_stack carries the full chain ($CS)" ;;
    *) fail "FT-03" "call_stack is not the full chain" "got '$CS'" ;;
esac

# --- FT-04: the chain must still verify (the plan's explicit warning) --------
CHAIN=$(cd "$W" && "$NAAB" --verify-telemetry-chain tel.jsonl 2>&1); CRC=$?
if [ "$CRC" -eq 0 ] && [[ "$CHAIN" == *"no breaks"* ]]; then
    pass "FT-04" "the tamper-evident chain still verifies with the new event in it"
else
    fail "FT-04" "the new event broke the telemetry chain" "rc=$CRC: $(printf '%s' "$CHAIN" | tail -1)"
fi

# --- FT-05 / FT-06: level is recorded, and hard still emits ------------------
if [ "$(field level)" = "advisory" ]; then
    pass "FT-05" "level records the level actually enforced (advisory)"
else
    fail "FT-05" "level field wrong" "got '$(field level)'"
fi

# At hard, enforce() THROWS. An emit placed after it would record nothing here —
# so this arm is what pins the emit ahead of enforcement.
run '{"level": "hard", "default": {"allowed_actions": ["FS_READ"]}}'
if [ "$RC" -eq 3 ] && [ "$(count)" -ge 1 ] && [ "$(field level)" = "hard" ]; then
    pass "FT-06" "a HARD block is still recorded (emit precedes enforce)"
else
    fail "FT-06" "the blocking case was not recorded" "rc=$RC count=$(count) level='$(field level)'"
fi

# --- FT-07 / FT-08: the success-expecting controls ---------------------------
run ""
if [ "$RC" -eq 0 ] && [ "$(count)" -eq 0 ]; then
    pass "FT-07" "control: no capabilities.functions section emits no event"
else
    fail "FT-07" "the tier emitted without being configured" "rc=$RC count=$(count)"
fi

run '{"default": {"allowed_actions": ["FS_READ", "FS_WRITE"]}}'
if [ "$RC" -eq 0 ] && [ "$(count)" -eq 0 ] && [ -f "$W/o.txt" ]; then
    pass "FT-08" "control: a declared action emits nothing and the write happens"
else
    fail "FT-08" "a permitted call produced an event" "rc=$RC count=$(count)"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; fi
exit 0
