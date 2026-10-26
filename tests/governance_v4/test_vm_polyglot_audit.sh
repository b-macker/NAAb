#!/usr/bin/env bash
# ============================================================
# test_vm_polyglot_audit.sh -- the default engine writes the audit trail's
# polyglot entries and counts a runtime .exec() against the block limit
# (A37's VM half; Bundle 2)
#
# Both were the tree-walker's alone. The tree-walker calls
# logPolyglotExecution() for an inline block and for each runtime .exec(), and
# counts each .exec() against limits.execution.polyglot_blocks; the VM did
# neither (an inline block was counted, a runtime .exec() was not). Measured
# before the fix, mode enforce, limit 2: three js.exec() calls all ran on the
# VM, exit 0, and the audit file was never written; the tree-walker counted the
# third as BLOCKED and logged three entries. Under enforce only the VM runs now
# (--tree-walk exits 4), so on master's design `audit.log_events.
# polyglot_executed` recorded nothing in an enforce run, and the block limit
# could be walked past with .exec().
#
#   PA-00  CONTROL: the probe program under mode audit on the tree-walker (the
#          oracle) logs one entry per inline block and per .exec() call --
#          without it, a VM count of 0 could be a probe that sees nothing
#   PA-01  VM, enforce: each inline block is logged (language and line)
#   PA-02  VM, enforce: each runtime .exec() is logged
#   PA-03  VM, enforce: polyglot_timing adds duration=; PA-03c without it, none
#   PA-04  CONTROL: both log_events flags off -> no polyglot entry, so the
#          entries come from these flags
#   PA-05  VM, enforce: a runtime .exec() counts against
#          limits.execution.polyglot_blocks -- the third of three calls under a
#          limit of 2 is refused before it runs
#   PA-05c CONTROL: the same program under a limit of 3 runs to the end
#   PA-06  inline blocks and .exec() calls share one count (limit 3, two
#          blocks then two calls: the second call is refused)
#
# Kill table (each mutant rebuilt and run):
#   no log at the VM inline site          PA-01, PA-02, PA-03
#   no log at the VM .exec() site         PA-02
#   no count at the VM .exec() site       PA-05, PA-06
#   .exec() line read as ip - 4 again     PA-02
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }
report() { echo ""; echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; }

echo "=== VM polyglot audit entries and the .exec() block count ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-vmpa-XXXXXX")"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

# cfg DIR MODE LOG_EVENTS [LIMITS]
cfg() {
    mkdir -p "$1"
    printf '{ "version": "4.0", "mode": "%s", "security": { "sandbox_level": "elevated" },\n  "audit": { "level": "basic", "output_file": "audit.jsonl", "log_events": { %s } }%s }\n' \
        "$2" "$3" "${4:+,
  $4}" > "$1/govern.json"
}
# Two inline blocks (sql on line 3, javascript on line 6), then three runtime
# .exec() calls (lines 10, 12 and 14), each followed by a print so a refusal shows
# where it stopped.
PROG='runtime js = javascript.start()
main {
    let a = <<sql
SELECT 1 AS x
>>
    let b = <<javascript
2 + 2
>>
    print("INLINE_DONE")
    let c = js.exec("3 + 3")
    print("EXEC1_DONE")
    let d = js.exec("4 + 4")
    print("EXEC2_DONE")
    let e = js.exec("5 + 5")
    print("EXEC3_DONE")
    print("END")
}'
# run DIR [naab args...] -> OUT RC
run() { local d="$1"; shift; printf '%s\n' "$PROG" > "$d/p.naab"; rm -f "$d/audit.jsonl"
        OUT="$(cd "$d" && timeout 60 "$NAAB" "$@" p.naab 2>&1)"; RC=$?; }
entries() { local n; n="$(grep -c '"event":"polyglot_executed"' "$1/audit.jsonl" 2>/dev/null)" || true; echo "${n:-0}"; }
entry_at() { local e; e="$(grep '"event":"polyglot_executed"' "$1/audit.jsonl" 2>/dev/null)"
             e="$(grep -F "\"line\":$2," <<<"$e")"; grep -qF "lang=$3" <<<"$e"; }

ON='"polyglot_executed": true'
# 2 inline blocks + 3 .exec() calls
EXPECT=5

# --- PA-00: the oracle -------------------------------------------------------
cfg "$W/oracle" audit "$ON"
run "$W/oracle" --tree-walk
ORACLE="$(entries "$W/oracle")"
if [ "$RC" = 0 ] && [[ "$OUT" == *END* ]] && [ "$ORACLE" = "$EXPECT" ]; then
    ok "PA-00" "control: the tree-walker (mode audit) logs $ORACLE entries for 2 blocks and 3 .exec() calls"
else
    bad "PA-00" "the oracle should log $EXPECT entries -- every VM arm below measures nothing" "rc=$RC entries=$ORACLE $OUT"
fi

# --- PA-01 / PA-02: the VM under enforce --------------------------------------
cfg "$W/vm" enforce "$ON"
run "$W/vm"
if [ "$RC" != 0 ] || [[ "$OUT" != *END* ]]; then
    bad "PA-01" "the program did not run on the VM under enforce" "rc=$RC $OUT"
    bad "PA-02" "the program did not run on the VM under enforce"
else
    if entry_at "$W/vm" 3 sql && entry_at "$W/vm" 6 javascript; then
        ok "PA-01" "VM: each inline block is logged with its language and line"
    else
        bad "PA-01" "VM: an inline block was not logged" "$(cat "$W/vm/audit.jsonl" 2>/dev/null || echo '(no audit file)')"
    fi
    if entry_at "$W/vm" 10 javascript && entry_at "$W/vm" 12 javascript && entry_at "$W/vm" 14 javascript &&
       [ "$(entries "$W/vm")" = "$EXPECT" ]; then
        ok "PA-02" "VM: each runtime .exec() is logged ($EXPECT entries in all, as the oracle)"
    else
        bad "PA-02" "VM: the .exec() calls were not each logged ($(entries "$W/vm") of $EXPECT)" \
            "$(cat "$W/vm/audit.jsonl" 2>/dev/null || echo '(no audit file)')"
    fi
fi

# --- PA-03: timing ------------------------------------------------------------
cfg "$W/timing" enforce "$ON, \"polyglot_timing\": true"
run "$W/timing"
T3="$(grep '"event":"polyglot_executed"' "$W/timing/audit.jsonl" 2>/dev/null)"; T3="$(grep -F '"line":3,' <<<"$T3")"
if [ "$RC" = 0 ] && grep -q 'duration=[0-9]*us' <<<"$T3"; then
    ok "PA-03" "VM: polyglot_timing adds the block's duration"
else
    bad "PA-03" "VM: no duration on the inline block's entry" "rc=$RC $(cat "$W/timing/audit.jsonl" 2>/dev/null || echo '(no audit file)')"
fi
if [ "$(entries "$W/vm")" -gt 0 ] && ! grep -q 'duration=' "$W/vm/audit.jsonl"; then
    ok "PA-03c" "control: without polyglot_timing, no duration"
else
    bad "PA-03c" "an entry carries a duration without polyglot_timing, or there are no entries" \
        "$(cat "$W/vm/audit.jsonl" 2>/dev/null || echo '(no audit file)')"
fi

# --- PA-04: both flags off ------------------------------------------------------
cfg "$W/off" enforce '"polyglot_executed": false, "polyglot_timing": false'
run "$W/off"
if [ "$RC" = 0 ] && [[ "$OUT" == *END* ]] && [ "$(entries "$W/off")" = 0 ]; then
    ok "PA-04" "control: with both log_events flags off, no polyglot entry is written"
else
    bad "PA-04" "polyglot entries without the flags (rc=$RC, $(entries "$W/off") entries)" "$OUT"
fi

# --- PA-05 / PA-05c: the block count -------------------------------------------
# 2 inline blocks + 3 calls = 5 executions. Limit 4: the third call is the fifth.
cfg "$W/lim4" enforce "$ON" '"limits": { "execution": { "polyglot_blocks": 4 } }'
run "$W/lim4"
if [ "$RC" = 3 ] && [[ "$OUT" == *EXEC2_DONE* ]] && [[ "$OUT" != *EXEC3_DONE* ]] &&
   [[ "$OUT" == *polyglot_blocks* || "$OUT" == *"polyglot block"* ]]; then
    ok "PA-05" "VM: the fifth execution, a .exec() call, is refused under a limit of 4 (exit 3, before it ran)"
else
    bad "PA-05" "VM: a .exec() call past limits.execution.polyglot_blocks ran (rc=$RC)" "$OUT"
fi
cfg "$W/lim5" enforce "$ON" '"limits": { "execution": { "polyglot_blocks": 5 } }'
run "$W/lim5"
if [ "$RC" = 0 ] && [[ "$OUT" == *END* ]]; then
    ok "PA-05c" "control: under a limit of 5 the same program runs to the end"
else
    bad "PA-05c" "the limit refuses a program within it (rc=$RC)" "$OUT"
fi

# --- PA-06: one count for blocks and calls ---------------------------------------
cfg "$W/lim3" enforce "$ON" '"limits": { "execution": { "polyglot_blocks": 3 } }'
run "$W/lim3"
if [ "$RC" = 3 ] && [[ "$OUT" == *EXEC1_DONE* ]] && [[ "$OUT" != *EXEC2_DONE* ]]; then
    ok "PA-06" "VM: inline blocks and .exec() calls share one count (the second call is the fourth)"
else
    bad "PA-06" "VM: the count did not stop at the fourth execution (rc=$RC)" "$OUT"
fi

report
[ "$FAIL" -eq 0 ]
