#!/usr/bin/env bash
# ============================================================
# test_no_effect_settings.sh -- a setting that enforces nothing says so
#
# Settings-liveness step 3 (docs/settings-liveness.md, A171). The owner's
# decision of 2026-11-16: of the template settings that look like protection
# and stop nothing, the ones with no clear meaning, no detector, or a working
# setting that does the job leave the template and naab-lang init, and the
# loader warns when a config sets one (warnNoEffectSettings() in
# governance_config.cpp). The settings that guard nothing at all (formatting,
# loader options, sub-options) leave quietly. Both groups are listed in
# tests/governance_v4/template_coverage_baseline.txt; this suite reads the
# WARNED group from there, so the loader's table and the record are compared,
# not copied.
#
#   NE-00   CONTROL: a config with none of them, and one with a working sibling
#           in the same sections, prints no such warning
#   NE-01   each WARNED setting, set alone, is named by exactly one warning
#   NE-02   all of them at once: the warned names are exactly the WARNED group
#   NE-03   the shipped template runs Hello World with no such warning
#   NE-04   no init preset writes a setting from either step-3 group
#   NE-05*  the warning tells the truth: the setting at a value that must trip
#           changes nothing, while a working setting with the same meaning
#           refuses the same program in the same harness. A control that does
#           not fire makes its arm UNMEASURABLE, never a pass.
#     a  limits.execution.total_executions   vs limits.execution.polyglot_blocks
#     b  code_injection.block_ldap/xpath/template_injection
#                                            vs block_sql_injection_patterns
#     c  polyglot.parallel.timeout_per_block vs limits.timeout.per_block
#     d  polyglot.output.max_output_lines, restrictions.polyglot_output.max_size
#                                            vs limits.data.output_size
#     e  polyglot.output.require_json_pipe, polyglot_output.require_structured /
#        validate_json                       vs restrictions.polyglot_output: "json"
#     f  audit.log_events.checks_passed/checks_failed: the trail holds no check
#        entry, while polyglot_executed writes one in the same run
#     g  limits.memory.per_block_mb          vs runtime.memory_limit (a child
#        process allocating 300 MB; UNMEASURABLE where rlimits do not bind)
#   Traced only, no arm: input_size (no defined meaning), max_parallel_blocks,
#   the persistent_runtime limits, max_bound_variables, require_naab_return,
#   validate_encoding, record_decisions, record_proof_objects -- each has no
#   reader outside the loader, the extends merge and the ratchet.
#
# KILL TABLE (each mutant run by hand on 2026-11-16; each turned its arm red)
#   K1 the warnNoEffectSettings() call removed           -> NE-01, NE-02
#   K2 the input_size entry removed from the table        -> NE-01[input_size], NE-02
#   K3 warn whether or not the setting is present         -> NE-00
#   K4 init writes limits.execution.total_executions again -> NE-04
#   K5 the template lists audit.log_events.checks_failed  -> NE-03
#   K6 total_executions loaded as polyglot_blocks         -> NE-05a
#   K7 the baseline's WARNED group gains a setting the loader does not warn
#      about (harness mutant)                             -> NE-01, NE-02
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
BASELINE="$SCRIPT_DIR/template_coverage_baseline.txt"
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/naab_noeffect_XXXXXX")"
PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs; exit 3 must mean the gate
cleanup() { teardown_isolated_trust; [ -n "${NE_KEEP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

WARN_RE='is parsed but not enforced -- no check reads it'

# The two step-3 groups, read from the baseline by their headings.
group() {  # HEADING-PREFIX -> the keys listed under it
    python3 -I - "$BASELINE" "$1" <<'PY'
import sys
lines = open(sys.argv[1], encoding="utf-8").read().split("\n")
on = False
for l in lines:
    if l.startswith("# --- "):
        on = l.startswith(sys.argv[2])
        continue
    if on and l.strip() and not l.startswith("#"):
        print(l.strip())
PY
}
WARNED="$(group '# --- WARNED, settings-liveness step 3')"
MOVED="$(group '# --- MOVED OUT, settings-liveness step 3')"
NW=$(grep -c . <<<"$WARNED"); NM=$(grep -c . <<<"$MOVED")
if [ "$NW" -lt 20 ] || [ "$NM" -lt 40 ]; then
    echo "  the baseline's step-3 groups were not found (warned=$NW moved=$NM)"; exit 1
fi

# json_with KEY VALUE -> a JSON object with that dotted key set
json_with() { python3 -I -c '
import json, sys
o = {}; cur = o; parts = sys.argv[1].split(".")
for p in parts[:-1]: cur = cur.setdefault(p, {})
cur[parts[-1]] = json.loads(sys.argv[2])
print(json.dumps(o))' "$1" "$2"; }

# mk DIR EXTRA_JSON -- an ordinary enforce config with EXTRA merged in
mk() {
    mkdir -p "$1"
    python3 -I -c '
import json, sys
base = {"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}}
def merge(a, b):
    for k, v in b.items():
        if isinstance(v, dict) and isinstance(a.get(k), dict): merge(a[k], v)
        else: a[k] = v
merge(base, json.loads(sys.argv[1] or "{}"))
print(json.dumps(base, indent=1))' "${2:-}" > "$1/govern.json"
}
run() {  # DIR PROGRAM -> OUT, RC
    printf '%s\n' "$2" > "$1/p.naab"
    OUT="$(cd "$1" && "$NAAB" run p.naab 2>&1)"; RC=$?
}
HELLO='main { print("HELLO_OK") }'
warned_names() { grep -o "\"[a-z_.]*\" $WARN_RE" <<<"$OUT" | sed 's/^"\([^"]*\)".*/\1/' | sort -u; }

echo "=== a setting that enforces nothing says so (settings-liveness step 3) ==="

# --- NE-00: no warning without them ---------------------------------------
d="$TEST_TMP/ne00"; mk "$d" '{"limits": {"execution": {"polyglot_blocks": 50}, "data": {"output_size": 1000000}}, "restrictions": {"code_injection": {"enabled": true, "block_sql_injection_patterns": true}, "polyglot_output": {"format": "any"}}, "audit": {"log_events": {"polyglot_executed": false}}}'
run "$d" "$HELLO"
if [ "$RC" = 0 ] && grep -q HELLO_OK <<<"$OUT" && ! grep -q "$WARN_RE" <<<"$OUT"; then
    pass NE-00 "CONTROL: working siblings in the same sections draw no no-effect warning"
else
    fail NE-00 "no warning expected" "rc=$RC: $(grep "$WARN_RE" <<<"$OUT" | head -2 | tr '\n' ' ')"
fi

# --- NE-01: each, alone ---------------------------------------------------
bad=""; n=0
while read -r k; do
    [ -z "$k" ] && continue
    n=$((n+1)); d="$TEST_TMP/ne01_$n"; mk "$d" "$(json_with "$k" 1)"
    run "$d" "$HELLO"
    names="$(warned_names)"
    if [ "$names" != "$k" ] || [ "$(grep -c "\"$k\" $WARN_RE" <<<"$OUT")" != 1 ] || ! grep -q HELLO_OK <<<"$OUT"; then
        bad="$bad $k[rc=$RC named=$(tr '\n' ',' <<<"$names")]"
    fi
done <<<"$WARNED"
if [ -z "$bad" ]; then pass NE-01 "each of the $NW WARNED settings, set alone, is named by exactly one warning"
else fail NE-01 "a WARNED setting is not named by exactly one warning" "$bad"; fi

# --- NE-02: all at once ---------------------------------------------------
all="{}"
while read -r k; do
    [ -z "$k" ] && continue
    all="$(python3 -I -c '
import json, sys
o = json.loads(sys.argv[1]); cur = o; parts = sys.argv[2].split(".")
for p in parts[:-1]: cur = cur.setdefault(p, {})
cur[parts[-1]] = 1
print(json.dumps(o))' "$all" "$k")"
done <<<"$WARNED"
d="$TEST_TMP/ne02"; mk "$d" "$all"; run "$d" "$HELLO"
got="$(warned_names)"; want="$(sort -u <<<"$WARNED")"
if [ "$got" = "$want" ]; then pass NE-02 "all $NW at once: the warned names are exactly the baseline's WARNED group"
else fail NE-02 "warned names differ from the WARNED group" "$(diff <(echo "$want") <(echo "$got") | tr '\n' ' ' | cut -c1-300)"; fi

# --- NE-03: the shipped template ------------------------------------------
# Its "extends" names a file that is not shipped, so it is removed first
# (as test_nested_unknown_keys.sh does); nothing else changes.
d="$TEST_TMP/ne03"; mkdir -p "$d"
python3 -I -c '
import json, sys
j = json.load(open(sys.argv[1], encoding="utf-8")); j.pop("extends", None)
json.dump(j, open(sys.argv[2], "w", encoding="utf-8"), indent=1)' "$REPO/govern-template.json" "$d/govern.json"
run "$d" "$HELLO"
if grep -q HELLO_OK <<<"$OUT" && ! grep -q "$WARN_RE" <<<"$OUT"; then
    pass NE-03 "the shipped template runs Hello World with no no-effect warning"
elif ! grep -q HELLO_OK <<<"$OUT"; then
    unm NE-03 "the template did not run Hello World here (rc=$RC): $(tail -2 <<<"$OUT" | tr '\n' ' ' | cut -c1-160)"
else
    fail NE-03 "the template sets a no-effect setting" "$(grep "$WARN_RE" <<<"$OUT" | head -3 | tr '\n' ' ')"
fi

# --- NE-04: init presets --------------------------------------------------
bad=""
for preset in relaxed standard strict paranoid; do
    d="$TEST_TMP/ne04_$preset"; mkdir -p "$d"
    (cd "$d" && "$NAAB" init --preset "$preset" --governance >/dev/null 2>&1)
    if [ ! -f "$d/govern.json" ]; then bad="$bad $preset[no govern.json]"; continue; fi
    hits="$(python3 -I - "$d/govern.json" <<PY
import json, sys
j = json.load(open(sys.argv[1], encoding="utf-8"))
keys = """$WARNED
$MOVED""".split()
for k in keys:
    o = j; ok = True
    for p in k.split("."):
        if isinstance(o, dict) and p in o: o = o[p]
        else: ok = False; break
    if ok: print(k)
PY
)"
    [ -n "$hits" ] && bad="$bad $preset[$(tr '\n' ',' <<<"$hits")]"
done
if [ -z "$bad" ]; then pass NE-04 "no init preset writes a setting from either step-3 group ($((NW+NM)) settings)"
else fail NE-04 "init writes a step-3 setting" "$bad"; fi

# --- NE-05: the warning tells the truth -----------------------------------
# truth ID WHAT CONTROL_EXTRA SUBJECT_EXTRA PROGRAM
#   the control config must refuse the program (exit 3); the subject config
#   must run it (exit 0) -- the setting changed nothing.
truth() {
    local id="$1" what="$2" cextra="$3" sextra="$4" prog="$5"
    local dc="$TEST_TMP/${id}_c" ds="$TEST_TMP/${id}_s"
    mk "$dc" "$cextra"; run "$dc" "$prog"; local crc=$RC cout="$OUT"
    if [ "$crc" != 3 ]; then
        unm "$id" "$what: the control did not refuse (rc=$crc): $(tail -2 <<<"$cout" | tr '\n' ' ' | cut -c1-160)"; return
    fi
    mk "$ds" "$sextra"; run "$ds" "$prog"
    if [ "$RC" = 0 ] && grep -q DONE <<<"$OUT"; then
        pass "$id" "$what: changes nothing (exit 0), while the control refuses the same program"
    else
        fail "$id" "$what: the warning says no check reads it, but the run changed" "rc=$RC: $(tail -2 <<<"$OUT" | tr '\n' ' ' | cut -c1-200)"
    fi
}

THREE='main {
  let a = <<javascript
1 + 1
>>
  let b = <<javascript
2 + 2
>>
  let c = <<javascript
3 + 3
>>
  print("DONE")
}'
truth NE-05a "limits.execution.total_executions: 1, three blocks" \
    '{"limits": {"execution": {"polyglot_blocks": 1}}}' \
    '{"limits": {"execution": {"total_executions": 1}}}' "$THREE"

INJ_SQL='main {
  let r = <<javascript
var user = "x";
var q = "SELECT * FROM t WHERE name = " + "'"'"'" + user;
q.length
>>
  print("DONE")
}'
INJ_OTHER='main {
  let r = <<javascript
var user = "x)(|(uid=*";
var q = "(uid=" + user + ")";
var t = "{{" + user + "}}";
var x = "//user[name=" + user + "]";
(q + t + x).length
>>
  print("DONE")
}'
# the control runs a different program (SQL) on purpose: it proves the
# injection check is live in this harness; the subject program is shaped like
# the three injections the switches name.
dc="$TEST_TMP/NE-05b_c"; mk "$dc" '{"restrictions": {"code_injection": {"enabled": true}}}'; run "$dc" "$INJ_SQL"
if [ "$RC" != 3 ]; then
    unm NE-05b "block_ldap/xpath/template_injection: the SQL control did not refuse (rc=$RC)"
else
    ds="$TEST_TMP/NE-05b_s"; mk "$ds" '{"restrictions": {"code_injection": {"enabled": true, "block_ldap_injection": true, "block_xpath_injection": true, "block_template_injection": true}}}'
    run "$ds" "$INJ_OTHER"; rc_on=$RC
    ds2="$TEST_TMP/NE-05b_s2"; mk "$ds2" '{"restrictions": {"code_injection": {"enabled": true, "block_ldap_injection": false, "block_xpath_injection": false, "block_template_injection": false}}}'
    run "$ds2" "$INJ_OTHER"; rc_off=$RC
    if [ "$rc_on" = 0 ] && [ "$rc_off" = 0 ]; then
        pass NE-05b "block_ldap/xpath/template_injection: on and off run the same program alike (exit 0), while SQL concatenation is refused"
    else
        fail NE-05b "block_ldap/xpath/template_injection changed the run" "on=$rc_on off=$rc_off"
    fi
fi

SLOW='main {
  let r = <<javascript
var t = Date.now(); while (Date.now() - t < 2500) {}
1
>>
  print("DONE")
}'
truth NE-05c "polyglot.parallel.timeout_per_block: 1, a 2.5 s block" \
    '{"limits": {"timeout": {"per_block": 1}}}' \
    '{"polyglot": {"parallel": {"timeout_per_block": 1}}}' "$SLOW"

LINES='main {
  let r = <<javascript
"a,b,c,d,e,f,g,h"
>>
  print("DONE")
}'
truth NE-05d "polyglot.output.max_output_lines / restrictions.polyglot_output.max_size: 1" \
    '{"limits": {"data": {"output_size": 1}}}' \
    '{"polyglot": {"output": {"max_output_lines": 1}}, "restrictions": {"polyglot_output": {"max_size": 1}}}' "$LINES"

NOTJSON='main {
  let r = <<javascript
"this is not json"
>>
  print("DONE")
}'
truth NE-05e "require_json_pipe / require_structured / validate_json, a non-JSON string" \
    '{"restrictions": {"polyglot_output": "json"}}' \
    '{"polyglot": {"output": {"require_json_pipe": true}}, "restrictions": {"polyglot_output": {"format": "any", "require_structured": true, "validate_json": true}}}' "$NOTJSON"

# f: the audit trail
d="$TEST_TMP/ne05f"; mk "$d" '{"audit": {"level": "basic", "output_file": "audit.jsonl", "log_events": {"polyglot_executed": true, "checks_passed": true, "checks_failed": true}}}'
run "$d" "$THREE"
if [ "$RC" != 0 ] || [ ! -s "$d/audit.jsonl" ] || ! grep -q '"polyglot_executed"' "$d/audit.jsonl"; then
    unm NE-05f "audit.log_events.checks_*: the control entry (polyglot_executed) was not written (rc=$RC)"
elif grep -qi '"event":"[a-z_]*check' "$d/audit.jsonl"; then
    fail NE-05f "the audit trail holds a check entry; the warning says it records none" "$(grep -i check "$d/audit.jsonl" | head -1 | cut -c1-200)"
else
    pass NE-05f "audit.log_events.checks_passed/failed: the trail holds no check entry, while polyglot_executed is written in the same run"
fi

# g: memory, through a child process
MEM='use process
main {
  let r = process.run("python3", ["-c", "b = bytearray(300 * 1024 * 1024); print(len(b) > 0)"])
  if r.get("exit_code", 1) == 0 { print("DONE") } else { throw "child refused: " + string(r.get("exit_code", -1)) }
}'
dc="$TEST_TMP/ne05g_c"; mk "$dc" '{"runtime": {"memory_limit": 64}}'; run "$dc" "$MEM"; crc=$RC
d0="$TEST_TMP/ne05g_0"; mk "$d0" ''; run "$d0" "$MEM"; zrc=$RC
if [ "$zrc" != 0 ]; then
    unm NE-05g "limits.memory.per_block_mb: the child cannot allocate 300 MB here even with no limit (rc=$zrc)"
elif [ "$crc" = 0 ]; then
    unm NE-05g "limits.memory.per_block_mb: runtime.memory_limit does not bind a child here (no rlimits)"
else
    ds="$TEST_TMP/ne05g_s"; mk "$ds" '{"limits": {"memory": {"per_block_mb": 64}}}'; run "$ds" "$MEM"
    if [ "$RC" = 0 ]; then pass NE-05g "limits.memory.per_block_mb: 64 lets a child allocate 300 MB, while runtime.memory_limit: 64 does not"
    else fail NE-05g "limits.memory.per_block_mb changed the run" "rc=$RC: $(tail -2 <<<"$OUT" | tr '\n' ' ' | cut -c1-200)"; fi
fi

echo
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
