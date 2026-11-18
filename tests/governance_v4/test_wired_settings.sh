#!/usr/bin/env bash
# ============================================================
# Settings that were parsed and read by nothing, wired (A175, step 3 PR 2 --
# the owner's decisions of 2026-11-16 and 2026-11-18)
# ============================================================
# Each setting below loaded, appeared in the template and changed nothing
# (settings-liveness step 3 traced every reader; docs/settings-liveness.md).
# The owner chose to make these work. Their meanings are the record's:
#
#   limits.code.max_total_polyglot_lines   the program-wide total of polyglot
#       code, each distinct block counted once (a block in a loop is one block)
#   limits.timeout.total_polyglot          the time every polyglot block of the
#       run may spend in total; the last block is interrupted at what is left
#   limits.memory.total_mb                 caps the run's memory budget (the
#       one runtime.memory_limit sets); it never raises it
#   restrictions.dangerous_calls.blocklist_extra   names the project adds to the
#       dangerous calls, matched as literal text like banned_functions
#   restrictions.imports {mode, allowed} and
#   languages.per_language.<lang>.imports {mode, allowed}
#       the import allowlist, STATIC ("2 a"): every import a block writes must
#       be listed, a computed import is refused, and an allowlist for a
#       language whose imports cannot be listed refuses the config (exit 4).
#       The blocked lists still apply in either mode.
#   agents.<n>.tool_timeout_seconds        a tool past the limit is a HARD block
#       ("1 b", the A2 precedent); default 10
#   agent_dispatch.max_retries_per_run     N retries across the run, then the
#       failing call returns its error
#   approval.default_expiry_hours          the lifetime `--approve` grants;
#       --expiry may shorten it, never extend it
# trust.require_fresh_signature is in test_signature_staleness.sh ST-06..10.
#
# Every refusal arm sits beside a CONTROL that the same program does its work
# when the setting allows it; a control that cannot run here makes its arms
# UNMEASURABLE (no Python, no agent stub, a platform that applies no child
# memory limit). The observable is what the program prints after the refused
# step, or a file the refused step would have written -- never the exit
# status alone.
#
#   polyglot lines
#   PL-00   CONTROL: no limit, both blocks run
#   PL-01   limit below the two blocks' total: exit 3, the second is refused
#   PL-01c  CONTROL: a limit equal to the total, both run
#   PL-02   one block run three times under a limit it fits once: all run
#   polyglot time
#   PT-00   CONTROL: no total, two 2.5 s blocks run
#   PT-01   total 3 s: exit 3, the second is refused, and interrupted (the run
#           takes at least 1.2 s less than PT-00; run out and refused after,
#           it would take as long)
#   PT-02   mode audit: reported, every block runs
#   memory (process.run child)
#   MM-00   CONTROL: no memory setting, a 300 MB child allocation succeeds
#   MM-00m  CONTROL: runtime.memory_limit 64 refuses it (the platform applies
#           child memory limits; else MM-01/02 are UNMEASURABLE)
#   MM-01   limits.memory.total_mb 64 refuses it
#   MM-02   total_mb 8192 beside runtime.memory_limit 64: still refused (the
#           cap never raises the budget)
#   blocklist_extra
#   BX-00   CONTROL: dangerous_calls on without the extra, Math.max runs
#   BX-01   blocklist_extra ["Math.max"]: exit 3, the program stops there
#   BX-02   CONTROL: the entry is literal text -- "Mathxmax" is not matched
#   BX-03   matched without case: "MATH.MAX" refuses Math.max
#   imports
#   IA-00   CONTROL: no allowlist, `import os` runs
#   IA-01   CONTROL: per-language allowlist [json], `import json` runs
#   IA-02   the same allowlist, `import os`: exit 3
#   IA-03   CONTROL: allowlist [os] admits `import os.path` (nested)
#   IA-03b  allowlist [os.path] does not admit `import os`
#   IA-03c  allowlist [js] does not admit `import json` (a separator bounds it)
#   IA-04   a computed import (`__import__("js" + "on")`) is refused
#   IA-05   restrictions.imports form, level hard: `import os` refused;
#   IA-05c  CONTROL: the same config runs `import json`
#   IA-06   allowlist [os] and blocked [os]: refused (the blocked list still
#           applies in allowlist mode)
#   IA-07   `from os import path` under allowlist [json]: refused
#   IA-08   CONTROL: a JavaScript block that imports nothing runs under a
#           JavaScript allowlist
#   IA-09   `require("fs")` under a JavaScript allowlist: refused
#   IA-10..13  the config is refused (exit 4, nothing runs): an unknown mode,
#           an allowlist for shell, a restrictions allowlist naming no
#           language, the "any" key
#   IA-14   CONTROL: mode "blocklist" written out loads and runs
#   IA-15   CONTROL: a bound-variable block and codegen.run_with_args run under
#           an allowlist that does not list json (the engine's own binding
#           code is not the program's import)
#   tool time limit (agent stub)
#   TT-01   tool_timeout_seconds 1, a 3 s tool: exit 3, the tool did not finish
#   TT-01c  CONTROL: tool_timeout_seconds 60, the same tool finishes
#   TT-02   the key absent (default 10), a 12 s tool: refused
#   TT-03   mode audit: the tool finishes (audit stops nothing)
#   retry cap (agent stub: three 500s, then a reply)
#   RT-00   CONTROL: no cap, four requests, the reply arrives
#   RT-01   max_retries_per_run 1: two requests, the call returns its error
#   RT-02   CONTROL: cap 3 (exactly what the call needs): the reply arrives
#   approval lifetime (`naab-lang --approve`, a throwaway key)
#   AP-00   CONTROL: no govern.json: 24 h, and --expiry 48 gives 48 h
#   AP-01   default_expiry_hours 2: 2 h
#   AP-02   2, --expiry 1: 1 h (the flag shortens)
#   AP-03   2, --expiry 48: 2 h, with a note (the flag cannot extend)
#   AP-04   CONTROL: a govern.json without the key, --expiry 48: 48 h
#   AP-05   24 written out, --expiry 48: 24 h (the config sets it)
#   AP-06   inherited through extends: 2 h
#   AP-07   0: the approval has already expired
#   AP-08   a govern.json that does not load: exit 4, no approval written
#   reload ratchet (naab-gov ratchet OLD NEW --ratchet-only)
#   RA-00   CONTROL: the same allowlist reloads
#   RA-01   blocklist -> allowlist: accepted (a tightening an entry diff refuses)
#   RA-02   allowlist -> blocklist: refused
#   RA-03   a module added: refused;   RA-04  CONTROL: one removed: accepted
#   RA-05   CONTROL: os -> os.path: accepted;   RA-06  os.path -> os: refused
#   RA-07   CONTROL: the restrictions form at soft -> the per-language form
#           (hard): accepted;   RA-08  the reverse: refused (no level key
#           changes, so only the allowlist comparison can see it)
#   RA-09   the allowlist moved to another language: refused
#   RB-01   a blocklist_extra entry removed: refused;   RB-02  CONTROL: added
#   a mid-run reload (signed, its own trust store; config_swap.sh)
#   RL-01c  CONTROL: a reload to a generous line total runs the last block
#   RL-01   a reload to a total the last block alone fits: refused, because
#           the blocks before the reload are counted
#
# Red first: every subject arm fails on a build of ec009c5e (observed
# 2026-11-18). Passing there, as they should: the controls, and MM-02, IA-06,
# AP-02 -- they hold the line against a loosening the old build could not make.
#
# KILL TABLE (each mutant built, this suite run, the arms that FAILED;
# observed 2026-11-18 on this change):
#   L1  every execution of a block counted again ................ PL-02
#   L2  the line total never enforced ........................... PL-01, RL-01
#   L3  lines counted only while a limit is set ................. RL-01
#   T1  the timer ignores the time left of the total ............ PT-01
#   T2  a block's time not charged to the total ................. PT-01, PT-02
#   T3  the timer armed in audit mode ........................... PT-02
#   M1  total_mb ignored ........................................ MM-01 (and naab_ratchet_property: 288 failures)
#   M2  total_mb raises the budget (max for min) ................ MM-01, MM-02 (and naab_ratchet_property: 1152)
#   X1  blocklist_extra not read ................................ BX-01, BX-03
#   X2  entries not escaped (a regular expression) .............. BX-02
#   X3  entries matched with case ............................... BX-03
#   I1  a computed import admitted .............................. IA-04
#   I2  the per-language allowlist not applied .................. IA-02, 03b, 03c, 04, 07, 09
#   I3  the restrictions allowlist not applied .................. IA-05
#   I4  checkImports() returns early unless a blocked list is set  IA-02, 03b, 03c, 04, 07, 09
#   I5  an entry admits its parent .............................. IA-03b, RA-06
#   I6  no separator boundary ("js" admits "json") .............. IA-03c
#   C1  the allowlist config check not called at load ........... IA-10..13
#   R1  an allowlist loosening not reported on reload ........... RA-02, 03, 06, 08, 09
#   R2  the reload comparison ignores the level ................. RA-08
#   R3  the reload comparison probes only an unlisted name ...... RA-03, RA-06
#   R4  the reload comparison refuses every change .............. RA-01, 04, 05, 07
#   R5  blocklist_extra's declared order removed ................ RB-01
#   A1  the tool timer not armed ................................ TT-01, TT-02
#   A2  a tool past the limit not refused ....................... TT-01, TT-02
#   A3  the tool timer armed in audit mode ...................... TT-03
#   A4  only a written tool_timeout_seconds binds ............... TT-02
#   A5  retries never capped .................................... RT-01
#   A6  the retry cap off by one ................................ RT-01, RT-02
#   P1  the configured approval lifetime not consulted .......... AP-01, 03, 05, 06, 07
#   P2  --expiry may extend it .................................. AP-03, AP-05
#   P3  only a lifetime above 0 binds ........................... AP-07
#   P4  a config that does not load is ignored .................. AP-08
#   P5  a lifetime written as the default ignored ............... AP-05
# Not mutated: IA-15 and the other controls (they guard against a gate that
# refuses too much; a mutant there would only show the suite does not
# over-refuse, which the controls already do).
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok(){ PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad(){ FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip(){ SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

T="$(mktemp -d)"
STUB_PORT=""
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup(){ stop_stub 2>/dev/null; type stop_swap_operators >/dev/null 2>&1 && stop_swap_operators; teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
unset NAAB_SIGNING_KEY NAAB_GOVERN_KEY

SEC='"version":"5.0","security":{"sandbox_level":"elevated"}'

# run NAME CONFIG_BODY PROGRAM [MODE] -> OUT RC SECS D
run() {
    D="$T/$1"; mkdir -p "$D"
    printf '{%s,"mode":"%s"%s}\n' "$SEC" "${4:-enforce}" "$2" > "$D/govern.json"
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$D/govern.json" 2>/dev/null \
        || { echo "  harness: $1 config is not JSON"; FAIL=$((FAIL+1)); }
    printf '%s\n' "$3" > "$D/p.naab"
    local s=$SECONDS
    OUT="$(cd "$D" && timeout 90s "$NAAB" run p.naab 2>&1)"; RC=$?
    SECS=$((SECONDS - s))
}
has(){ grep -qF -- "$1" <<<"$OUT"; }
show(){ grep -m3 -E "${1:-Error|error|refused|BLOCK}" <<<"$OUT" | cut -c1-160 | tr '\n' ' '; }

# ---------------------------------------------------------------------------
echo "=== polyglot lines: limits.code.max_total_polyglot_lines ==="
TWO='main {
  let a = <<javascript
var x = 1;
"one"
>>
  print("A=" + a)
  let b = <<javascript
var y = 2;
"two"
>>
  print("B=" + b)
}'
run PL00 '' "$TWO"
if [ $RC -eq 0 ] && has "A=one" && has "B=two"; then ok PL-00 "CONTROL: no limit, both blocks run"
else bad PL-00 "the program did not run (rc=$RC)" "$(show)"; fi
PL_LIVE=$([ $RC -eq 0 ] && has "B=two" && echo 1 || echo 0)
# The lines a block counts are what the engine reports; the limit is set from
# the program's own two blocks so the arms do not depend on how the trailing
# newline is counted.
run PL01 ',"limits":{"code":{"max_total_polyglot_lines":3}}' "$TWO"
PER_BLOCK=$(grep -oE 'this javascript block added [0-9]+' <<<"$OUT" | grep -oE '[0-9]+$' | head -1)
if [ "$PL_LIVE" != 1 ]; then skip PL-01 "the control did not run"
elif [ $RC -eq 3 ] && has "A=one" && ! has "B=two" && has "Polyglot code totals"; then
    ok PL-01 "a limit below the total refuses the second block (exit 3)"
else bad PL-01 "not refused (rc=$RC)" "$(show)"; fi
if [ -z "$PER_BLOCK" ]; then skip PL-01c "PL-01 reported no per-block count"
else
    run PL01c ",\"limits\":{\"code\":{\"max_total_polyglot_lines\":$((PER_BLOCK * 2))}}" "$TWO"
    if [ $RC -eq 0 ] && has "A=one" && has "B=two"; then ok PL-01c "CONTROL: a limit equal to the total ($((PER_BLOCK * 2)) lines) runs both"
    else bad PL-01c "refused at the limit (rc=$RC)" "$(show)"; fi
    run PL02 ",\"limits\":{\"code\":{\"max_total_polyglot_lines\":$PER_BLOCK}}" 'main {
  for i in 0..3 {
    let a = <<javascript
var x = 1;
"one"
>>
    print("L=" + a)
  }
  print("DONE")
}'
    n=$(grep -c '^L=one' <<<"$OUT")
    if [ $RC -eq 0 ] && [ "$n" -eq 3 ] && has DONE; then ok PL-02 "one block in a loop is counted once (3 runs under a one-block limit)"
    else bad PL-02 "a repeated block counted again (rc=$RC, runs=$n)" "$(show)"; fi
fi

# ---------------------------------------------------------------------------
echo "=== polyglot time: limits.timeout.total_polyglot ==="
# Two 2.5 s blocks under a 3 s total: the second is interrupted when 0.5 s of
# the total is left, so the run takes about 2 s less than the control, which
# runs both; run to its end and refused only afterwards, it would take as long
# as the control. Timed in milliseconds against the control's own time, so a
# slow start (Windows) moves both.
now_ms() { if [ -n "${EPOCHREALTIME:-}" ]; then local t="${EPOCHREALTIME/[.,]/}"; echo $((t / 1000)); else echo $((SECONDS * 1000)); fi; }
TWO_SLOW='main {
  for i in 0..2 {
    let r = <<javascript
var t = Date.now(); while (Date.now() - t < 2500) {}
"ran"
>>
    print("T" + string(i) + "=" + r)
  }
  print("DONE")
}'
t0=$(now_ms)
run PT00 '' "$TWO_SLOW"
el_ctl=$(( $(now_ms) - t0 ))
if [ $RC -eq 0 ] && has "T1=ran" && has DONE; then ok PT-00 "CONTROL: no total, both 2.5 s blocks run (${el_ctl} ms)"
else bad PT-00 "the program did not run (rc=$RC)" "$(show)"; fi
PT_LIVE=$([ $RC -eq 0 ] && has DONE && echo 1 || echo 0)
t0=$(now_ms)
run PT01 ',"limits":{"timeout":{"total_polyglot":3}}' "$TWO_SLOW"
el=$(( $(now_ms) - t0 ))
if [ "$PT_LIVE" != 1 ]; then skip PT-01 "the control did not run"
elif [ $RC -eq 3 ] && has "T0=ran" && ! has "T1=ran" && ! has DONE \
     && has "used the run's polyglot time" && [ $el -lt $((el_ctl - 1200)) ]; then
    ok PT-01 "a 3 s total refuses the second block, interrupted (exit 3, ${el} ms against the control's ${el_ctl})"
else bad PT-01 "not refused or not interrupted (rc=$RC, ${el} ms against the control's ${el_ctl})" "$(show)"; fi
run PT02 ',"limits":{"timeout":{"total_polyglot":3}}' "$TWO_SLOW" audit
if [ "$PT_LIVE" != 1 ]; then skip PT-02 "the control did not run"
elif [ $RC -eq 0 ] && has DONE && has "polyglot time"; then ok PT-02 "audit mode reports and stops nothing"
else bad PT-02 "audit mode wrong (rc=$RC)" "$(show)"; fi

# ---------------------------------------------------------------------------
echo "=== memory: limits.memory.total_mb ==="
MEMP='use process
main {
  let r = process.run("python3", ["-c", "b = bytearray(300 * 1024 * 1024); print(len(b) > 0)"])
  if r.get("exit_code", 1) == 0 { print("ALLOC_OK") } else { print("ALLOC_REFUSED") }
}'
run MM00 '' "$MEMP"
if [ $RC -eq 0 ] && has ALLOC_OK; then ok MM-00 "CONTROL: no memory setting, the allocation succeeds"
else bad MM-00 "the allocation failed without a limit (rc=$RC)" "$(show 'ALLOC|rror')"; fi
MM_BASE=$([ $RC -eq 0 ] && has ALLOC_OK && echo 1 || echo 0)
run MM00m ',"runtime":{"memory_limit":64}' "$MEMP"
MM_LIVE=0
if [ "$MM_BASE" = 1 ] && [ $RC -eq 0 ] && has ALLOC_REFUSED; then MM_LIVE=1
    ok MM-00m "CONTROL: runtime.memory_limit 64 refuses the allocation here"
else skip MM-00m "this platform applies no child memory limit (rc=$RC: $(show 'ALLOC|rror'))"; fi
run MM01 ',"limits":{"memory":{"total_mb":64}}' "$MEMP"
if [ "$MM_LIVE" != 1 ]; then skip MM-01 "MM-00m did not refuse"
elif [ $RC -eq 0 ] && has ALLOC_REFUSED; then ok MM-01 "total_mb 64 caps the child's budget"
else bad MM-01 "total_mb did not cap the budget (rc=$RC)" "$(show 'ALLOC|rror')"; fi
run MM02 ',"runtime":{"memory_limit":64},"limits":{"memory":{"total_mb":8192}}' "$MEMP"
if [ "$MM_LIVE" != 1 ]; then skip MM-02 "MM-00m did not refuse"
elif [ $RC -eq 0 ] && has ALLOC_REFUSED; then ok MM-02 "a larger total_mb does not raise runtime.memory_limit"
else bad MM-02 "total_mb raised the budget (rc=$RC)" "$(show 'ALLOC|rror')"; fi

# ---------------------------------------------------------------------------
echo "=== restrictions.dangerous_calls.blocklist_extra ==="
BXP='main {
  let r = <<javascript
Math.max(1, 2)
>>
  print("DONE")
}'
run BX00 ',"restrictions":{"dangerous_calls":{"level":"hard"}}' "$BXP"
if [ $RC -eq 0 ] && has DONE; then ok BX-00 "CONTROL: without the extra entry, Math.max runs"
else bad BX-00 "refused without the entry (rc=$RC)" "$(show)"; fi
BX_LIVE=$([ $RC -eq 0 ] && has DONE && echo 1 || echo 0)
run BX01 ',"restrictions":{"dangerous_calls":{"level":"hard","blocklist_extra":["Math.max"]}}' "$BXP"
if [ "$BX_LIVE" != 1 ]; then skip BX-01 "the control did not run"
elif [ $RC -eq 3 ] && ! has DONE && has "Math.max"; then ok BX-01 "a listed name refuses the block (exit 3)"
else bad BX-01 "not refused (rc=$RC)" "$(show)"; fi
run BX02 ',"restrictions":{"dangerous_calls":{"level":"hard","blocklist_extra":["Math.max"]}}' 'main {
  let r = <<javascript
var Mathxmax = 3;
Mathxmax
>>
  print("DONE")
}'
if [ $RC -eq 0 ] && has DONE; then ok BX-02 "CONTROL: the entry is literal text, not a pattern"
else bad BX-02 "\".\" matched as a pattern (rc=$RC)" "$(show)"; fi
run BX03 ',"restrictions":{"dangerous_calls":{"level":"hard","blocklist_extra":["MATH.MAX"]}}' "$BXP"
if [ "$BX_LIVE" != 1 ]; then skip BX-03 "the control did not run"
elif [ $RC -eq 3 ] && ! has DONE; then ok BX-03 "matched without case"
else bad BX-03 "not refused (rc=$RC)" "$(show)"; fi

# ---------------------------------------------------------------------------
echo "=== import allowlist ==="
pyprog() { printf 'main {\n  let r = <<python\n%s\n"RAN"\n>>\n  print("R=" + r)\n}\n' "$1"; }
PLA() { printf ',"languages":{"per_language":{"python":{"imports":{%s}}}}' "$1"; }
run IA00 '' "$(pyprog 'import os')"
if [ $RC -eq 0 ] && has "R=RAN"; then ok IA-00 "CONTROL: no allowlist, import os runs"; PY_LIVE=1
else skip IA-00 "no Python block runs here (rc=$RC: $(show))"; PY_LIVE=0; fi
ia_runs() {  # ID WHAT
    if [ "$PY_LIVE" != 1 ]; then skip "$1" "IA-00 did not run"
    elif [ $RC -eq 0 ] && has "R=RAN"; then ok "$1" "$2"
    else bad "$1" "refused (rc=$RC)" "$(show)"; fi
}
ia_refused() {  # ID WHAT TEXT
    if [ "$PY_LIVE" != 1 ]; then skip "$1" "IA-00 did not run"
    elif [ $RC -eq 3 ] && ! has "R=RAN" && has "$3"; then ok "$1" "$2"
    else bad "$1" "not refused (rc=$RC)" "$(show)"; fi
}
run IA01 "$(PLA '"mode":"allowlist","allowed":["json"]')" "$(pyprog 'import json')"
ia_runs IA-01 "CONTROL: a listed import runs"
run IA02 "$(PLA '"mode":"allowlist","allowed":["json"]')" "$(pyprog 'import os')"
ia_refused IA-02 "an unlisted import is refused (exit 3)" "Import not on the allowlist"
run IA03 "$(PLA '"mode":"allowlist","allowed":["os"]')" "$(pyprog 'import os.path')"
ia_runs IA-03 "CONTROL: an entry admits the modules nested under it"
run IA03b "$(PLA '"mode":"allowlist","allowed":["os.path"]')" "$(pyprog 'import os')"
ia_refused IA-03b "a nested entry does not admit its parent" "Import not on the allowlist"
run IA03c "$(PLA '"mode":"allowlist","allowed":["js"]')" "$(pyprog 'import json')"
ia_refused IA-03c "an entry admits only names nested at a separator (\"js\" is not \"json\")" "Import not on the allowlist"
run IA04 "$(PLA '"mode":"allowlist","allowed":["json"]')" "$(pyprog 'm = __import__("js" + "on")')"
ia_refused IA-04 "a computed import is refused" "computed import"
run IA05 ',"restrictions":{"imports":{"level":"hard","mode":"allowlist","allowed":{"python":["json"]}}}' "$(pyprog 'import os')"
ia_refused IA-05 "the restrictions.imports form refuses an unlisted import" "Import not on the allowlist"
run IA05c ',"restrictions":{"imports":{"level":"hard","mode":"allowlist","allowed":{"python":["json"]}}}' "$(pyprog 'import json')"
ia_runs IA-05c "CONTROL: the restrictions form runs a listed import"
run IA06 "$(PLA '"mode":"allowlist","allowed":["os"],"blocked":["os"]')" "$(pyprog 'import os')"
ia_refused IA-06 "the blocked list still applies in allowlist mode" "os"
run IA07 "$(PLA '"mode":"allowlist","allowed":["json"]')" "$(pyprog 'from os import path')"
ia_refused IA-07 "a from-import of an unlisted module is refused" "Import not on the allowlist"
JSA=',"languages":{"per_language":{"javascript":{"imports":{"mode":"allowlist","allowed":["lodash"]}}}}'
run IA08 "$JSA" 'main {
  let r = <<javascript
var x = 2;
"J" + x
>>
  print("R=" + r)
}'
if [ $RC -eq 0 ] && has "R=J2"; then ok IA-08 "CONTROL: a block that imports nothing runs under an allowlist"; JS_LIVE=1
else bad IA-08 "refused (rc=$RC)" "$(show)"; JS_LIVE=0; fi
run IA09 "$JSA" 'main {
  let r = <<javascript
var fs = require("fs");
"J"
>>
  print("R=" + r)
}'
if [ "$JS_LIVE" != 1 ]; then skip IA-09 "IA-08 did not run"
elif [ $RC -eq 3 ] && ! has "R=J" && has "Import not on the allowlist"; then ok IA-09 "require() of an unlisted module is refused"
else bad IA-09 "not refused by the allowlist (rc=$RC)" "$(show)"; fi
cfg_refused() {  # ID WHAT
    if [ $RC -eq 4 ] && ! has "R=RAN"; then ok "$1" "$2"
    else bad "$1" "the config loaded (rc=$RC)" "$(show)"; fi
}
run IA10 "$(PLA '"mode":"strict","allowed":["json"]')" "$(pyprog 'import json')"
cfg_refused IA-10 "an unknown mode refuses the config (exit 4)"
run IA11 ',"languages":{"per_language":{"shell":{"imports":{"mode":"allowlist","allowed":["x"]}}}}' "$(pyprog 'import json')"
cfg_refused IA-11 "an allowlist for a language whose imports cannot be listed refuses the config"
run IA12 ',"restrictions":{"imports":{"mode":"allowlist"}}' "$(pyprog 'import json')"
cfg_refused IA-12 "a restrictions allowlist that names no language refuses the config"
run IA13 ',"restrictions":{"imports":{"mode":"allowlist","allowed":{"any":["json"]}}}' "$(pyprog 'import json')"
cfg_refused IA-13 "the \"any\" key refuses the config"
run IA14 "$(PLA '"mode":"blocklist","blocked":["socket"]')" "$(pyprog 'import json')"
ia_runs IA-14 "CONTROL: mode \"blocklist\" written out loads and runs"
# Bound variables reach a block through declarations the ENGINE writes; the
# allowlist reads the program's imports, so a list without json must not
# refuse them (the inline block and codegen.run_with_args).
run IA15 ',"codegen":{"enabled":true}'"$(PLA '"mode":"allowlist","allowed":["os"]')" 'use codegen
main {
  let x = {"a": [1, 2], "b": "s"}
  let r = <<python[x]
str(len(x))
>>
  print("R=" + r)
  let c = codegen.run_with_args("python", "print(len(x))", {"x": [1, 2, 3]})
  print("C=" + string(c.get("output")))
}'
if [ "$PY_LIVE" != 1 ]; then skip IA-15 "IA-00 did not run"
elif [ $RC -eq 0 ] && has "R=2" && has "C=3"; then ok IA-15 "CONTROL: the engine's code for bound variables is not judged as the program's imports"
else bad IA-15 "refused (rc=$RC)" "$(show)"; fi

# ---------------------------------------------------------------------------
echo "=== the reload ratchet on the wired lists (naab-gov ratchet) ==="
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
# ratchet NAME OLD_BODY NEW_BODY -> OUT RC (0 accepted, 1 refused)
ratchet() {
    local d="$T/$1"; mkdir -p "$d/old" "$d/new"
    printf '{%s,"mode":"enforce"%s}\n' "$SEC" "$2" > "$d/old/govern.json"
    printf '{%s,"mode":"enforce"%s}\n' "$SEC" "$3" > "$d/new/govern.json"
    OUT="$("$NAAB_GOV" ratchet "$d/old/govern.json" "$d/new/govern.json" --ratchet-only 2>&1)"; RC=$?
}
r_ok() {  # ID WHAT
    if [ $RC -eq 0 ]; then ok "$1" "$2"; else bad "$1" "refused (rc=$RC)" "$(show 'loosen|rror|import')"; fi
}
r_no() {  # ID WHAT TEXT
    if [ $RC -eq 1 ] && has "$3"; then ok "$1" "$2"; else bad "$1" "accepted (rc=$RC)" "$(show 'loosen|tighten|rror')"; fi
}
if [ ! -x "$NAAB_GOV" ]; then
    for id in RA-00 RA-01 RA-02 RA-03 RA-04 RA-05 RA-06 RA-07 RA-08 RA-09 RB-01 RB-02; do
        skip "$id" "naab-gov not built"; done
else
    A_JSON="$(PLA '"mode":"allowlist","allowed":["json"]')"
    ratchet RA00 "$A_JSON" "$A_JSON";  r_ok RA-00 "CONTROL: the same allowlist reloads"
    ratchet RA01 '' "$A_JSON";         r_ok RA-01 "blocklist -> allowlist is a tightening (accepted)"
    ratchet RA02 "$A_JSON" "$(PLA '"mode":"blocklist"')"
    r_no RA-02 "allowlist -> blocklist is refused" "import allowlist (python)"
    ratchet RA03 "$A_JSON" "$(PLA '"mode":"allowlist","allowed":["json","os"]')"
    r_no RA-03 "adding a module is refused" "'os'"
    ratchet RA04 "$(PLA '"mode":"allowlist","allowed":["json","os"]')" "$A_JSON"
    r_ok RA-04 "removing a module is accepted"
    ratchet RA05 "$(PLA '"mode":"allowlist","allowed":["os"]')" "$(PLA '"mode":"allowlist","allowed":["os.path"]')"
    r_ok RA-05 "narrowing an entry to a nested module is accepted"
    ratchet RA06 "$(PLA '"mode":"allowlist","allowed":["os.path"]')" "$(PLA '"mode":"allowlist","allowed":["os"]')"
    r_no RA-06 "widening an entry to its parent is refused" "'os'"
    RS() { printf ',"restrictions":{"imports":{"level":"%s","mode":"allowlist","allowed":{"python":["json"]}}}' "$1"; }
    # The restrictions.imports block stays: its presence switches the check on,
    # and removing it is refused on its own (order present).
    ratchet RA07 "$(RS soft)" ',"restrictions":{"imports":{"level":"soft","mode":"blocklist"}}'"$A_JSON"
    r_ok RA-07 "the restrictions form at soft -> the per-language form (hard) is accepted"
    # The reverse of RA-07: no level key changes, so only the allowlist
    # comparison can see that an unlisted import is now refused at soft
    # (passable with an override) where it was refused at hard.
    ratchet RA08 ',"restrictions":{"imports":{"level":"soft","mode":"blocklist"}}'"$A_JSON" "$(RS soft)"
    r_no RA-08 "the per-language form (hard) -> the restrictions form at soft is refused" "import allowlist (python)"
    ratchet RA09 "$A_JSON" ',"languages":{"per_language":{"javascript":{"imports":{"mode":"allowlist","allowed":["x"]}}}}'
    r_no RA-09 "moving the allowlist to another language is refused" "import allowlist (python)"
    BXR() { printf ',"restrictions":{"dangerous_calls":{"level":"hard","blocklist_extra":%s}}' "$1"; }
    ratchet RB01 "$(BXR '["Math.max","eval2"]')" "$(BXR '["Math.max"]')"
    r_no RB-01 "removing a blocklist_extra entry is refused" "blocklist_extra"
    ratchet RB02 "$(BXR '["Math.max"]')" "$(BXR '["Math.max","eval2"]')"
    r_ok RB-02 "adding a blocklist_extra entry is accepted"
fi

# ---------------------------------------------------------------------------
echo "=== a reload that adds max_total_polyglot_lines counts what already ran ==="
# The total is counted whether or not a limit is set, so a mid-run reload that
# adds one (a tightening) judges every block the run has executed. A signed
# reload needs a trusted key, which these arms keep in their own trust store.
RL_STORE="$T/rl-trust"; mkdir -p "$RL_STORE"
RL_KEY="$T/rl-key.pem"
rl_sign() { (cd "$1" && NAAB_TRUST_STORE_DIR="$RL_STORE" NAAB_SIGNING_KEY="$RL_KEY" "$NAAB" --sign-governance >/dev/null 2>&1); }
source "$SCRIPT_DIR/../helpers/config_swap.sh"
RL_LIVE=1
NAAB_TRUST_STORE_DIR="$RL_STORE" "$NAAB" --keygen "$RL_KEY" >/dev/null 2>&1 \
    && NAAB_TRUST_STORE_DIR="$RL_STORE" "$NAAB" --trust-key "$RL_KEY.pub" >/dev/null 2>&1 || RL_LIVE=0
# rl_run NAME NEXT_LIMIT -> OUT RC
rl_run() {
    local d="$T/$1" n="$T/$1.next"; mkdir -p "$d" "$n"
    printf '{%s,"mode":"enforce"}\n' "$SEC" > "$d/govern.json"
    printf '{%s,"mode":"enforce","limits":{"code":{"max_total_polyglot_lines":%s}}}\n' "$SEC" "$2" > "$n/govern.json"
    rl_sign "$d"; rl_sign "$n"
    cat > "$d/r.naab" <<EOF
main {
    let a = <<javascript
var x = 1;
"one"
>>
    print("A=" + a)
    let s = <<python
import os, time
def _swap(src, dst):
    if os.path.exists(".swap_done"):
        os.remove(".swap_done")
    with open(".swap_req", "w") as f:
        f.write(src + "\t" + dst + "\n")
    open(".swap_req.ready", "w").close()
    for _ in range(1200):
        if os.path.exists(".swap_done"):
            return
        time.sleep(0.05)
    raise RuntimeError("config swap operator did not respond")
time.sleep(1)
_swap("$n/govern.json.sig", "$d/govern.json.sig")
_swap("$n/govern.json", "$d/govern.json")
"swapped"
>>
    print(s)
    let b = <<javascript
var y = 2;
"two"
>>
    print("B=" + b)
}
EOF
    start_swap_operator "$d"
    OUT="$(cd "$d" && NAAB_TRUST_STORE_DIR="$RL_STORE" timeout 120 "$NAAB" r.naab 2>&1)"; RC=$?
    stop_swap_operators
}
if [ "$RL_LIVE" != 1 ]; then skip RL-01c "could not make a signing key"; skip RL-01 "could not make a signing key"
else
    rl_run RL01c 1000
    if has swapped && [ $RC -eq 0 ] && has "B=two"; then RL_OK=1; ok RL-01c "CONTROL: a reload to a generous limit runs the last block"
    elif ! has swapped; then RL_OK=0; skip RL-01c "the config swap did not happen ($(show 'rror|swap'))"
    else RL_OK=0; bad RL-01c "rc=$RC" "$(show 'rror|Reload|BLOCK')"; fi
    # 6 lines: the last block alone fits, and so do the two JavaScript blocks
    # together (PL-01c: 3 lines each); with the swap block counted, they do not.
    rl_run RL01 6
    if [ "$RL_OK" != 1 ]; then skip RL-01 "RL-01c did not run"
    elif [ $RC -eq 3 ] && has swapped && ! has "B=two" && has "Polyglot code totals"; then
        ok RL-01 "a limit the last block alone fits is refused: the blocks before the reload count"
    else bad RL-01 "rc=$RC" "$(show 'rror|Reload|totals')"; fi
fi

# ---------------------------------------------------------------------------
echo "=== agents: tool_timeout_seconds and agent_dispatch.max_retries_per_run ==="
export FAKE_KEY_W="fake-key-w"
# stub_case NAME RESPONSES AGENT_EXTRA TOP_EXTRA TOOL_SECONDS [MODE]
#   -> OUT RC REQS TOOL_DONE (STUB_OK=0 when the stub did not start)
stub_case() {
    local W="$T/$1"; mkdir -p "$W"
    printf '{"responses": %s}\n' "$2" > "$W/fixture.json"
    STUB_OK=1
    start_stub "$W/fixture.json" "$W" || { STUB_OK=0; return; }
    printf '{"mode":"%s","security":{"sandbox_level":"elevated"},%s"agents":{"w":{"provider":"gemini","model":"m","api_base":"http://127.0.0.1:%s","api_key_env":"FAKE_KEY_W","max_tokens":100,"max_turns":20,"tools":["slow"]%s}}}\n' \
        "${6:-enforce}" "$4" "$STUB_PORT" "$3" > "$W/govern.json"
    cat > "$W/t.naab" <<N
use agent
use file
use time
fn slow(n) {
    let t = time.now_millis()
    while time.now_millis() - t < $5 * 1000 { let z = 1 }
    file.write("tool_done.txt", "x")
    return "ok"
}
main {
    agent.register_tool("slow", slow, {"description": "slow", "parameters": {"n": {"type": "integer", "description": "n"}}})
    let h = agent.create("w")
    let r = agent.send(h, "go")
    print("CONTENT=" + string(r.get("content")))
}
N
    OUT="$(cd "$W" && timeout 90 "$NAAB" t.naab 2>&1)"; RC=$?
    stop_stub
    REQS=$(ls "$W"/req_*.json 2>/dev/null | wc -l | tr -d ' ')
    TOOL_DONE=$([ -f "$W/tool_done.txt" ] && echo yes || echo no)
}
TOOLCALL='[{"tool_calls": [{"name": "slow", "args": {"n": 1}}]}, {"content": "FINAL"}]'
TOOLS_ON=', "tools_enabled": true, "retry": {"max_attempts": 1}'
stub_case TT01c "$TOOLCALL" "$TOOLS_ON, \"tool_timeout_seconds\": 60" '' 3
if [ "$STUB_OK" != 1 ]; then TT_LIVE=0; skip TT-01c "the agent stub did not start"
elif [ $RC -eq 0 ] && [ "$TOOL_DONE" = yes ] && has "CONTENT=FINAL"; then TT_LIVE=1
    ok TT-01c "CONTROL: under a 60 s limit the 3 s tool finishes"
else TT_LIVE=0; bad TT-01c "the tool did not finish (rc=$RC, done=$TOOL_DONE)" "$(show)"; fi
stub_case TT01 "$TOOLCALL" "$TOOLS_ON, \"tool_timeout_seconds\": 1" '' 3
if [ "$TT_LIVE" != 1 ]; then skip TT-01 "TT-01c did not run"
elif [ $RC -eq 3 ] && [ "$TOOL_DONE" = no ] && has "ran past its time limit" && ! has "CONTENT=FINAL"; then
    ok TT-01 "a tool past a 1 s limit is a HARD block and does not finish"
else bad TT-01 "not stopped (rc=$RC, done=$TOOL_DONE)" "$(show)"; fi
stub_case TT02 "$TOOLCALL" "$TOOLS_ON" '' 12
if [ "$TT_LIVE" != 1 ]; then skip TT-02 "TT-01c did not run"
elif [ $RC -eq 3 ] && [ "$TOOL_DONE" = no ] && has "ran past its time limit (10 seconds)"; then
    ok TT-02 "the key absent, the default 10 s applies"
else bad TT-02 "the default did not apply (rc=$RC, done=$TOOL_DONE)" "$(show)"; fi
stub_case TT03 "$TOOLCALL" "$TOOLS_ON, \"tool_timeout_seconds\": 1" '' 3 audit
if [ "$TT_LIVE" != 1 ]; then skip TT-03 "TT-01c did not run"
elif [ $RC -eq 0 ] && [ "$TOOL_DONE" = yes ] && has "CONTENT=FINAL"; then ok TT-03 "audit mode stops nothing"
else bad TT-03 "audit mode stopped the tool (rc=$RC, done=$TOOL_DONE)" "$(show)"; fi

FAILS='[{"status": 500, "error": "x"}, {"status": 500, "error": "x"}, {"status": 500, "error": "x"}, {"content": "OK"}]'
NO_TOOLS=', "tools_enabled": false, "retry": {"max_attempts": 5, "backoff_ms": 1, "jitter": false}'
stub_case RT00 "$FAILS" "$NO_TOOLS" '' 0
if [ "$STUB_OK" != 1 ]; then RT_LIVE=0; skip RT-00 "the agent stub did not start"
elif [ $RC -eq 0 ] && [ "$REQS" -eq 4 ] && has "CONTENT=OK"; then RT_LIVE=1; ok RT-00 "CONTROL: no cap, three retries, the reply arrives"
else RT_LIVE=0; bad RT-00 "rc=$RC requests=$REQS" "$(show)"; fi
stub_case RT01 "$FAILS" "$NO_TOOLS" '"agent_dispatch": {"max_retries_per_run": 1},' 0
if [ "$RT_LIVE" != 1 ]; then skip RT-01 "RT-00 did not run"
elif [ $RC -ne 0 ] && [ "$REQS" -eq 2 ] && ! has "CONTENT=OK"; then ok RT-01 "cap 1: one retry, then the call returns its error"
else bad RT-01 "rc=$RC requests=$REQS" "$(show)"; fi
stub_case RT02 "$FAILS" "$NO_TOOLS" '"agent_dispatch": {"max_retries_per_run": 3},' 0
if [ "$RT_LIVE" != 1 ]; then skip RT-02 "RT-00 did not run"
elif [ $RC -eq 0 ] && [ "$REQS" -eq 4 ] && has "CONTENT=OK"; then ok RT-02 "CONTROL: a cap of exactly three retries lets the reply arrive"
else bad RT-02 "rc=$RC requests=$REQS" "$(show)"; fi

# ---------------------------------------------------------------------------
echo "=== approval.default_expiry_hours ==="
AK="$T/approver.pem"
"$NAAB" --keygen "$AK" >/dev/null 2>&1
AP_LIVE=1
[ -f "$AK" ] || { AP_LIVE=0; }
# A govern.json in a directory above $T would be discovered by every arm.
d="$T"; while [ "$d" != "/" ] && [ -n "$d" ]; do d="$(dirname "$d")"; [ -f "$d/govern.json" ] && AP_LIVE=0; done
# approve NAME CONFIG|- [FLAGS...] -> OUT RC HOURS (rounded; "none" without a file)
approve() {
    local name="$1" cfg="$2"; shift 2
    D="$T/$name"; mkdir -p "$D"
    [ "$cfg" != "-" ] && printf '%s\n' "$cfg" > "$D/govern.json"
    OUT="$(cd "$D" && NAAB_SIGNING_KEY="$AK" timeout 30 "$NAAB" --approve some.rule "$@" 2>&1)"; RC=$?
    if [ -f "$D/.naab/approvals.json" ]; then
        HOURS=$(python3 -c 'import json,sys,time; t=json.load(sys.stdin)["some.rule"]["expiry_timestamp"]; print(round((t - time.time()) / 3600))' < "$D/.naab/approvals.json")
    else HOURS=none; fi
}
ap() {  # ID WANT WHAT
    if [ "$AP_LIVE" != 1 ]; then skip "$1" "no signing key, or a govern.json above the test directory"
    elif [ $RC -eq 0 ] && [ "$HOURS" = "$2" ]; then ok "$1" "$3"
    else bad "$1" "rc=$RC hours=$HOURS (want $2)" "$(show 'rror|longer|Approval')"; fi
}
AP2='{"version":"5.0","mode":"enforce","approval":{"default_expiry_hours":2}}'
approve AP00 -;                  ap AP-00 24 "CONTROL: no govern.json, 24 h"
approve AP00f - --expiry 48;     ap AP-00f 48 "CONTROL: no govern.json, --expiry 48 gives 48 h"
approve AP01 "$AP2";             ap AP-01 2 "the configured lifetime applies"
approve AP02 "$AP2" --expiry 1;  ap AP-02 1 "--expiry shortens it"
approve AP03 "$AP2" --expiry 48; ap AP-03 2 "--expiry cannot extend it"
if [ "$AP_LIVE" = 1 ] && ! has "longer than this project allows"; then bad AP-03n "no note that the flag was cut" "$(show)"; fi
approve AP04 '{"version":"5.0","mode":"enforce"}' --expiry 48
ap AP-04 48 "CONTROL: a config without the key leaves the flag alone"
approve AP05 '{"version":"5.0","mode":"enforce","approval":{"default_expiry_hours":24}}' --expiry 48
ap AP-05 24 "24 written out binds the flag"
mkdir -p "$T/AP06"; printf '%s\n' "$AP2" > "$T/AP06/parent.json"
approve AP06 '{"version":"5.0","mode":"enforce","extends":"./parent.json"}'
ap AP-06 2 "inherited through extends"
approve AP07 '{"version":"5.0","mode":"enforce","approval":{"default_expiry_hours":0}}'
ap AP-07 0 "a lifetime of 0 grants an approval that has already expired"
approve AP08 '{"version":"5.0","mode":"enforce","approval":{"default_expiry_hours":2},}'
if [ "$AP_LIVE" != 1 ]; then skip AP-08 "no signing key"
elif [ $RC -eq 4 ] && [ "$HOURS" = none ]; then ok AP-08 "a govern.json that does not load grants nothing (exit 4)"
else bad AP-08 "rc=$RC hours=$HOURS" "$(show)"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
