#!/usr/bin/env bash
# ============================================================
# Refusals a program could catch, and limits that refused nothing
# (A88, A168, A2 -- the owner's decision of 2026-11-14: "Governance block,
# wire up to enforce and tighten")
# ============================================================
# Three things, each observed before this change:
#
#   A88   every codegen rule (enabled, the language lists, the size and line
#         caps, the cumulative caps) refused codegen.run() with a plain error:
#         a try/catch swallowed it, the run ended "Governance: PASS", nothing
#         was recorded, and codegen.level (default hard) was read by nothing.
#   A168  agent.create() past exposure_tracking.max_unique_agents: the same.
#   A2    limits.timeout.per_block, languages.per_language.<lang>.timeout and
#         limits.code.max_lines_per_block were read by getters with no caller:
#         a 3 s block ran under a 1 s limit.
#
# Now each is decided by the engine through enforce(): codegen at
# codegen.level, agent creation at exposure_tracking.level, the three limits
# at HARD. A refusal still REFUSES at every level -- at advisory, detect, an
# overridden soft and in audit mode the code is not run and the agent is not
# created; only whether the run ends differs.
#
# The observable is a side effect, never the exit status alone: a marker file
# the refused code would have written, a line the program prints after its
# try/catch. Every refusal arm follows a CONTROL that the same program does
# its work when the setting allows it; a control that does not run here makes
# its arms UNMEASURABLE (no Python on build-windows, no shell executor).
#
#   codegen (A88)
#   RE-00       CONTROL: codegen allowed, the code runs and writes its marker
#   RE-01..07   one per rule, the call inside try/catch: exit 3, the catch does
#               not run, the marker is absent (07: the first call ran, the
#               second did not)
#   RE-08       level advisory: recorded as a warning, still refused, catchable
#   RE-09/09o   level soft: exit 3; with --governance-override still refused
#   RE-10       level detect: catchable, still refused
#   RE-11       mode audit: still refused
#   agent.create (A168)
#   RE-20       CONTROL: no limit, the second agent is created
#   RE-21       level hard: exit 3, the catch does not run
#   RE-22       level unset (advisory): a warning, still refused, catchable
#   RE-23       level soft with --governance-override: still refused
#   block time limit (A2)
#   RE-30       CONTROL: no limit, a 3 s Python block finishes
#   RE-31       limits.timeout.per_block 1: exit 3 inside try/catch, the block
#               did not finish, and the run took under 3 s (it was interrupted)
#   RE-32       languages.per_language.python.timeout 1: the same
#   RE-33       CONTROL: the language's own timeout (20) is used before
#               per_block (1), so the block finishes
#   RE-34/34c   a shell block; RE-35/35c a JavaScript block; RE-36/36c
#               codegen.run()
#   RE-37       mode audit: reported, the block finishes (audit stops nothing)
#   block line limit (A2)
#   RE-40       CONTROL: no limit, a 7-line block runs
#   RE-41       limits.code.max_lines_per_block 3: exit 3
#   RE-42       CONTROL: the language's own max_lines (50) is used first
#
# KILL TABLE (each mutant built, the suite run, the arm that FAILED; observed
# 2026-11-14 on 2fc53ea2 plus this change):
#   M1  refuse() in codegen_impl.cpp does not call the engine ...... RE-01..07, 08, 09
#   M2  refuse() returns after an advisory instead of throwing ..... RE-08, 09o, 10, 11
#   M3  refuseCodegen() enforces at ADVISORY, not codegen.level .... RE-01..07, 09
#   M4  agent.create does not call refuseAgentCreation() ........... RE-21, 22
#   M5  agent.create returns after the engine call (no throw) ...... RE-22, 23
#   M6  underBlockTimeLimit() does not arm its timer ............... RE-31, 32, 34, 35, 36
#   M7  underBlockTimeLimit() does not check the time after ........ RE-31, 32, 34, 35, 36, 37
#   M8  blockTimeLimitSeconds() ignores the language's timeout ..... RE-32, 33
#   M9  blockTimeLimitSeconds() ignores limits.timeout.per_block ... RE-31, 34, 35, 36, 37
#   M10 the VM's inline site not wrapped ........................... RE-31, 32, 34, 35, 37
#   M11 codegen.run()'s site not wrapped ........................... RE-36
#   M12 checkCodeSize() ignores limits.code.max_lines_per_block .... RE-41
#   M13 checkCodeSize() prefers per_block to the language's own .... RE-42
#   M14 the persistent executor's wait ignores the deadline ........ RE-34
#   M15 the timer armed in audit mode too .......................... RE-37
# And by setting removal on the test build (docs/settings-liveness.md):
#   NAAB=build-mut/naab-lang NAAB_DROP_SETTING=<setting> bash <this suite>
#   limits.timeout.per_block .......................... RE-31
#   languages.per_language.python.timeout ............. RE-32
#   limits.code.max_lines_per_block ................... RE-41
#   codegen.level ..................................... RE-08
#   exposure_tracking.level ........................... RE-21
#
# NOT HERE: codegen.max_nesting_depth, max_cumulative_calls_per_agent, the
# CODEGEN_EXEC action and allow_tainted_code go through the same refuse()
# call (traced) but have no fixture; the tree-walker's routes carry the same
# time limit (traced, not run here: it is refused under enforce).
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
cleanup(){ teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
export FAKE_KEY_RE=fake

SEC='"version":"4.0","security":{"sandbox_level":"elevated"}'

# run DIR CONFIG_BODY PROGRAM [FLAG] [MODE] -> OUT RC CFG_OK SECS D
run() {
    D="$T/$1"; mkdir -p "$D"; rm -f "$D"/M*
    printf '{%s,"mode":"%s"%s}\n' "$SEC" "${5:-enforce}" "$2" > "$D/govern.json"
    CFG_OK=0; python3 -c 'import json,sys; json.load(sys.stdin)' < "$D/govern.json" && CFG_OK=1
    printf '%s\n' "$3" > "$D/p.naab"
    local a=("$NAAB" run p.naab); [ -n "${4:-}" ] && a+=("$4")
    local s=$SECONDS
    OUT="$(cd "$D" && timeout 90s "${a[@]}" 2>&1)"; RC=$?
    SECS=$((SECONDS - s))
}
has(){ grep -qF -- "$1" <<<"$OUT"; }
mark(){ [ -e "$D/$1" ]; }
show(){ echo "cfg=$CFG_OK rc=$RC secs=$SECS markers=[$(cd "$D" && ls M* 2>/dev/null | tr '\n' ' ')] out=$(tail -c 300 <<<"$OUT" | tr '\n' ' ')"; }

# ------------------------------------------------------------------
echo "== codegen refusals go through the engine (A88)"
# Two calls. The first writes M1, the second (longer, more lines) writes M2.
CG='use codegen
main {
  try { let a = codegen.run("python", "open(\"M1\",\"w\").write(\"x\")\nx = 1\nx") } catch (e) { print("CAUGHT1") }
  try { let b = codegen.run("python", "open(\"M2\",\"w\").write(\"x\")\n# a longer second program\n# with\n# more\n# lines\ny = 2\ny") } catch (e) { print("CAUGHT2") }
  print("DONE")
}'
CG_OK=0
run re00 ',"codegen":{"enabled":true}' "$CG"
if [ "$CFG_OK" = 1 ] && [ "$RC" = 0 ] && has DONE && mark M1 && mark M2 && ! has CAUGHT; then
    ok "RE-00" "CONTROL: with codegen allowed both programs run and write their markers"; CG_OK=1
else
    skip "RE-00" "codegen.run(python) does not run here ($(show))"
fi

# cg ID what CONFIG TEXT  -- the FIRST call is refused: exit 3, nothing caught, no marker
cg_first() {
    if [ "$CG_OK" != 1 ]; then skip "$1" "no control"; return; fi
    run "$1" "$3" "$CG"
    if [ "$CFG_OK" = 1 ] && [ "$RC" = 3 ] && has "$4" && ! has CAUGHT && ! has DONE && ! mark M1 && ! mark M2; then
        ok "$1" "$2: a governance block (exit 3) a try/catch does not swallow, and the code did not run"
    else
        bad "$1" "$2 was not a governance block" "$(show)"
    fi
}
# the SECOND call is refused: the first ran, the second did not
cg_second() {
    if [ "$CG_OK" != 1 ]; then skip "$1" "no control"; return; fi
    run "$1" "$3" "$CG"
    if [ "$CFG_OK" = 1 ] && [ "$RC" = 3 ] && has "$4" && ! has CAUGHT && ! has DONE && mark M1 && ! mark M2; then
        ok "$1" "$2: the first program ran, the second is a governance block (exit 3) and did not run"
    else
        bad "$1" "$2 was not a governance block at the second call" "$(show)"
    fi
}
cg_first  RE-01 "codegen.enabled false"          ',"codegen":{"enabled":false}'                                  "not enabled"
cg_first  RE-02 "codegen.blocked_languages"      ',"codegen":{"enabled":true,"blocked_languages":["python"]}'    "is blocked for dynamic code"
cg_first  RE-03 "codegen.allowed_languages"      ',"codegen":{"enabled":true,"allowed_languages":["javascript"]}' "is not allowed for dynamic code"
cg_second RE-04 "codegen.max_code_size_bytes"    ',"codegen":{"enabled":true,"max_code_size_bytes":50}'          "code exceeds maximum size"
cg_second RE-05 "codegen.max_code_lines"         ',"codegen":{"enabled":true,"max_code_lines":4}'                "code exceeds maximum line count"
cg_second RE-06 "codegen.max_cumulative_calls"   ',"codegen":{"enabled":true,"max_cumulative_calls":1}'          "cumulative call limit reached"
cg_second RE-07 "codegen.max_cumulative_code_bytes" ',"codegen":{"enabled":true,"max_cumulative_code_bytes":60}' "cumulative code size limit reached"

# Levels: the run may go on, the code never runs.
BL='"enabled":true,"blocked_languages":["python"]'
if [ "$CG_OK" = 1 ]; then
    run re08 ",\"codegen\":{$BL,\"level\":\"advisory\"}" "$CG"
    if [ "$RC" = 0 ] && has CAUGHT1 && has CAUGHT2 && has DONE && ! mark M1 && ! mark M2 && has "WARNING codegen.blocked_languages"; then
        ok "RE-08" "level advisory: recorded as a warning, the program goes on, and the code still did not run"
    else bad "RE-08" "advisory codegen refusal: not (warned, refused, catchable)" "$(show)"; fi

    run re09 ",\"codegen\":{$BL,\"level\":\"soft\"}" "$CG"
    if [ "$RC" = 3 ] && ! has CAUGHT && ! mark M1; then
        ok "RE-09" "level soft: a governance block (exit 3)"
    else bad "RE-09" "soft codegen refusal did not block" "$(show)"; fi
    run re09o ",\"codegen\":{$BL,\"level\":\"soft\"}" "$CG" --governance-override
    if [ "$RC" = 0 ] && has CAUGHT1 && has DONE && ! mark M1 && ! mark M2; then
        ok "RE-09o" "level soft with --governance-override: the run goes on, the code still did not run"
    else bad "RE-09o" "an overridden soft codegen refusal ran the code or stopped the run" "$(show)"; fi

    run re10 ",\"codegen\":{$BL,\"level\":\"detect\"}" "$CG"
    if [ "$RC" = 0 ] && has CAUGHT1 && has DONE && ! mark M1 && ! mark M2; then
        ok "RE-10" "level detect: catchable, the code did not run"
    else bad "RE-10" "detect codegen refusal: not (catchable, refused)" "$(show)"; fi

    run re11 ",\"codegen\":{$BL}" "$CG" "" audit
    if [ "$RC" = 0 ] && has CAUGHT1 && has DONE && ! mark M1 && ! mark M2; then
        ok "RE-11" "mode audit: the run goes on, the code still did not run"
    else bad "RE-11" "audit mode ran refused codegen or stopped the run" "$(show)"; fi
else
    for a in RE-08 RE-09 RE-09o RE-10 RE-11; do skip "$a" "no control"; done
fi

# ------------------------------------------------------------------
echo "== agent.create past max_unique_agents (A168)"
AG='"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:9","api_key_env":"FAKE_KEY_RE","system_prompt":"You review billing invoices."'
AGENTS=",\"agents\":{\"a\":{$AG},\"b\":{$AG}}"
AP='use agent
main {
  let a = agent.create("a")
  try {
    let b = agent.create("b")
    print("CREATED2")
  } catch (e) { print("CAUGHT") }
  print("DONE")
}'
run re20 "$AGENTS" "$AP"
if [ "$CFG_OK" = 1 ] && [ "$RC" = 0 ] && has CREATED2 && has DONE; then
    ok "RE-20" "CONTROL: with no limit the second agent is created"
    run re21 ",\"exposure_tracking\":{\"max_unique_agents\":1,\"level\":\"hard\"}$AGENTS" "$AP"
    if [ "$RC" = 3 ] && has "max agent count reached" && ! has CAUGHT && ! has CREATED2 && ! has DONE; then
        ok "RE-21" "level hard: a governance block (exit 3) a try/catch does not swallow"
    else bad "RE-21" "the agent past max_unique_agents was not a governance block" "$(show)"; fi
    run re22 ",\"exposure_tracking\":{\"max_unique_agents\":1}$AGENTS" "$AP"
    if [ "$RC" = 0 ] && has "Governance warning" && has CAUGHT && ! has CREATED2 && has DONE; then
        ok "RE-22" "level unset (advisory): a warning is recorded and the agent is still not created"
    else bad "RE-22" "advisory agent refusal: not (warned, refused)" "$(show)"; fi
    run re23 ",\"exposure_tracking\":{\"max_unique_agents\":1,\"level\":\"soft\"}$AGENTS" "$AP" --governance-override
    if [ "$RC" = 0 ] && has CAUGHT && ! has CREATED2 && has DONE; then
        ok "RE-23" "level soft with --governance-override: the agent is still not created"
    else bad "RE-23" "an overridden soft refusal created the agent or stopped the run" "$(show)"; fi
else
    skip "RE-20" "agent.create does not run here ($(show))"
    for a in RE-21 RE-22 RE-23; do skip "$a" "no control"; done
fi

# ------------------------------------------------------------------
echo "== block time limit (A2)"
PY='main {
  try {
    let r = <<python
import time
t = time.time()
while time.time() - t < 3:
    pass
open("M","w").write("x")
1
>>
  } catch (e) { print("CAUGHT") }
  print("DONE")
}'
SH='main {
  try {
    let r = <<shell
sleep 3
touch M
echo hi
>>
  } catch (e) { print("CAUGHT") }
  print("DONE")
}'
JS='main {
  try {
    let r = <<javascript
var t = Date.now(); var n = 0;
while (Date.now() - t < 3000) { n = n + 1; }
n
>>
  } catch (e) { print("CAUGHT") }
  print("DONE")
}'
CGT='use codegen
main {
  try { let a = codegen.run("python", "import time\nt = time.time()\nwhile time.time() - t < 3:\n    pass\nopen(\"M\",\"w\").write(\"x\")\nx = 1\nx") } catch (e) { print("CAUGHT") }
  print("DONE")
}'
PB=',"limits":{"timeout":{"per_block":1}}'

# limited ID what CONTROL_CFG LIMIT_CFG PROGRAM NEEDS_MARKER
# control: runs to the end (and writes M when the program has one).
# limited: exit 3, not caught, M absent, and the run was cut short (< 3 s).
limited() {
    local id="$1"
    run "${id}c" "$3" "$5"
    if [ "$CFG_OK" != 1 ] || [ "$RC" != 0 ] || ! has DONE || has CAUGHT || { [ "$6" = 1 ] && ! mark M; }; then
        skip "${id}c" "the block does not run here ($(show))"; skip "$id" "no control"; return
    fi
    ok "${id}c" "CONTROL: without the limit the 3 s block finishes"
    run "$id" "$4" "$5"
    if [ "$CFG_OK" = 1 ] && [ "$RC" = 3 ] && has "ran past its time limit" && ! has CAUGHT && ! has DONE && ! mark M && [ "$SECS" -lt 3 ]; then
        ok "$id" "$2: stopped at its limit, a governance block (exit 3) a try/catch does not swallow"
    else
        bad "$id" "$2 did not stop the block as a governance block" "$(show)"
    fi
}
PY_OK=0
run re30 '' "$PY"
if [ "$CFG_OK" = 1 ] && [ "$RC" = 0 ] && has DONE && mark M && ! has CAUGHT; then
    ok "RE-30" "CONTROL: with no limit a 3 s Python block finishes"; PY_OK=1
    run re31 "$PB" "$PY"
    if [ "$RC" = 3 ] && has "ran past its time limit" && ! has CAUGHT && ! has DONE && ! mark M && [ "$SECS" -lt 3 ]; then
        ok "RE-31" "limits.timeout.per_block: the block is stopped at 1 s, a governance block (exit 3)"
    else bad "RE-31" "limits.timeout.per_block did not stop the block" "$(show)"; fi
    run re32 ',"languages":{"per_language":{"python":{"timeout":1}}}' "$PY"
    if [ "$RC" = 3 ] && has "ran past its time limit" && ! has CAUGHT && ! has DONE && ! mark M && [ "$SECS" -lt 3 ]; then
        ok "RE-32" "languages.per_language.python.timeout: the block is stopped at 1 s, a governance block (exit 3)"
    else bad "RE-32" "the language's timeout did not stop the block" "$(show)"; fi
    run re33 ',"limits":{"timeout":{"per_block":1}},"languages":{"per_language":{"python":{"timeout":20}}}' "$PY"
    if [ "$RC" = 0 ] && has DONE && mark M; then
        ok "RE-33" "CONTROL: the language's own timeout (20) is used before per_block (1): the block finishes"
    else bad "RE-33" "per_block overrode the language's own longer timeout" "$(show)"; fi
    run re37 "$PB" "$PY" "" audit
    if [ "$RC" = 0 ] && has DONE && mark M && has "ran past its time limit"; then
        ok "RE-37" "mode audit: the overrun is reported and the block finishes"
    else bad "RE-37" "audit mode: not (reported, block finished)" "$(show)"; fi
else
    skip "RE-30" "a Python block does not run here ($(show))"
    for a in RE-31 RE-32 RE-33 RE-37; do skip "$a" "no control"; done
fi
limited RE-34 "a shell block under per_block" ',"capabilities":{"shell":{"enabled":true}}' \
    ",\"capabilities\":{\"shell\":{\"enabled\":true}}$PB" "$SH" 1
limited RE-35 "a JavaScript block under per_block" '' "$PB" "$JS" 0
limited RE-36 "codegen.run() under per_block" ',"codegen":{"enabled":true}' ",\"codegen\":{\"enabled\":true}$PB" "$CGT" 1

# ------------------------------------------------------------------
echo "== block line limit (A2)"
if [ "$PY_OK" = 1 ]; then
    ok "RE-40" "CONTROL: with no limit the 7-line block runs (RE-30)"
    run re41 ',"limits":{"code":{"max_lines_per_block":3}}' "$PY"
    if [ "$RC" = 3 ] && has "block has 7 lines (max: 3)" && ! has CAUGHT && ! has DONE && ! mark M; then
        ok "RE-41" "limits.code.max_lines_per_block: a longer block is a governance block (exit 3)"
    else bad "RE-41" "limits.code.max_lines_per_block did not refuse a longer block" "$(show)"; fi
    run re42 ',"limits":{"code":{"max_lines_per_block":3}},"languages":{"per_language":{"python":{"max_lines":50}}}' "$PY"
    if [ "$RC" = 0 ] && has DONE && mark M; then
        ok "RE-42" "CONTROL: the language's own max_lines (50) is used before max_lines_per_block (3)"
    else bad "RE-42" "max_lines_per_block overrode the language's own limit" "$(show)"; fi
else
    for a in RE-40 RE-41 RE-42; do skip "$a" "no Python control"; done
fi

echo ""
echo "refusals enforced: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
