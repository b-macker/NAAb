#!/usr/bin/env bash
# test_gov_heal.sh -- naab-gov heal (Bundle 15 PR 4)
#
# `naab-gov heal TELEMETRY --config GOVERN.JSON` proposes one kind of edit: a
# level key the config WRITES as "advisory", whose rule repeated in a verified
# run of that config, raised to "soft". Report only. Exit 0 nothing to propose,
# 1 a proposal, 2 UNMEASURABLE (the evidence did not hold), 4 usage.
#
# Every telemetry file here is written by a real `naab-lang run`; the suite
# computes no hash and never re-seals an edited copy, so an edited file always
# fails the chain check (the owner's rule: no forged-chain demo). No arm reads
# a key variable.
#
# Arms:
#   HL-00   CONTROL: three advisory taint violations -> taint_tracking.level
#           proposed, exit 1, and the ratchet's own notice quoted
#   HL-00e  ... and the proposal does what it says: with that one edit made by
#           hand the same program is refused (exit 3) where it ran (exit 0)
#   HL-01   two violations: below --min 3 -> exit 0, nothing proposed
#   HL-01b  CONTROL: the same file with --min 2 -> proposed
#   HL-02   one event edited in a copy: the chain does not verify -> exit 2,
#           nothing proposed, in the text report and in --json
#   HL-03   a run with its last line (RunEnd) cut off -> exit 2
#   HL-04   the config given is not the one the run used -> exit 2
#   HL-05   two runs in one file under two configs: only the run under the
#           given config is counted, the other is named as skipped
#   HL-06   a run that accepted a mid-run reload is skipped
#   HL-06c  CONTROL: the same program with no reload is counted
#   HL-07   a repeated rule whose level the file does not write -> named, not
#           proposed, exit 0
#   HL-08   a "level" the engine does not read (the ratchet names no
#           tightening) -> named, not proposed
#   HL-08c  CONTROL: the same rule with the key where the engine reads it
#   HL-09   a shorthand level the ratchet refuses to change -> named with the
#           ratchet's reason, not proposed
#   HL-10   nothing is written: config, telemetry and the directory unchanged,
#           the scratch directory left empty
#   HL-11   --json parses and carries the proposal
#   HL-12   usage errors and a config that does not load exit 4
#   HL-13   three violations at "detect" are not counted or listed
#
# Kill table (each mutant built, suite re-run; observed 2026-11-14):
#   replay is not asked                          -> HL-02 HL-03 red
#   a chain that DIVERGED is accepted            -> HL-02 red
#   an unverifiable chain is accepted            -> HL-03 red
#   proposals despite a refusal                  -> HL-02 red (its --json half;
#       the text report hides them, and the first HL-02 read only the text)
#   the config identity is not compared          -> HL-04 HL-05 red
#   a reloaded run is counted                    -> HL-06 red
#   no usable run is fine                        -> HL-04 HL-06 red
#   --min is ignored                             -> HL-01 HL-05 red
#   non-advisory violations are counted          -> HL-13 red
#   an absent level key is proposed              -> HL-07 red
#   a key the ratchet does not name is proposed  -> HL-08 red
#   an edit the ratchet refuses is proposed      -> HL-09 red
#   the ratchet is not asked                     -> HL-00 HL-08 HL-09 red
#   the candidate is written beside the config   -> HL-10 red (the first HL-10
#       measured a directory an earlier arm had already healed in)
#   the proposed value is hard                   -> HL-00 red
#   --config is optional                         -> HL-12 red
#   an unknown option is ignored                 -> HL-12 red
#   the json drops the count                     -> HL-11 red
#   always a proposal                            -> HL-01..09 HL-13 red
#   events of every run are pooled               -> HL-05 red
# Not mutated: the branch for a written level that is not "advisory" while the
# events are (no real run found that produces it), and the plain-name filter on
# rule names and run ids (an arm for it would need an edited file, which fails
# the chain check first).
#
# No secret or PII value is written here.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="$(dirname "$NAAB")/naab-gov"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/config_swap.sh"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_healtest_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }
# An arm this PLATFORM cannot measure (Windows: the build registers no in-process
# Python, and the swap operator is a POSIX shell job). Counted and printed like
# any other; it fails the suite everywhere except on that platform.
UNM_PLATFORM=0; ON_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) ON_WINDOWS=1 ;; esac
unm_platform() { unm "$@"; [ "$ON_WINDOWS" -eq 1 ] && UNM_PLATFORM=$((UNM_PLATFORM+1)); return 0; }

echo "=== naab-gov heal (Bundle 15 PR 4) ==="
if [ ! -x "$NAAB" ] || [ ! -x "$GOV" ]; then
    unm HL-ALL "naab-lang or naab-gov is not built beside $NAAB"
    echo ""; echo "Results: 0 passed, 0 failed, 1 unmeasurable"; exit 1
fi
setup_isolated_trust
cleanup() { stop_swap_operators; teardown_isolated_trust; [ -n "${HL_KEEP:-}" ] && { echo "kept: $TMP"; return; }; rm -rf "$TMP"; }
trap cleanup EXIT
SCRATCH="$TMP/scratch"; mkdir -p "$SCRATCH"

TEL='"telemetry": {"enabled": true, "output_file": "tel.jsonl", "tamper_evidence": {"enabled": true}}'
HEAD='"mode": "enforce", "security": {"sandbox_level": "elevated"}'
TAINT_CFG="{ $HEAD, $TEL,
  \"taint_tracking\": {\"enabled\": true, \"level\": \"advisory\", \"sources\": [\"env.get\"], \"sinks\": [\"file.write\"], \"sanitizers\": []} }"

# taint_prog N -> a program that writes a tainted value N times
taint_prog() {
    printf 'use env\nuse file\nmain {\n    let a = env.get("PATH")\n'
    local i; for i in $(seq 1 "$1"); do printf '    file.write("o%s.txt", a)\n' "$i"; done
    printf '    print("done")\n}\n'
}
FN_PROG='use file
fn a() { file.write("a.txt", "x") }
fn b() { file.write("b.txt", "x") }
fn c() { file.write("c.txt", "x") }
main { a() b() c() print("done") }
'
# project DIR CONFIG-TEXT PROGRAM-TEXT -> the directory, config and program
project() { mkdir -p "$1"; printf '%s\n' "$2" > "$1/govern.json"; printf '%s' "$3" > "$1/prog.naab"; }
# run_prog DIR -> RUN_RC, output in DIR/run.out
run_prog() { RUN_RC=0; (cd "$1" && timeout 120s "$NAAB" run prog.naab > run.out 2>&1) || RUN_RC=$?; }
# heal DIR [ARGS...] -> OUT, RC   (telemetry DIR/tel.jsonl, config DIR/govern.json unless ARGS give --config)
heal() {
    local d="$1"; shift
    local cfg=(--config govern.json) a
    for a in "$@"; do [ "$a" = "--config" ] && cfg=(); done
    RC=0; OUT=$(cd "$d" && TMPDIR="$SCRATCH" timeout 120s "$GOV" heal "${HEAL_FILE:-tel.jsonl}" "${cfg[@]}" "$@" 2>&1) || RC=$?
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
advisories() {  # DIR RULE -> how many advisory RuleViolation events the file holds for RULE
    python3 -c '
import json, sys
n = 0
for line in sys.stdin:
    try: e = json.loads(line)
    except Exception: continue
    if e.get("event_type") == "RuleViolation" and e.get("level") == "advisory" and e.get("rule_name") == sys.argv[1]: n += 1
sys.stdout.write(str(n))' "$2" < "$1/tel.jsonl" 2>/dev/null || true
}

# ---- the control: a real run with three advisory violations ----------------------
A="$TMP/a"; project "$A" "$TAINT_CFG" "$(taint_prog 3)"; run_prog "$A"
A_N=$(advisories "$A" taint_tracking.sink_violation)
if [ "$RUN_RC" -ne 0 ] || [ "$A_N" != "3" ]; then
    unm HL-ALL "the fixture run did not record three advisory violations (rc=$RUN_RC, recorded=${A_N:-none}): $(tail -2 "$A/run.out" | tr '\n' ' ' | cut -c1-200)"
    echo ""; echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi
cp -a "$A" "$TMP/z"   # HL-10's copy: taken before any heal has run in it
heal "$A"
if [ "$RC" -eq 1 ] && has 'taint_tracking.level: "advisory" -> "soft"' && has "taint_tracking.sink_violation" \
   && has "the ratchet accepts it as a tightening: taint_tracking.level: advisory -> soft" && has "result: PROPOSED"; then
    pass HL-00 "CONTROL: three advisory violations -> taint_tracking.level advisory -> soft, the ratchet's notice quoted"
else fail HL-00 "a repeated advisory under its own config should be proposed" "rc=$RC: $(tail -6 <<<"$OUT" | tr '\n' ' ' | cut -c1-300)"; fi

# The proposal's effect, made by hand as a person would.
E="$TMP/e"; project "$E" "$(sed 's/"level": "advisory"/"level": "soft"/' <<<"$TAINT_CFG")" "$(taint_prog 3)"; run_prog "$E"
if [ "$RUN_RC" -eq 3 ] && [ ! -e "$E/o1.txt" ]; then
    pass HL-00e "with the proposed edit the same program is refused (exit 3, nothing written) where it ran before"
else fail HL-00e "the proposed key should govern the rule it is proposed for" "rc=$RUN_RC o1=$([ -e "$E/o1.txt" ] && echo written || echo absent)"; fi

# ---- the threshold ---------------------------------------------------------------
B="$TMP/b"; project "$B" "$TAINT_CFG" "$(taint_prog 2)"; run_prog "$B"
heal "$B"
if [ "$RC" -eq 0 ] && has "Proposed (0)" && has "result: NOTHING TO PROPOSE"; then
    pass HL-01 "two violations are below --min 3: nothing proposed, exit 0"
else fail HL-01 "two violations should not be 'repeated' at --min 3" "rc=$RC: $(tail -4 <<<"$OUT" | tr '\n' ' ' | cut -c1-200)"; fi
heal "$B" --min 2
if [ "$RC" -eq 1 ] && has 'taint_tracking.level: "advisory" -> "soft"'; then
    pass HL-01b "CONTROL: the same file with --min 2 is proposed (the run and its two events were read)"
else fail HL-01b "--min 2 should propose on two violations" "rc=$RC"; fi

# ---- the evidence must hold ------------------------------------------------------
# One event's rule name changed in a COPY; nothing is re-sealed, so the chain fails.
python3 -c '
import sys
out = []; done = False
for line in sys.stdin:
    if not done and "\"RuleViolation\"" in line and "taint_tracking.sink_violation" in line:
        line = line.replace("taint_tracking.sink_violation", "taint_tracking.sink_violatiox"); done = True
    out.append(line)
sys.stdout.write("".join(out))' < "$A/tel.jsonl" > "$A/edited.jsonl"
# --min 2: the edit leaves two events of the real rule, enough to propose from if the refusal did not stop it.
HEAL_FILE=edited.jsonl heal "$A" --json --min 2
e_json=$(python3 -c '
import json, sys
d = json.load(sys.stdin)
sys.stdout.write("ok" if d["result"] == "UNMEASURABLE" and d["proposals"] == [] and d["refused"] else "bad")' <<<"$OUT" 2>/dev/null || true)
e_json_rc=$RC
HEAL_FILE=edited.jsonl heal "$A"
if [ "$RC" -eq 2 ] && [ "$e_json_rc" -eq 2 ] && [ "$e_json" = "ok" ] && has "the telemetry chain does not verify" && ! has "Proposed (" && has "result: UNMEASURABLE"; then
    pass HL-02 "an edited copy: the chain does not verify, nothing is proposed, exit 2"
else fail HL-02 "an edited telemetry file must not yield a proposal" "rc=$RC: $(grep -A3 'did not hold' <<<"$OUT" | tr '\n' ' ' | cut -c1-240)"; fi
head -n -1 "$A/tel.jsonl" > "$A/cut.jsonl"
HEAL_FILE=cut.jsonl heal "$A"
if [ "$RC" -eq 2 ] && has "could not be verified" && ! has "Proposed ("; then
    pass HL-03 "a run with no RunEnd: the chain could not be verified, nothing is proposed, exit 2"
else fail HL-03 "a run cut before its RunEnd must not yield a proposal" "rc=$RC: $(grep -A3 'did not hold' <<<"$OUT" | tr '\n' ' ' | cut -c1-240)"; fi

# ---- the config must be the run's ------------------------------------------------
printf '%s\n' "$(sed 's/"sanitizers": \[\]/"sanitizers": ["string.trim"]/' <<<"$TAINT_CFG")" > "$A/other.json"
heal "$A" --config other.json
if [ "$RC" -eq 2 ] && has "no run in the file ran under this config" && has "ran under another config" && ! has "Proposed ("; then
    pass HL-04 "a config that is not the run's: the run is skipped by name, nothing proposed, exit 2"
else fail HL-04 "a run must not be read against another config" "rc=$RC: $(tail -8 <<<"$OUT" | tr '\n' ' ' | cut -c1-300)"; fi

# Two runs, one file, two configs: three violations under the first, two under the second.
M="$TMP/m"; project "$M" "$TAINT_CFG" "$(taint_prog 3)"; run_prog "$M"; m1=$RUN_RC
cp "$M/govern.json" "$M/first.json"
printf '%s\n' "$(sed 's/"sanitizers": \[\]/"sanitizers": ["string.trim"]/' <<<"$TAINT_CFG")" > "$M/govern.json"
taint_prog 2 > "$M/prog.naab"; run_prog "$M"
if [ "$m1" -ne 0 ] || [ "$RUN_RC" -ne 0 ] || [ "$(advisories "$M" taint_tracking.sink_violation)" != "5" ]; then
    unm HL-05 "the two runs did not record 3 + 2 violations (rc=$m1/$RUN_RC)"
else
    heal "$M"; second_rc=$RC; second_out="$OUT"
    heal "$M" --config first.json
    if [ "$second_rc" -eq 0 ] && grep -q -F "Runs under this config: 1" <<<"$second_out" && grep -q -F "ran under another config" <<<"$second_out" \
       && grep -q -F "Proposed (0)" <<<"$second_out" \
       && [ "$RC" -eq 1 ] && has "Runs under this config: 1" && has "(3 advisory violation(s): taint_tracking.sink_violation)"; then
        pass HL-05 "two runs under two configs: each config counts only its own run (2 -> nothing, 3 -> proposed), the other named"
    else fail HL-05 "events of a run under another config must not be counted" "second rc=$second_rc first rc=$RC: $(grep -F 'advisory violation' <<<"$OUT" | head -2 | tr '\n' ' ')"; fi
fi

# ---- a run that reloaded its config ----------------------------------------------
# The engine compares the config's modification time in whole seconds, so the
# swap is asked for more than a second after the load.
RELOAD_TAIL='    time.sleep(1.2)
    file.write(".swap_req", "SWAPDIR/tight.json\tSWAPDIR/govern.json\n")
    file.write(".swap_req.ready", "")
    let n = 0
    while n < 2000 {
        if file.exists(".swap_done") { break }
        time.sleep(0.05)
        n = n + 1
    }
    let r = <<javascript
1 + 1
>>
    print("done")
}
'
reload_prog() {  # $1 = yes to request the swap, $2 = the run directory
    printf 'use env\nuse file\nuse time\nmain {\n    let a = env.get("PATH")\n    file.write("o1.txt", a)\n    file.write("o2.txt", a)\n    file.write("o3.txt", a)\n'
    # The operator is a shell process outside the program: it gets full paths.
    if [ "$1" = yes ]; then printf '%s' "${RELOAD_TAIL//SWAPDIR/$2}"; else printf '    let r = <<javascript\n1 + 1\n>>\n    print("done")\n}\n'; fi
}
R="$TMP/r"; project "$R" "$TAINT_CFG" "$(reload_prog yes "$R")"
# The reload tightens something unrelated to the level, so it is accepted.
printf '%s\n' "$(sed 's/"sinks": \["file.write"\]/"sinks": ["file.write", "io.write_file"]/' <<<"$TAINT_CFG")" > "$R/tight.json"
start_swap_operator "$R"; run_prog "$R"; stop_swap_operators
RC0="$TMP/r0"; project "$RC0" "$TAINT_CFG" "$(reload_prog no)"; run_prog "$RC0"; r0_rc=$RUN_RC
accepted_reload=$(grep -c '"CONFIG_ADJUSTMENT"' "$R/tel.jsonl" 2>/dev/null || true)
if ! grep -q '"accepted":"true"' <<<"$(grep '"CONFIG_ADJUSTMENT"' "$R/tel.jsonl" 2>/dev/null)"; then
    unm_platform HL-06 "the run recorded no accepted reload to skip (CONFIG_ADJUSTMENT events: ${accepted_reload:-0}): $(tail -2 "$R/run.out" | tr '\n' ' ' | cut -c1-160)"
    unm_platform HL-06c "no reload run to compare against"
else
    cp "$R/tel.jsonl" "$TMP/r.tel"
    # The file on disk is now the reloaded config; the run's identity is the one it started under.
    printf '%s\n' "$TAINT_CFG" > "$R/govern.json"
    heal "$R"
    if [ "$RC" -eq 2 ] && has "accepted a mid-run reload" && ! has "Proposed ("; then
        pass HL-06 "a run that accepted a mid-run reload is skipped by name, and nothing is proposed from it"
    else fail HL-06 "events after a reload are not the given file's" "rc=$RC: $(tail -8 <<<"$OUT" | tr '\n' ' ' | cut -c1-300)"; fi
    heal "$RC0"
    if [ "$r0_rc" -eq 0 ] && [ "$RC" -eq 1 ] && has 'taint_tracking.level: "advisory" -> "soft"'; then
        pass HL-06c "CONTROL: the same program with no reload is counted and proposed"
    else fail HL-06c "the no-reload control should be proposed" "run rc=$r0_rc heal rc=$RC"; fi
fi

# ---- repeated, and not proposed --------------------------------------------------
FN_HEAD="{ $HEAD, $TEL,"
N1="$TMP/n1"; project "$N1" "$FN_HEAD \"capabilities\": {\"functions\": {\"default\": {\"allowed_actions\": []}}} }" "$FN_PROG"; run_prog "$N1"
N2="$TMP/n2"; project "$N2" "$FN_HEAD \"capabilities\": {\"level\": \"advisory\", \"functions\": {\"default\": {\"allowed_actions\": []}}} }" "$FN_PROG"; run_prog "$N2"
N3="$TMP/n3"; project "$N3" "$FN_HEAD \"capabilities\": {\"functions\": {\"level\": \"advisory\", \"default\": {\"allowed_actions\": []}}} }" "$FN_PROG"; run_prog "$N3"
if [ "$(advisories "$N1" capabilities.functions)" != "3" ] || [ "$(advisories "$N2" capabilities.functions)" != "3" ] || [ "$(advisories "$N3" capabilities.functions)" != "3" ]; then
    unm HL-07 "the function-capability fixture did not record three advisory violations"
    unm HL-08 "the function-capability fixture did not record three advisory violations"
    unm HL-08c "the function-capability fixture did not record three advisory violations"
else
    heal "$N1"
    if [ "$RC" -eq 0 ] && has "Proposed (0)" && has "capabilities.functions (3 advisory violation(s)): the config writes no level key for it"; then
        pass HL-07 "a repeated rule whose level the file does not write is named and not proposed"
    else fail HL-07 "an absent level key must not be proposed as an addition" "rc=$RC: $(grep -A2 'not proposed' <<<"$OUT" | tr '\n' ' ' | cut -c1-240)"; fi
    heal "$N2"
    if [ "$RC" -eq 0 ] && has "Proposed (0)" && has "capabilities.level (capabilities.functions): the ratchet accepts the edit but names no tightening"; then
        pass HL-08 "a 'level' the engine does not read is named and not proposed"
    else fail HL-08 "a key the ratchet does not name as tightened must not be proposed" "rc=$RC: $(grep -A2 'Proposed\|not proposed' <<<"$OUT" | tr '\n' ' ' | cut -c1-300)"; fi
    heal "$N3"
    if [ "$RC" -eq 1 ] && has 'capabilities.functions.level: "advisory" -> "soft"'; then
        pass HL-08c "CONTROL: the same rule with its level where the engine reads it is proposed"
    else fail HL-08c "the written, read level key should be proposed" "rc=$RC"; fi
fi

# A shorthand level. The check reads polyglot blocks, so this arm needs an executor.
SH_PROG='main {
    let a = <<python
import pdb
print("debug: a")
x = 1
x
>>
    let b = <<python
import pdb
print("debug: b")
y = 2
y
>>
    let c = <<python
import pdb
print("debug: c")
z = 3
z
>>
    print("done")
}
'
S="$TMP/s"; project "$S" "$FN_HEAD \"code_quality\": {\"no_debug_artifacts\": \"advisory\"} }" "$SH_PROG"; run_prog "$S"
if [ "$(advisories "$S" code_quality.no_debug_artifacts)" != "3" ]; then
    unm_platform HL-09 "the shorthand fixture did not record three advisory violations (rc=$RUN_RC, recorded=$(advisories "$S" code_quality.no_debug_artifacts))"
else
    heal "$S"
    if [ "$RC" -eq 0 ] && has "Proposed (0)" && has "code_quality.no_debug_artifacts (code_quality.no_debug_artifacts): the ratchet refuses advisory -> soft here (code_quality.no_debug_artifacts:"; then
        pass HL-09 "a shorthand level the ratchet refuses to change is named with the ratchet's reason, not proposed"
    else fail HL-09 "an edit the ratchet refuses must not be proposed" "rc=$RC: $(grep -A2 'Proposed\|not proposed' <<<"$OUT" | tr '\n' ' ' | cut -c1-300)"; fi
fi

# ---- only ADVISORY violations count ------------------------------------------------
# The same three violations at "detect" (each caught by the program): repeated,
# and not this command's business.
DET_PROG='use env
use file
main {
    let a = env.get("PATH")
    try { file.write("o1.txt", a) } catch (e) { print("caught 1") }
    try { file.write("o2.txt", a) } catch (e) { print("caught 2") }
    try { file.write("o3.txt", a) } catch (e) { print("caught 3") }
    print("done")
}
'
D="$TMP/d"; project "$D" "$(sed 's/"level": "advisory"/"level": "detect"/' <<<"$TAINT_CFG")" "$DET_PROG"; run_prog "$D"
d_n=$(python3 -c '
import json, sys
n = 0
for line in sys.stdin:
    try: e = json.loads(line)
    except Exception: continue
    if e.get("event_type") == "RuleViolation" and e.get("level") == "detect" and e.get("rule_name") == "taint_tracking.sink_violation": n += 1
sys.stdout.write(str(n))' < "$D/tel.jsonl" 2>/dev/null || true)
if [ "${d_n:-0}" -lt 3 ]; then
    unm HL-13 "the detect fixture did not record three violations (rc=$RUN_RC, recorded=${d_n:-none})"
else
    heal "$D"
    if [ "$RC" -eq 0 ] && has "Proposed (0)" && ! has "not proposed" && ! has "taint_tracking.sink_violation"; then
        pass HL-13 "three violations at \"detect\" are not advisory: not counted, not listed"
    else fail HL-13 "a violation that is not advisory must not be counted" "rc=$RC: $(grep -A2 'Proposed\|not proposed' <<<"$OUT" | tr '\n' ' ' | cut -c1-240)"; fi
fi

# ---- nothing is written ----------------------------------------------------------
sum() { (cd "$1" && find . -type f ! -name run.out | LC_ALL=C sort | while read -r f; do printf '%s ' "$f"; cksum < "$f"; done) | cksum; }
Z="$TMP/z"; before=$(sum "$Z"); heal "$Z"; after=$(sum "$Z")
left=$(find "$SCRATCH" -mindepth 1 | head -3 | tr '\n' ' ')
if [ "$RC" -eq 1 ] && [ "$before" = "$after" ] && [ -z "$left" ]; then
    pass HL-10 "nothing is written: config, telemetry and directory unchanged, the scratch directory empty"
else fail HL-10 "heal is report only" "rc=$RC changed=$([ "$before" = "$after" ] && echo no || echo yes) left='$left'"; fi

# ---- json, usage -----------------------------------------------------------------
heal "$A" --json
J=$(python3 -c '
import json, sys
d = json.load(sys.stdin)
p = d["proposals"]
ok = d["result"] == "PROPOSED" and len(p) == 1 and p[0]["key"] == "taint_tracking.level" and p[0]["from"] == "advisory" \
     and p[0]["to"] == "soft" and p[0]["count"] == 3 and p[0]["ratchet"] == "accepted" and d["refused"] == [] and len(d["runs_used"]) == 1
sys.stdout.write("ok" if ok else "bad")' <<<"$OUT" 2>/dev/null || true)
if [ "$RC" -eq 1 ] && [ "$J" = "ok" ]; then pass HL-11 "--json parses and carries the proposal, its count and the run used"
else fail HL-11 "--json should carry the proposal" "rc=$RC verdict=${J:-unparsed}"; fi

u1=0; (cd "$A" && "$GOV" heal tel.jsonl >/dev/null 2>&1) || u1=$?
u2=0; (cd "$A" && "$GOV" heal --config govern.json >/dev/null 2>&1) || u2=$?
u3=0; (cd "$A" && "$GOV" heal tel.jsonl --config govern.json --apply >/dev/null 2>&1) || u3=$?
u4=0; (cd "$A" && "$GOV" heal missing.jsonl --config govern.json >/dev/null 2>&1) || u4=$?
printf '{ "mode": ' > "$A/broken.json"
u5=0; (cd "$A" && "$GOV" heal tel.jsonl --config broken.json >/dev/null 2>&1) || u5=$?
u6=0; (cd "$A" && "$GOV" heal tel.jsonl --config govern.json --min 1 >/dev/null 2>&1) || u6=$?
if [ "$u1" -eq 4 ] && [ "$u2" -eq 4 ] && [ "$u3" -eq 4 ] && [ "$u4" -eq 4 ] && [ "$u5" -eq 4 ] && [ "$u6" -eq 4 ]; then
    pass HL-12 "usage errors exit 4 (no config, no file, an unknown option, a missing file, a config that is not JSON, --min 1)"
else fail HL-12 "usage errors should exit 4" "noconfig=$u1 nofile=$u2 option=$u3 missing=$u4 broken=$u5 min1=$u6"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq "$UNM_PLATFORM" ]
